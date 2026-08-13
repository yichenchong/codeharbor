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

bool hasNonEmptyString(const QJsonObject &obj, const QString &key)
{
    const QJsonValue value = obj.value(key);
    return value.isString() && !value.toString().isEmpty();
}

std::optional<Group> parseGroup(const QJsonObject& obj)
{
    if (!hasNonEmptyString(obj, QStringLiteral("id")))
        return std::nullopt;
    Group group;
    group.id = GroupId{obj.value(QStringLiteral("id")).toString()};
    group.serverId = ServerId{obj.value(QStringLiteral("serverId")).toString()};
    group.name = obj.value(QStringLiteral("name")).toString();
    group.position = obj.value(QStringLiteral("position")).toInt();
    group.collapsed = obj.value(QStringLiteral("collapsed")).toBool();
    return group;
}

std::optional<DevSession> parseSession(const QJsonObject& obj)
{
    if (!hasNonEmptyString(obj, QStringLiteral("id")))
        return std::nullopt;
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
    session.pinned = obj.value(QStringLiteral("pinned")).toBool();
    return session;
}

std::optional<ViewerPane> parseViewerPane(const QJsonObject& obj)
{
    if (!hasNonEmptyString(obj, QStringLiteral("id")))
        return std::nullopt;
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

std::optional<TerminalPane> parseTerminalPane(const QJsonObject& obj)
{
    if (!hasNonEmptyString(obj, QStringLiteral("id")))
        return std::nullopt;
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

// The `tmuxTargets` of a delete result (remote/src/rpc-types.ts
// DeleteWithTmuxTargetsResult): the remote tmux sessions the server reports
// that delete really destroyed, in the transaction that destroyed them.
//
// ONE shape is accepted without the field: `{ "ok": true }` from a codeharbord
// older than it, which decodes to an empty list — "nothing to kill". That is
// the safe direction, because the only alternative is for the client to guess
// target names and the server is the sole minting site (SPEC 5.2).
//
// Everything else is a MALFORMED result, not an empty one. A bare `true`, a
// missing or false `ok`, or a `tmuxTargets` that is not an array of strings all
// used to decode to "success, nothing to kill", so a response the client did not
// understand looked exactly like a clean delete of a pane-less session: the
// sidebar refreshed as though rows had gone, and any session behind that
// response was stranded with nobody told. Returning nullopt makes the caller
// report it instead.
std::optional<QStringList> parseTmuxTargets(const QJsonValue& result)
{
    if (!result.isObject())
        return std::nullopt;
    const QJsonObject obj = result.toObject();
    if (!obj.value(QStringLiteral("ok")).isBool()
        || !obj.value(QStringLiteral("ok")).toBool()) {
        return std::nullopt;
    }
    const QJsonValue reported = obj.value(QStringLiteral("tmuxTargets"));
    if (reported.isUndefined() || reported.isNull())
        return QStringList();  // an older server: it destroyed rows, not sessions
    if (!reported.isArray())
        return std::nullopt;
    const QJsonArray array = reported.toArray();
    QStringList targets;
    targets.reserve(array.size());
    for (const QJsonValue& value : array) {
        if (!value.isString())
            return std::nullopt;
        const QString target = value.toString();
        // The server already omits panes with no target; a blank that reaches
        // here names nothing and must not become a kill request.
        if (!target.isEmpty())
            targets.append(target);
    }
    return targets;
}

// A layout slot is either an inline split-tree object or null/absent; decode the
// former through SplitNode::tryFromJson and the latter to std::nullopt.
//
// tryFromJson rather than fromJson: fromJson reports "not a valid split tree" by
// returning its default empty-leaf value, which is also exactly what a genuinely
// stored empty leaf parses to (closing a region's last pane persists one; see
// src/app/SessionLayouts.cpp). A rejected tree must read as "no layout", not as
// a perfectly valid single blank pane: SessionLayouts deliberately leaves a
// region null when its layout cannot be loaded (SessionLayouts.h), because a
// fabricated one-pane layout is something the user would edit over, silently
// overwriting the layout the server still holds.
std::optional<SplitNode> parseLayoutTree(const QJsonValue& value)
{
    if (!value.isObject())
        return std::nullopt;
    return SplitNode::tryFromJson(value.toObject());
}

bool readOptionalArray(const QJsonObject &obj, const QString &key, QJsonArray &out)
{
    if (!obj.contains(key))
        return true;
    const QJsonValue value = obj.value(key);
    if (!value.isArray())
        return false;
    out = value.toArray();
    return true;
}

// Nested list elements are skipped unless they are valid JSON objects with a
// non-empty identity. QJsonValue::toObject() turns a non-object into an EMPTY
// object, and an object missing its id would likewise map to a fully-default
// record that looks real to the rest of the client. A malformed element is
// dropped instead, so a broken element costs its own entry and nothing more.
std::optional<SessionNode> parseSessionNode(const QJsonObject& obj)
{
    const std::optional<DevSession> session = parseSession(obj);
    if (!session)
        return std::nullopt;

    SessionNode node;
    node.session = *session;

    QJsonArray viewers;
    if (!readOptionalArray(obj, QStringLiteral("viewerPanes"), viewers))
        return std::nullopt;
    node.viewerPanes.reserve(viewers.size());
    for (const QJsonValue& viewer : viewers) {
        if (!viewer.isObject())
            continue;
        const std::optional<ViewerPane> pane = parseViewerPane(viewer.toObject());
        if (pane)
            node.viewerPanes.append(*pane);
    }

    QJsonArray terminals;
    if (!readOptionalArray(obj, QStringLiteral("terminalPanes"), terminals))
        return std::nullopt;
    node.terminalPanes.reserve(terminals.size());
    for (const QJsonValue& terminal : terminals) {
        if (!terminal.isObject())
            continue;
        const std::optional<TerminalPane> pane =
            parseTerminalPane(terminal.toObject());
        if (pane)
            node.terminalPanes.append(*pane);
    }

    QJsonObject layouts;
    if (obj.contains(QStringLiteral("layouts"))) {
        const QJsonValue value = obj.value(QStringLiteral("layouts"));
        if (!value.isObject())
            return std::nullopt;
        layouts = value.toObject();
    }
    node.viewerLayout = parseLayoutTree(layouts.value(QStringLiteral("viewer")));
    node.terminalLayout =
        parseLayoutTree(layouts.value(QStringLiteral("terminal")));
    return node;
}

std::optional<GroupNode> parseGroupNode(const QJsonObject& obj)
{
    const std::optional<Group> group = parseGroup(obj);
    if (!group)
        return std::nullopt;

    GroupNode node;
    node.group = *group;
    QJsonArray sessions;
    if (!readOptionalArray(obj, QStringLiteral("sessions"), sessions))
        return std::nullopt;
    node.sessions.reserve(sessions.size());
    for (const QJsonValue& session : sessions) {
        if (!session.isObject())
            continue;
        const std::optional<SessionNode> parsed =
            parseSessionNode(session.toObject());
        if (parsed)
            node.sessions.append(*parsed);
    }
    return node;
}

// Takes the already-kind-checked array rather than a QJsonValue: toArray() on
// anything else yields an EMPTY array, which would report a wrong-kind result
// as an empty-but-successful sidebar. The one caller checks isArray() first,
// and this signature is what keeps a future one from forgetting.
QVector<GroupNode> parseGroupList(const QJsonArray& array)
{
    QVector<GroupNode> groups;
    groups.reserve(array.size());
    for (const QJsonValue& group : array) {
        if (!group.isObject())
            continue;
        const std::optional<GroupNode> parsed = parseGroupNode(group.toObject());
        if (parsed)
            groups.append(*parsed);
    }
    return groups;
}

// workspace.getLayout returns a SessionLayout row (or null) and setLayout the
// upserted row; callers only need the split tree, held under the "tree" key.
// The null case belongs to getLayout alone and is handled at the call site, so
// this takes an already-validated row object.
std::optional<SplitNode> parseLayoutRow(const QJsonObject& row)
{
    return parseLayoutTree(row.value(QStringLiteral("tree")));
}

// --- ch:: struct -> request-params JSON -------------------------------------
// Inverse of the parse helpers. Optional members are written only when set so an
// unset field is genuinely absent on the wire (the server then falls back to its
// default or the current value), never sent as an explicit null.

// A switch rather than a two-way test on Region::Terminal: an enumerator added
// later would silently be sent to the server as "viewer", writing one region's
// layout over another's. The switch makes the compiler point at this function
// instead (-Wswitch), and the trailing return only satisfies the "control
// reaches end of non-void function" rule for an out-of-range cast value.
QString regionKey(Region region)
{
    switch (region) {
    case Region::Viewer:
        return QStringLiteral("viewer");
    case Region::Terminal:
        return QStringLiteral("terminal");
    }
    return QStringLiteral("viewer");
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
    if (params.pinned)
        obj[QStringLiteral("pinned")] = *params.pinned;
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
    if (params.pinned)
        obj[QStringLiteral("pinned")] = *params.pinned;
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

// --- successful-result validation -------------------------------------------
// A response with no `error` still says nothing about the SHAPE of its result,
// and QJsonValue's accessors are lossy about that: toObject() turns null, a
// number, a string or an array into an EMPTY object, and toArray() does the same
// for anything that is not an array. Fed to the parse helpers above, an empty
// object yields a fully-DEFAULT record whose id is the empty string — exactly
// the entry parseSessionNode()/parseGroupList() go out of their way to drop for
// nested elements, because the rest of the client cannot tell it from a real
// group/session/pane and will key maps and sidebar rows by that empty id.
// Top-level results get the same treatment: a result of the wrong JSON kind is
// reported as a failure instead of being decoded into a plausible-looking blank.
//
// -32603 is JSON-RPC 2.0's reserved "internal error", the same code
// CodeharbordClient uses for the failures it synthesizes itself
// (src/remote/CodeharbordClient.cpp), so callers need no new code path.
constexpr int kMalformedResultCode = -32603;

// `method` and `expected` are string literals with static storage; they are only
// ever formatted into the message.
RpcError malformedResult(const char* method, const char* expected)
{
    RpcError error;
    error.code = kMalformedResultCode;
    error.message = QStringLiteral("malformed %1 result: expected %2")
                        .arg(QString::fromLatin1(method),
                             QString::fromLatin1(expected));
    return error;
}

// -32602 is JSON-RPC 2.0's reserved "invalid params", and is what the server
// answers for a request it refuses on shape. A request this class refuses
// BEFORE sending it fails with the same code and the same method name, so a
// caller sees one kind of error whether the refusal happened here or there.
constexpr int kInvalidParamsCode = -32602;

// `method` and `problem` are string literals with static storage; they are only
// ever formatted into the message.
RpcError invalidParams(const char* method, const char* problem)
{
    RpcError error;
    error.code = kInvalidParamsCode;
    error.message = QStringLiteral("invalid %1 params: %2")
                        .arg(QString::fromLatin1(method),
                             QString::fromLatin1(problem));
    return error;
}

// Deliver a locally-detected failure on the SAME terms as a server one: later,
// from the event loop, never inside the method the caller is still in. Every
// method here is documented async and its callers are written for that — one
// that took its callback synchronously would re-enter them halfway through
// their own call to this class, a shape no other method has. The delivery is
// posted to the borrowed client, which is the object the real reply would have
// come from and outlives this repository by contract.
template <typename Callback>
void failAsync(QObject* context, Callback cb, RpcError error)
{
    QMetaObject::invokeMethod(
        context,
        [cb = std::move(cb), error = std::move(error)]() mutable {
            cb(std::nullopt, std::move(error));
        },
        Qt::QueuedConnection);
}

// Response handler for every workspace.* method whose result is a single record
// object: forwards a server error verbatim, rejects a non-object result and an
// object without the record's required non-empty id, and otherwise hands the
// decoded record to `cb`. `decode` maps the result object to an optional typed
// payload so malformed objects cannot become blank records.
template <typename Callback, typename Decode>
auto recordHandler(const char* method, Callback cb, Decode decode)
{
    return [method, cb = std::move(cb), decode](
               QJsonValue result, std::optional<RpcError> error) {
        if (error) {
            cb(std::nullopt, std::move(error));
            return;
        }
        if (!result.isObject()) {
            cb(std::nullopt, malformedResult(method, "a JSON object"));
            return;
        }
        auto decoded = decode(result.toObject());
        if (!decoded) {
            cb(std::nullopt,
               malformedResult(method, "a JSON object with a non-empty id"));
            return;
        }
        cb(std::move(decoded), std::nullopt);
    };
}

} // namespace

WorkspaceDb::WorkspaceDb(CodeharbordClient* client) : m_client(client)
{
    // Every method dereferences m_client unconditionally. Catch a null here,
    // at the construction site that got it wrong, rather than at whichever
    // async call happens to run first.
    Q_ASSERT(m_client);
}

void WorkspaceDb::list(const ServerId& serverId, ListCallback cb)
{
    const QJsonObject params{{QStringLiteral("serverId"), serverId.value}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceList), params,
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb({}, std::move(error));
                return;
            }
            // Not an array: toArray() would silently yield an EMPTY one, i.e. an
            // empty sidebar reported as a successful load, which the caller
            // cannot distinguish from a server that genuinely has no groups.
            if (!result.isArray()) {
                cb({}, malformedResult(rpc::kMethodWorkspaceList,
                                       "a JSON array of groups"));
                return;
            }
            cb(parseGroupList(result.toArray()), std::nullopt);
        });
}

