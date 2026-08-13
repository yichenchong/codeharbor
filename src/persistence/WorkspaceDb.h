#pragma once

#include "CodeharbordClient.h"
#include "Ids.h"
#include "SplitTree.h"
#include "WorkspaceTypes.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstddef>
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
//
// The server reads the nullable-text fields (workingDirectory, tmuxTarget,
// startupCommand, harness, taskDescription, defaultWorkingDirectory, handler,
// title) as THREE-valued on an update: absent means "keep what is stored", JSON
// null means "clear the column to NULL", and a string means "store that string"
// (remote/src/workspace.ts, e.g. updateTerminalPane). A std::optional<QString>
// spans only two of those three: disengaged omits the key, and engaged always
// writes a JSON string, an empty one included. There is deliberately no way to
// send JSON null from here, so the server's clear-to-NULL branch is not
// reachable through this class; no caller needs to clear a field, and a second
// way to spell "absent" would cost more than the gap does.
//
// An engaged optional holding an empty string is therefore a WRITE of "", not a
// clear. On tmuxTarget that write is refused rather than applied: the server
// requires 1-N characters of [A-Za-z0-9_-], so an empty tmuxTarget fails the
// whole request with -32602 instead of blanking the column.

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
    std::optional<bool> pinned;
};

struct UpdateSessionParams {
    DevSessionId id;
    std::optional<QString> name;
    std::optional<QString> repositoryRoot;
    std::optional<QString> defaultWorkingDirectory;
    std::optional<QString> taskDescription;
    std::optional<int> position;
    std::optional<bool> archived;
    std::optional<bool> pinned;
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

// Find the `terminal_panes` row ONE layout leaf owns. At least one of `id` and
// `name` must be set; supplying neither is a caller error this class refuses
// locally (see resolveTerminalPane) on the same terms the server would.
//
//   * `id` — the row's own identity, taken from the layout leaf
//     (SplitNode::terminalPaneId). A pure lookup, and the normal case. Empty
//     means "not addressed by row"; TerminalId, like every other id here.
//
//   * `name` — a layout slot label ("terminal-1", …), lookup-or-CREATE, and
//     ONLY for a leaf stored before layouts carried a row id, where the label
//     is genuinely the historical key. The caller writes the answer's id back
//     into the leaf, so a leaf takes this path once in its life.
//
//     Deliberately a plain QString and NOT an id type: a slot label is not an
//     identity and this class says so twice over (kSchemaVersion 4 below dropped
//     the server's UNIQUE (dev_session_id, name) for exactly that reason, and a
//     closed pane keeps its label for the next pane to reuse). It is the same
//     plain string as SplitNode::paneId, which is where it comes from.
//
// Deliberately narrow — it is a lookup key, not a row editor — so a caller
// cannot use it to rewrite a pane it merely wanted to find. `workingDirectory`
// applies only if the row has to be created.
struct ResolveTerminalPaneParams {
    ServerId serverId;
    DevSessionId devSessionId;
    TerminalId id;
    QString name;
    std::optional<QString> workingDirectory;
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
// result OR an RpcError (SPEC 10.3) — WorkspaceDb never throws and never touches
// local storage. A server error is forwarded verbatim. The only errors
// synthesized here rather than received all describe one thing: a response that
// reports success but whose result is not the shape the method promises. That
// is failed with the reserved code -32603, because decoding it would manufacture
// a record with an empty id that the rest of the client cannot tell from a real
// one. Concretely, a result that is not the expected JSON kind (an object per
// record, an array for list()), and, for setLayout, a SessionLayout row whose
// `tree` is missing or fails split-tree validation. getLayout is the sole method
// for which a JSON null result is legitimate — it means "this region has no
// persisted layout" and delivers std::nullopt with no error; it is also the sole
// method that maps an unreadable stored tree to that same "no layout" verdict
// rather than to an error, because a region whose layout cannot be loaded must
// stay empty instead of inviting the user to edit a fabricated one.
//
// Two failures are detected here rather than received, and both are the
// CALLER's mistake, not the server's: an addressing-mode violation in
// resolveTerminalPane and a setLayout tree that cannot be serialized. They are
// reported as -32602 ("invalid params"), the code the server itself would have
// answered with, and they are delivered ASYNCHRONOUSLY like every other reply —
// posted to the client's event loop, never invoked before the method returns —
// so no caller is re-entered from inside its own call.
//
// Lifetime: `client` is borrowed, not owned, must be non-null, and must outlive
// this object. Passing a literal nullptr does not compile; a null computed at
// runtime trips the constructor's assertion, which is where the mistake is,
// instead of surfacing as a crash inside whichever method happened to be called
// first. Each pending callback is owned by that client, NOT by WorkspaceDb, and
// runs at most once — possibly long after WorkspaceDb itself is gone, since
// destroying the repository cancels nothing. A callback that captures a QObject
// therefore has to guard its own lifetime; the house pattern is a QPointer
// captured by value and checked before use (see src/app/SessionLayouts.cpp).
class WorkspaceDb {
public:
    // Informational only: the client runs no migrations (SPEC 11.2). Kept in
    // lockstep with remote/sql/schema.sql and WORKSPACE_SCHEMA_VERSION so the
    // three move together. 3 is where terminal_panes.tmux_target became UNIQUE:
    // two panes on one target attach the same remote shell (SPEC 5.2). 4 drops
    // v3's UNIQUE (dev_session_id, name): a slot label is not an identity, and
    // a closed pane keeps its row and its label while a new pane takes the same
    // label. 5 adds the server-owned Dev Session pinned bit with a false
    // default for every existing row.
    static constexpr int kSchemaVersion = 5;

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
    // Answer of a delete that can destroy terminal panes. `tmuxTargets` is what
    // the SERVER reports it actually destroyed (remote/src/rpc-types.ts
    // DeleteWithTmuxTargetsResult), collected inside the deleting transaction.
    //
    // It exists because the client cannot work this out for itself: it would
    // have to read its own last workspace.list, and a pane another client
    // created or retargeted since then is destroyed by the same delete. Killing
    // from that snapshot would leave the pane's shell running under a name
    // nothing can ever produce again (SPEC 4.4). Empty on a server too old to
    // report the field, which reads as "nothing to kill" — the safe direction,
    // since the only alternative is inventing a target.
    using DeleteCallback =
        std::function<void(QStringList tmuxTargets, std::optional<RpcError>)>;

