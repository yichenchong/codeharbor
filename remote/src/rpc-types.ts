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
// is known for the affected path (created/modified).
export interface WatchEvent {
    subscriptionId: string;
    path: string;
    event: "created" | "modified" | "deleted" | "renamed";
    revision?: string;
}

// Directory listing (SPEC 7.5 / 8.3). Added to the catalog for the viewer
// workstream's directory viewer; entries are unordered (the client sorts).
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

// Server -> client notification method name for an active watch subscription
// (SPEC 8.7). This is a NOTIFICATION name (no id, no response), deliberately
// absent from RPC_METHODS, which enumerates only request methods. It
// lives here in the contract so the wire name stays shared between the server
// (codeharbord.ts) and the C++ client (RpcTypes.h).
export const RPC_WATCH_EVENT_NOTIFICATION = "file.watchEvent";

// Implementation-defined server error code (JSON-RPC 2.0 reserves -32000..-32099
// for such errors) for a writeFile whose expectedRevision no longer matches the
// file's current revision. The server rejects the write rather than silently
// overwriting concurrent changes (SPEC 8.4 / 8.6).
export const RPC_REVISION_MISMATCH = -32001;

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
    updateTerminalPane: "workspace.updateTerminalPane",
    deleteTerminalPane: "workspace.deleteTerminalPane",
    getLayout: "workspace.getLayout",
    setLayout: "workspace.setLayout",
} as const;

export type RpcWorkspaceMethodKey = keyof typeof RPC_WORKSPACE_METHODS;
export type RpcWorkspaceMethodName =
    (typeof RPC_WORKSPACE_METHODS)[RpcWorkspaceMethodKey];
