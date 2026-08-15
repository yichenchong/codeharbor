// C1 — RPC method catalog (docs/PLAN.md). Typed request/result shapes for the
// initial SPEC 8.3 editing file method set, plus stable method-name constants
// and application-level error codes. Consumed by codeharbord.ts (server) and
// mirrored in C++ at src/remote/RpcTypes.h (client). Owner: R.
//
// Revision tokens (SPEC 8.4): every read returns an opaque `string` revision
// token; every write echoes the loaded token back as `expectedRevision`. The
// token is OPAQUE — clients treat it as a bag of bytes and NEVER derive, parse,
// or synthesize it. Only the server mints revisions. A write whose
// `expectedRevision` no longer matches the file is rejected (never silently
// overwritten — SPEC 8.6) with RPC_REVISION_MISMATCH.

export interface StatParams {
    path: string;
}

export interface StatResult {
    path: string;
    kind: "file" | "directory" | "symlink" | "other";
    size: number;
    mtimeMs: number;
    mode: number;
    revision: string;
    // Whether the LINK-FOLLOWED target is writable by this process (fs.access
    // W_OK on the resolved path; false on any error). Lets the client mark a
    // buffer read-only up front (X12) instead of discovering it only on save.
    writable: boolean;
}

export interface ReadFileParams {
    path: string;
    offset?: number;
    length?: number;
}

export interface ReadFileResult {
    path: string;
    encoding: "utf-8" | "base64";
    content: string;
    revision: string;
    /**
     * True when `content` is NOT the whole file: bytes are missing before the
     * returned window (`offset` past byte 0) or after it (`length` stopping
     * short of the end, including a read starting at or past the end of a
     * non-empty file). False only when `content` carries every byte of the
     * file. Definition and every return path live in readFile (files.ts).
     */
    truncated: boolean;
}

export interface WriteFileParams {
    path: string;
    content: string;
    encoding?: "utf-8" | "base64";
    expectedRevision: string;
    // Optional POSIX file mode (e.g. 0o600). When present the atomic write
    // creates the temp file with this mode and leaves the final file with
    // exactly it; when absent, the current mode-preservation behaviour (RF7)
    // stands. Used by the editor for private recovery snapshots (ED15).
    mode?: number;
}

export interface WriteFileResult {
    path: string;
    revision: string;
}

export interface ResolvePathParams {
    path: string;
    base?: string;
}

export interface ResolvePathResult {
    path: string;
    insideRepositoryRoot: boolean;
}

export interface WatchParams {
    path: string;
}

export interface WatchResult {
    subscriptionId: string;
}

export interface UnwatchParams {
    subscriptionId: string;
}

export interface UnwatchResult {
    ok: true;
}

// Server -> client notification emitted for an active watch subscription. Sent
// as a JSON-RPC notification (no id); `revision` is present when a new revision
// is known for the affected path (created/modified). A watch is attached to one
// path and the daemon has no rename event source; `renamed` here used to promise
// a notification the server can never emit.
export interface WatchEvent {
    subscriptionId: string;
    path: string;
    event: "created" | "modified" | "deleted";
    revision?: string;
}

// Directory listing (SPEC 7.5 / 8.3). Added to the catalog for the viewer
// workstream's directory viewer.
//
// ORDERING IS PART OF THE CONTRACT: `entries` arrives sorted ascending by raw
// file NAME, compared as byte-for-byte JavaScript strings — not by locale, not
// by kind, not with directories first. listDirectory (files.ts) sorts before it
// answers so one directory always has one wire representation, and a test in
// remote/test/files.test.ts pins that order. This comment used to say the
// entries were unordered and the client had to sort them, which stopped being
// true when that sort landed: a client author trusting it would either add a
// second, redundant sort or assume the order carries no meaning and reshuffle a
// listing that was already canonical. A client that wants a DIFFERENT
// presentation order (directories first, case-insensitive, locale-aware) still
// sorts for itself — what it must not assume is that the server order is
// arbitrary.
export interface ListDirectoryParams {
    path: string;
}

export interface DirectoryEntry {
    name: string;
    kind: StatResult["kind"];
}