    explicit WorkspaceDb(CodeharbordClient* client);
    // No repository without a client: every method here dereferences it
    // unconditionally.
    WorkspaceDb(std::nullptr_t) = delete;

    // Nested read: every group on `serverId` with its sessions, panes and
    // layouts.
    //
    // Deliberately WITHOUT the server's `pinnedOnly` request filter, even
    // though workspace.list accepts one. Pinning is server-owned state, but
    // which pinned/archived rows the sidebar SHOWS is a client-local
    // presentation choice held by ch::SessionsModel: it filters the tree it
    // already has, so toggling the star costs no round trip and turning it off
    // brings the rows straight back. Asking the server to pre-filter would
    // return a tree missing rows the client still needs; the app-level tests
    // assert that this request never carries `pinnedOnly`.
    void list(const ServerId& serverId, ListCallback cb);
    // Groups.
    void createGroup(const CreateGroupParams& params, GroupCallback cb);
    void updateGroup(const UpdateGroupParams& params, GroupCallback cb);
    void deleteGroup(const GroupId& id, DeleteCallback cb);
    void reorderGroups(const ServerId& serverId,
                       const QVector<GroupId>& orderedIds, OkCallback cb);

    // Sessions.
    void createSession(const CreateSessionParams& params, SessionCallback cb);
    void updateSession(const UpdateSessionParams& params, SessionCallback cb);
    void deleteSession(const DevSessionId& id, DeleteCallback cb);
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
    //
    // MINT a new row. This is what a newly created terminal layout leaf calls:
    // the row it gets back is that leaf's terminal for good, and its id goes
    // into the leaf (SplitNode::terminalPaneId). `name` is the slot LABEL and
    // is not required to be free - a closed pane keeps its row and its label,
    // and a new pane reusing that label wants a new terminal, not the old one
    // (see remote/sql/schema.sql on why the pair is not unique).
    void createTerminalPane(const CreateTerminalPaneParams& params,
                            TerminalPaneCallback cb);
    // FIND the row a layout leaf already owns (SPEC 5.2). This is how a
    // terminal pane learns which remote tmux session is its own.
    //
    // By row id it is a pure lookup. By slot label — legacy layouts only — it
    // is lookup-or-create, done atomically ON THE SERVER, and deliberately one
    // call rather than list()+createTerminalPane(): two clients running that
    // pair concurrently both see no row and both create one, giving a single
    // slot two rows, two server-minted targets and two tmux sessions. The
    // server does it inside one BEGIN IMMEDIATE transaction, so the two
    // converge on one row and neither client needs a retry path.
    //
    // At least one of `params.id` and `params.name` must be non-empty; the id
    // wins when both are, and only it is sent. Setting NEITHER is refused HERE,
    // with -32602 and no request on the wire. It used to go out as an empty
    // `name`, asking the server to look up — or create — a pane called "".
    void resolveTerminalPane(const ResolveTerminalPaneParams& params,
                             TerminalPaneCallback cb);
    void updateTerminalPane(const UpdateTerminalPaneParams& params,
                            TerminalPaneCallback cb);
    // No deleteTerminalPane wrapper: nothing in the client deletes a pane row on
    // its own. A pane leaves through its region's layout, and closing a pane
    // kills its tmux session through TerminalFactory first (SPEC 4.4). The
    // daemon still serves workspace.deleteTerminalPane; when something here
    // needs it, it should be added with the DeleteCallback shape the other two
    // deletes use, because that method reports tmux targets too.

    // Per-region split layouts. getLayout delivers std::nullopt when the region
    // has no persisted layout (and likewise when the stored tree is unreadable);
    // setLayout delivers the stored tree on success, and fails with -32603
    // rather than delivering an empty std::nullopt if the echoed row carries no
    // valid tree. A `tree` that SplitNode::tryToJson() refuses is never sent:
    // it would store bytes no client could load back, so setLayout fails it
    // with -32602 instead.
    void getLayout(const DevSessionId& devSessionId, Region region,
                   LayoutCallback cb);
    void setLayout(const ServerId& serverId, const DevSessionId& devSessionId,
                   Region region, const SplitNode& tree, LayoutCallback cb);

private:
    CodeharbordClient* m_client;
};

} // namespace ch