void WorkspaceDb::createGroup(const CreateGroupParams& params, GroupCallback cb)
{
    m_client->call(QString::fromLatin1(rpc::kMethodWorkspaceCreateGroup),
                   serializeCreateGroup(params),
                   recordHandler(rpc::kMethodWorkspaceCreateGroup, std::move(cb),
                                 parseGroup));
}

void WorkspaceDb::updateGroup(const UpdateGroupParams& params, GroupCallback cb)
{
    m_client->call(QString::fromLatin1(rpc::kMethodWorkspaceUpdateGroup),
                   serializeUpdateGroup(params),
                   recordHandler(rpc::kMethodWorkspaceUpdateGroup, std::move(cb),
                                 parseGroup));
}

void WorkspaceDb::deleteGroup(const GroupId& id, DeleteCallback cb)
{
    const QJsonObject params{{QStringLiteral("id"), id.value}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceDeleteGroup), params,
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(QStringList(), std::move(error));
                return;
            }
            if (const std::optional<QStringList> targets = parseTmuxTargets(result))
                cb(*targets, std::nullopt);
            else
                cb(QStringList(),
                   malformedResult(rpc::kMethodWorkspaceDeleteGroup,
                                   "an object with ok:true and, if present, a "
                                   "tmuxTargets array of strings"));
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
            cb(std::move(error));
        });
}

