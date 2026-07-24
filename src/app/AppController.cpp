#include "AppController.h"

#include "UiStateStore.h"

#include <QFileInfo>
#include <QDir>
#include <QPointer>

namespace ch {

AppController::AppController(CodeharbordClient* client, QObject* parent)
    : QObject(parent)
    , m_client(client)
    , m_db(std::make_unique<WorkspaceDb>(client))
    , m_sessionsModel(new SessionsModel(this))
    , m_uiState(new UiStateStore(QString(), this))
{
}

AppController::~AppController() = default;

void AppController::setServerId(const QString& serverId)
{
    if (m_serverId.value == serverId)
        return;
    m_serverId.value = serverId;
    emit serverIdChanged();
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

void AppController::refresh()
{
    QPointer<AppController> self(this);
    m_db->list(m_serverId, [self](QVector<GroupNode> nodes,
                                  std::optional<RpcError> err) {
        if (!self)
            return;
        if (self->reportIfError(err))
            return;
        self->m_sessionsModel->setGroups(toGroupRows(nodes));
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
