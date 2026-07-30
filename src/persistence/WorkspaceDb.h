#pragma once

#include "CodeharbordClient.h"
#include "Ids.h"
#include "SplitTree.h"
#include "WorkspaceTypes.h"

#include <QString>
#include <QVector>

#include <functional>
#include <optional>

namespace ch {

// The two split-tree regions of a Dev Session (SPEC 4.5). Mirrors the server's
// `Region` union ("viewer" | "terminal") used by workspace.getLayout/setLayout.
enum class Region { Viewer, Terminal };

// One Dev Session with its ordered panes and per-region split layouts, mirroring
// the nested workspace.list / workspace.duplicateSession wire shape. A region
// that has no persisted layout is std::nullopt.
struct SessionNode {
    DevSession session;
    QVector<ViewerPane> viewerPanes;
    QVector<TerminalPane> terminalPanes;
    std::optional<SplitNode> viewerLayout;
    std::optional<SplitNode> terminalLayout;
};

// One sidebar Group with its ordered Dev Sessions (workspace.list wire shape).
struct GroupNode {
    Group group;
    QVector<SessionNode> sessions;
};

// --- Mutation parameter shapes (mirror the workspace.ts param interfaces) ----
// Required members map 1:1 to the wire fields. Optional members are omitted from
// the request when unset, letting the server apply its default (create) or keep
// the current value (update).

struct CreateGroupParams {
    ServerId serverId;
    QString name;
    std::optional<int> position;
    std::optional<bool> collapsed;
};

struct UpdateGroupParams {
    GroupId id;
    std::optional<QString> name;
    std::optional<int> position;
    std::optional<bool> collapsed;
};

struct CreateSessionParams {
    ServerId serverId;
    GroupId groupId;
    QString name;
    QString repositoryRoot;
    std::optional<QString> defaultWorkingDirectory;
    std::optional<QString> taskDescription;
    std::optional<int> position;
    std::optional<bool> archived;
};

struct UpdateSessionParams {
    DevSessionId id;
    std::optional<QString> name;
    std::optional<QString> repositoryRoot;
    std::optional<QString> defaultWorkingDirectory;
    std::optional<QString> taskDescription;
    std::optional<int> position;
    std::optional<bool> archived;
};

struct MoveSessionParams {
    DevSessionId id;
    GroupId groupId;
    std::optional<int> position;
};

struct CreateViewerPaneParams {
    ServerId serverId;
    DevSessionId devSessionId;
    QString url;
    std::optional<QString> handler;
    std::optional<QString> title;
    std::optional<int> position;
};

struct UpdateViewerPaneParams {
    ViewerPaneId id;
    std::optional<QString> url;
    std::optional<QString> handler;
    std::optional<QString> title;
    std::optional<int> position;
};

struct CreateTerminalPaneParams {
    ServerId serverId;
    DevSessionId devSessionId;
    QString name;
    std::optional<QString> workingDirectory;
    std::optional<QString> tmuxTarget;
    std::optional<QString> startupCommand;
    std::optional<QString> harness;
    std::optional<int> position;
};

struct UpdateTerminalPaneParams {
    TerminalId id;
    std::optional<QString> name;
    std::optional<QString> workingDirectory;
    std::optional<QString> tmuxTarget;
    std::optional<QString> startupCommand;
    std::optional<QString> harness;
    std::optional<int> position;
};

// Client-side workspace repository (SPEC 11.2). Holds no SQLite or filesystem
// state: the authoritative database lives on the codeharbord host, so every
// operation is a `workspace.*` JSON-RPC round-trip through the injected
// CodeharbordClient, whose camelCase result JSON is mapped into the ch:: data
// model. All methods are async; each takes a callback receiving EITHER the typed
// result OR an RpcError forwarded verbatim from the server (SPEC 10.3) —
// WorkspaceDb never throws and never touches local storage.
//
// Lifetime: `client` is borrowed, not owned, and must outlive this object. Each
// pending callback is owned by that client, NOT by WorkspaceDb, and runs at most
// once — possibly long after WorkspaceDb itself is gone, since destroying the
// repository cancels nothing. A callback that captures a QObject therefore has
// to guard its own lifetime; the house pattern is a QPointer captured by value
// and checked before use (see src/app/SessionLayouts.cpp).
class WorkspaceDb {
public:
    // Informational only: the client runs no migrations (SPEC 11.2). Kept in
    // lockstep with remote/sql/schema.sql and WORKSPACE_SCHEMA_VERSION so the
    // three move together.
    static constexpr int kSchemaVersion = 2;

    using ListCallback =
        std::function<void(QVector<GroupNode>, std::optional<RpcError>)>;
    using GroupCallback =
        std::function<void(std::optional<Group>, std::optional<RpcError>)>;
    using SessionCallback =
        std::function<void(std::optional<DevSession>, std::optional<RpcError>)>;
    using SessionNodeCallback =
        std::function<void(std::optional<SessionNode>, std::optional<RpcError>)>;
    using ViewerPaneCallback =
        std::function<void(std::optional<ViewerPane>, std::optional<RpcError>)>;
    using TerminalPaneCallback =
        std::function<void(std::optional<TerminalPane>, std::optional<RpcError>)>;
    using LayoutCallback =
        std::function<void(std::optional<SplitNode>, std::optional<RpcError>)>;
    using OkCallback = std::function<void(std::optional<RpcError>)>;

    explicit WorkspaceDb(CodeharbordClient* client);

    // Nested read: groups -> sessions -> {viewerPanes, terminalPanes, layouts}.
    void list(const ServerId& serverId, ListCallback cb);

    // Groups.
    void createGroup(const CreateGroupParams& params, GroupCallback cb);
    void updateGroup(const UpdateGroupParams& params, GroupCallback cb);
    void deleteGroup(const GroupId& id, OkCallback cb);
    void reorderGroups(const ServerId& serverId,
                       const QVector<GroupId>& orderedIds, OkCallback cb);

    // Sessions.
    void createSession(const CreateSessionParams& params, SessionCallback cb);
    void updateSession(const UpdateSessionParams& params, SessionCallback cb);
    void deleteSession(const DevSessionId& id, OkCallback cb);
    void reorderSessions(const GroupId& groupId,
                         const QVector<DevSessionId>& orderedIds, OkCallback cb);
    void moveSessionToGroup(const MoveSessionParams& params, SessionCallback cb);
    void duplicateSession(const DevSessionId& id, SessionNodeCallback cb);

    // Viewer panes.
    void createViewerPane(const CreateViewerPaneParams& params,
                          ViewerPaneCallback cb);
    void updateViewerPane(const UpdateViewerPaneParams& params,
                          ViewerPaneCallback cb);
    void deleteViewerPane(const ViewerPaneId& id, OkCallback cb);

    // Terminal panes.
    void createTerminalPane(const CreateTerminalPaneParams& params,
                            TerminalPaneCallback cb);
    void updateTerminalPane(const UpdateTerminalPaneParams& params,
                            TerminalPaneCallback cb);
    void deleteTerminalPane(const TerminalId& id, OkCallback cb);

    // Per-region split layouts. getLayout delivers std::nullopt when the region
    // has no persisted layout; setLayout delivers the stored tree on success.
    void getLayout(const DevSessionId& devSessionId, Region region,
                   LayoutCallback cb);
    void setLayout(const ServerId& serverId, const DevSessionId& devSessionId,
                   Region region, const SplitNode& tree, LayoutCallback cb);

private:
    CodeharbordClient* m_client;
};

} // namespace ch
