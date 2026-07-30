#include "WorkspaceDb.h"

#include "RpcTypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <utility>

namespace ch {

namespace {

// --- JSON -> ch:: struct mapping --------------------------------------------
// The remote returns camelCase result objects (workspace.ts toGroup/toSession/
// toViewerPane/toTerminalPane already rename server_id -> serverId etc.), so the
// wire keys are camelCase here. Bare id strings are lifted into their
// strongly-typed wrappers.
//
// INTENTIONAL NARROWING (nullable text): the schema's nullable-text columns —
// dev_sessions.default_working_directory / task_description, viewer_panes.handler
// / title, terminal_panes.working_directory / tmux_target / startup_command /
// harness — arrive as either a JSON string or JSON null. The ch:: model types
// them as plain QString (WorkspaceTypes.h, workstream M), NOT
// std::optional<QString>, so QJsonValue::toString() collapses BOTH SQL NULL and
// the empty string to an empty QString: the two are indistinguishable on the
// client, and the server's null-clear path (updateSession/update*Pane sending an
// explicit null) round-trips back to "" here rather than a distinct "unset".
// This is deliberate: representing null faithfully would require widening those
// QString members to std::optional across WorkspaceTypes.h and every consumer
// (ViewerModel, the sidebar model, equality/serialization), an invasive
// cross-workstream model change out of proportion to a display value that is
// empty either way. We keep the narrowing and document it rather than reach into
// the shared model. If a future feature must distinguish cleared-vs-empty, widen
// the model types first, then decode via QJsonValue::isNull() here.

Group parseGroup(const QJsonObject& obj)
{
    Group group;
    group.id = GroupId{obj.value(QStringLiteral("id")).toString()};
    group.serverId = ServerId{obj.value(QStringLiteral("serverId")).toString()};
    group.name = obj.value(QStringLiteral("name")).toString();
    group.position = obj.value(QStringLiteral("position")).toInt();
    group.collapsed = obj.value(QStringLiteral("collapsed")).toBool();
    return group;
}

DevSession parseSession(const QJsonObject& obj)
{
    DevSession session;
    session.id = DevSessionId{obj.value(QStringLiteral("id")).toString()};
    session.serverId = ServerId{obj.value(QStringLiteral("serverId")).toString()};
    session.groupId = GroupId{obj.value(QStringLiteral("groupId")).toString()};
    session.name = obj.value(QStringLiteral("name")).toString();
    session.repositoryRoot =
        obj.value(QStringLiteral("repositoryRoot")).toString();
    session.defaultWorkingDirectory =
        obj.value(QStringLiteral("defaultWorkingDirectory")).toString();
    session.taskDescription =
        obj.value(QStringLiteral("taskDescription")).toString();
    session.position = obj.value(QStringLiteral("position")).toInt();
    session.archived = obj.value(QStringLiteral("archived")).toBool();
    return session;
}

ViewerPane parseViewerPane(const QJsonObject& obj)
{
    ViewerPane pane;
    pane.id = ViewerPaneId{obj.value(QStringLiteral("id")).toString()};
    pane.serverId = ServerId{obj.value(QStringLiteral("serverId")).toString()};
    pane.devSessionId =
        DevSessionId{obj.value(QStringLiteral("devSessionId")).toString()};
    pane.url = obj.value(QStringLiteral("url")).toString();
    pane.handler = obj.value(QStringLiteral("handler")).toString();
    pane.title = obj.value(QStringLiteral("title")).toString();
    pane.position = obj.value(QStringLiteral("position")).toInt();
    return pane;
}

TerminalPane parseTerminalPane(const QJsonObject& obj)
{
    TerminalPane pane;
    pane.id = TerminalId{obj.value(QStringLiteral("id")).toString()};
    pane.serverId = ServerId{obj.value(QStringLiteral("serverId")).toString()};
    pane.devSessionId =
        DevSessionId{obj.value(QStringLiteral("devSessionId")).toString()};
    pane.name = obj.value(QStringLiteral("name")).toString();
    pane.workingDirectory =
        obj.value(QStringLiteral("workingDirectory")).toString();
    pane.tmuxTarget = obj.value(QStringLiteral("tmuxTarget")).toString();
    pane.startupCommand =
        obj.value(QStringLiteral("startupCommand")).toString();
    pane.harness = obj.value(QStringLiteral("harness")).toString();
    pane.position = obj.value(QStringLiteral("position")).toInt();
    return pane;
}

// A layout slot is either an inline split-tree object or null/absent; decode the
// former through SplitNode::fromJson and the latter to std::nullopt.
std::optional<SplitNode> parseLayoutTree(const QJsonValue& value)
{
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject obj = value.toObject();
    const SplitNode tree = SplitNode::fromJson(obj);
    // SplitNode::fromJson reports "this is not a valid split tree" by returning
    // its default empty-leaf sentinel - which is also exactly what a genuinely
    // stored empty leaf parses to (closing a region's last pane persists one;
    // see src/app/SessionLayouts.cpp). Telling the two apart needs the input: a
    // leaf ALWAYS parses, so a rejection can only come from an object whose
    // declared type is not "leaf" (an unknown or absent type, or a split with a
    // children/ratios count mismatch, a non-finite or non-positive ratio, or
    // nesting past the depth cap). Report a rejected tree as "no layout" rather
    // than as a perfectly valid single blank pane: SessionLayouts deliberately
    // leaves a region null when its layout cannot be loaded (SessionLayouts.h),
    // because a fabricated one-pane layout is something the user would edit
    // over, silently overwriting the layout the server still holds.
    const bool rejected = tree.isLeaf()
            && obj.value(QStringLiteral("type")).toString()
                    != QStringLiteral("leaf");
    if (rejected)
        return std::nullopt;
    return tree;
}

// Nested list elements are skipped unless they are JSON objects. QJsonValue::
// toObject() turns a non-object into an EMPTY object, which every parse helper
// above happily maps to a fully-default record: an entry with an empty id that
// looks real to the rest of the client and can collide with other id-keyed
// state. A malformed element is dropped instead, so a broken element costs its
// own entry and nothing more.
SessionNode parseSessionNode(const QJsonObject& obj)
{
    SessionNode node;
    node.session = parseSession(obj);

    const QJsonArray viewers = obj.value(QStringLiteral("viewerPanes")).toArray();
    node.viewerPanes.reserve(viewers.size());
    for (const QJsonValue& viewer : viewers) {
        if (viewer.isObject())
            node.viewerPanes.append(parseViewerPane(viewer.toObject()));
    }

    const QJsonArray terminals =
        obj.value(QStringLiteral("terminalPanes")).toArray();
    node.terminalPanes.reserve(terminals.size());
    for (const QJsonValue& terminal : terminals) {
        if (terminal.isObject())
            node.terminalPanes.append(parseTerminalPane(terminal.toObject()));
    }

    const QJsonObject layouts = obj.value(QStringLiteral("layouts")).toObject();
    node.viewerLayout = parseLayoutTree(layouts.value(QStringLiteral("viewer")));
    node.terminalLayout =
        parseLayoutTree(layouts.value(QStringLiteral("terminal")));
    return node;
}

GroupNode parseGroupNode(const QJsonObject& obj)
{
    GroupNode node;
    node.group = parseGroup(obj);
    const QJsonArray sessions = obj.value(QStringLiteral("sessions")).toArray();
    node.sessions.reserve(sessions.size());
    for (const QJsonValue& session : sessions) {
        if (session.isObject())
            node.sessions.append(parseSessionNode(session.toObject()));
    }
    return node;
}

QVector<GroupNode> parseGroupList(const QJsonValue& result)
{
    const QJsonArray array = result.toArray();
    QVector<GroupNode> groups;
    groups.reserve(array.size());
    for (const QJsonValue& group : array) {
        if (group.isObject())
            groups.append(parseGroupNode(group.toObject()));
    }
    return groups;
}

// workspace.getLayout returns a SessionLayout row (or null) and setLayout the
// upserted row; callers only need the split tree, held under the "tree" key.
std::optional<SplitNode> parseLayoutResult(const QJsonValue& result)
{
    if (!result.isObject())
        return std::nullopt;
    return parseLayoutTree(result.toObject().value(QStringLiteral("tree")));
}

// --- ch:: struct -> request-params JSON -------------------------------------
// Inverse of the parse helpers. Optional members are written only when set so an
// unset field is genuinely absent on the wire (the server then falls back to its
// default or the current value), never sent as an explicit null.

QString regionKey(Region region)
{
    return region == Region::Terminal ? QStringLiteral("terminal")
                                      : QStringLiteral("viewer");
}

template <typename IdType>
QJsonArray serializeIds(const QVector<IdType>& ids)
{
    QJsonArray array;
    for (const IdType& id : ids)
        array.append(id.value);
    return array;
}

QJsonObject serializeCreateGroup(const CreateGroupParams& params)
{
    QJsonObject obj{
        {QStringLiteral("serverId"), params.serverId.value},
        {QStringLiteral("name"), params.name},
    };
    if (params.position)
        obj[QStringLiteral("position")] = *params.position;
    if (params.collapsed)
        obj[QStringLiteral("collapsed")] = *params.collapsed;
    return obj;
}

QJsonObject serializeUpdateGroup(const UpdateGroupParams& params)
{
    QJsonObject obj{{QStringLiteral("id"), params.id.value}};
    if (params.name)
        obj[QStringLiteral("name")] = *params.name;
    if (params.position)
        obj[QStringLiteral("position")] = *params.position;
    if (params.collapsed)
        obj[QStringLiteral("collapsed")] = *params.collapsed;
    return obj;
}

QJsonObject serializeCreateSession(const CreateSessionParams& params)
{
    QJsonObject obj{
        {QStringLiteral("serverId"), params.serverId.value},
        {QStringLiteral("groupId"), params.groupId.value},
        {QStringLiteral("name"), params.name},
        {QStringLiteral("repositoryRoot"), params.repositoryRoot},
    };
    if (params.defaultWorkingDirectory)
        obj[QStringLiteral("defaultWorkingDirectory")] =
            *params.defaultWorkingDirectory;
    if (params.taskDescription)
        obj[QStringLiteral("taskDescription")] = *params.taskDescription;
    if (params.position)
        obj[QStringLiteral("position")] = *params.position;
    if (params.archived)
        obj[QStringLiteral("archived")] = *params.archived;
    return obj;
}

QJsonObject serializeUpdateSession(const UpdateSessionParams& params)
{
    QJsonObject obj{{QStringLiteral("id"), params.id.value}};
    if (params.name)
        obj[QStringLiteral("name")] = *params.name;
    if (params.repositoryRoot)
        obj[QStringLiteral("repositoryRoot")] = *params.repositoryRoot;
    if (params.defaultWorkingDirectory)
        obj[QStringLiteral("defaultWorkingDirectory")] =
            *params.defaultWorkingDirectory;
    if (params.taskDescription)
        obj[QStringLiteral("taskDescription")] = *params.taskDescription;
    if (params.position)
        obj[QStringLiteral("position")] = *params.position;
    if (params.archived)
        obj[QStringLiteral("archived")] = *params.archived;
    return obj;
}

QJsonObject serializeMoveSession(const MoveSessionParams& params)
{
    QJsonObject obj{
        {QStringLiteral("id"), params.id.value},
        {QStringLiteral("groupId"), params.groupId.value},
    };
    if (params.position)
        obj[QStringLiteral("position")] = *params.position;
    return obj;
}

QJsonObject serializeCreateViewerPane(const CreateViewerPaneParams& params)
{
    QJsonObject obj{
        {QStringLiteral("serverId"), params.serverId.value},
        {QStringLiteral("devSessionId"), params.devSessionId.value},
        {QStringLiteral("url"), params.url},
    };
    if (params.handler)
        obj[QStringLiteral("handler")] = *params.handler;
    if (params.title)
        obj[QStringLiteral("title")] = *params.title;
    if (params.position)
        obj[QStringLiteral("position")] = *params.position;
    return obj;
}

QJsonObject serializeUpdateViewerPane(const UpdateViewerPaneParams& params)
{
    QJsonObject obj{{QStringLiteral("id"), params.id.value}};
    if (params.url)
        obj[QStringLiteral("url")] = *params.url;
    if (params.handler)
        obj[QStringLiteral("handler")] = *params.handler;
    if (params.title)
        obj[QStringLiteral("title")] = *params.title;
    if (params.position)
        obj[QStringLiteral("position")] = *params.position;
    return obj;
}

QJsonObject serializeCreateTerminalPane(const CreateTerminalPaneParams& params)
{
    QJsonObject obj{
        {QStringLiteral("serverId"), params.serverId.value},
        {QStringLiteral("devSessionId"), params.devSessionId.value},
        {QStringLiteral("name"), params.name},
    };
    if (params.workingDirectory)
        obj[QStringLiteral("workingDirectory")] = *params.workingDirectory;
    if (params.tmuxTarget)
        obj[QStringLiteral("tmuxTarget")] = *params.tmuxTarget;
    if (params.startupCommand)
        obj[QStringLiteral("startupCommand")] = *params.startupCommand;
    if (params.harness)
        obj[QStringLiteral("harness")] = *params.harness;
    if (params.position)
        obj[QStringLiteral("position")] = *params.position;
    return obj;
}

QJsonObject serializeUpdateTerminalPane(const UpdateTerminalPaneParams& params)
{
    QJsonObject obj{{QStringLiteral("id"), params.id.value}};
    if (params.name)
        obj[QStringLiteral("name")] = *params.name;
    if (params.workingDirectory)
        obj[QStringLiteral("workingDirectory")] = *params.workingDirectory;
    if (params.tmuxTarget)
        obj[QStringLiteral("tmuxTarget")] = *params.tmuxTarget;
    if (params.startupCommand)
        obj[QStringLiteral("startupCommand")] = *params.startupCommand;
    if (params.harness)
        obj[QStringLiteral("harness")] = *params.harness;
    if (params.position)
        obj[QStringLiteral("position")] = *params.position;
    return obj;
}

} // namespace

WorkspaceDb::WorkspaceDb(CodeharbordClient* client) : m_client(client) {}

void WorkspaceDb::list(const ServerId& serverId, ListCallback cb)
{
    const QJsonObject params{{QStringLiteral("serverId"), serverId.value}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceList), params,
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb({}, error);
                return;
            }
            cb(parseGroupList(result), std::nullopt);
        });
}

