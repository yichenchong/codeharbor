#include "AppController.h"

#include "UiStateStore.h"

#include <QCryptographicHash>
#include <QScopeGuard>
#include <QFileInfo>
#include <QDir>
#include <QJsonObject>
#include <QPointer>
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

} // namespace

AppController::AppController(CodeharbordClient* client, QObject* parent)
    : QObject(parent)
    , m_client(client)
    , m_db(std::make_unique<WorkspaceDb>(client))
    , m_sessionsModel(new SessionsModel(this))
    , m_uiState(new UiStateStore(QString(), this))
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
        m_activeSessionId.clear();
        if (m_layouts)
            m_layouts->load(QString());  // clears both region trees
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
        // the last known workspace tree. The QPointer-free lambda is safe: the
        // connection is bound to `this` as the context object, so Qt severs it
        // automatically when this controller is destroyed (no UAF), and the
        // disconnect above severs it on re-set.
        connect(m_agentMonitor, &AgentStatusMonitor::agentStateChanged, this,
                [this](const QString&, const QString&, int) { rebuildRows(); });
        connect(m_agentMonitor, &AgentStatusMonitor::unseenChanged, this,
                [this](const QString&, bool) { rebuildRows(); });
    }
    // Re-merge immediately so a monitor set after the initial load reflects any
    // state it already accumulated, and a clear drops back to bare rows.
    rebuildRows();
}

void AppController::setConnection(SshConnectionPool* pool,
                                  SessionBootstrap* bootstrap,
                                  ServerProfiles* profiles,
                                  SessionLayouts* layouts)
{
    // Re-injection must not stack duplicate connections onto a second
    // bootstrap's signals (and must not leave the first one still driving us).
    if (m_bootstrap && m_bootstrap != bootstrap)
        disconnect(m_bootstrap, nullptr, this, nullptr);

    m_pool = pool;
    m_bootstrap = bootstrap;
    m_profiles = profiles;
    m_layouts = layouts;

    // setConnection may land after a serverId is already known (test order, or
    // a re-injection); seed the layouts key so it is never one server behind.
    if (m_layouts)
        m_layouts->setServerId(m_serverId.value);

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
                    case SessionBootstrap::State::Disconnected:
                        setConnectionState(QStringLiteral("disconnected"));
                        break;
                    }
                });
        connect(m_bootstrap, &SessionBootstrap::wired, this,
                [this] { adoptServerIdentity(); });
        connect(m_bootstrap, &SessionBootstrap::error, this,
                [this](const QString& message) {
                    m_connectionError = message;
                    // While a connect attempt is in flight the failure may be
                    // EXPECTED: an unknown host key is deliberately refused so
                    // the user can be asked. Surfacing "SSH connection failed"
                    // for that would tell the user something went wrong when the
                    // app is simply waiting on their answer. Hold it until the
                    // attempt resolves, then either drop it (prompt raised) or
                    // report it (genuine failure).
                    if (m_connecting)
                        m_heldConnectError = message;
                    else
                        emit error(message);
                    emit connectionStateChanged();
                });
    }
    emit connectionChanged();
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
    startConnect(profileId, QString(), QString(),
                 CredentialKind::KeyPassphrase);
}