export interface ListDirectoryResult {
    path: string;
    entries: DirectoryEntry[];
}

// Stable wire method names for the file set (SPEC 8.3, 7.5). The handlers live
// in files.ts (the `fileMethods` table codeharbord.ts spreads into its method
// map); these string values are the frozen contract.
// listDirectory (SPEC 7.5) was added after the initial six for the viewer
// workstream — bump RPC_SCHEMA_VERSION (defined in remote/src/codeharbord.ts,
// not in this file) when this set changes.
export const RPC_METHODS = {
    stat: "file.stat",
    readFile: "file.readFile",
    writeFile: "file.writeFile",
    resolvePath: "file.resolvePath",
    watch: "file.watch",
    unwatch: "file.unwatch",
    listDirectory: "file.listDirectory",
} as const;

export type RpcMethodKey = keyof typeof RPC_METHODS;
export type RpcMethodName = (typeof RPC_METHODS)[RpcMethodKey];

// Server introspection request. The handler lives in codeharbord.ts rather
// than a spread-in method group, but its name is still part of the shared
// wire contract and is mirrored by kMethodServerInfo in RpcTypes.h.
export const RPC_SERVER_INFO_METHOD = "server.info";

// The server.info payload. Every other method in the catalog has its result
// shape declared here, and these two did not: nothing tied the two
// introspection handlers in codeharbord.ts to the contract the C++ client reads
// (AppController::onServerInfo), so a field could be renamed or dropped on the
// server without a single compile error on either side — it would surface as
// the client silently degrading (a missing `serverId` orphans the whole
// workspace view) rather than as a build failure.
export interface ServerInfoResult {
    // Always "codeharbord". Lets the client tell this daemon from anything else
    // an SSH command might have started on the far end.
    name: string;
    // Release version, shown to the USER verbatim in the client's
    // "Server too old" message.
    version: string;
    // Wire-contract revision the client gates compatibility on.
    schemaVersion: number;
    // Stable, persisted identity of the workspace database on this host
    // (SPEC 3.5). Clients key their stored view of the remote workspace by it.
    serverId: string;
    // The REMOTE user's data directory for crash-recovery snapshots (SPEC
    // 11.3). Additive and OPTIONAL: an older server omits it and the client
    // degrades to no-recovery rather than failing the compatibility gate.
    recoveryDir?: string;
}

// Server -> client notification method name for an active watch subscription
// (SPEC 8.7). This is a NOTIFICATION name (no id, no response), deliberately
// absent from RPC_METHODS, which enumerates only request methods. It
// lives here in the contract so the wire name stays shared between the server
// (codeharbord.ts) and the C++ client (RpcTypes.h).
export const RPC_WATCH_EVENT_NOTIFICATION = "file.watchEvent";

// Transport keepalive. Not an application method and not part of any group:
// the C++ client (CodeharbordClient::enableHeartbeat) sends it on a timer to
// answer "is the peer still reading and writing?", and drops the transport when
// several consecutive probes go unanswered. The handler lives in codeharbord's
// static method map next to server.info, and is keyed off THIS constant so the
// dispatch table and the contract cannot drift. Kept as the bare name `ping`
// rather than moved into the `server.` group: it is the wire name every
// already-deployed daemon answers, and the keepalive is precisely the call that
// must work before the client knows anything about the peer.
export const RPC_PING_METHOD = "ping";

// The keepalive payload. Its only job is to be a well-formed, cheap success:
// the client (CodeharbordClient::enableHeartbeat) counts unanswered probes and
// never inspects the body.
export interface PingResult {
    pong: true;
}

// Server -> client notification: watch notifications for these subscriptions
// were DROPPED and will never be delivered. The daemon writes notifications to
// a stdout the SSH channel drains; when that consumer stalls the daemon queues
// pending notifications, and that queue is bounded (MAX_PENDING_WATCH_EVENTS /
// MAX_PENDING_WATCH_BYTES in codeharbord.ts) so a churning directory cannot
// grow it until the process is killed. Coalescing absorbs the common case, but
// once the bound is genuinely reached events are discarded — and a client that
// silently misses a change shows stale content while believing it is current,
// which is worse than being told. This notification is that telling: for each
// listed subscription the client MUST re-read the watched path rather than
// trust its cached copy. It is a NOTIFICATION name (no id, no response), so
// like RPC_WATCH_EVENT_NOTIFICATION it is deliberately absent from RPC_METHODS.
export const RPC_WATCH_EVENTS_LOST_NOTIFICATION = "file.watchEventsLost";

