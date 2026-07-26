#pragma once

#include <QString>
#include <QVector>
#include <optional>

namespace ch::rpc {

// C1 — RPC method catalog (docs/PLAN.md). C++ mirror of the frozen TypeScript
// contract in remote/src/rpc-types.ts for the initial SPEC 8.3 editing file
// method set. Header-only: pure data shapes and method-name constants bound by
// the R-client workstream. Distinct from the server-side implementation in
// remote/.
//
// Revision tokens (SPEC 8.4) are OPAQUE strings minted by the server. The
// client stores and echoes them verbatim as expectedRevision on writes and
// NEVER derives, parses, or synthesizes them. A write whose expectedRevision no
// longer matches is rejected (never silently overwritten — SPEC 8.6) with
// kRevisionMismatch.

// Application-level JSON-RPC error code for a writeFile whose expectedRevision
// no longer matches the file's current revision (SPEC 8.4 / 8.6).
inline constexpr int kRevisionMismatch = -32001;

// Stable wire method names for the initial file set (SPEC 8.3). These mirror the
// values in RPC_METHODS in remote/src/rpc-types.ts.
inline constexpr auto kMethodStat = "file.stat";
inline constexpr auto kMethodReadFile = "file.readFile";
inline constexpr auto kMethodWriteFile = "file.writeFile";
inline constexpr auto kMethodResolvePath = "file.resolvePath";
inline constexpr auto kMethodWatch = "file.watch";
inline constexpr auto kMethodUnwatch = "file.unwatch";
inline constexpr auto kMethodListDirectory = "file.listDirectory";

// Server -> client notification method name for an active watch subscription
// (SPEC 8.7). A NOTIFICATION name (no id, no response), deliberately NOT part
// of the request methods above. Mirrors RPC_WATCH_EVENT_NOTIFICATION in
// remote/src/rpc-types.ts.
inline constexpr auto kWatchEventNotification = "file.watchEvent";

// --- Server introspection ---------------------------------------------------
//
// Mirrors the `server.info` handler in remote/src/codeharbord.ts.
inline constexpr auto kMethodServerInfo = "server.info";

// Result of `server.info`. `serverId` (SPEC 3.5) is the STABLE, SERVER-OWNED
// identity of the workspace database on that host: minted by codeharbord on
// first use and persisted, so it survives restarts and is the SAME for every
// process sharing the database. It is the value the client must key a remote
// workspace by — every stored row's server_id refers to it. The client NEVER
// derives or substitutes it (a locally minted id would leave the user staring
// at an empty workspace while their real rows sit orphaned on the server), and
// it does NOT change when the host, port, user, or repository path changes:
// those describe the route to the data, not the data.
struct ServerInfoResult {
    QString name;
    QString version;
    int schemaVersion;
    QString serverId;
};

enum class Kind { File, Directory, Symlink, Other };
enum class Encoding { Utf8, Base64 };
enum class WatchEventKind { Created, Modified, Deleted, Renamed };

struct StatParams {
    QString path;
};

struct StatResult {
    QString path;
    Kind kind;
    qint64 size;
    qint64 mtimeMs;
    qint64 mode;
    QString revision;
};

struct ReadFileParams {
    QString path;
    std::optional<qint64> offset;
    std::optional<qint64> length;
};

struct ReadFileResult {
    QString path;
    Encoding encoding;
    QString content;
    QString revision;
    bool truncated;
};

struct WriteFileParams {
    QString path;
    QString content;
    std::optional<Encoding> encoding;
    QString expectedRevision;
};

struct WriteFileResult {
    QString path;
    QString revision;
};

struct ResolvePathParams {
    QString path;
    std::optional<QString> base;
};

struct ResolvePathResult {
    QString path;
    bool insideRepositoryRoot;
};

struct WatchParams {
    QString path;
};

struct WatchResult {
    QString subscriptionId;
};

struct UnwatchParams {
    QString subscriptionId;
};

struct UnwatchResult {
    bool ok;
};

// Server -> client notification for an active watch subscription. `revision` is
// populated when a new revision is known for the affected path.
struct WatchEvent {
    QString subscriptionId;
    QString path;
    WatchEventKind event;
    std::optional<QString> revision;
};

struct ListDirectoryParams {
    QString path;
};

// One entry in a directory listing (SPEC 7.5). Mirrors DirectoryEntry in
// remote/src/rpc-types.ts; server order is unspecified, so the client sorts.
struct DirectoryEntry {
    QString name;
    Kind kind;
};

struct ListDirectoryResult {
    QString path;
    QVector<DirectoryEntry> entries;
};

// --- tmux session discovery (SPEC 10.2) -------------------------------------
//
// Mirrors the `tmux.*` group in remote/src/rpc-types.ts. It lets the client
// list and ADOPT tmux sessions that already exist on the host instead of
// assuming its own naming scheme. Absence is not failure: a host with no tmux
// binary, or with no server running, returns an empty/false RESULT rather than
// a JSON-RPC error, so the client must not treat emptiness as a fault.

// Stable wire method names, mirroring RPC_TMUX_METHODS.
inline constexpr auto kMethodListSessions = "tmux.listSessions";
inline constexpr auto kMethodSessionExists = "tmux.sessionExists";
inline constexpr auto kMethodKillSession = "tmux.killSession";

// Mirrors TmuxSession. `created` is a UNIX timestamp in SECONDS (tmux's
// session_created), not milliseconds like StatResult::mtimeMs.
struct TmuxSession {
    QString name;
    int windows;
    qint64 created;
    bool attached;
};

// tmux.listSessions takes no parameters; the result IS the session array.
using ListSessionsResult = QVector<TmuxSession>;

struct SessionExistsParams {
    QString name;
};

struct SessionExistsResult {
    bool exists;
};

struct KillSessionParams {
    QString name;
};

// kill-session is idempotent and reports no payload (mirrors the empty `{}`).
struct KillSessionResult {};

// --- workspace persistence (SPEC 4.2, 11.1) ---------------------------------
//
// Mirrors the `workspace.*` group in remote/src/rpc-types.ts. This is the
// client's CRUD surface over the server-owned workspace database; the data
// shapes live in src/persistence/WorkspaceDb.h, only the wire names belong to
// the contract.
//
// Stable wire method names, mirroring RPC_WORKSPACE_METHODS. Keep this block in
// the same order as the TypeScript table: remote/test/rpc-mirror.test.ts parses
// these `kMethodWorkspace*` definitions out of this header and fails if the two
// sides' method-name sets diverge.
inline constexpr auto kMethodWorkspaceList = "workspace.list";
inline constexpr auto kMethodWorkspaceCreateGroup = "workspace.createGroup";
inline constexpr auto kMethodWorkspaceUpdateGroup = "workspace.updateGroup";
inline constexpr auto kMethodWorkspaceDeleteGroup = "workspace.deleteGroup";
inline constexpr auto kMethodWorkspaceReorderGroups = "workspace.reorderGroups";
inline constexpr auto kMethodWorkspaceCreateSession = "workspace.createSession";
inline constexpr auto kMethodWorkspaceUpdateSession = "workspace.updateSession";
inline constexpr auto kMethodWorkspaceDeleteSession = "workspace.deleteSession";
inline constexpr auto kMethodWorkspaceReorderSessions =
    "workspace.reorderSessions";
inline constexpr auto kMethodWorkspaceMoveSessionToGroup =
    "workspace.moveSessionToGroup";
inline constexpr auto kMethodWorkspaceDuplicateSession =
    "workspace.duplicateSession";
inline constexpr auto kMethodWorkspaceCreateViewerPane =
    "workspace.createViewerPane";
inline constexpr auto kMethodWorkspaceUpdateViewerPane =
    "workspace.updateViewerPane";
inline constexpr auto kMethodWorkspaceDeleteViewerPane =
    "workspace.deleteViewerPane";
inline constexpr auto kMethodWorkspaceCreateTerminalPane =
    "workspace.createTerminalPane";
inline constexpr auto kMethodWorkspaceUpdateTerminalPane =
    "workspace.updateTerminalPane";
inline constexpr auto kMethodWorkspaceDeleteTerminalPane =
    "workspace.deleteTerminalPane";
inline constexpr auto kMethodWorkspaceGetLayout = "workspace.getLayout";
inline constexpr auto kMethodWorkspaceSetLayout = "workspace.setLayout";

} // namespace ch::rpc