void WorkspaceDb::createSession(const CreateSessionParams& params,
                                SessionCallback cb)
{
    m_client->call(QString::fromLatin1(rpc::kMethodWorkspaceCreateSession),
                   serializeCreateSession(params),
                   recordHandler(rpc::kMethodWorkspaceCreateSession,
                                 std::move(cb), parseSession));
}

void WorkspaceDb::updateSession(const UpdateSessionParams& params,
                                SessionCallback cb)
{
    m_client->call(QString::fromLatin1(rpc::kMethodWorkspaceUpdateSession),
                   serializeUpdateSession(params),
                   recordHandler(rpc::kMethodWorkspaceUpdateSession,
                                 std::move(cb), parseSession));
}

void WorkspaceDb::deleteSession(const DevSessionId& id, DeleteCallback cb)
{
    const QJsonObject params{{QStringLiteral("id"), id.value}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceDeleteSession), params,
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(QStringList(), std::move(error));
                return;
            }
            if (const std::optional<QStringList> targets = parseTmuxTargets(result))
                cb(*targets, std::nullopt);
            else
                cb(QStringList(),
                   malformedResult(rpc::kMethodWorkspaceDeleteSession,
                                   "an object with ok:true and, if present, a "
                                   "tmuxTargets array of strings"));
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
            cb(std::move(error));
        });
}

