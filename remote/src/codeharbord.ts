// codeharbord (SPEC 10): the remote workspace service. Launched over SSH as
// `codeharbord rpc --stdio` and speaks newline-delimited JSON-RPC 2.0 on
// stdin/stdout. It need not run permanently.
//
// Bootstrap scope: request framing, dispatch, and two introspection methods so
// the transport can be exercised end-to-end. Workspace DB, file, watch, tmux,
// and agent methods (SPEC 10.2) land in the workstreams in docs/PLAN.md.

import readline from "node:readline";
// pathToFileURL, not string concatenation: process.argv[1] is a filesystem
// path, and a path containing a space (or any character a URL must
// percent-encode) would never compare equal to import.meta.url, silently
// turning the CLI entry point below into a no-op.
import { pathToFileURL } from "node:url";

// Frozen file-method catalog (C1, docs/PLAN.md). RPC_METHODS/RPC_REVISION_MISMATCH
// are re-exported so the wire names and error code stay linked to the transport.
// The file.* handlers (incl. listDirectory) are registered from files.ts.
import { fileMethods, fileWatchService, isRevisionMismatch } from "./files.ts";
// Workspace persistence method group (workstream P). `workspace.*` is P's own
// method group and is deliberately absent from the frozen C1 file catalog.
// serverIdentity() is this host's stable, persisted id, reported by server.info.
import { serverIdentity, WORKSPACE_METHODS } from "./workspace.ts";
// tmux session discovery (SPEC 10.2). Its own `tmux.*` method group, likewise
// outside the frozen C1 file catalog.
import { TMUX_METHODS } from "./tmux.ts";
import { RPC_REVISION_MISMATCH, RPC_WATCH_EVENT_NOTIFICATION } from "./rpc-types.ts";
export { RPC_METHODS } from "./rpc-types.ts";
export { RPC_REVISION_MISMATCH, RPC_WATCH_EVENT_NOTIFICATION };

export const RPC_SERVER_NAME = "codeharbord";
export const RPC_SERVER_VERSION = "0.1.0";
// Bumped 1 -> 2 when file.listDirectory joined the C1 catalog (SPEC 7.5).
// Bumped 2 -> 3 when the tmux.* discovery group joined the catalog (SPEC 10.2).
// Bumped 3 -> 4 when server.info gained `serverId` (SPEC 3.5).
export const RPC_SCHEMA_VERSION = 4;

export interface RpcRequest {
    jsonrpc: "2.0";
    // Absent (undefined) marks a JSON-RPC notification: no response is returned.
    id?: string | number | null;
    method: string;
    params?: unknown;
}

export interface RpcSuccess {
    jsonrpc: "2.0";
    id: string | number | null;
    result: unknown;
}

export interface RpcError {
    jsonrpc: "2.0";
    id: string | number | null;
    error: { code: number; message: string; data?: unknown };
}

export type RpcResponse = RpcSuccess | RpcError;

// JSON-RPC 2.0 reserved error codes used at bootstrap.
export const RPC_PARSE_ERROR = -32700;
export const RPC_INVALID_REQUEST = -32600;
export const RPC_METHOD_NOT_FOUND = -32601;
export const RPC_INVALID_PARAMS = -32602;
export const RPC_INTERNAL_ERROR = -32603;

type MethodHandler = (params: unknown) => unknown | Promise<unknown>;

// Static method table (SPEC 10.2 methods are added here as they land).
const methods: Record<string, MethodHandler> = {
    ping: () => ({ pong: true }),
    // `name`, `version` and `schemaVersion` are frozen; `serverId` (SPEC 3.5)
    // is the stable identity of the workspace database on THIS host, minted on
    // first read and unchanged thereafter. Clients key their view of the remote
    // workspace by it, so it is read from the DB rather than synthesized: an
    // id that changed per process or per route would orphan every stored row.
    "server.info": () => ({
        name: RPC_SERVER_NAME,
        version: RPC_SERVER_VERSION,
        schemaVersion: RPC_SCHEMA_VERSION,
        serverId: serverIdentity(),
    }),
    ...fileMethods,
    ...WORKSPACE_METHODS,
    ...TMUX_METHODS,
};

