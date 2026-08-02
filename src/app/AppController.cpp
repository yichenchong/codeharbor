#include "AppController.h"

#include "EditorFactory.h"
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

} // namespace

AppController::AppController(CodeharbordClient* client, QObject* parent)
    : QObject(parent)
    , m_client(client)
    , m_db(std::make_unique<WorkspaceDb>(client))
    , m_sessionsModel(new SessionsModel(this))
    , m_uiState(new UiStateStore(QString(), this))
    , m_settings(new AppSettings(QString(), this))
{
    // Once the sidebar has authoritative rows, reopen whatever Dev Session the
    // user was last in (no-op if one is already active or none was remembered).
    connect(this, &AppController::refreshed, this,
            [this] { restoreActiveSession(); });
}

AppController::~AppController() = default;

void AppController::setServerId(const QString& serverId)
{
    if (m_serverId.value == serverId)
        return;
    m_serverId.value = serverId;
    // The layouts repository is keyed by the same id. Keeping it in lockstep
    // HERE, rather than at each call site, is what stops a setLayout write
    // landing under the previous server's key after a switch.
    if (m_layouts)
        m_layouts->setServerId(serverId);
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
        emit activeSessionChanged();
    }
    emit serverIdChanged();
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
        // targeted dataChanged(), not a full reset). The QPointer-free lambda is
        // safe: the
        // connection is bound to `this` as the context object, so Qt severs it
        // automatically when this controller is destroyed (no UAF), and the
        // disconnect above severs it on re-set.
        connect(m_agentMonitor, &AgentStatusMonitor::agentStateChanged, this,
                [this](const QString&, const QString&, int) { applyAgentStateUpdate(); });
        connect(m_agentMonitor, &AgentStatusMonitor::unseenChanged, this,
                [this](const QString&, bool) { applyAgentStateUpdate(); });
    }
    // Re-merge immediately so a monitor set after the initial load reflects any
    // state it already accumulated, and a clear drops back to bare rows.
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

    m_pool = pool;
    m_bootstrap = bootstrap;
    m_profiles = profiles;
    m_layouts = layouts;

    // setConnection may land after a serverId is already known (test order, or
    // a re-injection); seed the layouts key so it is never one server behind.
    if (m_layouts)
        m_layouts->setServerId(m_serverId.value);

    if (m_pool) {
        connect(m_pool, &SshConnectionPool::diagnosticLogChanged, this,
                &AppController::connectionDiagnosticsChanged);
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
        // The bootstrap reconnects on its own (backoff per SPEC 5.6); mirror its
        // state so the UI can show "reconnecting" instead of going quietly dead,
        // and re-adopt the server identity on every successful (re)wire.
        connect(m_bootstrap, &SessionBootstrap::stateChanged, this,
                [this](SessionBootstrap::State state) {
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
                        setConnectionState(QStringLiteral("failed"),
                                           m_connectionError);
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
                });
        connect(m_bootstrap, &SessionBootstrap::wired, this,
                [this] { adoptServerIdentity(); });
        connect(m_bootstrap, &SessionBootstrap::error, this,
                [this](const QString& message) {
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
    emit connectionDiagnosticsChanged();
}

QString AppController::sshDiagnostics() const
{
    return m_pool ? m_pool->diagnosticLog() : QString();
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
    disconnectServer();
    m_bootstrap->requestRemoteUpgrade();
    startConnect(profileId, QString());
}

void AppController::startConnect(const QString& profileId,
                                 QString acceptedFingerprint)
{
    if (!m_bootstrap || !m_profiles || !m_pool)
        return;
    // One handshake at a time: a second invocation while a connect is running -
    // or while the user still owes us a host-key answer - would race two
    // sessions onto one pool and could re-enter the host-key callback. The flag
    // therefore stays set across the prompt and is cleared by resolveHostKey().
    if (m_connecting)
        return;

    const QVariantMap profile = m_profiles->profile(profileId);
    if (profile.isEmpty()) {
        m_credentials.clear();
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
        // The chain succeeded: its secrets have done their job and go now.
        m_credentials.clear();
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
    if (m_bootstrap) {
        m_tearingDown = true;
        const auto restore = qScopeGuard([this] { m_tearingDown = false; });
        m_bootstrap->disconnectSession();
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
        emit activeSessionChanged();
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
    emit error(message);
    // Refuse rather than limp on. Deferred by one event-loop turn because both
    // callers run INSIDE a CodeharbordClient response callback and the teardown
    // drops that very client's transport. The QPointer keeps the deferred work
    // from touching a controller destroyed in the meantime; QTimer's context
    // overload would already cancel it, and the explicit check documents why.
    QPointer<AppController> self(this);
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

bool AppController::dropActiveSessionIfGone()
{
    // Empty is the resting state, not a loss; and a session still in the tree
    // is simply fine. Both make this a no-op, so the transition below happens
    // exactly once and never thrashes on subsequent refreshes.
    if (m_activeSessionId.isEmpty() || sessionExists(m_activeSessionId))
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
    // forgotten, or from another client). Activating it would load a phantom:
    // both getLayout calls error, the toast fires on every launch, and the
    // terminal region attaches panes to a Dev Session the server has never
    // heard of. Forget it instead of reopening it.
    if (!sessionExists(remembered)) {
        m_uiState->setActiveSession(m_serverId.value, QString());
        return;
    }
    activateSession(remembered);
}

void AppController::activateSession(QString devSessionId)
{
    if (devSessionId.isEmpty())
        return;
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

QVector<GroupRow> AppController::computeRows()
{
    // Start from the pure persisted mapping (terminals empty), then overlay the
    // live agent state from the monitor (the source of truth). Deriving the
    // terminals fresh from m_lastNodes on every call is what makes a workspace
    // refresh and an agent event mutually non-destructive.
    QVector<GroupRow> rows = toGroupRows(m_lastNodes);
    if (m_agentMonitor) {
        // toGroupRows preserves the node order 1:1, so the row tree lines up
        // index-for-index with m_lastNodes; walk them in lockstep.
        for (qsizetype gi = 0; gi < m_lastNodes.size(); ++gi) {
            const GroupNode& groupNode = m_lastNodes.at(gi);
            GroupRow& groupRow = rows[gi];
            for (qsizetype si = 0; si < groupNode.sessions.size(); ++si) {
                const SessionNode& sessionNode = groupNode.sessions.at(si);
                SessionRow& sessionRow = groupRow.sessions[si];
                const QString& devSessionId = sessionNode.session.id.value;
                sessionRow.terminals.reserve(sessionNode.terminalPanes.size());
                for (const TerminalPane& pane : sessionNode.terminalPanes) {
                    TerminalStatus status;
                    status.id = pane.id;
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
                        && !m_agentMonitor->hasUnseen(devSessionId))
                        agent = AgentState::Idle;
                    status.agent = agent;
                    sessionRow.terminals.push_back(status);
                }
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
            QSet<QString> liveDevSessions;
            for (const GroupNode& groupNode : self->m_lastNodes)
                for (const SessionNode& sessionNode : groupNode.sessions)
                    liveDevSessions.insert(sessionNode.session.id.value);
            self->m_agentMonitor->retainDevSessions(liveDevSessions);
            for (const GroupNode& groupNode : self->m_lastNodes) {
                for (const SessionNode& sessionNode : groupNode.sessions) {
                    for (const TerminalPane& pane : sessionNode.terminalPanes) {
                        self->m_agentMonitor->setTerminalHarness(
                            sessionNode.session.id.value, pane.id.value, pane.harness);
                    }
                }
            }
        }
        const bool droppedActive = self->dropActiveSessionIfGone();
        self->rebuildRows();
        if (droppedActive || self->activeSessionRepoRoot() != repoRootBefore)
            emit self->activeSessionChanged();
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

} // namespace ch