void WorkspaceDb::moveSessionToGroup(const MoveSessionParams& params,
                                     SessionCallback cb)
{
    m_client->call(QString::fromLatin1(rpc::kMethodWorkspaceMoveSessionToGroup),
                   serializeMoveSession(params),
                   recordHandler(rpc::kMethodWorkspaceMoveSessionToGroup,
                                 std::move(cb), parseSession));
}

void WorkspaceDb::duplicateSession(const DevSessionId& id,
                                   SessionNodeCallback cb)
{
    const QJsonObject params{{QStringLiteral("id"), id.value}};
    m_client->call(QString::fromLatin1(rpc::kMethodWorkspaceDuplicateSession),
                   params,
                   recordHandler(rpc::kMethodWorkspaceDuplicateSession,
                                 std::move(cb), parseSessionNode));
}

void WorkspaceDb::createViewerPane(const CreateViewerPaneParams& params,
                                   ViewerPaneCallback cb)
{
    m_client->call(QString::fromLatin1(rpc::kMethodWorkspaceCreateViewerPane),
                   serializeCreateViewerPane(params),
                   recordHandler(rpc::kMethodWorkspaceCreateViewerPane,
                                 std::move(cb), parseViewerPane));
}

void WorkspaceDb::updateViewerPane(const UpdateViewerPaneParams& params,
                                   ViewerPaneCallback cb)
{
    m_client->call(QString::fromLatin1(rpc::kMethodWorkspaceUpdateViewerPane),
                   serializeUpdateViewerPane(params),
                   recordHandler(rpc::kMethodWorkspaceUpdateViewerPane,
                                 std::move(cb), parseViewerPane));
}