void WorkspaceDb::createGroup(const CreateGroupParams& params, GroupCallback cb)
{
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceCreateGroup),
        serializeCreateGroup(params),
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseGroup(result.toObject()), std::nullopt);
        });
}

void WorkspaceDb::updateGroup(const UpdateGroupParams& params, GroupCallback cb)
{
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceUpdateGroup),
        serializeUpdateGroup(params),
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseGroup(result.toObject()), std::nullopt);
        });
}

void WorkspaceDb::deleteGroup(const GroupId& id, OkCallback cb)
{
    const QJsonObject params{{QStringLiteral("id"), id.value}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceDeleteGroup), params,
        [cb = std::move(cb)](QJsonValue, std::optional<RpcError> error) {
            cb(error);
        });
}

void WorkspaceDb::reorderGroups(const ServerId& serverId,
                                const QVector<GroupId>& orderedIds,
                                OkCallback cb)
{
    const QJsonObject params{
        {QStringLiteral("serverId"), serverId.value},
        {QStringLiteral("orderedIds"), serializeIds(orderedIds)},
    };
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceReorderGroups), params,
        [cb = std::move(cb)](QJsonValue, std::optional<RpcError> error) {
            cb(error);
        });
}

void WorkspaceDb::createSession(const CreateSessionParams& params,
                                SessionCallback cb)
{
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceCreateSession),
        serializeCreateSession(params),
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseSession(result.toObject()), std::nullopt);
        });
}

