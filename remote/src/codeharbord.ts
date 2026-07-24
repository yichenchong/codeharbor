// codeharbord (SPEC 10): the remote workspace service. Launched over SSH as
// `codeharbord rpc --stdio` and speaks newline-delimited JSON-RPC 2.0 on
// stdin/stdout. It need not run permanently.
//
// Bootstrap scope: request framing, dispatch, and two introspection methods so
// the transport can be exercised end-to-end. Workspace DB, file, watch, tmux,
// and agent methods (SPEC 10.2) land in the workstreams in docs/PLAN.md.

import readline from "node:readline";

export const RPC_SERVER_NAME = "codeharbord";
export const RPC_SERVER_VERSION = "0.1.0";
export const RPC_SCHEMA_VERSION = 1;

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

type MethodHandler = (params: unknown) => unknown;

// Static method table (SPEC 10.2 methods are added here as they land).
const methods: Record<string, MethodHandler> = {
    ping: () => ({ pong: true }),
    "server.info": () => ({
        name: RPC_SERVER_NAME,
        version: RPC_SERVER_VERSION,
        schemaVersion: RPC_SCHEMA_VERSION,
    }),
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
export function dispatch(value: unknown): RpcResponse {
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
    return { jsonrpc: "2.0", id: value.id, result: handler(value.params) };
}

/** Handle one raw JSONL request line, returning the response to serialize. */
export function handleLine(line: string): RpcResponse {
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
    rl.on("line", (line) => {
        if (line.trim().length === 0) return;
        process.stdout.write(`${JSON.stringify(handleLine(line))}\n`);
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
