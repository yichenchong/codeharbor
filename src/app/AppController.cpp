#include "AppController.h"

#include "UiStateStore.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QDir>
#include <QJsonObject>
#include <QPointer>

namespace ch {

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
    m_pool = pool;
    m_bootstrap = bootstrap;
    m_profiles = profiles;
    m_layouts = layouts;

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
    if (!m_bootstrap || !m_profiles || !m_pool)
        return;
    // One handshake at a time: a second invocation while a connect is running -
    // or while the user still owes us a host-key answer - would race two
    // sessions onto one pool and could re-enter the host-key callback.
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
    setConnectionState(QStringLiteral("connecting"));

    // Unknown key: refuse THIS attempt, remember the fingerprint, and let the
    // user decide. Accepting re-runs connectToProfile with the fingerprint
    // pre-approved, so the decision never blocks inside the handshake.
    QPointer<AppController> self(this);
    m_pool->setHostKeyCallback(
        [self](const QString& host, const QString& keyType,
               const QByteArray& keyBlob, KnownHosts::Verdict) {
            if (!self)
                return SshConnectionPool::HostKeyDecision::Reject;
            const QString fingerprint =
                QString::fromLatin1(
                    QCryptographicHash::hash(keyBlob, QCryptographicHash::Sha256)
                        .toBase64(QByteArray::OmitTrailingEquals));
            if (self->m_acceptedFingerprint == fingerprint) {
                self->m_acceptedFingerprint.clear();
                return SshConnectionPool::HostKeyDecision::Accept;
            }
            self->m_pendingFingerprint = fingerprint;
            self->m_pendingHostKeyInfo = qMakePair(host, keyType);
            return SshConnectionPool::HostKeyDecision::Reject;
        });

    const bool ok = m_bootstrap->connectAndWire(
        profile.value(QStringLiteral("host")).toString(),
        static_cast<quint16>(profile.value(QStringLiteral("port")).toInt()),
        profile.value(QStringLiteral("user")).toString(),
        profile.value(QStringLiteral("nodePath")).toString(),
        profile.value(QStringLiteral("repoRoot")).toString());

    m_connecting = false;
    if (ok) {
        m_profiles->setActiveId(profileId);
        return;  // wired() -> adoptServerIdentity()
    }
    if (!m_pendingFingerprint.isEmpty()) {
        setConnectionState(QStringLiteral("hostkey"));
        emit hostKeyPrompt(m_pendingHostKeyInfo.first, m_pendingHostKeyInfo.second,
                           m_pendingFingerprint);
        return;
    }
    setConnectionState(QStringLiteral("failed"), m_connectionError);
}

void AppController::resolveHostKey(bool accept)
{
    const QString fingerprint = m_pendingFingerprint;
    const QString profileId = m_pendingProfileId;
    m_pendingFingerprint.clear();
    if (!accept || fingerprint.isEmpty() || profileId.isEmpty()) {
        setConnectionState(QStringLiteral("disconnected"));
        return;
    }
    m_acceptedFingerprint = fingerprint;
    connectToProfile(profileId);
}

void AppController::disconnectServer()
{
    if (!m_bootstrap)
        return;
    m_bootstrap->disconnectSession();
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
                       const QString id = result.toObject()
                                              .value(QStringLiteral("serverId"))
                                              .toString();
                       if (id.isEmpty()) {
                           emit self->error(
                               tr("Server did not report an identity; refusing to "
                                  "key this workspace to a client-side id."));
                           return;
                       }
                       if (self->m_layouts)
                           self->m_layouts->setServerId(id);
                       // setServerId() refreshes the sidebar; restore the last
                       // session once those rows arrive.
                       self->setServerId(id);
                   });
}

void AppController::restoreActiveSession()
{
    if (m_activeSessionId.isEmpty() && m_uiState) {
        const QString remembered = m_uiState->activeSession();
        if (!remembered.isEmpty())
            activateSession(remembered);
    }
}

void AppController::activateSession(QString devSessionId)
{
    if (devSessionId.isEmpty())
        return;
    m_activeSessionId = devSessionId;
    if (m_uiState)
        m_uiState->setActiveSession(devSessionId);
    if (m_layouts) {
        m_layouts->setServerId(m_serverId.value);
        m_layouts->load(devSessionId);
    }
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
        self->m_lastNodes = std::move(nodes);
        self->rebuildRows();
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
