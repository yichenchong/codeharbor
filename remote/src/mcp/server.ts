// codeharbor-mcp: a Model Context Protocol server exposing CodeHarbor's viewer
// panes as tools, over stdio.
//
// This is the surface an AI harness should use. Claude Code, Codex and Oh My Pi
// all speak MCP, so one server covers all three, and it is strictly better than
// teaching each of them to run a shell command:
//
//   * typed tools with schemas, so a model does not have to get shell quoting
//     right to open a file;
//   * Codex launches a local stdio MCP server OUTSIDE its command sandbox, while
//     a sandboxed shell command may be denied the Unix socket entirely;
//   * tool results are structured, so a refusal reaches the model as data rather
//     than as text it has to parse out of stderr.
//
// HAND-ROLLED ON PURPOSE. MCP over stdio is newline-delimited JSON-RPC 2.0, and
// this repository's remote package has ZERO runtime dependencies because it is
// executed from a checkout over SSH — adding an SDK would mean an npm install on
// every server anyone connects to. The framing is the daemon's own
// createLineFramer, so the two transports cannot disagree about what a line is.
//
// It never throws out of a request: an unhandled rejection would take the server
// down mid-conversation and the harness would report only that its tool provider
// died.

import { pathToFileURL } from "node:url";

import { createLineFramer, RPC_SERVER_VERSION } from "../codeharbord.ts";
import { CONTROL_OPS, type ControlOp } from "../control.ts";
import {
    MISSING_COORDINATES_MESSAGE,
    readCoordinates,
    sendControlRequest,
} from "../control-client.ts";

// The MCP revision this server implements. Reported verbatim in the initialize
// result; a client that speaks a different one is expected to adapt or to say so
// itself, which is what the field is for.
export const MCP_PROTOCOL_VERSION = "2025-06-18";

export const MCP_SERVER_NAME = "codeharbor";

// One MCP message may not exceed this. A viewer tool call is a handful of short
// strings, so the bound is a framing guard rather than a real limit; it exists
// because an unbounded line buffer is how a stdio server gets killed by a peer
// that lost a newline.
export const MAX_MCP_LINE_BYTES = 1024 * 1024;

// JSON-RPC 2.0 reserved codes, spelled here rather than imported from the RPC
// contract: this is a DIFFERENT protocol that happens to share an envelope, and
// coupling the two would invite a future change to one to silently alter the
// other.
const MCP_INVALID_REQUEST = -32600;
const MCP_METHOD_NOT_FOUND = -32601;
const MCP_INVALID_PARAMS = -32602;
const MCP_INTERNAL_ERROR = -32603;

interface ToolSpec {
    name: string;
    op: ControlOp;
    description: string;
    inputSchema: Record<string, unknown>;
}

const PANE_DESCRIPTION =
    "Viewer pane id, as reported by viewer_list (for example \"viewer-1\"). " +
    "Omit to act on the focused viewer pane.";

const URL_DESCRIPTION =
    "What to show: an http(s):// address, a file:// URL naming a file on the " +
    "CodeHarbor server, or a remote path. A relative path resolves against the " +
    "Dev Session's repository root; a trailing slash asks for the directory " +
    "listing rather than the file.";

