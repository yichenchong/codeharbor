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
    truncated: boolean;
}

export interface WriteFileParams {
    path: string;
    content: string;
    encoding?: "utf-8" | "base64";
    expectedRevision: string;
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

// Stable wire method names for the initial file set (SPEC 8.3). Handlers are
// added in the R-server workstream; these string values are the frozen contract.
export const RPC_METHODS = {
    stat: "file.stat",
    readFile: "file.readFile",
    writeFile: "file.writeFile",
    resolvePath: "file.resolvePath",
    watch: "file.watch",
    unwatch: "file.unwatch",
} as const;

export type RpcMethodKey = keyof typeof RPC_METHODS;
export type RpcMethodName = (typeof RPC_METHODS)[RpcMethodKey];

// Implementation-defined server error code (JSON-RPC 2.0 reserves -32000..-32099
// for such errors) for a writeFile whose expectedRevision no longer matches the
// file's current revision. The server rejects the write rather than silently
// overwriting concurrent changes (SPEC 8.4 / 8.6).
export const RPC_REVISION_MISMATCH = -32001;