// Params of RPC_WATCH_EVENTS_LOST_NOTIFICATION. Only subscriptions still live
// at the moment the notification is written are listed: an unwatched
// subscription has no consumer left to inform.
export interface WatchEventsLost {
    subscriptionIds: string[];
}

// Server -> client notification carrying ONE viewer-pane command an agent asked
// for through the control socket (remote/src/control.ts). A NOTIFICATION name
// (no id, no response), so like the two watch names above it is deliberately
// absent from RPC_METHODS.
//
// Why a notification and not a server-initiated REQUEST: the C++ client has no
// server-request dispatcher — CodeharbordClient treats a message carrying both
// `method` and `id` as malformed on purpose — and giving it one for this would
// be a second inbound shape for a single feature. The answer travels back as an
// ordinary client request instead (RPC_VIEWER_COMMAND_RESULT_METHOD), which the
// transport already carries in that direction.
export const RPC_VIEWER_COMMAND_NOTIFICATION = "viewer.command";

// Params of RPC_VIEWER_COMMAND_NOTIFICATION.
//
// `commandId` correlates the answer; it is minted by the daemon and opaque to
// the client. `devSessionId` is the Dev Session the ASKING terminal belongs to,
// and the client refuses a command for any session other than the one on screen
// — the QML viewer tree only ever holds the active session, so applying it
// anywhere else would edit a layout nobody is looking at. `terminalId` is
// provenance: which pane's agent asked. `op` is one of control.ts's CONTROL_OPS
// and `args` is that op's free-form argument object, both validated by the
// daemon before relay and re-validated by the client.
export interface ViewerCommandParams {
    commandId: string;
    devSessionId: string;
    terminalId: string;
    op: string;
    args: Record<string, unknown>;
}

// Client -> server request delivering the outcome of one relayed viewer
// command. Registered in codeharbord's static method table next to `ping` and
// `server.info` rather than in a spread-in group: it belongs to the transport's
// own control channel, not to the file/workspace/tmux domains.
//
// Answering an id the daemon no longer holds is NOT an error — it may already
// have timed the command out — so the handler succeeds either way and the
// client needs no special case for a late answer.
export const RPC_VIEWER_COMMAND_RESULT_METHOD = "viewer.commandResult";

// Params of RPC_VIEWER_COMMAND_RESULT_METHOD. `error` is present only when
// `ok` is false and its `code` is one of control.ts's CONTROL_ERROR_CODES;
// `data` is the op's result payload (a pane id, an inventory) when it has one.
export interface ViewerCommandResultParams {
    commandId: string;
    ok: boolean;
    error?: { code: string; message: string };
    data?: Record<string, unknown>;
}

export interface ViewerCommandResultResult {
    ok: true;
}

// Implementation-defined server error code (JSON-RPC 2.0 reserves -32000..-32099
// for such errors) for a writeFile whose expectedRevision no longer matches the
// file's current revision. The server rejects the write rather than silently
// overwriting concurrent changes (SPEC 8.4 / 8.6).
export const RPC_REVISION_MISMATCH = -32001;

// Implementation-defined server error code for a workspace write that could not
// take the database's write lock before workspace.ts's busy timeout ran out.
// Deliberately NOT -32603: nothing in the server malfunctioned and nothing was
// half-applied — several codeharbord processes share one database file (SPEC
// 3.5 / workspace.ts serverId), the caller simply lost a race, and the request
// can be retried verbatim. Also not -32602: the params were perfectly valid.
// The C++ client has NO special case for this code, BY DECISION rather than by
// omission: with the busy timeout in force this is close to unreachable, and an
// automatic client retry would invent a new failure mode (a write repeated
// after a partial effect) for a case that should not happen. The generic path —
// show the server's message to the user — is the right handling for a rare
// transient, so codeharbord phrases that message as an instruction, not as a
// SQLite string.
export const RPC_DATABASE_BUSY = -32002;

