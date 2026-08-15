// Producer side of the viewer control channel: the half an agent's tooling runs.
//
// Both agent-facing entry points are thin wrappers over this module — the MCP
// server (remote/src/mcp/server.ts) and the codeharbor-view CLI
// (remote/src/tools/viewctl.ts) — so the socket path, the coordinate lookup, the
// timeout and the never-throw contract exist once.
//
// NEVER REJECTS. Every failure — no socket, refused connection, no answer,
// garbage on the wire — resolves to a well-formed ControlResponse carrying a
// refusal. An agent's tool call must come back with a reason it can act on; a
// thrown exception inside a harness is just an opaque tool crash, and SPEC 6.4's
// rule for the status producers ("never block or break the agent") is the same
// rule here.

import net from "node:net";

import {
    CH_CONTROL_VERSION,
    CONTROL_ERROR_CODES,
    CONTROL_REQUEST_TIMEOUT_MS,
    MAX_CONTROL_LINE_BYTES,
    resolveControlSocketPath,
    type ControlErrorCode,
    type ControlOp,
    type ControlResponse,
} from "./control.ts";

/**
 * How long the producer waits for an answer.
 *
 * Deliberately LONGER than the daemon's own CONTROL_REQUEST_TIMEOUT_MS, so when
 * the desktop is merely slow the daemon's specific `timeout` refusal wins the
 * race and the agent is told what actually happened. This bound only fires when
 * the daemon itself is wedged or gone.
 */
export const CONTROL_CLIENT_TIMEOUT_MS = CONTROL_REQUEST_TIMEOUT_MS + 1000;

/**
 * The terminal pane an agent is running in, as CodeHarbor exported it into the
 * pane's tmux session (TerminalController::tmuxNewSessionCommand).
 *
 * These are the same two names every harness hook already reads, and they are
 * inherited by anything the agent starts, which is why the MCP server sees them
 * without CodeHarbor having to know that harness exists. Null when either is
 * missing: an event or command with a blank id is structurally valid and
 * unroutable, which is worse than none.
 */
export function readCoordinates(
    env: NodeJS.ProcessEnv = process.env,
): { devSessionId: string; terminalId: string } | null {
    const devSessionId = (env.OMP_DEV_SESSION_ID ?? "").trim();
    const terminalId = (env.OMP_TERMINAL_ID ?? "").trim();
    if (devSessionId.length === 0 || terminalId.length === 0) return null;
    return { devSessionId, terminalId };
}

/** The message shown when the coordinates are missing, said once. */
export const MISSING_COORDINATES_MESSAGE =
    "OMP_DEV_SESSION_ID and OMP_TERMINAL_ID are not set, so this process cannot " +
    "say which CodeHarbor pane it belongs to. Start the agent inside a terminal " +
    "pane that CodeHarbor created (an existing tmux session predating CodeHarbor " +
    "does not inherit them).";

function refusal(code: ControlErrorCode, message: string): ControlResponse {
    return { version: CH_CONTROL_VERSION, ok: false, error: { code, message } };
}

function parseResponse(line: string): ControlResponse {
    let decoded: unknown;
    try {
        decoded = JSON.parse(line);
    } catch {
        return refusal("failed", `the server answered with a non-JSON line: ${line.slice(0, 200)}`);
    }
    if (typeof decoded !== "object" || decoded === null || Array.isArray(decoded)) {
        return refusal("failed", "the server answered with something other than an object");
    }
    const value = decoded as Record<string, unknown>;
    if (typeof value.ok !== "boolean") {
        return refusal("failed", "the server's answer carried no ok flag");
    }
    if (value.ok) {
        const data =
            typeof value.data === "object" && value.data !== null && !Array.isArray(value.data)
                ? (value.data as Record<string, unknown>)
                : undefined;
        return data
            ? { version: CH_CONTROL_VERSION, ok: true, data }
            : { version: CH_CONTROL_VERSION, ok: true };
    }
    const rawError = value.error;
    const error =
        typeof rawError === "object" && rawError !== null && !Array.isArray(rawError)
            ? (rawError as Record<string, unknown>)
            : {};
    const code = typeof error.code === "string" ? error.code : "";
    return refusal(
        (CONTROL_ERROR_CODES as readonly string[]).includes(code)
            ? (code as ControlErrorCode)
            : "failed",
        typeof error.message === "string" && error.message.length > 0
            ? error.message
            : "the viewer command failed",
    );
}

/**
 * Send one viewer command and resolve with the answer.
 *
 * One connection per request, closed by the server after its single reply —
 * which is what lets a refusal be delivered on the same socket without any
 * correlation state on this side.
 */
export function sendControlRequest(
    op: ControlOp,
    args: Record<string, unknown> = {},
    env: NodeJS.ProcessEnv = process.env,
    socketPath: string = resolveControlSocketPath(env),
    timeoutMs: number = CONTROL_CLIENT_TIMEOUT_MS,
): Promise<ControlResponse> {
    const coordinates = readCoordinates(env);
    if (!coordinates) {
        return Promise.resolve(refusal("bad_request", MISSING_COORDINATES_MESSAGE));
    }

    return new Promise<ControlResponse>((resolve) => {
        let settled = false;
        const finish = (response: ControlResponse): void => {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            socket.destroy();
            resolve(response);
        };

        const timer = setTimeout(() => {
            finish(
                refusal(
                    "timeout",
                    `no answer from the CodeHarbor server at ${socketPath} within ${timeoutMs} ms`,
                ),
            );
        }, timeoutMs);

        const socket = net.createConnection(socketPath);
        socket.setEncoding("utf8");

        socket.on("error", (err: Error) => {
            // ENOENT/ECONNREFUSED is the ordinary "CodeHarbor is not connected to
            // this server right now" case, and it is worth naming precisely: an
            // agent otherwise cannot tell it from a command that was refused.
            finish(
                refusal(
                    "failed",
                    `cannot reach the CodeHarbor server at ${socketPath}: ${err.message}`,
                ),
            );
        });

        socket.on("connect", () => {
            const payload = `${JSON.stringify({
                version: CH_CONTROL_VERSION,
                devSessionId: coordinates.devSessionId,
                terminalId: coordinates.terminalId,
                op,
                args,
            })}\n`;
            if (Buffer.byteLength(payload) > MAX_CONTROL_LINE_BYTES) {
                finish(
                    refusal(
                        "bad_request",
                        `the request is larger than the ${MAX_CONTROL_LINE_BYTES}-byte limit`,
                    ),
                );
                return;
            }
            socket.write(payload);
        });

        let buffer = "";
        socket.on("data", (chunk: string) => {
            buffer += chunk;
            const newline = buffer.indexOf("\n");
            if (newline < 0) {
                // The server writes exactly one bounded line; anything longer is
                // a peer this side should not keep buffering for.
                if (Buffer.byteLength(buffer) > MAX_CONTROL_LINE_BYTES) {
                    finish(refusal("failed", "the server's answer had no newline within its bound"));
                }
                return;
            }
            finish(parseResponse(buffer.slice(0, newline)));
        });

        socket.on("close", () => {
            // Closed without a complete line: the daemon died mid-command, or the
            // command was accepted by a daemon whose stdout had already failed.
            finish(refusal("failed", "the CodeHarbor server closed the connection without answering"));
        });
    });
}