void WorkspaceDb::updateSession(const UpdateSessionParams& params,
                                SessionCallback cb)
{
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceUpdateSession),
        serializeUpdateSession(params),
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseSession(result.toObject()), std::nullopt);
        });
}

void WorkspaceDb::deleteSession(const DevSessionId& id, OkCallback cb)
{
    const QJsonObject params{{QStringLiteral("id"), id.value}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceDeleteSession), params,
        [cb = std::move(cb)](QJsonValue, std::optional<RpcError> error) {
            cb(error);
        });
}

void WorkspaceDb::reorderSessions(const GroupId& groupId,
                                  const QVector<DevSessionId>& orderedIds,
                                  OkCallback cb)
{
    const QJsonObject params{
        {QStringLiteral("groupId"), groupId.value},
        {QStringLiteral("orderedIds"), serializeIds(orderedIds)},
    };
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceReorderSessions), params,
        [cb = std::move(cb)](QJsonValue, std::optional<RpcError> error) {
            cb(error);
        });
}

void WorkspaceDb::moveSessionToGroup(const MoveSessionParams& params,
                                     SessionCallback cb)
{
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceMoveSessionToGroup),
        serializeMoveSession(params),
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseSession(result.toObject()), std::nullopt);
        });
}

void WorkspaceDb::duplicateSession(const DevSessionId& id,
                                   SessionNodeCallback cb)
{
    const QJsonObject params{{QStringLiteral("id"), id.value}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceDuplicateSession), params,
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseSessionNode(result.toObject()), std::nullopt);
        });
}