function isRpcRequest(value: unknown): value is RpcRequest {
    if (typeof value !== "object" || value === null) return false;
    const r = value as Record<string, unknown>;
    return (
        r.jsonrpc === "2.0" &&
        typeof r.method === "string" &&
        (r.id === undefined || typeof r.id === "string" || typeof r.id === "number" || r.id === null)
    );
}

/**
 * Dispatch one already-decoded request object. Pure and total: unknown methods
 * and malformed requests produce JSON-RPC error responses rather than throwing.
 *
 * Batches (a JSON array of request objects) are deliberately NOT supported: the
 * only client is the C++ CodeharbordClient, which writes exactly one request
 * per line and correlates replies by id. An array therefore fails
 * isRpcRequest() and is answered with a single Invalid Request rather than an
 * array of responses.
 */
export async function dispatch(value: unknown): Promise<RpcResponse | null> {
    if (!isRpcRequest(value)) {
        return {
            jsonrpc: "2.0",
            id: null,
            error: { code: RPC_INVALID_REQUEST, message: "Invalid Request" },
        };
    }
    const handler = methods[value.method];
    // JSON-RPC 2.0: `params`, when present, MUST be a structured value — an
    // object or an array. Every handler immediately treats it as a record, so a
    // primitive (or null) is rejected here as Invalid params instead of
    // surfacing later as a confusing internal TypeError from deep in a handler.
    const paramsOk =
        value.params === undefined ||
        (typeof value.params === "object" && value.params !== null);
    // A request with no id is a JSON-RPC notification: dispatch it for its side
    // effects and return NO response, regardless of outcome. Narrowing on
    // `value.id` here also lets the response branches below type `id` as present.
    if (value.id === undefined) {
        if (handler && paramsOk) {
            try {
                await handler(value.params);
            } catch {
                // Notifications get no response, so swallow handler errors.
            }
        }
        return null;
    }
    const id = value.id;
    if (!handler) {
        return {
            jsonrpc: "2.0",
            id,
            error: { code: RPC_METHOD_NOT_FOUND, message: `Method not found: ${value.method}` },
        };
    }
    if (!paramsOk) {
        return {
            jsonrpc: "2.0",
            id,
            error: {
                code: RPC_INVALID_PARAMS,
                message: `Invalid params for ${value.method}: expected an object or array`,
            },
        };
    }
    try {
        const result = await handler(value.params);
        // JSON-RPC 2.0 requires exactly one of `result`/`error` on a response.
        // `undefined` is dropped by JSON.stringify, which would put a response
        // carrying NEITHER member on the wire, so a handler with no payload
        // reports null instead.
        return { jsonrpc: "2.0", id, result: result === undefined ? null : result };
    } catch (err) {
        if (isRevisionMismatch(err)) {
            return {
                jsonrpc: "2.0",
                id,
                error: { code: RPC_REVISION_MISMATCH, message: err.message, data: err.data },
            };
        }
        return {
            jsonrpc: "2.0",
            id,
            error: {
                code: RPC_INTERNAL_ERROR,
                message: err instanceof Error ? err.message : String(err),
            },
        };
    }
}

/** Handle one raw JSONL request line, returning the response to serialize. */
export async function handleLine(line: string): Promise<RpcResponse | null> {
    let decoded: unknown;
    try {
        decoded = JSON.parse(line);
    } catch {
        return {
            jsonrpc: "2.0",
            id: null,
            error: { code: RPC_PARSE_ERROR, message: "Parse error" },
        };
    }
    return dispatch(decoded);
}