void AppController::startConnect(const QString& profileId,
                                 QString acceptedFingerprint, QString secret,
                                 CredentialKind secretKind)
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
    // remote host's password-auth endpoint. `CredentialReply::promptRequested`
    // lets authenticate() stop at the first outstanding prompt instead of
    // overwriting it with the next auth method.
    //
    // The secret is captured BY VALUE and consumed exactly once. It is never a
    // member, never reaches ServerProfiles/QSettings, and is never logged.
    m_pool->setCredentialCallback(
        [self, secret = std::move(secret), secretKind](
            const QString& user, CredentialKind kind) mutable {
            if (!self)
                return SshConnectionPool::CredentialReply{};
            if (!secret.isEmpty() && kind == secretKind) {
                const QString once = secret;
                secret.clear();
                return SshConnectionPool::CredentialReply{once, false};
            }
            // A supplied credential for the other auth method must not cause a
            // second prompt. This is how an explicit "Use password" choice
            // skips the passphrase rung.
            if (!secret.isEmpty() || !self->m_connecting)
                return SshConnectionPool::CredentialReply{};

            self->m_credentialRequested = true;
            self->m_credentialUser = user;
            self->m_credentialLabel = credentialLabel(kind);
            self->m_credentialKind = kind;
            return SshConnectionPool::CredentialReply{{}, true};
        });

    const bool ok = m_bootstrap->connectAndWire(
        profile.value(QStringLiteral("host")).toString(),
        static_cast<quint16>(profile.value(QStringLiteral("port")).toInt()),
        profile.value(QStringLiteral("user")).toString(),
        profile.value(QStringLiteral("nodePath")).toString(),
        profile.value(QStringLiteral("repoRoot")).toString(),
        profile.value(QStringLiteral("identityFile")).toString());

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
        emit hostKeyPrompt(m_pendingHostKeyInfo.first, m_pendingHostKeyInfo.second,
                           m_pendingFingerprint);
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
    // A genuine failure: report it now that we know it was not the host-key path.
    if (!m_heldConnectError.isEmpty()) {
        const QString held = m_heldConnectError;
        m_heldConnectError.clear();
        emit error(held);
    }
    setConnectionState(QStringLiteral("failed"), m_connectionError);
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
        setConnectionState(QStringLiteral("disconnected"));
        return;
    }
    startConnect(profileId, fingerprint, QString(),
                 CredentialKind::KeyPassphrase);
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

    const CredentialKind submittedKind =
        kind == QLatin1String("password") ? CredentialKind::Password
                                          : CredentialKind::KeyPassphrase;
    if (kind != QLatin1String("password")
        && kind != QLatin1String("keyPassphrase")) {
        return;
    }

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
        // Cancelled. Nothing is retried and nothing is kept.
        setConnectionState(QStringLiteral("disconnected"));
        return;
    }
    // Moved, not copied: this frame keeps no second reference to the secret,
    // and startConnect() moves it straight into the pool callback that spends
    // it. A wrong credential fails one attempt; a failed passphrase may then
    // ask separately for a password, but is never used as one.
    startConnect(profileId, fingerprint, std::move(secret), submittedKind);
}

void AppController::disconnectServer()
{
    if (!m_bootstrap)
        return;
    // A disconnect while a host-key prompt is parked also abandons that
    // attempt; leaving m_connecting set would wedge every later connect.
    m_connecting = false;
    m_pendingFingerprint.clear();
    m_pendingHostKeyInfo = {};
    m_approvedFingerprint.clear();
    m_credentialRequested = false;
    m_credentialUser.clear();
    m_credentialLabel.clear();

    // Scope-guarded so an early return or a throw inside the teardown cannot
    // leave errors permanently muted. The guard is the ONLY thing bounding the
    // mute window, and that is sound because the failures are synchronous:
    // disconnectSession() -> unwire() -> CodeharbordClient::failAllPending()
    // runs every pending callback inline before returning, so they all land
    // inside this scope. A client that ever deferred them would need this flag
    // cleared by the arrival of the LAST failure instead.
    m_tearingDown = true;
    const auto restore = qScopeGuard([this] { m_tearingDown = false; });
    m_bootstrap->disconnectSession();

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
                       if (err) {
                           emit self->error(err->message);
                           return;
                       }
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
                                  "schema %3 or newer. Update the CodeHarbor "
                                  "remote on that host and reconnect.")
                                   .arg(remoteVersion.isEmpty()
                                            ? tr("(version not reported)")
                                            : remoteVersion)
                                   .arg(schema)
                                   .arg(kMinimumServerSchemaVersion);
                           emit self->error(message);
                           // Refuse rather than limp on. Deferred by one event-
                           // loop turn because we are INSIDE a
                           // CodeharbordClient response callback and the
                           // teardown drops that very client's transport.
                           QTimer::singleShot(0, self, [self, message] {
                               if (!self)
                                   return;
                               self->disconnectServer();
                               self->setConnectionState(
                                   QStringLiteral("failed"), message);
                           });
                           return;
                       }
                       const QString id =
                           info.value(QStringLiteral("serverId")).toString();
                       if (id.isEmpty()) {
                           emit self->error(
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

void AppController::rebuildRows()
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
    m_sessionsModel->setGroups(std::move(rows));
}

void AppController::refresh()
{
    // Nothing to ask when no transport is bound: the call would fail instantly
    // with "no transport bound" and surface as a user-facing error toast, which
    // is exactly what a user sees on a cold start before they have connected.
    // This is a capability check, NOT a filter on the error text - a transport
    // that exists and then dies mid-session still reports its failure verbatim.
    if (m_client && !m_client->transport())
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