void WorkspaceDb::createViewerPane(const CreateViewerPaneParams& params,
                                   ViewerPaneCallback cb)
{
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceCreateViewerPane),
        serializeCreateViewerPane(params),
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseViewerPane(result.toObject()), std::nullopt);
        });
}

void WorkspaceDb::updateViewerPane(const UpdateViewerPaneParams& params,
                                   ViewerPaneCallback cb)
{
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceUpdateViewerPane),
        serializeUpdateViewerPane(params),
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseViewerPane(result.toObject()), std::nullopt);
        });
}

void WorkspaceDb::deleteViewerPane(const ViewerPaneId& id, OkCallback cb)
{
    const QJsonObject params{{QStringLiteral("id"), id.value}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceDeleteViewerPane), params,
        [cb = std::move(cb)](QJsonValue, std::optional<RpcError> error) {
            cb(error);
        });
}

void WorkspaceDb::createTerminalPane(const CreateTerminalPaneParams& params,
                                     TerminalPaneCallback cb)
{
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceCreateTerminalPane),
        serializeCreateTerminalPane(params),
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseTerminalPane(result.toObject()), std::nullopt);
        });
}

void WorkspaceDb::updateTerminalPane(const UpdateTerminalPaneParams& params,
                                     TerminalPaneCallback cb)
{
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceUpdateTerminalPane),
        serializeUpdateTerminalPane(params),
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseTerminalPane(result.toObject()), std::nullopt);
        });
}