// The tool catalog. `op` ties each tool to the control-channel operation it
// performs, so there is no second dispatch table to keep in step.
export const TOOLS: readonly ToolSpec[] = [
    {
        name: "viewer_list",
        op: "list",
        description:
            "List the CodeHarbor viewer panes of the Dev Session this terminal belongs to, " +
            "with the address each is showing and which one has focus. Call this first when " +
            "you need a pane id.",
        inputSchema: { type: "object", properties: {}, additionalProperties: false },
    },
    {
        name: "viewer_open",
        op: "open",
        description:
            "Show a file, directory, or web page in a CodeHarbor viewer pane. Use this to put " +
            "something in front of the user rather than pasting it into the terminal.",
        inputSchema: {
            type: "object",
            properties: {
                url: { type: "string", description: URL_DESCRIPTION },
                pane: { type: "string", description: PANE_DESCRIPTION },
                newPane: {
                    type: "boolean",
                    description:
                        "Split the target pane and show the address in the new pane instead of " +
                        "replacing what the target is showing.",
                },
                kind: {
                    type: "string",
                    description:
                        "Force a specific handler instead of letting CodeHarbor choose " +
                        "(the \"Open as\" vocabulary, e.g. an editor rather than the rendered view). " +
                        "Omit unless the user asked for a particular one.",
                },
            },
            required: ["url"],
            additionalProperties: false,
        },
    },
    {
        name: "viewer_close",
        op: "close",
        description:
            "Close a CodeHarbor viewer pane. Unsaved editor changes in that pane are NOT " +
            "confirmed first, so ask the user before closing a pane you did not open.",
        inputSchema: {
            type: "object",
            properties: { pane: { type: "string", description: PANE_DESCRIPTION } },
            additionalProperties: false,
        },
    },
    {
        name: "viewer_split",
        op: "split",
        description:
            "Split a CodeHarbor viewer pane, leaving both halves empty of new content. " +
            "Returns the new pane's id. Prefer viewer_open with newPane when you already " +
            "know what to show.",
        inputSchema: {
            type: "object",
            properties: {
                orientation: {
                    type: "string",
                    enum: ["horizontal", "vertical"],
                    description:
                        "horizontal puts the new pane beside the target; vertical puts it below.",
                },
                pane: { type: "string", description: PANE_DESCRIPTION },
            },
            required: ["orientation"],
            additionalProperties: false,
        },
    },
    {
        name: "viewer_focus",
        op: "focus",
        description: "Give keyboard focus to a CodeHarbor viewer pane and make it the active one.",
        inputSchema: {
            type: "object",
            properties: { pane: { type: "string", description: PANE_DESCRIPTION } },
            required: ["pane"],
            additionalProperties: false,
        },
    },
    {
        name: "viewer_reload",
        op: "reload",
        description:
            "Re-fetch what a CodeHarbor viewer pane is showing. Use it after writing a file " +
            "the user already has open, when the pane has not picked the change up.",
        inputSchema: {
            type: "object",
            properties: { pane: { type: "string", description: PANE_DESCRIPTION } },
            additionalProperties: false,
        },
    },
];

function toolByName(name: unknown): ToolSpec | undefined {
    if (typeof name !== "string") return undefined;
    return TOOLS.find((tool) => tool.name === name);
}

/** A tool result carrying text. `isError` is how MCP reports a failed call. */
function textResult(text: string, isError: boolean): Record<string, unknown> {
    return { content: [{ type: "text", text }], isError };
}

export interface McpResponse {
    jsonrpc: "2.0";
    id: string | number | null;
    result?: unknown;
    error?: { code: number; message: string };
}

function success(id: string | number | null, result: unknown): McpResponse {
    return { jsonrpc: "2.0", id, result };
}

function failure(id: string | number | null, code: number, message: string): McpResponse {
    return { jsonrpc: "2.0", id, error: { code, message } };
}

/**
 * Handle one decoded MCP message. Resolves with the response to write, or null
 * for a notification (which by JSON-RPC rule gets no reply).
 *
 * `env` is threaded through rather than read from process.env so a test can drive
 * the whole server without touching the ambient environment.
 */
export async function handleMessage(
    value: unknown,
    env: NodeJS.ProcessEnv = process.env,
): Promise<McpResponse | null> {
    if (typeof value !== "object" || value === null || Array.isArray(value)) {
        return failure(null, MCP_INVALID_REQUEST, "Invalid Request");
    }
    const message = value as Record<string, unknown>;
    if (message.jsonrpc !== "2.0" || typeof message.method !== "string") {
        return failure(null, MCP_INVALID_REQUEST, "Invalid Request");
    }
    const id =
        typeof message.id === "string" || typeof message.id === "number" ? message.id : null;
    const isNotification = message.id === undefined;

    switch (message.method) {
        case "initialize":
            return isNotification
                ? null
                : success(id, {
                      protocolVersion: MCP_PROTOCOL_VERSION,
                      capabilities: { tools: {} },
                      serverInfo: { name: MCP_SERVER_NAME, version: RPC_SERVER_VERSION },
                      instructions:
                          "Use these tools to show things in the user's CodeHarbor viewer panes " +
                          "instead of dumping file contents into the terminal. Call viewer_list " +
                          "when you need a pane id.",
                  });
        // Lifecycle notifications. Answered with nothing, and NOT with
        // "method not found": a client that gets an error back for its own
        // handshake notification reports the server as broken.
        case "notifications/initialized":
        case "notifications/cancelled":
        case "notifications/roots/list_changed":
            return null;
        case "ping":
            return isNotification ? null : success(id, {});
        case "tools/list":
            // The tool LIST is served even without pane coordinates, so `/mcp`
            // shows the server as healthy and the model can see what exists; a
            // call then explains precisely what is missing.
            return isNotification
                ? null
                : success(id, {
                      tools: TOOLS.map((tool) => ({
                          name: tool.name,
                          description: tool.description,
                          inputSchema: tool.inputSchema,
                      })),
                  });
        case "tools/call": {
            if (isNotification) return null;
            const params =
                typeof message.params === "object" &&
                message.params !== null &&
                !Array.isArray(message.params)
                    ? (message.params as Record<string, unknown>)
                    : {};
            const tool = toolByName(params.name);
            if (!tool) {
                return failure(id, MCP_INVALID_PARAMS, `Unknown tool: ${String(params.name)}`);
            }
            const args =
                typeof params.arguments === "object" &&
                params.arguments !== null &&
                !Array.isArray(params.arguments)
                    ? (params.arguments as Record<string, unknown>)
                    : {};
            if (!readCoordinates(env)) {
                return success(id, textResult(MISSING_COORDINATES_MESSAGE, true));
            }
            // A tool FAILURE is a successful JSON-RPC response carrying
            // isError — that is the MCP contract, and it is what lets the model
            // read the reason and try something else instead of seeing its tool
            // provider error out.
            const response = await sendControlRequest(tool.op, args, env);
            if (response.ok) {
                return success(id, textResult(JSON.stringify(response.data ?? {}), false));
            }
            return success(
                id,
                textResult(
                    `${response.error?.code ?? "failed"}: ${response.error?.message ?? "the viewer command failed"}`,
                    true,
                ),
            );
        }
        default:
            return isNotification
                ? null
                : failure(id, MCP_METHOD_NOT_FOUND, `Method not found: ${message.method}`);
    }
}

