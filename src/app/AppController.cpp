#include "AppController.h"

#include "AgentEvent.h"
#include "EditorFactory.h"
#include "TerminalFactory.h"
#include "UiStateStore.h"

#include <QCryptographicHash>
#include <QScopeGuard>
#include <QFileInfo>
#include <QDir>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

namespace ch {

namespace {

using CredentialKind = SshConnectionPool::CredentialKind;

QString credentialLabel(CredentialKind kind)
{
    return kind == CredentialKind::KeyPassphrase
               ? QStringLiteral("Private-key passphrase")
               : QStringLiteral("Password");
}

QString credentialKindName(CredentialKind kind)
{
    return kind == CredentialKind::KeyPassphrase
               ? QStringLiteral("keyPassphrase")
               : QStringLiteral("password");
}

// The fingerprint as OpenSSH prints it, for a human to compare against.
//
// Internally a fingerprint is base64(SHA-256(key blob)) with the trailing '='
// padding dropped, and it stays in that bare form everywhere it is COMPARED
// (m_approvedFingerprint against the value re-derived inside the host-key
// callback). But the approval dialog asks the user to check the string against
// what the server's administrator reports, and every OpenSSH tool that reports
// it — `ssh-keygen -lf`, `ssh-keyscan | ssh-keygen -lf -`, ssh's own
// trust-on-first-use question — writes the same base64 with a literal `SHA256:`
// in front of it. Without that prefix the two strings do not look alike at a
// glance, which discourages the very comparison being asked for. So the prefix
// is added HERE, at the one point the value is handed to the user interface, and
// nowhere else.
QString displayFingerprint(const QString& fingerprint)
{
    return QStringLiteral("SHA256:") + fingerprint;
}

// One (Dev Session, terminal pane) harness registration. refresh() gathers the
// whole set from the authoritative tree BEFORE pushing any of it to the agent
// monitor, so the push loop never walks a member container across a call that
// can re-enter this controller.
struct HarnessRegistration {
    QString devSessionId;
    QString terminalPaneId;
    QString harness;
};

} // namespace

AppController::AppController(CodeharbordClient* client, QObject* parent)
    : QObject(parent)
    , m_client(client)
    , m_db(std::make_unique<WorkspaceDb>(client))
    , m_sessionsModel(new SessionsModel(this))
    , m_uiState(new UiStateStore(QString(), this))
    , m_settings(new AppSettings(QString(), this))
    , m_logBuffer(new LogBuffer(this))
{
    // Once the sidebar has authoritative rows, reopen whatever Dev Session the
    // user was last in (no-op if one is already active or none was remembered).
    connect(this, &AppController::refreshed, this,
            [this] { restoreActiveSession(); });
}

AppController::~AppController()
{
    // Signal connections bound to `this` are severed by ~QObject, but the
    // pool's host-key and credential policies are NOT connections: they are
    // plain std::functions the pool stores, and the pool outlives this
    // controller (main.cpp declares it first, so it is destroyed last). Left
    // behind, they are lambdas holding a QPointer that is now null, so every
    // later handshake runs through a callback that can only ever answer
    // "reject" - which is the pool's own default anyway, only reached the slow
    // way and with a dead observer wired into it. Hand the pool its default
    // back explicitly.
    if (m_pool) {
        m_pool->setHostKeyCallback({});
        m_pool->setCredentialCallback({});
    }
}

void AppController::setServerId(const QString& serverId)
{
    if (m_serverId.value == serverId)
        return;
    QPointer<AppController> self(this);
    m_serverId.value = serverId;
    // Every workspace.list response belongs to the server id that was current
    // when it was issued. Invalidate all earlier generations BEFORE clearing
    // the visible cache, so a reply already in the client's router cannot
    // repopulate this sidebar after the switch.
    ++m_refreshGeneration;
    m_lastNodes.clear();
    m_terminalStates.clear();
    rebuildRows();
    if (!self)
        return;
    // The layouts repository is keyed by the same id. Keeping it in lockstep
    // HERE, rather than at each call site, is what stops a setLayout write
    // landing under the previous server's key after a switch.
    if (m_layouts)
        m_layouts->setServerId(serverId);
    if (!self)
        return;
    // A Dev Session belongs to exactly one server. Carrying the previous
    // server's active session across a switch would show its panes against the
    // new server's workspace and make the next layout write pair the OLD
    // devSessionId with the NEW serverId. Drop it (and its cached trees);
    // restoreActiveSession() reinstates whatever THIS server remembers as soon
    // as its rows arrive.
    if (!m_activeSessionId.isEmpty()) {
        // forget=false: the session is not GONE, it simply is not this server's
        // to show. Whatever the PREVIOUS server remembers stays remembered, so
        // switching back reopens it.
        clearActiveSession(/*forget=*/false);
        if (!self)
            return;
        emit activeSessionChanged();
        if (!self)
            return;
    }
    emit serverIdChanged();
    if (!self)
        return;
    // Switching the active server must reload the sidebar from that server's
    // authoritative tree; nothing else re-drives refresh() on a server change,
    // so without this a server switch would leave the previous server's rows
    // (or, at startup before any server is set, empty) on screen. The stale-
    // result guard in refresh() makes this safe to race with the initial
    // Component.onCompleted refresh.
    refresh();
}

QVector<GroupRow> AppController::toGroupRows(const QVector<GroupNode>& nodes)
{
    QVector<GroupRow> rows;
    rows.reserve(nodes.size());
    for (const GroupNode& node : nodes) {
        GroupRow row;
        row.group = node.group;
        row.sessions.reserve(node.sessions.size());
        for (const SessionNode& sessionNode : node.sessions) {
            SessionRow sessionRow;
            sessionRow.session = sessionNode.session;
            const QString& repoRoot = sessionNode.session.repositoryRoot;
            // QFileInfo::fileName() returns empty for a trailing-slash path, so
            // normalize first (also collapses `.`/`..` and redundant separators).
            sessionRow.subtitle =
                repoRoot.isEmpty()
                    ? QString()
                    : QFileInfo(QDir::cleanPath(repoRoot)).fileName();
            // terminals left empty: live terminal status is merged by the
            // terminal workstream, not derived from the persisted read.
            row.sessions.push_back(std::move(sessionRow));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool AppController::reportIfError(const std::optional<RpcError>& err)
{
    if (err) {
        // A teardown WE initiated fails every in-flight call by design (the
        // client synthesises "transport closed with request pending"). That is
        // the expected consequence of the user clicking Disconnect, not a fault
        // to paint red. Errors outside this window - including a transport that
        // dies on its own mid-session - are still reported verbatim (SPEC 10.3).
        if (m_tearingDown)
            return true;
        emit error(err->message);
        return true;
    }
    return false;
}

void AppController::setAgentMonitor(AgentStatusMonitor* monitor)
{
    if (m_agentMonitor == monitor)
        return;
    // Drop any prior wiring so a re-set (or clear) never leaves a dangling
    // connection firing rebuildRows() from a stale monitor.
    if (m_agentMonitor)
        disconnect(m_agentMonitor, nullptr, this, nullptr);
    m_agentMonitor = monitor;
    if (m_agentMonitor) {
        // Any agent transition or unseen-flag flip re-derives the badges from
        // the last known workspace tree and pushes them incrementally (a
        // targeted dataChanged(), not a full reset). The connection is bound
        // to `this` as the context object, so Qt severs it automatically when
        // this controller is destroyed (and QPointer protects the monitor
        // member if the caller destroys it first).
        connect(m_agentMonitor, &AgentStatusMonitor::agentStateChanged, this,
                [this](const QString&, const QString&, int) { applyAgentStateUpdate(); });
        connect(m_agentMonitor, &AgentStatusMonitor::unseenChanged, this,
                [this](const QString&, bool) { applyAgentStateUpdate(); });
    }
    // Re-merge immediately so a monitor set after the initial load reflects any
    // state it already accumulated, and a clear drops back to bare rows.
    rebuildRows();
}
void AppController::setTerminalFactory(TerminalFactory* factory)
{
    if (m_terminalFactory == factory)
        return;
    if (m_terminalFactory)
        disconnect(m_terminalFactory, nullptr, this, nullptr);
    // State belongs to the injected factory instance. Keeping it when the
    // factory is replaced would let a new graph inherit a stale disconnected
    // (or ready) status for the same server and pane ids.
    m_terminalStates.clear();
    m_terminalFactory = factory;
    if (m_terminalFactory) {
        // TerminalFactory is the only owner of pane/controller identity. The
        // controller merely stamps each event with the server id it arrived
        // from and ignores anything from a profile that is no longer current.
        connect(m_terminalFactory, &TerminalFactory::terminalStateChanged, this,
                [this](const QString& serverId, const QString& devSessionId,
                       const QString& terminalId, TerminalState state) {
                    if (serverId != m_serverId.value || devSessionId.isEmpty()
                        || terminalId.isEmpty()) {
                        return;
                    }
                    m_terminalStates[devSessionId].insert(terminalId, state);
                    applyAgentStateUpdate();
                });
    }
    // A factory may already have resolved panes before injection (a test or a
    // reconfigured QML graph), so the current tree must be re-merged now even
    // though no state signal is guaranteed to follow this setter.
    rebuildRows();
}

void AppController::setEditorFactory(EditorFactory* factory)
{
    // Not owned; see the header. Null is fine — recoveryDir simply never gets
    // forwarded and per-pane recovery stays disabled.
    m_editorFactory = factory;
}

void AppController::setConnection(SshConnectionPool* pool,
                                  SessionBootstrap* bootstrap,
                                  ServerProfiles* profiles,
                                  SessionLayouts* layouts)
{
    QPointer<AppController> self(this);
    // Re-injection must not stack a second set of connections onto the same
    // signals: a doubled stateChanged/wired/error handler would issue two
    // server.info calls per wire and show every connect failure twice. Severing
    // first is unconditional for that reason - re-injecting the SAME bootstrap
    // is the case a `!=` test silently gets wrong.
    if (m_bootstrap)
        disconnect(m_bootstrap, nullptr, this, nullptr);
    if (m_pool)
        disconnect(m_pool, nullptr, this, nullptr);
    if (m_profiles)
        disconnect(m_profiles, nullptr, this, nullptr);
    // The pool's host-key and credential policies are std::functions, not
    // signal connections, so disconnect() above does not touch them. A pool we
    // are letting go of must not keep callbacks that reach back into this
    // controller: SessionBootstrap's reconnect ladder re-handshakes through
    // whatever is installed, and a pool we no longer drive could otherwise
    // overwrite the pending fingerprint of an attempt running on the NEW pool.
    if (m_pool && m_pool != pool) {
        m_pool->setHostKeyCallback({});
        m_pool->setCredentialCallback({});
    }

    // Only reset the suffix guard when the pool actually CHANGES. Re-injecting
    // the same pool is an explicitly supported path (see the disconnect above),
    // and clearing the guard unconditionally made syncSshDiagnostics() treat
    // the whole transcript as new again, copying every line the log buffer
    // already held a second time.
    const bool poolChanged = m_pool != pool;
    m_pool = pool;
    m_bootstrap = bootstrap;
    m_profiles = profiles;
    m_layouts = layouts;
    if (poolChanged)
        m_lastSshDiagnostics.clear();

    // setConnection may land after a serverId is already known (test order, or
    // a re-injection); seed the layouts key so it is never one server behind.
    if (m_layouts)
        m_layouts->setServerId(m_serverId.value);
    if (!self)
        return;

    if (m_pool) {
        connect(m_pool, &SshConnectionPool::diagnosticLogChanged, this,
                [this, self] {
                    if (!self)
                        return;
                    syncSshDiagnostics();
                    if (!self)
                        return;
                    emit connectionDiagnosticsChanged();
                });
        // A pool can already hold a transcript when it is injected by a test
        // or by a reconnecting startup. Copy it before the first QML binding
        // reads the property, so no remote explanation is lost.
        syncSshDiagnostics();
        if (!self)
            return;
    }
    if (m_profiles) {
        // A profile save that could not take its interprocess lock still saves,
        // but without the safeguard that stops a second copy of CodeHarbor from
        // overwriting the server list. ServerProfiles hands up the cause; the
        // sentence is built here because this is the surface that shows it.
        //
        // Tone is deliberate: it leads with "saved". Worded as a failure the
        // user would retype a profile that is already on disk, which is worse
        // than saying nothing. It goes to the ordinary non-blocking toast — this
        // is a notice, not something to interrupt anybody with — and
        // ServerProfiles only raises it once per outage, so a run of saves
        // cannot turn it into a stutter.
        connect(m_profiles, &ServerProfiles::saveDegraded, this,
                [this](const QString& reason) {
                    emit error(tr("Server profile saved, but without the "
                                  "safeguard that keeps another copy of "
                                  "CodeHarbor from overwriting your server "
                                  "list: %1.")
                                   .arg(reason));
                });
    }
    if (m_bootstrap) {
        connect(m_bootstrap, &SessionBootstrap::channelDiagnostic, this,
                [this](const QString& role, const QString& text) {
                    // SessionBootstrap's channel stream is remote stderr and
                    // libssh channel faults. It never carries the credential
                    // callback arguments, so forwarding it is safe and gives
                    // the log pane a clear daemon origin.
                    m_logBuffer->appendRemote(
                        QStringLiteral("daemon"), QStringLiteral("ssh.channel"),
                        role, text, QtInfoMsg);
                });
        connect(m_bootstrap, &SessionBootstrap::provisioning, this,
                [this](const QString& message) {
                    m_logBuffer->appendRemote(
                        QStringLiteral("daemon"), QStringLiteral("provisioning"),
                        QStringLiteral("SessionBootstrap"), message, QtInfoMsg);
                });
        // The bootstrap reconnects on its own (backoff per SPEC 5.6); mirror its
        // state so the UI can show "reconnecting" instead of going quietly dead,
        // and re-adopt the server identity on every successful (re)wire.
        const auto mirrorState = [this, self](SessionBootstrap::State state) {
            if (!self)
                return;
            switch (state) {
            case SessionBootstrap::State::Connecting:
                setConnectionState(QStringLiteral("connecting"));
                break;
            case SessionBootstrap::State::Wired:
                setConnectionState(QStringLiteral("connected"));
                break;
            case SessionBootstrap::State::Reconnecting:
                setConnectionState(QStringLiteral("reconnecting"));
                break;
            case SessionBootstrap::State::Failed:
                setConnectionState(QStringLiteral("failed"), m_connectionError);
                break;
            case SessionBootstrap::State::Provisioning:
                // First connect to a server with no usable remote
                // service: SessionBootstrap is installing it. Its own
                // state rather than "connecting", because installing
                // downloads and unpacks a release tarball and can take
                // far longer than a handshake — a footer that says
                // "connecting" for thirty seconds is indistinguishable
                // from a hung connect. Every surface that renders a
                // connection state has a case for this string
                // (ConnectSheet.qml, SessionsSidebar.qml) and
                // tst_uxshell enumerates them.
                setConnectionState(QStringLiteral("provisioning"));
                break;
            case SessionBootstrap::State::Disconnected:
                setConnectionState(QStringLiteral("disconnected"));
                break;
            }
        };
        // Keep the QML property correct even when this controller is injected
        // after a bootstrap that is already running. The normal main() order
        // wires us before the first connect, but a late injection is a valid
        // test and reconfiguration path; without this read-back the signal
        // that published the current state has already been missed.
        connect(m_bootstrap, &SessionBootstrap::stateChanged, this, mirrorState);
        mirrorState(m_bootstrap->state());
        if (!self)
            return;
        // A bootstrap can already be Wired when it is injected (for example,
        // an environment-driven session created by an orchestrator). Its
        // `wired` signal is past, so repeat the identity handshake here rather
        // than leaving the sidebar keyed to the empty server id.
        if (m_bootstrap->state() == SessionBootstrap::State::Wired
            && m_client && m_client->transport())
            adoptServerIdentity();
        if (!self)
            return;
        connect(m_bootstrap, &SessionBootstrap::wired, this,
                [this] { adoptServerIdentity(); });
        connect(m_bootstrap, &SessionBootstrap::error, this,
                [this, self](const QString& message) {
                    if (!self)
                        return;
                    // connectionStateChanged is the NOTIFY of both
                    // connectionState and connectionError, so it must fire when
                    // one of them actually moves and not on every repeat. The
                    // reconnect ladder re-emits the SAME failure text on every
                    // rung, which used to re-notify every binding on the
                    // connection footer several times a second for as long as a
                    // host stayed down.
                    const bool changed = m_connectionError != message;
                    m_connectionError = message;
                    // While a connect attempt is in flight the failure may be
                    // EXPECTED: an unknown host key is deliberately refused so
                    // the user can be asked. Surfacing "SSH connection failed"
                    // for that would tell the user something went wrong when the
                    // app is simply waiting on their answer. Hold it until the
                    // attempt resolves, then either drop it (prompt raised) or
                    // report it (genuine failure). `error` is a one-shot event,
                    // not a property, so it is emitted for every failure even
                    // when the text repeats.
                    if (m_connecting)
                        m_heldConnectError = message;
                    else
                        emit error(message);
                    if (!self)
                        return;
                    if (changed)
                        emit connectionStateChanged();
                });
        // Straight to the toast, deliberately bypassing the hold above: this
        // fires on an attempt that goes on to SUCCEED, and the hold's success
        // path throws its message away. What failed is the thing the user
        // asked for, not the connection.
        connect(m_bootstrap, &SessionBootstrap::upgradeFailed, this,
                [this](const QString& message) { emit error(message); });
    }
    emit connectionChanged();
    if (!self)
        return;
    emit connectionDiagnosticsChanged();
}

QString AppController::sshDiagnostics() const
{
    return m_pool ? m_pool->diagnosticLog() : QString();
}

void AppController::syncSshDiagnostics()
{
    const QString current = m_pool ? m_pool->diagnosticLog() : QString();
    if (current == m_lastSshDiagnostics)
        return;

    QString newText;
    if (m_lastSshDiagnostics.isEmpty()) {
        newText = current;
    } else if (current.startsWith(m_lastSshDiagnostics)) {
        newText = current.mid(m_lastSshDiagnostics.size());
    } else if (!current.isEmpty()) {
        // The pool may have dropped the front of its own transcript at its
        // character cap. There is no stable line id in that API, so copying the
        // new snapshot is safer than silently losing the diagnostic that caused
        // the rollover. The shared log buffer has its own independent cap.
        newText = current;
    }
    m_lastSshDiagnostics = current;
    m_logBuffer->appendRemote(QStringLiteral("ssh"), QStringLiteral("libssh"),
                               QStringLiteral("SshConnectionPool"), newText,
                               QtInfoMsg);
}

void AppController::setConnectionState(const QString& state, const QString& err)
{
    if (m_connectionState == state && m_connectionError == err)
        return;
    m_connectionState = state;
    m_connectionError = err;
    emit connectionStateChanged();
}

void AppController::connectToProfile(QString profileId)
{
    // The guard comes FIRST, before anything is cleared. startConnect() refuses
    // a second connect while one is in flight or parked on a prompt, so
    // clearing the chain up here wiped the secrets and the `asked` flags of an
    // attempt that is still alive: a stray second click on Connect while the
    // password sheet was up threw away the passphrase the user had already
    // given, and the retry that followed arrived with the key locked again -
    // the exact `publickey,password` dead end the chain exists to avoid.
    if (m_connecting)
        return;
    // A user-initiated connect starts a FRESH chain: nothing a previous chain
    // gathered may be replayed at this one.
    m_credentials.clear();
    startConnect(profileId, QString());
}

void AppController::upgradeRemoteService(QString profileId)
{
    if (!m_bootstrap || !m_profiles)
        return;
    // Same guard as connectToProfile(), and for the same reason: a second
    // attempt while one is in flight (or parked on a host-key or credential
    // prompt) would race two sessions onto one pool. Arming the bootstrap here
    // and then not connecting would also leave the flag set for whichever
    // attempt eventually runs, turning an ignored click into a surprise
    // reinstall.
    if (m_connecting)
        return;
    if (profileId.isEmpty())
        profileId = m_profiles->activeId();
    if (profileId.isEmpty() || m_profiles->profile(profileId).isEmpty()) {
        emit error(tr("Choose a server before updating its CodeHarbor remote "
                      "service."));
        return;
    }

    // The install replaces the very files the live session is running from, so
    // the session goes first. This also clears the chain, which is why the arm
    // below comes after it.
    QPointer<AppController> self(this);
    disconnectServer();
    // disconnectServer() drives the bootstrap's teardown and emits both
    // activeSessionChanged and connectionStateChanged, and a listener is
    // allowed to tear the connection spine - or this very controller - down
    // while those are delivered. `self` covers the controller; the QPointer
    // member covers the bootstrap. Reading either raw would not.
    if (!self || !m_bootstrap)
        return;
    m_bootstrap->requestRemoteUpgrade();
    startConnect(profileId, QString());
}

void AppController::startConnect(const QString& profileId,
                                 QString acceptedFingerprint)
{
    // Every setConnectionState() below, and connectAndWire() itself, delivers
    // signals synchronously to QML and to the bootstrap handlers installed in
    // setConnection(). A listener there is allowed to destroy this controller -
    // the rest of the class already assumes exactly that - so the members
    // touched after each of those points have to be reached through a guard
    // rather than through a `this` that may already be gone.
    QPointer<AppController> self(this);
    if (!m_bootstrap || !m_profiles || !m_pool) {
        // There is no spine to dial with, so the chain is over before it began
        // and nothing it armed may survive it. An upgrade request left standing
        // here would turn whatever the user connects to next into an
        // unasked-for reinstall of the remote service.
        if (m_bootstrap)
            m_bootstrap->cancelRemoteUpgrade();
        return;
    }
    // One handshake at a time: a second invocation while a connect is running -
    // or while the user still owes us a host-key answer - would race two
    // sessions onto one pool and could re-enter the host-key callback. The flag
    // therefore stays set across the prompt and is cleared by resolveHostKey().
    if (m_connecting)
        return;

    const QVariantMap profile = m_profiles->profile(profileId);
    if (profile.isEmpty()) {
        // Reachable with a chain already in flight: the user asks to update a
        // server, parks on its host-key prompt, deletes the profile from the
        // connect sheet behind it and then accepts. The chain ends HERE, so it
        // gets the same full teardown the ordinary failure tail below performs
        // - including the armed upgrade, which would otherwise ambush the next
        // ordinary connect, and the profile id and held error, which the next
        // chain's failure path would report as though they had just happened.
        m_credentials.clear();
        m_pendingProfileId.clear();
        m_heldConnectError.clear();
        m_bootstrap->cancelRemoteUpgrade();
        setConnectionState(QStringLiteral("failed"),
                           tr("No such server profile."));
        return;
    }

    m_connecting = true;
    m_pendingProfileId = profileId;
    m_pendingFingerprint.clear();
    m_pendingHostKeyInfo = {};
    // Remembered for the whole chain, so a credential retry re-pins the key the
    // user already approved rather than asking about it all over again: a
    // connect refused at the AUTH stage never reached the code that persists a
    // newly trusted key, so the next attempt would meet it as Unknown again.
    m_approvedFingerprint = acceptedFingerprint;
    m_credentialRequested = false;
    m_credentialUser.clear();
    m_credentialLabel.clear();
    m_credentialKind = CredentialKind::KeyPassphrase;
    setConnectionState(QStringLiteral("connecting"));
    if (!self)
        return;

    // Copies, not moves: the chain keeps its secrets so the NEXT attempt in the
    // same chain can re-satisfy the method this one already got past. A server
    // requiring `publickey,password` needs the passphrase again on the attempt
    // that finally carries the password.
    installPoolCallbacks(std::move(acceptedFingerprint),
                         m_credentials.keyPassphrase, m_credentials.password);

    const bool ok = m_bootstrap->connectAndWire(
        profile.value(QStringLiteral("host")).toString(),
        static_cast<quint16>(profile.value(QStringLiteral("port")).toInt()),
        profile.value(QStringLiteral("user")).toString(),
        profile.value(QStringLiteral("nodePath")).toString(),
        profile.value(QStringLiteral("repoRoot")).toString(),
        profile.value(QStringLiteral("identityFile")).toString());
    // connectAndWire() runs the whole handshake inline and emits stateChanged,
    // error, provisioning, channelDiagnostic and (on success) wired along the
    // way. Nothing below may touch this controller until it is known to be
    // alive.
    if (!self)
        return;

    // The attempt is over as far as the pool is concerned, so re-install the
    // very same policy with NOTHING armed. Both callbacks stay installed on
    // purpose (SessionBootstrap's reconnect ladder re-handshakes through them,
    // and an unknown key met with nobody waiting must be refused), but neither
    // the secret nor the host-key approval may outlive the one attempt it was
    // typed for: a secret libssh never asked for would otherwise sit in the
    // callback for the rest of the process, and an approval the attempt never
    // reached the key check to spend would arm the next connect to any host.
    installPoolCallbacks(QString(), QString(), QString());

    if (ok) {
        m_connecting = false;
        m_heldConnectError.clear();
        m_pendingFingerprint.clear();
        m_pendingHostKeyInfo = {};
        m_approvedFingerprint.clear();
        m_credentialRequested = false;
        m_credentialUser.clear();
        m_credentialLabel.clear();
        m_credentialKind = CredentialKind::KeyPassphrase;
        // The chain succeeded: its secrets and its pending profile have done
        // their job and go now. Keeping the profile id would leave stale
        // attempt state behind until the next disconnect or connect.
        m_credentials.clear();
        m_pendingProfileId.clear();
        m_profiles->setActiveId(profileId);
        return;  // wired() -> adoptServerIdentity()
    }
    if (!m_pendingFingerprint.isEmpty()) {
        // The attempt is NOT over: it is parked on the user's answer, and
        // m_connecting stays set so nothing can start a second one underneath
        // it and swap m_pendingProfileId/m_pendingFingerprint out from under
        // resolveHostKey(). The refusal that got us here is expected, so its
        // error is dropped rather than shown.
        m_heldConnectError.clear();
        setConnectionState(QStringLiteral("hostkey"));
        if (!self)
            return;
        // displayFingerprint(), not the bare value: what leaves here is read by
        // a person and compared against ssh-keygen's output.
        emit hostKeyPrompt(m_pendingHostKeyInfo.first, m_pendingHostKeyInfo.second,
                           displayFingerprint(m_pendingFingerprint));
        return;
    }
    if (m_credentialRequested) {
        // The connection is parked on exactly one credential method. The kind
        // accompanies the prompt so QML can offer "Use password" without ever
        // reclassifying a private-key passphrase.
        m_heldConnectError.clear();
        setConnectionState(QStringLiteral("credential"));
        if (!self)
            return;
        emit credentialPrompt(m_credentialUser,
                              profile.value(QStringLiteral("host")).toString(),
                              m_credentialLabel,
                              credentialKindName(m_credentialKind));
        return;
    }
    m_connecting = false;
    m_approvedFingerprint.clear();
    // The chain is over and it failed. Nothing gathered along the way — the
    // secrets, nor the profile the chain was dialling — may be carried into the
    // next one.
    m_credentials.clear();
    m_pendingProfileId.clear();
    // Including an upgrade request the chain never got to spend. It survives a
    // parked prompt above — the retry that follows is the same user action —
    // but this is the chain ENDING, and an armed request left behind would turn
    // whatever the user connects to next into an unasked-for reinstall.
    if (m_bootstrap)
        m_bootstrap->cancelRemoteUpgrade();
    // A genuine failure: report it now that we know it was not the host-key path.
    if (!m_heldConnectError.isEmpty()) {
        const QString held = m_heldConnectError;
        m_heldConnectError.clear();
        emit error(held);
    }
    if (!self)
        return;
    setConnectionState(QStringLiteral("failed"), m_connectionError);
}

void AppController::installPoolCallbacks(QString acceptedFingerprint,
                                         QString keyPassphrase,
                                         QString password)
{
    if (!m_pool)
        return;

    // Unknown key: refuse THIS attempt, remember the fingerprint, and let the
    // user decide. Accepting re-runs the connect with the fingerprint
    // pre-approved, so the decision never blocks inside the handshake.
    //
    // The approval is captured BY VALUE and consumed exactly once. It used to
    // live on the controller and was cleared only on the accept path, so every
    // other exit (profile deleted while the prompt was up, TCP/auth failure
    // before the key check, a key that turned out to be already known) left an
    // approval armed for the next connect to any host. It never had teeth -
    // acceptance still requires an exact SHA-256 match - but "a stale approval
    // is only saved by a hash collision" is not an invariant worth keeping.
    QPointer<AppController> self(this);
    m_pool->setHostKeyCallback(
        [self, accepted = std::move(acceptedFingerprint)](
            const QString& host, const QString& keyType,
            const QByteArray& keyBlob, KnownHosts::Verdict) mutable {
            if (!self)
                return SshConnectionPool::HostKeyDecision::Reject;
            const QString fingerprint =
                QString::fromLatin1(
                    QCryptographicHash::hash(keyBlob, QCryptographicHash::Sha256)
                        .toBase64(QByteArray::OmitTrailingEquals));
            if (!accepted.isEmpty() && accepted == fingerprint) {
                accepted.clear();  // one shot, for this key only
                return SshConnectionPool::HostKeyDecision::Accept;
            }
            // This callback outlives the attempt that installed it: it stays on
            // the pool, and SessionBootstrap's own reconnect ladder (SPEC 5.6)
            // re-handshakes through it with nobody waiting on an answer. Only an
            // attempt WE started may arm a prompt; otherwise a reconnect that
            // meets an Unknown key would overwrite the very fingerprint the user
            // is being asked about, and resolveHostKey() would then retry the
            // original profile pinned to a fingerprint that was never shown.
            // Outside an attempt the key is simply refused - the same answer the
            // pool gives when no callback is installed at all.
            if (!self->m_connecting)
                return SshConnectionPool::HostKeyDecision::Reject;
            self->m_pendingFingerprint = fingerprint;
            self->m_pendingHostKeyInfo = qMakePair(host, keyType);
            return SshConnectionPool::HostKeyDecision::Reject;
        });

    // Credentials follow the same park-and-retry shape as host keys. libssh
    // calls this while its handshake is active, so a dialog here would re-enter
    // the UI. The first request is refused, the sheet asks, and
    // submitCredential() starts one fresh handshake with the secret in hand.
    //
    // A passphrase and a server password have different security boundaries:
    // a failed key unlock must not silently send the local key passphrase to a
    // remote host's password-auth endpoint. That rule is structural here — each
    // secret is armed in its OWN variable and only the one matching the kind
    // libssh asked for is ever returned, so there is no code path along which a
    // passphrase could be answered to a password request.
    // `CredentialReply::promptRequested` lets authenticate() stop at the first
    // outstanding prompt instead of overwriting it with the next auth method.
    //
    // Both secrets are captured BY VALUE and each is spent at most once per
    // attempt. They never reach ServerProfiles/QSettings and are never logged.
    // A server requiring `publickey,password` legitimately needs BOTH within one
    // attempt, which is why two can be armed at the same time.
    m_pool->setCredentialCallback(
        [self, keyPassphrase = std::move(keyPassphrase),
         password = std::move(password)](const QString& user,
                                         CredentialKind kind) mutable {
            if (!self)
                return SshConnectionPool::CredentialReply{};
            QString& armed = kind == CredentialKind::KeyPassphrase
                                 ? keyPassphrase
                                 : password;
            if (!armed.isEmpty()) {
                const QString once = armed;
                armed.clear();
                return SshConnectionPool::CredentialReply{once, false};
            }
            // A kind the user has already answered in this chain must not be
            // asked again: that is how an explicit "Use password" choice skips
            // the passphrase rung, and it is what bounds the prompt chain to one
            // question per credential kind instead of an unending cycle.
            const bool alreadyAsked =
                kind == CredentialKind::KeyPassphrase
                    ? self->m_credentials.askedKeyPassphrase
                    : self->m_credentials.askedPassword;
            if (alreadyAsked || !self->m_connecting)
                return SshConnectionPool::CredentialReply{};

            self->m_credentialRequested = true;
            self->m_credentialUser = user;
            self->m_credentialLabel = credentialLabel(kind);
            self->m_credentialKind = kind;
            if (kind == CredentialKind::KeyPassphrase)
                self->m_credentials.askedKeyPassphrase = true;
            else
                self->m_credentials.askedPassword = true;
            return SshConnectionPool::CredentialReply{{}, true};
        });
}

void AppController::resolveHostKey(bool accept)
{
    // Only meaningful while a prompt WE raised is outstanding. Without this a
    // stray answer (a sheet left open behind a live session, a double click on
    // Reject, or a fingerprint stashed by an automatic reconnect's key check)
    // would drop a connected shell to "disconnected", or worse, hand a
    // fingerprint nobody was ever shown to a fresh connect.
    if (!m_connecting || m_pendingFingerprint.isEmpty())
        return;

    const QString fingerprint = m_pendingFingerprint;
    const QString profileId = m_pendingProfileId;
    m_pendingFingerprint.clear();
    m_pendingHostKeyInfo = {};
    m_approvedFingerprint.clear();
    m_connecting = false;  // the parked attempt ends here, either way

    if (!accept || profileId.isEmpty()) {
        // The chain is abandoned: nothing it gathered, and nothing it was
        // aiming at, may leak into the next one. m_heldConnectError in
        // particular can have been re-armed while the prompt was parked (the
        // bootstrap keeps emitting failures at us with m_connecting still set),
        // and a leftover would be reported by the NEXT chain's failure path as
        // though it had just happened.
        m_credentials.clear();
        m_pendingProfileId.clear();
        m_heldConnectError.clear();
        // The credential half of the attempt state goes with the host-key half:
        // a request flag left standing describes a prompt nobody can answer any
        // more, and submitCredential() reads it.
        m_credentialRequested = false;
        m_credentialUser.clear();
        m_credentialLabel.clear();
        // Including an armed upgrade: this chain is over without reaching the
        // install, and a request left behind would make the user's NEXT
        // ordinary connect reinstall the remote service unasked.
        if (m_bootstrap)
            m_bootstrap->cancelRemoteUpgrade();
        setConnectionState(QStringLiteral("disconnected"));
        return;
    }
    startConnect(profileId, fingerprint);
}

void AppController::submitCredential(QString secret)
{
    submitCredential(std::move(secret), credentialKindName(m_credentialKind));
}

void AppController::submitCredential(QString secret, QString kind)
{
    // Only meaningful while a prompt WE raised is outstanding — the same guard
    // resolveHostKey() carries, and for the same reason: a sheet left open
    // behind a live session must not be able to redial it.
    if (!m_connecting || !m_credentialRequested)
        return;

    // Validate before anything is derived from it: a `kind` neither branch
    // recognises is a QML bug, and silently treating it as a private-key
    // passphrase would offer the typed secret to the wrong auth method.
    if (kind != QLatin1String("password")
        && kind != QLatin1String("keyPassphrase")) {
        return;
    }
    const CredentialKind submittedKind =
        kind == QLatin1String("password") ? CredentialKind::Password
                                          : CredentialKind::KeyPassphrase;

    const QString profileId = m_pendingProfileId;
    // The key the user approved earlier in THIS chain, re-pinned for the retry
    // so the host-key prompt does not reappear behind the credential prompt.
    const QString fingerprint = m_approvedFingerprint;
    m_credentialRequested = false;
    m_credentialUser.clear();
    m_credentialLabel.clear();
    m_approvedFingerprint.clear();
    m_connecting = false;  // the parked attempt ends here, either way

    if (secret.isEmpty() || profileId.isEmpty()) {
        // Cancelled. Nothing is retried and nothing is kept — including the
        // secrets an earlier step of this chain already supplied, the profile
        // it was dialling, and any failure held back while it was parked.
        m_credentials.clear();
        m_pendingProfileId.clear();
        m_heldConnectError.clear();
        // Same as the rejected host key above: the upgrade this chain was
        // carrying dies with the chain, not with whatever is connected next.
        if (m_bootstrap)
            m_bootstrap->cancelRemoteUpgrade();
        setConnectionState(QStringLiteral("disconnected"));
        return;
    }
    // Moved into the chain's slot for its OWN kind, so the retry can offer it to
    // that method and to no other. The kind is also marked answered, which is
    // what lets an explicit "Use password" reply to a passphrase request skip
    // the passphrase rung instead of being asked about it again.
    //
    // The chain keeps it because one attempt may not be enough: a server that
    // requires a key AND a password needs the passphrase again on the attempt
    // that finally carries the password.
    if (submittedKind == CredentialKind::KeyPassphrase) {
        m_credentials.keyPassphrase = std::move(secret);
        m_credentials.askedKeyPassphrase = true;
    } else {
        m_credentials.password = std::move(secret);
        m_credentials.askedPassword = true;
    }
    startConnect(profileId, fingerprint);
}

void AppController::disconnectServer()
{
    // Deliberately NOT gated on m_bootstrap. Everything below the teardown call
    // is CLIENT-side state - a parked prompt, a gathered secret, the active Dev
    // Session - and it is just as wrong to keep any of it when there is no
    // session spine to tear down. An early return here left refuseServer()'s
    // deferred disconnect (and any shell built without a bootstrap) wedged with
    // m_connecting set, which refuses every later connect.
    //
    // A disconnect while a host-key prompt is parked also abandons that
    // attempt; leaving m_connecting set would wedge every later connect.
    m_connecting = false;
    m_pendingFingerprint.clear();
    m_pendingHostKeyInfo = {};
    m_approvedFingerprint.clear();
    // The abandoned attempt's profile and its held-back failure text go too.
    // A held error that outlived its attempt would be reported by the NEXT
    // chain's failure path as if it had just happened.
    m_pendingProfileId.clear();
    m_heldConnectError.clear();
    m_credentialRequested = false;
    m_credentialUser.clear();
    m_credentialLabel.clear();
    m_credentialKind = CredentialKind::KeyPassphrase;
    // Whatever the abandoned chain had gathered goes with it.
    m_credentials.clear();

    // Scope-guarded so an early return or a throw inside the teardown cannot
    // leave errors permanently muted. The guard is the ONLY thing bounding the
    // mute window, and that is sound because the failures are synchronous:
    // disconnectSession() -> unwire() -> CodeharbordClient::failAllPending()
    // runs every pending callback inline before returning, so they all land
    // inside this scope. A client that ever deferred them would need this flag
    // cleared by the arrival of the LAST failure instead.
    QPointer<AppController> self(this);
    QPointer<SessionBootstrap> bootstrap = m_bootstrap;
    if (bootstrap) {
        m_tearingDown = true;
        // disconnectSession() emits state changes synchronously. A listener is
        // allowed to delete this controller while that signal is delivered,
        // so the guard must not retain a raw `this` for its destructor.
        const auto restore = qScopeGuard([self] {
            if (self)
                self->m_tearingDown = false;
        });
        bootstrap->disconnectSession();
        if (!self)
            return;
    }

    // Land on a clear empty state instead of a half-live shell. The Dev
    // Session context is unreachable now, and leaving it "active" is not
    // cosmetic: SessionLayouts would still hold the devSessionId and still
    // pass canEdit(), so a Split command after Disconnect mutates and
    // republishes a tree whose setLayout fails - the UI shows a split the
    // server does not have, and because a reconnect never reloads a session
    // that is still active, the first edit that DOES land writes that
    // divergent tree back over the real one.
    //
    // forget=false: the session is unreachable, not gone. UiStateStore keeps
    // remembering it, so the reconnect's refresh -> restoreActiveSession
    // reopens it and reloads BOTH region trees from the server - which also
    // picks up any layout another client changed while we were down. The
    // reload is free of pane churn when nothing changed: applyLoadedTree only
    // republishes a region whose tree actually differs.
    //
    // This is the explicit user Disconnect only. The bootstrap's automatic
    // reconnect ladder never comes through here, so a dropped link still
    // leaves the panes exactly where they were.
    if (!m_activeSessionId.isEmpty()) {
        clearActiveSession(/*forget=*/false);
        if (!self)
            return;
        emit activeSessionChanged();
        if (!self)
            return;
    }

    setConnectionState(QStringLiteral("disconnected"));
}

void AppController::adoptServerIdentity()
{
    if (!m_client)
        return;
    QPointer<AppController> self(this);
    m_client->call(QStringLiteral("server.info"), QJsonObject{},
                   [self](QJsonValue result, std::optional<RpcError> err) {
                       if (!self)
                           return;
                       // reportIfError(), not a bare emit: a teardown WE
                       // started fails every in-flight call by design (the
                       // client synthesises "transport closed with request
                       // pending"), and this call is in flight for the whole
                       // window a user's Disconnect covers. Painting that red
                       // tells the user something broke when they are the one
                       // who pressed the button. Every other callback in this
                       // class already routes through the same gate.
                       if (self->reportIfError(err))
                           return;
                       const QJsonObject info = result.toObject();
                       // Compatibility gate, BEFORE anything is adopted.
                       // `schemaVersion` was parsed and then never checked, so
                       // a client one release ahead of its codeharbord keyed
                       // the whole workspace to the empty serverId an older
                       // server does not report — a healthy SSH session with a
                       // permanently empty sidebar and nothing on screen saying
                       // why. Version skew is the DEFAULT state under manual
                       // deployment, so it gets a real message.
                       const int schema =
                           info.value(QStringLiteral("schemaVersion")).toInt(0);
                       if (schema < kMinimumServerSchemaVersion) {
                           const QString remoteVersion =
                               info.value(QStringLiteral("version")).toString();
                           const QString message =
                               tr("Server too old: codeharbord %1 speaks "
                                  "workspace schema %2, but this client needs "
                                  "schema %3 or newer. Use \"Update server\" in "
                                  "the connect sheet to install the matching "
                                  "release, or update the CodeHarbor remote on "
                                  "that host yourself and reconnect.")
                                   .arg(remoteVersion.isEmpty()
                                            ? tr("(version not reported)")
                                            : remoteVersion)
                                   .arg(schema)
                                   .arg(kMinimumServerSchemaVersion);
                           self->refuseServer(message);
                           return;
                       }
                       // Forward the server's recovery directory to the editor
                       // factory so per-pane crash-recovery snapshots (SPEC 11.3)
                       // land under a SERVER-chosen path, correct across hosts.
                       // Additive/optional (schemaVersion unchanged): an older
                       // server omits it and recovery degrades to disabled.
                       if (self->m_editorFactory)
                           self->m_editorFactory->setRecoveryDir(
                               info.value(QStringLiteral("recoveryDir")).toString());
                       const QString id =
                           info.value(QStringLiteral("serverId")).toString();
                       if (id.isEmpty()) {
                           // Refused on the same terms as a too-old server, and
                           // for the same reason: workspace rows are keyed by
                           // this id, so carrying on would leave a healthy SSH
                           // session showing a permanently empty sidebar with
                           // nothing but a toast to explain it. Any server at
                           // or above the schema floor reports one (SPEC 3.5),
                           // so an empty value is a broken remote, not skew.
                           self->refuseServer(
                               tr("Server did not report an identity; refusing to "
                                  "key this workspace to a client-side id."));
                           return;
                       }
                       if (self->m_serverId.value == id) {
                           // Same server - the common case, since this runs on
                           // EVERY successful wire including every automatic
                           // reconnect. setServerId() would early-out and
                           // nothing else re-reads the workspace afterwards, so
                           // the sidebar would silently freeze on whatever it
                           // held when the link dropped (and `refreshed`, which
                           // drives restoreActiveSession, would never fire
                           // again). Exactly one list() per wire, not a storm.
                           self->refresh();
                           return;
                       }
                       // Different server behind this profile: setServerId()
                       // re-keys the layouts, drops the previous server's
                       // active session and refreshes.
                       self->setServerId(id);
                   });
}

void AppController::refuseServer(const QString& message)
{
    QPointer<AppController> self(this);
    emit error(message);
    // `error` is delivered synchronously to every connected slot, and one of
    // them is allowed to destroy this controller. Everything below touches
    // `this` (QTimer::singleShot takes it as the context object), so the
    // guard has to be checked here and not only inside the deferred lambda.
    if (!self)
        return;
    // Refuse rather than limp on. Deferred by one event-loop turn because both
    // callers run INSIDE a CodeharbordClient response callback and the teardown
    // drops that very client's transport. The QPointer keeps the deferred work
    // from touching a controller destroyed in the meantime; QTimer's context
    // overload would already cancel it, and the explicit check documents why.
    QTimer::singleShot(0, this, [self, message] {
        if (!self)
            return;
        self->disconnectServer();
        self->setConnectionState(QStringLiteral("failed"), message);
    });
}

bool AppController::sessionExists(const QString& devSessionId) const
{
    for (const GroupNode& group : m_lastNodes)
        for (const SessionNode& session : group.sessions)
            if (session.session.id.value == devSessionId)
                return true;
    return false;
}

bool AppController::sessionIsArchived(const QString& devSessionId) const
{
    for (const GroupNode& group : m_lastNodes)
        for (const SessionNode& session : group.sessions)
            if (session.session.id.value == devSessionId)
                return session.session.archived;
    return false;
}

bool AppController::dropActiveSessionIfGone()
{
    // An archived session is unavailable to the normal sidebar just like a
    // deleted one. Retire it before rebuilding the filtered model so panes
    // never remain on screen for a row the sidebar no longer exposes.
    if (m_activeSessionId.isEmpty()
        || (sessionExists(m_activeSessionId) && !sessionIsArchived(m_activeSessionId)))
        return false;

    clearActiveSession(/*forget=*/true);
    return true;
}

void AppController::clearActiveSession(bool forget)
{
    m_activeSessionId.clear();
    // Forget it for THIS server only: the key is per-server, and clearing it
    // wholesale would throw away another server's perfectly good session.
    if (forget && m_uiState)
        m_uiState->setActiveSession(m_serverId.value, QString());
    // Dropping the layout trees is the part that matters beyond cosmetics: a
    // SessionLayouts still holding the devSessionId keeps passing canEdit(), so
    // the next splitPane/closePane/setRatios mutates and republishes a tree it
    // cannot persist - the UI then shows a split the server does not have, and
    // the first edit that DOES land writes that divergent tree back.
    // load(QString()) clears both regions and deselects.
    if (m_layouts)
        m_layouts->load(QString());
}
void AppController::restoreActiveSession()
{
    // Never fight the user: once anything is active, the remembered id has
    // already done its job.
    if (!m_activeSessionId.isEmpty() || !m_uiState)
        return;
    const QString remembered = m_uiState->activeSession(m_serverId.value);
    if (remembered.isEmpty())
        return;
    // The remembered session may be gone (deleted here before the id was
    // forgotten, or from another client), or archived and therefore hidden
    // from the normal sidebar. Activating either would load a phantom/hidden
    // session and leave panes on screen with no visible row. Forget it instead
    // of reopening it.
    if (!sessionExists(remembered) || sessionIsArchived(remembered)) {
        m_uiState->setActiveSession(m_serverId.value, QString());
        return;
    }
    activateSession(remembered);
}

void AppController::activateSession(QString devSessionId)
{
    // Activation is driven by a sidebar row, but a stale click can arrive
    // while a refresh is replacing that row. Refuse ids the authoritative
    // cache does not contain (and archived rows hidden by the default filter)
    // instead of loading a phantom layout and remembering it for next launch.
    if (devSessionId.isEmpty() || !sessionExists(devSessionId)
        || sessionIsArchived(devSessionId)) {
        return;
    }
    m_activeSessionId = devSessionId;
    if (m_uiState)
        m_uiState->setActiveSession(m_serverId.value, devSessionId);
    // Deliberately NOT short-circuited when the id is unchanged: re-picking the
    // current session is the user's natural retry after a layout load failed.
    if (m_layouts)
        m_layouts->load(devSessionId);
    emit activeSessionChanged();
}

QString AppController::activeSessionRepoRoot() const
{
    for (const GroupNode& group : m_lastNodes)
        for (const SessionNode& session : group.sessions)
            if (session.session.id.value == m_activeSessionId)
                return session.session.repositoryRoot;
    return {};
}

QVector<GroupRow> AppController::computeRows() const
{
    // Start from the persisted tree and overlay both live sources: TerminalFactory
    // owns per-pane connection state, while AgentStatusMonitor owns agent state.
    // Rebuilding the vectors from m_lastNodes on every call keeps a refresh or
    // either live event from wiping the other source.
    QVector<GroupRow> rows = toGroupRows(m_lastNodes);
    // toGroupRows preserves the node order 1:1, so the row tree lines up
    // index-for-index with m_lastNodes; walk them in lockstep.
    for (qsizetype gi = 0; gi < m_lastNodes.size(); ++gi) {
        const GroupNode& groupNode = m_lastNodes.at(gi);
        GroupRow& groupRow = rows[gi];
        for (qsizetype si = 0; si < groupNode.sessions.size(); ++si) {
            const SessionNode& sessionNode = groupNode.sessions.at(si);
            SessionRow& sessionRow = groupRow.sessions[si];
            const QString& devSessionId = sessionNode.session.id.value;
            const auto terminalStates = m_terminalStates.constFind(devSessionId);
            sessionRow.terminals.reserve(sessionNode.terminalPanes.size());
            for (const TerminalPane& pane : sessionNode.terminalPanes) {
                TerminalStatus status;
                status.id = pane.id;
                if (terminalStates != m_terminalStates.constEnd()) {
                    const auto state = terminalStates->constFind(pane.id.value);
                    if (state != terminalStates->constEnd())
                        status.connection = state.value();
                }
                status.agent = AgentState::Unknown;
                if (m_agentMonitor) {
                    AgentState agent = static_cast<AgentState>(
                        m_agentMonitor->stateFor(devSessionId, pane.id.value));
                    // The monitor keeps a terminal at IdleUnseen even after the
                    // Dev Session's completion has been marked seen (markSeen
                    // clears only the per-session unseen flag, not the terminal's
                    // raw agent state). If we copied IdleUnseen through,
                    // aggregateSessionState would keep the row FinishedUnseen and
                    // the badge would never clear. Downgrade IdleUnseen -> Idle
                    // for the row once the session is no longer flagged unseen.
                    if (agent == AgentState::IdleUnseen
                        && !m_agentMonitor->hasUnseen(devSessionId)) {
                        agent = AgentState::Idle;
                    }
                    status.agent = agent;
                }
                sessionRow.terminals.push_back(std::move(status));
            }
        }
    }
    return rows;
}

void AppController::rebuildRows()
{
    // Structural refresh: replace the whole tree (used on workspace reload).
    m_sessionsModel->setGroups(computeRows());
}

void AppController::applyAgentStateUpdate()
{
    // An agent event never alters the sidebar's structure, only per-terminal
    // state feeding each session's badge. Push it incrementally so the sidebar
    // is not fully reset - and every delegate destroyed and recreated - on
    // every single status flip; updateTerminalStates emits a targeted
    // dataChanged() for just the rows whose aggregate state actually moved.
    m_sessionsModel->updateTerminalStates(computeRows());
}

void AppController::refresh()
{
    // Nothing to ask when no transport is bound: the call would fail instantly
    // with "no transport bound" and surface as a user-facing error toast, which
    // is exactly what a user sees on a cold start before they have connected.
    // This is a capability check, NOT a filter on the error text - a transport
    // that exists and then dies mid-session still reports its failure verbatim.
    if (!m_client || !m_client->transport())
        return;

    // Each refresh is a full-tree re-read; several can be in flight at once
    // (every mutation chains one, plus the sidebar's initial load and any
    // setServerId). The client routes responses by id, so replies may arrive
    // out of order — without a guard an older list() result could overwrite a
    // newer one, leaving the sidebar stale. Stamp each refresh with a monotonic
    // generation and only let the most recent one mutate the model. Errors are
    // still reported verbatim regardless of generation (a real server error is
    // worth surfacing even if superseded).
    const quint64 generation = ++m_refreshGeneration;
    QPointer<AppController> self(this);
    // Deliberately NOT filtered on the server. The sidebar's "only pinned" and
    // "show archived" switches are presentation state (SPEC 11.2), and
    // SessionsModel already hides the rows they hide. Asking the server for a
    // FILTERED tree as well made `m_lastNodes` - the copy every other decision
    // in this class reads - stop describing the workspace: with the pin filter
    // on, an unpinned Dev Session looked deleted, so sessionExists() reported
    // false and dropActiveSessionIfGone() retired the session the user was
    // working in, and sessionCountForGroup() undercounted the sessions a group
    // deletion would destroy. One authoritative tree, filtered only for
    // display.
    m_db->list(m_serverId, [self, generation](QVector<GroupNode> nodes,
                                              std::optional<RpcError> err) {
        if (!self)
            return;
        if (self->reportIfError(err))
            return;
        if (generation != self->m_refreshGeneration)
            return; // a newer refresh has superseded this result
        // activeSessionRepoRoot is derived from these rows but notified by
        // activeSessionChanged, which a refresh does not otherwise emit. A repo
        // root edited on the server would leave the terminal region bound to
        // the stale working directory forever.
        const QString repoRootBefore = self->activeSessionRepoRoot();
        self->m_lastNodes = std::move(nodes);
        // TerminalFactory events can outlive a pane row: a pane may be closed
        // between two workspace reads. Retain only the state represented by
        // this authoritative tree so the cache cannot grow forever or repaint
        // a later pane that happens to use the same local id.
        QHash<QString, QSet<QString>> liveTerminalIds;
        for (const GroupNode& groupNode : self->m_lastNodes) {
            for (const SessionNode& sessionNode : groupNode.sessions) {
                QSet<QString>& paneIds =
                    liveTerminalIds[sessionNode.session.id.value];
                for (const TerminalPane& pane : sessionNode.terminalPanes)
                    paneIds.insert(pane.id.value);
            }
        }
        for (auto sessionIt = self->m_terminalStates.begin();
             sessionIt != self->m_terminalStates.end();) {
            const auto liveSessionIt =
                liveTerminalIds.constFind(sessionIt.key());
            if (liveSessionIt == liveTerminalIds.constEnd()) {
                sessionIt = self->m_terminalStates.erase(sessionIt);
                continue;
            }
            QHash<QString, TerminalState>& states = sessionIt.value();
            for (auto stateIt = states.begin(); stateIt != states.end();) {
                if (!liveSessionIt->contains(stateIt.key()))
                    stateIt = states.erase(stateIt);
                else
                    ++stateIt;
            }
            ++sessionIt;
        }
        // Only HERE: past the error return (an RpcError means "we do not know",
        // not "it is gone") and past the generation guard, so a stale list()
        // can never retire a session the newest tree still has.
        // AG7: evict agent state for Dev Sessions the freshly rebuilt tree no
        // longer lists. The tree is authoritative here, so a whole Dev Session
        // subtree that is gone is genuinely gone; retainDevSessions drops it as
        // a unit (never on terminal close, which would lose an unseen-completion
        // badge). Done before rebuildRows so the merge sees only live state.
        //
        // The same walk republishes every pane's harness (SPEC 6.6): the monitor
        // has to know which panes run the adapterless "generic" harness, because
        // those take their agent state from terminal output activity rather
        // than from the wire, and this tree is where the harness column lives.
        // AFTER the eviction, which drops the previous registrations with their
        // Dev Session subtrees.
        //
        // ch::TerminalFactory registers a harness too, from the row it just
        // resolved for a pane. That is NOT a duplicate of this walk and neither
        // one can be dropped: this is the only registration a pane the user has
        // never opened ever gets, and it is what re-registers everything after
        // the eviction above; the factory's is the only one a pane that was
        // resolved since the last refresh gets. setTerminalHarness is
        // idempotent, so both running costs nothing.
        if (self->m_agentMonitor) {
            // The retain set and the harness registrations come out of ONE walk
            // of the authoritative tree, up front, so the push loop below walks
            // a LOCAL list instead of m_lastNodes. retainDevSessions() and
            // setTerminalHarness() reach the monitor, whose signals come back
            // through applyAgentStateUpdate() into the sidebar model and from
            // there into QML - and a handler there is allowed to re-enter this
            // controller and replace m_lastNodes. A range-for over the member
            // across those calls would be walking a vector something else had
            // reallocated underneath it.
            QSet<QString> liveDevSessions;
            QVector<HarnessRegistration> registrations;
            for (const GroupNode& groupNode : self->m_lastNodes) {
                for (const SessionNode& sessionNode : groupNode.sessions) {
                    const QString& devSessionId = sessionNode.session.id.value;
                    liveDevSessions.insert(devSessionId);
                    for (const TerminalPane& pane : sessionNode.terminalPanes) {
                        registrations.push_back(
                            {devSessionId, pane.id.value, pane.harness});
                    }
                }
            }
            self->m_agentMonitor->retainDevSessions(liveDevSessions);
            // Guarded BEFORE each dereference, not after it: a check that runs
            // once the call it is meant to guard has already returned has
            // nothing left to protect.
            for (const HarnessRegistration& registration : registrations) {
                if (!self)
                    return;
                // The monitor vanishing mid-loop stops the REGISTRATIONS, but
                // must not abandon the rest of this callback. m_lastNodes was
                // already replaced above, and the sidebar has not been rebuilt
                // from it yet: returning here would leave every session name,
                // subtitle, group name and collapsed flag from the PREVIOUS
                // tree on screen until some later refresh, and would skip
                // dropActiveSessionIfGone() and `refreshed` (which is what
                // drives restoreActiveSession) with it.
                if (!self->m_agentMonitor)
                    break;
                self->m_agentMonitor->setTerminalHarness(
                    registration.devSessionId, registration.terminalPaneId,
                    registration.harness);
            }
        }
        if (!self)
            return;
        const bool droppedActive = self->dropActiveSessionIfGone();
        self->rebuildRows();
        if (!self)
            return;
        if (droppedActive || self->activeSessionRepoRoot() != repoRootBefore) {
            emit self->activeSessionChanged();
            if (!self)
                return;
        }
        emit self->refreshed();
    });
}

void AppController::createGroup(QString name)
{
    CreateGroupParams params;
    params.serverId = m_serverId;
    params.name = std::move(name);
    m_db->createGroup(params, refreshOnSuccess<std::optional<Group>>());
}

void AppController::renameGroup(QString id, QString name)
{
    UpdateGroupParams params;
    params.id = GroupId{std::move(id)};
    params.name = std::move(name);
    m_db->updateGroup(params, refreshOnSuccess<std::optional<Group>>());
}

void AppController::setGroupCollapsed(QString id, bool collapsed)
{
    UpdateGroupParams params;
    params.id = GroupId{std::move(id)};
    params.collapsed = collapsed;
    m_db->updateGroup(params, refreshOnSuccess<std::optional<Group>>());
}
void AppController::deleteGroup(QString id)
{
    m_db->deleteGroup(GroupId{std::move(id)}, refreshOnSuccess<>());
}

int AppController::sessionCountForGroup(const QString& id) const
{
    for (const GroupNode& group : m_lastNodes) {
        if (group.group.id.value == id)
            return static_cast<int>(group.sessions.size());
    }
    return 0;
}

void AppController::reorderGroups(QStringList orderedIds)
{
    QVector<GroupId> ids;
    ids.reserve(orderedIds.size());
    for (const QString& id : orderedIds)
        ids.push_back(GroupId{id});
    m_db->reorderGroups(m_serverId, ids, refreshOnSuccess<>());
}

void AppController::createSession(QString groupId, QString name, QString repoRoot)
{
    CreateSessionParams params;
    params.serverId = m_serverId;
    params.groupId = GroupId{std::move(groupId)};
    params.name = std::move(name);
    params.repositoryRoot = std::move(repoRoot);
    m_db->createSession(params, refreshOnSuccess<std::optional<DevSession>>());
}

void AppController::renameSession(QString id, QString name)
{
    UpdateSessionParams params;
    params.id = DevSessionId{std::move(id)};
    params.name = std::move(name);
    m_db->updateSession(params, refreshOnSuccess<std::optional<DevSession>>());
}
void AppController::setSessionPinned(QString id, bool pinned)
{
    UpdateSessionParams params;
    params.id = DevSessionId{std::move(id)};
    params.pinned = pinned;
    m_db->updateSession(params, refreshOnSuccess<std::optional<DevSession>>());
}
void AppController::updateSessionArchived(QString id, bool archived)
{
    UpdateSessionParams params;
    params.id = DevSessionId{std::move(id)};
    params.archived = archived;
    const QString sessionId = params.id.value;
    QPointer<AppController> self(this);
    m_db->updateSession(
        params,
        [self, sessionId, archived](std::optional<DevSession>,
                                    std::optional<RpcError> err) {
            if (!self)
                return;
            if (self->reportIfError(err))
                return;
            // An archived active session would disappear under the default
            // filter while its panes remained live. Retire the active context
            // on the acknowledged archive, just as deletion retires it once
            // the authoritative refresh confirms the row is gone.
            if (archived && self->m_activeSessionId == sessionId) {
                self->clearActiveSession(/*forget=*/true);
                if (!self)
                    return;
                emit self->activeSessionChanged();
                if (!self)
                    return;
            }
            self->refresh();
        });
}

void AppController::archiveSession(QString id)
{
    updateSessionArchived(std::move(id), true);
}

void AppController::unarchiveSession(QString id)
{
    updateSessionArchived(std::move(id), false);
}

void AppController::duplicateSession(QString id)
{
    m_db->duplicateSession(DevSessionId{std::move(id)},
                          refreshOnSuccess<std::optional<SessionNode>>());
}

void AppController::moveSession(QString id, QString groupId, int position)
{
    MoveSessionParams params;
    params.id = DevSessionId{std::move(id)};
    params.groupId = GroupId{std::move(groupId)};
    params.position = position;
    m_db->moveSessionToGroup(params, refreshOnSuccess<std::optional<DevSession>>());
}

void AppController::deleteSession(QString id)
{
    m_db->deleteSession(DevSessionId{std::move(id)}, refreshOnSuccess<>());
}

void AppController::reorderSessions(QString groupId, QStringList orderedIds)
{
    QVector<DevSessionId> ids;
    ids.reserve(orderedIds.size());
    for (const QString& id : orderedIds)
        ids.push_back(DevSessionId{id});
    m_db->reorderSessions(GroupId{std::move(groupId)}, ids, refreshOnSuccess<>());
}

void AppController::setTerminalPaneHarness(QString terminalPaneId, QString harness)
{
    // The vocabulary lives in one place, next to the parser that has to agree
    // with it; an empty string is the fifth legal value and means "plain shell,
    // no harness", which isHarnessWire deliberately does not accept.
    if (!harness.isEmpty() && !detail::isHarnessWire(harness)) {
        emit error(tr("\"%1\" is not a terminal harness CodeHarbor knows.")
                       .arg(harness));
        return;
    }
    UpdateTerminalPaneParams params;
    params.id = TerminalId{std::move(terminalPaneId)};
    params.harness = std::move(harness);
    // refresh() on success is not bookkeeping here: its harness walk is the one
    // thing that re-registers the pane with the agent monitor, so without it
    // the new value would sit on the server and change nothing on screen.
    m_db->updateTerminalPane(params, refreshOnSuccess<std::optional<TerminalPane>>());
}

QString AppController::terminalPaneHarness(const QString& terminalPaneId) const
{
    for (const GroupNode& group : m_lastNodes)
        for (const SessionNode& session : group.sessions)
            for (const TerminalPane& pane : session.terminalPanes)
                if (pane.id.value == terminalPaneId)
                    return pane.harness;
    return {};
}

} // namespace ch