void WorkspaceDb::deleteTerminalPane(const TerminalId& id, OkCallback cb)
{
    const QJsonObject params{{QStringLiteral("id"), id.value}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceDeleteTerminalPane), params,
        [cb = std::move(cb)](QJsonValue, std::optional<RpcError> error) {
            cb(error);
        });
}

void WorkspaceDb::getLayout(const DevSessionId& devSessionId, Region region,
                            LayoutCallback cb)
{
    const QJsonObject params{
        {QStringLiteral("devSessionId"), devSessionId.value},
        {QStringLiteral("region"), regionKey(region)},
    };
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceGetLayout), params,
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseLayoutResult(result), std::nullopt);
        });
}

void WorkspaceDb::setLayout(const ServerId& serverId,
                            const DevSessionId& devSessionId, Region region,
                            const SplitNode& tree, LayoutCallback cb)
{
    const QJsonObject params{
        {QStringLiteral("serverId"), serverId.value},
        {QStringLiteral("devSessionId"), devSessionId.value},
        {QStringLiteral("region"), regionKey(region)},
        {QStringLiteral("tree"), tree.toJson()},
    };
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceSetLayout), params,
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, error);
                return;
            }
            cb(parseLayoutResult(result), std::nullopt);
        });
}

} // namespace ch