// Implementation-defined server error code for a request that is perfectly
// well-formed but whose ANSWER — or, for the last entry, whose own PAYLOAD —
// would exceed a server resource bound, so it was refused outright and nothing
// was changed. Five conditions raise it today: the first four in files.ts, the
// last in workspace.ts.
//
//   * file.listDirectory on a directory whose listing would not fit in one
//     transport frame (MAX_DIRECTORY_LISTING_BYTES). Serializing it anyway put
//     a line past MAX_LINE_BYTES on the wire, and BOTH ends drop the transport
//     on an over-cap frame — so one huge directory cost the user the whole
//     workspace connection, every terminal and every editor in it, with no
//     message naming the directory.
//   * file.watch past MAX_WATCH_SUBSCRIPTIONS live subscriptions, each of which
//     holds an OS watch handle and a poll timer.
//   * file.readFile on a file whose raw size is past MAX_FILE_READ_BYTES, or a
//     ranged read whose window is that large. Reading it in would have meant a
//     single allocation of the whole file.
//   * file.readFile whose ENCODED answer is past MAX_FILE_RESPONSE_BYTES, which
//     is the same wire problem as the directory case above: base64 expansion and
//     JSON escaping can make a within-limit file into an over-cap frame.
//   * workspace.setLayout whose serialized split tree is past
//     MAX_LAYOUT_TREE_BYTES. The bound is on the way IN rather than on the
//     reply, because a stored tree is read back by workspace.list: an absurd
//     one accepted once would make every later listing of that workspace
//     unanswerable. Checked before the transaction opens, so the previously
//     stored layout for the region is left exactly as it was.
//
// Deliberately NOT -32603: nothing malfunctioned and nothing is half-applied.
// Not -32602 either: the params are valid, the server simply will not answer at
// that size. The C++ client has no special case for it and shows the message to
// the USER verbatim, so every one of these messages is phrased for a person and
// names the limit that bit.
export const RPC_RESOURCE_LIMIT = -32003;

// JSON-RPC 2.0 reserved error codes. They live in this contract module rather
// than in codeharbord.ts because the param guards in validate.ts tag their
// errors with RPC_INVALID_PARAMS, and validate.ts is imported BY the modules
// codeharbord.ts imports — sourcing the constant from codeharbord.ts would
// close an import cycle. codeharbord.ts re-exports all five, so every existing
// importer (and the C++-mirror tests) keeps its current import path.
export const RPC_PARSE_ERROR = -32700;
export const RPC_INVALID_REQUEST = -32600;
export const RPC_METHOD_NOT_FOUND = -32601;
export const RPC_INVALID_PARAMS = -32602;
export const RPC_INTERNAL_ERROR = -32603;

// --- tmux session discovery (SPEC 10.2, docs/PLAN.md R-server) ---------------
//
// The client must be able to LIST and ADOPT tmux sessions that already exist on
// the host rather than assuming its own naming scheme. These methods are their
// own `tmux.*` group: RPC_METHODS above stays exactly the file.* catalog.
//
// Environment, not error: a host with no tmux binary, or with tmux installed
// but no server running, is the NORMAL state of a fresh box. Those cases yield
// an empty/false RESULT, never a JSON-RPC error.

export interface TmuxSession {
    name: string;
    // Number of windows in the session (tmux `session_windows`).
    windows: number;
    // Session creation time as a UNIX timestamp in SECONDS (tmux
    // `session_created`), which is what tmux reports — not milliseconds.
    created: number;
    attached: boolean;
}

// tmux.listSessions takes no parameters and resolves to the session array
// directly (empty when tmux is absent or no server is running).
export type ListSessionsResult = TmuxSession[];

export interface SessionExistsParams {
    name: string;
}

export interface SessionExistsResult {
    exists: boolean;
}

export interface KillSessionParams {
    name: string;
}

// Deliberately empty: kill-session is idempotent and reports no payload.
export type KillSessionResult = Record<string, never>;