void WorkspaceDb::deleteViewerPane(const ViewerPaneId& id, OkCallback cb)
{
    const QJsonObject params{{QStringLiteral("id"), id.value}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceDeleteViewerPane), params,
        [cb = std::move(cb)](QJsonValue, std::optional<RpcError> error) {
            cb(std::move(error));
        });
}

void WorkspaceDb::createTerminalPane(const CreateTerminalPaneParams& params,
                                     TerminalPaneCallback cb)
{
    m_client->call(QString::fromLatin1(rpc::kMethodWorkspaceCreateTerminalPane),
                   serializeCreateTerminalPane(params),
                   recordHandler(rpc::kMethodWorkspaceCreateTerminalPane,
                                 std::move(cb), parseTerminalPane));
}

void WorkspaceDb::resolveTerminalPane(const ResolveTerminalPaneParams& params,
                                      TerminalPaneCallback cb)
{
    // The row id wins when it is there, because that is the leaf's own terminal
    // and the label is only its historical stand-in; the label is deliberately
    // not sent alongside it, so the server is never asked a question the caller
    // did not mean.
    const bool byRow = !params.id.value.isEmpty();
    // With NEITHER address there is no question to ask. This used to go out as
    // an empty `name`, i.e. "look up (or create) the pane called ''": the server
    // refuses it, so the caller did get an error, but only after a round trip
    // and phrased as though the empty name were a real request. Refuse it here,
    // with the same code the server would have used.
    if (!byRow && params.name.isEmpty()) {
        failAsync(m_client, std::move(cb),
                  invalidParams(rpc::kMethodWorkspaceResolveTerminalPane,
                                R"(give one of "id" (the terminal pane row, the )"
                                R"(normal case) or "name" (a layout slot label, )"
                                R"(legacy layouts only); neither was set)"));
        return;
    }

    QJsonObject obj{
        {QStringLiteral("serverId"), params.serverId.value},
        {QStringLiteral("devSessionId"), params.devSessionId.value},
    };
    if (byRow)
        obj[QStringLiteral("id")] = params.id.value;
    else
        obj[QStringLiteral("name")] = params.name;
    // Omitted rather than sent as null when unset, like every other optional in
    // this file: the server then applies its own default instead of being told
    // to store an explicit nothing.
    if (params.workingDirectory)
        obj[QStringLiteral("workingDirectory")] = *params.workingDirectory;
    m_client->call(QString::fromLatin1(rpc::kMethodWorkspaceResolveTerminalPane),
                   obj,
                   recordHandler(rpc::kMethodWorkspaceResolveTerminalPane,
                                 std::move(cb), parseTerminalPane));
}

