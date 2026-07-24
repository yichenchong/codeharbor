// codeharbord (SPEC 10): the remote workspace service. Launched over SSH as
// `codeharbord rpc --stdio` and speaks newline-delimited JSON-RPC 2.0 on
// stdin/stdout. It need not run permanently.
//
// Bootstrap scope: request framing, dispatch, and two introspection methods so
// the transport can be exercised end-to-end. Workspace DB, file, watch, tmux,
// and agent methods (SPEC 10.2) land in the workstreams in docs/PLAN.md.

import readline from "node:readline";

// Frozen file-method catalog (C1, docs/PLAN.md). RPC_METHODS/RPC_REVISION_MISMATCH
// are re-exported so the wire names and error code stay linked to the transport.
// The file.* handlers (incl. listDirectory) are registered from files.ts.
import { fileMethods, fileWatchService, isRevisionMismatch } from "./files.ts";
// Workspace persistence method group (workstream P). `workspace.*` is P's own
// method group and is deliberately absent from the frozen C1 file catalog.
import { WORKSPACE_METHODS } from "./workspace.ts";
import { RPC_REVISION_MISMATCH, RPC_WATCH_EVENT_NOTIFICATION } from "./rpc-types.ts";
export { RPC_METHODS } from "./rpc-types.ts";
export { RPC_REVISION_MISMATCH, RPC_WATCH_EVENT_NOTIFICATION };

export const RPC_SERVER_NAME = "codeharbord";
export const RPC_SERVER_VERSION = "0.1.0";
// Bumped 1 -> 2 when file.listDirectory joined the C1 catalog (SPEC 7.5).
export const RPC_SCHEMA_VERSION = 2;

export interface RpcRequest {
    jsonrpc: "2.0";
    id: string | number | null;
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
export const RPC_INTERNAL_ERROR = -32603;

type MethodHandler = (params: unknown) => unknown | Promise<unknown>;

// Static method table (SPEC 10.2 methods are added here as they land).
const methods: Record<string, MethodHandler> = {
    ping: () => ({ pong: true }),
    "server.info": () => ({
        name: RPC_SERVER_NAME,
        version: RPC_SERVER_VERSION,
        schemaVersion: RPC_SCHEMA_VERSION,
    }),
    ...fileMethods,
    ...WORKSPACE_METHODS,
};

function isRpcRequest(value: unknown): value is RpcRequest {
    if (typeof value !== "object" || value === null) return false;
    const r = value as Record<string, unknown>;
    return (
        r.jsonrpc === "2.0" &&
        typeof r.method === "string" &&
        (typeof r.id === "string" || typeof r.id === "number" || r.id === null)
    );
}

/**
 * Dispatch one already-decoded request object. Pure and total: unknown methods
 * and malformed requests produce JSON-RPC error responses rather than throwing.
 */
export async function dispatch(value: unknown): Promise<RpcResponse> {
    if (!isRpcRequest(value)) {
        return {
            jsonrpc: "2.0",
            id: null,
            error: { code: RPC_INVALID_REQUEST, message: "Invalid Request" },
        };
    }
    const handler = methods[value.method];
    if (!handler) {
        return {
            jsonrpc: "2.0",
            id: value.id,
            error: { code: RPC_METHOD_NOT_FOUND, message: `Method not found: ${value.method}` },
        };
    }
    try {
        const result = await handler(value.params);
        return { jsonrpc: "2.0", id: value.id, result };
    } catch (err) {
        if (isRevisionMismatch(err)) {
            return {
                jsonrpc: "2.0",
                id: value.id,
                error: { code: RPC_REVISION_MISMATCH, message: err.message, data: err.data },
            };
        }
        return {
            jsonrpc: "2.0",
            id: value.id,
            error: {
                code: RPC_INTERNAL_ERROR,
                message: err instanceof Error ? err.message : String(err),
            },
        };
    }
}

/** Handle one raw JSONL request line, returning the response to serialize. */
export async function handleLine(line: string): Promise<RpcResponse> {
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
    // Relay watch notifications to the client as id-less JSON-RPC messages.
    fileWatchService.onWatchEvent((event) => {
        process.stdout.write(
            `${JSON.stringify({ jsonrpc: "2.0", method: RPC_WATCH_EVENT_NOTIFICATION, params: event })}\n`,
        );
    });
    rl.on("line", (line) => {
        if (line.trim().length === 0) return;
        void handleLine(line).then((response) => {
            process.stdout.write(`${JSON.stringify(response)}\n`);
        });
    });
}

if (import.meta.url === `file://${process.argv[1]}`) {
    const mode = process.argv[2];
    if (mode === "rpc") {
        runStdio();
    } else {
        process.stderr.write("usage: codeharbord rpc --stdio\n");
        process.exit(2);
    }
}