// Stable wire method names for the tmux group. Mirrored in C++ at
// src/remote/RpcTypes.h — bump RPC_SCHEMA_VERSION (defined in
// remote/src/codeharbord.ts, not in this file) when this set changes.
export const RPC_TMUX_METHODS = {
    listSessions: "tmux.listSessions",
    sessionExists: "tmux.sessionExists",
    killSession: "tmux.killSession",
} as const;

export type RpcTmuxMethodKey = keyof typeof RPC_TMUX_METHODS;
export type RpcTmuxMethodName = (typeof RPC_TMUX_METHODS)[RpcTmuxMethodKey];

// --- workspace persistence (SPEC 4.2, 11.1, docs/PLAN.md workstream P) -------
//
// The `workspace.*` group is the client's CRUD surface over the server-owned
// workspace database. Like the file.* and tmux.* groups above it is its own
// namespace: RPC_METHODS stays exactly the file.* catalog.
//
// Stable wire method names for the workspace group. Mirrored in C++ at
// src/remote/RpcTypes.h — bump RPC_SCHEMA_VERSION (defined in
// remote/src/codeharbord.ts, not in this file) when this set changes.
// remote/test/rpc-mirror.test.ts parses that header and fails if the two sides
// disagree, so a rename here without the matching C++ edit is caught at test
// time rather than at runtime as a method-not-found against a live server.
export interface WorkspaceListParams {
    serverId: string;
    // This is a client-local presentation choice. It is accepted here only as
    // a read predicate; the stored session pin itself remains workspace state.
    pinnedOnly?: boolean;
}

// What a workspace delete that can destroy terminal panes answers with.
//
// `ok` is unchanged and still the whole answer for a caller that only wanted
// the rows gone; `tmuxTargets` is the addition: the tmux target of every
// `terminal_panes` row the delete ACTUALLY destroyed, read inside the same
// transaction that destroyed it.
//
// Only the server can answer this. A client kills from its last workspace read,
// and a pane another client created or retargeted since that read is deleted
// here just the same — killing from the stale snapshot would leave that pane's
// shell (and any agent in it) running under a name nothing can ever produce
// again, which is the exact orphan the kill exists to prevent (SPEC 4.4).
//
// A row whose target is NULL or empty was never attached and has no remote
// session, so it is omitted here rather than reported as a blank the caller
// has to filter.
export interface DeleteWithTmuxTargetsResult {
    ok: true;
    tmuxTargets: string[];
}

export const RPC_WORKSPACE_METHODS = {
    list: "workspace.list",
    createGroup: "workspace.createGroup",
    updateGroup: "workspace.updateGroup",
    deleteGroup: "workspace.deleteGroup",
    reorderGroups: "workspace.reorderGroups",
    createSession: "workspace.createSession",
    updateSession: "workspace.updateSession",
    deleteSession: "workspace.deleteSession",
    reorderSessions: "workspace.reorderSessions",
    moveSessionToGroup: "workspace.moveSessionToGroup",
    duplicateSession: "workspace.duplicateSession",
    createViewerPane: "workspace.createViewerPane",
    updateViewerPane: "workspace.updateViewerPane",
    deleteViewerPane: "workspace.deleteViewerPane",
    createTerminalPane: "workspace.createTerminalPane",
    // Lookup-or-create for ONE layout slot, in one server-side transaction.
    // Not a convenience wrapper over list + createTerminalPane: two clients
    // running that pair concurrently both see no row and both create one, which
    // is two tmux sessions for one slot. Only the server can make it atomic.
    resolveTerminalPane: "workspace.resolveTerminalPane",
    updateTerminalPane: "workspace.updateTerminalPane",
    deleteTerminalPane: "workspace.deleteTerminalPane",
    getLayout: "workspace.getLayout",
    setLayout: "workspace.setLayout",
} as const;

export type RpcWorkspaceMethodKey = keyof typeof RPC_WORKSPACE_METHODS;
export type RpcWorkspaceMethodName =
    (typeof RPC_WORKSPACE_METHODS)[RpcWorkspaceMethodKey];