void WorkspaceDb::updateTerminalPane(const UpdateTerminalPaneParams& params,
                                     TerminalPaneCallback cb)
{
    m_client->call(QString::fromLatin1(rpc::kMethodWorkspaceUpdateTerminalPane),
                   serializeUpdateTerminalPane(params),
                   recordHandler(rpc::kMethodWorkspaceUpdateTerminalPane,
                                 std::move(cb), parseTerminalPane));
}

void WorkspaceDb::deleteTerminalPane(const TerminalId& id, OkCallback cb)
{
    const QJsonObject params{{QStringLiteral("id"), id.value}};
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceDeleteTerminalPane), params,
        [cb = std::move(cb)](QJsonValue, std::optional<RpcError> error) {
            cb(std::move(error));
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
                cb(std::nullopt, std::move(error));
                return;
            }
            // getLayout is the ONE workspace method whose success result is
            // legitimately JSON null: that is how the server says the region has
            // no persisted layout (SessionLayout | null in workspace.ts).
            if (result.isNull()) {
                cb(std::nullopt, std::nullopt);
                return;
            }
            if (!result.isObject()) {
                cb(std::nullopt,
                   malformedResult(rpc::kMethodWorkspaceGetLayout,
                                   "a SessionLayout object or null"));
                return;
            }
            cb(parseLayoutRow(result.toObject()), std::nullopt);
        });
}

void WorkspaceDb::setLayout(const ServerId& serverId,
                            const DevSessionId& devSessionId, Region region,
                            const SplitNode& tree, LayoutCallback cb)
{
    // A tree tryToJson() refuses is one no client could ever load back
    // (SplitTree.h): storing it would lose the user's layout silently, on the
    // NEXT launch rather than now. Fail the write instead of performing it.
    const std::optional<QJsonObject> treeJson = tree.tryToJson();
    if (!treeJson) {
        failAsync(m_client, std::move(cb),
                  invalidParams(rpc::kMethodWorkspaceSetLayout,
                                "`tree` is not a valid split tree"));
        return;
    }

    const QJsonObject params{
        {QStringLiteral("serverId"), serverId.value},
        {QStringLiteral("devSessionId"), devSessionId.value},
        {QStringLiteral("region"), regionKey(region)},
        {QStringLiteral("tree"), *treeJson},
    };
    m_client->call(
        QString::fromLatin1(rpc::kMethodWorkspaceSetLayout), params,
        [cb = std::move(cb)](QJsonValue result, std::optional<RpcError> error) {
            if (error) {
                cb(std::nullopt, std::move(error));
                return;
            }
            if (!result.isObject()) {
                cb(std::nullopt, malformedResult(rpc::kMethodWorkspaceSetLayout,
                                                 "a SessionLayout object"));
                return;
            }
            // Unlike getLayout, setLayout has NO legitimate "there is no layout
            // here" answer: it echoes the row it has just stored. So a row whose
            // `tree` is missing or fails SplitNode validation is a malformed
            // reply, not an absent layout, and handing the caller
            // (no tree, no error) would tell it the write succeeded while
            // silently giving it nothing to adopt.
            std::optional<SplitNode> stored = parseLayoutRow(result.toObject());
            if (!stored) {
                cb(std::nullopt,
                   malformedResult(rpc::kMethodWorkspaceSetLayout,
                                   "a SessionLayout object with a valid tree"));
                return;
            }
            cb(std::move(stored), std::nullopt);
        });
}

} // namespace ch