/** Decode and handle one input line. Blank lines are ignored. */
export async function handleLine(
    line: string,
    env: NodeJS.ProcessEnv = process.env,
): Promise<McpResponse | null> {
    if (!/\S/.test(line)) return null;
    let decoded: unknown;
    try {
        decoded = JSON.parse(line);
    } catch {
        return failure(null, MCP_INVALID_REQUEST, "Parse error");
    }
    try {
        return await handleMessage(decoded, env);
    } catch (err) {
        // handleMessage is total today; this guard keeps a future non-total path
        // from becoming an unhandled rejection that kills the server.
        return failure(
            null,
            MCP_INTERNAL_ERROR,
            err instanceof Error ? err.message : String(err),
        );
    }
}

export function runStdio(): void {
    process.stdout.on("error", () => {});
    process.stdin.on("error", () => {});
    // The control ops and the tool table are two spellings of one list; a tool
    // added without its op (or the reverse) would fail only when a model happened
    // to call it. Fail loudly at startup instead.
    for (const tool of TOOLS) {
        if (!(CONTROL_OPS as readonly string[]).includes(tool.op)) {
            process.stderr.write(`codeharbor-mcp: tool ${tool.name} names unknown op ${tool.op}\n`);
            process.exitCode = 2;
            return;
        }
    }
    // Requests are answered as they COMPLETE, not in arrival order: a tool call
    // waits on the desktop while a `tools/list` beside it answers instantly. The
    // client correlates by id, exactly as JSON-RPC allows.
    //
    // In-flight work is COUNTED because stdin closing is the shutdown signal, and
    // a client that writes its requests and closes the pipe (every scripted use,
    // and the harnesses that batch a turn) would otherwise have its answers
    // discarded: exiting on 'close' killed the process while the control-socket
    // round trip was still outstanding, so the tool calls silently produced
    // nothing at all.
    let inFlight = 0;
    let inputClosed = false;
    const finishIfDone = (): void => {
        if (inputClosed && inFlight === 0) process.exit(process.exitCode ?? 0);
    };
    const feed = createLineFramer(
        (line) => {
            inFlight += 1;
            void handleLine(line).then(
                (response) => {
                    inFlight -= 1;
                    if (response) {
                        try {
                            process.stdout.write(`${JSON.stringify(response)}\n`);
                        } catch {
                            // The peer is gone; nothing left to answer.
                        }
                    }
                    finishIfDone();
                },
                () => {
                    // handleLine is total; this arm exists so a future
                    // non-total path cannot leak the counter and hang the exit.
                    inFlight -= 1;
                    finishIfDone();
                },
            );
        },
        () => {
            process.stderr.write(
                `codeharbor-mcp: input line exceeded ${MAX_MCP_LINE_BYTES} bytes without a newline; closing\n`,
            );
            process.stdin.destroy();
        },
        MAX_MCP_LINE_BYTES,
    );
    process.stdin.on("data", feed);
    process.stdin.on("close", () => {
        inputClosed = true;
        finishIfDone();
    });
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
    runStdio();
}