export function runStdio(): void {
    const rl = readline.createInterface({ input: process.stdin });
    // The client half of the SSH channel can disappear at any moment, and the
    // next write then raises EPIPE as an 'error' event on stdout. Unhandled,
    // that event terminates the process with a stack trace; swallow it and let
    // the stdin 'close' handler below perform the orderly shutdown.
    process.stdout.on("error", () => {});
    // Relay watch notifications to the client as id-less JSON-RPC messages.
    fileWatchService.onWatchEvent((event) => {
        process.stdout.write(
            `${JSON.stringify({ jsonrpc: "2.0", method: RPC_WATCH_EVENT_NOTIFICATION, params: event })}\n`,
        );
    });
    // CONCURRENCY AND ORDERING, stated plainly because the wire cannot show it:
    //
    // Each input line starts its handler IMMEDIATELY, without awaiting the
    // previous one. Responses are therefore written in COMPLETION order, not in
    // arrival order: a slow file.readFile of a large file can be answered after
    // a later, faster file.stat. JSON-RPC 2.0 explicitly allows this, and the
    // client MUST match a response to its call by the `id` field alone — never
    // by position in the stream (ch::CodeharbordClient in
    // src/remote/CodeharbordClient.cpp does exactly that: it keys its pending
    // callbacks in a QHash<int, ResponseCallback> by request id). The only
    // ordered part of the stream is the framing:
    // one JSON object per line, so a reader never has to interleave two.
    //
    // The in-flight count is deliberately NOT bounded. A bound would need real
    // backpressure (pausing stdin, or queueing lines), and getting that wrong is
    // far more damaging than the unbounded case: pausing the single SSH channel
    // also stops the watch-notification stream and any request that would let a
    // stalled client make progress, and a queue that silently drops a line
    // leaves the client waiting on a reply that will never come. The exposure is
    // small by construction: the only producer is our own client, whose pending
    // calls are bounded by user actions; handlers never wait on another request,
    // so nothing can deadlock; the per-request memory is bounded by the client's
    // own read cap (InternalUrlSchemeHandler::kMaxInlineReadBytes); and the one
    // ordering hazard that would be a real bug — two concurrent writes to the
    // SAME path racing their revision check against their rename — is already
    // serialized per path by the write locks in files.ts.
    rl.on("line", (line) => {
        // Skip separator lines without copying the line: trim() on a multi-MiB
        // base64 file.writeFile frame duplicates the whole string just to answer
        // "is this blank?". The C++ reader avoids the same copy deliberately.
        if (!/\S/.test(line)) return;
        void handleLine(line)
            .then((response) => {
                if (!response) return;
                try {
                    process.stdout.write(`${JSON.stringify(response)}\n`);
                } catch {
                    // Only a non-serializable handler payload (a reference
                    // cycle, or a BigInt) reaches here. Answer the request with
                    // an internal error so the client's pending call completes
                    // instead of waiting forever for a line that cannot be
                    // produced.
                    process.stdout.write(
                        `${JSON.stringify({
                            jsonrpc: "2.0",
                            id: response.id,
                            error: {
                                code: RPC_INTERNAL_ERROR,
                                message: "Response could not be serialized",
                            },
                        })}\n`,
                    );
                }
            })
            .catch(() => {
                // handleLine is total today. This guard exists because an
                // unhandled promise rejection is fatal by default in Node: one
                // future non-total code path would take the whole service — and
                // every live watch subscription — down with it.
                process.stdout.write(
                    `${JSON.stringify({
                        jsonrpc: "2.0",
                        id: null,
                        error: { code: RPC_INTERNAL_ERROR, message: "Internal error" },
                    })}\n`,
                );
            });
    });
    rl.on("close", () => {
        // End of stdin means the SSH channel is gone. Watch subscriptions hold
        // live fs.watch handles that keep the event loop — and therefore this
        // process — alive indefinitely, so release them and let the process
        // exit on its own once in-flight work settles.
        fileWatchService.closeAll();
    });
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
    const mode = process.argv[2];
    if (mode === "rpc") {
        runStdio();
    } else {
        process.stderr.write("usage: codeharbord rpc --stdio\n");
        process.exit(2);
    }
}
