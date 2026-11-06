// The MCP server: the surface Claude Code, Codex and Oh My Pi actually call.
//
// Driven through handleLine(), i.e. the same entry point the stdio loop feeds, so
// the framing-to-answer path is exercised rather than the tool table alone. The
// tool NAMES and the initialize shape are the agent-visible contract: renaming a
// tool silently breaks every skill that documents it, and a malformed initialize
// result makes a harness report the whole server as dead.

import { test } from "node:test";
import assert from "node:assert/strict";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { mkdtempSync, rmSync, symlinkSync } from "node:fs";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { once } from "node:events";

import {
    CONTROL_SOCKET_ENV,
    mintControlSocketPath,
    startControlListener,
} from "../src/control.ts";
import {
    MCP_PROTOCOL_VERSION,
    MCP_SERVER_NAME,
    TOOLS,
    handleLine,
    createMcpSession,
    MAX_MCP_INFLIGHT,
    MAX_MCP_LINE_BYTES,
    MAX_MCP_PENDING_OUTPUT_BYTES,
} from "../src/mcp/server.ts";
import { RPC_SERVER_VERSION } from "../src/codeharbord.ts";

const NO_ENV: NodeJS.ProcessEnv = { HOME: "/home/nobody" };

function request(id: number, method: string, params?: unknown): string {
    return JSON.stringify(params === undefined
        ? { jsonrpc: "2.0", id, method }
        : { jsonrpc: "2.0", id, method, params });
}

/** The text of a tools/call result, with its error flag. */
function toolText(result: unknown): { text: string; isError: boolean } {
    assert.ok(result && typeof result === "object" && "content" in result, "no content in result");
    const content = result.content;
    assert.ok(Array.isArray(content) && content.length === 1, "expected exactly one content block");
    const block: unknown = content[0];
    assert.ok(block && typeof block === "object" && "text" in block, "content block has no text");
    const text = block.text;
    assert.equal(typeof text, "string");
    const isError = "isError" in result ? result.isError : false;
    assert.equal(typeof isError, "boolean");
    return { text: String(text), isError: isError === true };
}

test("initialize reports the protocol, the tool capability and the server version", async () => {
    const response = await handleLine(
        request(1, "initialize", {
            protocolVersion: MCP_PROTOCOL_VERSION,
            capabilities: {},
            clientInfo: { name: "test", version: "0" },
        }),
        NO_ENV,
    );
    assert.ok(response);
    assert.equal(response.id, 1);
    assert.deepEqual(response.error, undefined);
    const result = response.result;
    assert.ok(result && typeof result === "object");
    assert.ok("protocolVersion" in result && result.protocolVersion === MCP_PROTOCOL_VERSION);
    assert.ok("capabilities" in result);
    assert.deepEqual(result.capabilities, { tools: {} });
    assert.ok("serverInfo" in result);
    // The version is the remote package's own, so a harness's server list names
    // the release the user is actually running.
    assert.deepEqual(result.serverInfo, { name: MCP_SERVER_NAME, version: RPC_SERVER_VERSION });
});

test("the handshake notification is answered with nothing at all", async () => {
    // An error response to a client's own lifecycle notification makes the
    // harness report the server as broken before a single tool is listed.
    assert.equal(
        await handleLine(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }), NO_ENV),
        null,
    );
    assert.equal(
        await handleLine(JSON.stringify({ jsonrpc: "2.0", method: "notifications/cancelled" }), NO_ENV),
        null,
    );
});

test("tools/list serves the six viewer tools with their schemas", async () => {
    const response = await handleLine(request(2, "tools/list"), NO_ENV);
    assert.ok(response);
    const result = response.result;
    assert.ok(result && typeof result === "object" && "tools" in result);
    const tools = result.tools;
    assert.ok(Array.isArray(tools));
    // EXACT names, spelled out: every skill file documents these strings, so a
    // rename here silently breaks all three harness integrations.
    assert.deepEqual(
        tools.map((tool) => (tool && typeof tool === "object" && "name" in tool ? tool.name : null)),
        [
            "viewer_list",
            "viewer_open",
            "viewer_close",
            "viewer_split",
            "viewer_focus",
            "viewer_reload",
        ],
    );
    for (const tool of tools) {
        assert.ok(tool && typeof tool === "object");
        assert.ok("description" in tool && typeof tool.description === "string" && tool.description.length > 0);
        assert.ok("inputSchema" in tool && tool.inputSchema && typeof tool.inputSchema === "object");
    }
});

test("tools/list is served even with no pane coordinates", async () => {
    // The server must look healthy in `/mcp` so the model can see what exists;
    // the CALL is where the missing environment is explained.
    const response = await handleLine(request(3, "tools/list"), NO_ENV);
    assert.ok(response);
    assert.equal(response.error, undefined);
});

test("a tool call without pane coordinates explains what is missing", async () => {
    const response = await handleLine(
        request(4, "tools/call", { name: "viewer_list", arguments: {} }),
        NO_ENV,
    );
    assert.ok(response);
    // A tool FAILURE is a successful JSON-RPC response carrying isError: that is
    // what lets the model read the reason instead of seeing its tool provider
    // crash.
    assert.equal(response.error, undefined);
    const { text, isError } = toolText(response.result);
    assert.equal(isError, true);
    assert.match(text, /OMP_DEV_SESSION_ID/);
    assert.match(text, /OMP_TERMINAL_ID/);
});

test("an unknown tool is an invalid-params error, not a silent success", async () => {
    const response = await handleLine(
        request(5, "tools/call", { name: "viewer_teleport", arguments: {} }),
        NO_ENV,
    );
    assert.ok(response);
    assert.equal(response.error?.code, -32602);
    assert.match(response.error?.message ?? "", /viewer_teleport/);
});

test("malformed tool names are invalid params with the caller's id (TIN2)", async () => {
    for (const name of [undefined, null, false, 42, [], {}, { toString: null }]) {
        const response = await handleLine(
            request(53, "tools/call", { name }),
            NO_ENV,
        );
        assert.equal(response?.id, 53);
        assert.equal(response?.error?.code, -32602);
        assert.equal(response?.result, undefined);
    }
    // A bad call must not poison the rest of the MCP conversation.
    assert.deepEqual((await handleLine(request(54, "ping"), NO_ENV))?.result, {});
});

test("an unknown method is method-not-found", async () => {
    const response = await handleLine(request(6, "resources/list"), NO_ENV);
    assert.ok(response);
    assert.equal(response.error?.code, -32601);
});

test("a non-JSON line is a parse error rather than a crash", async () => {
    const response = await handleLine("{not json", NO_ENV);
    assert.ok(response);
    assert.equal(response.error?.code, -32700);
});

test("a blank line is ignored", async () => {
    assert.equal(await handleLine("   \n", NO_ENV), null);
});

test("ping answers, so a harness health check succeeds", async () => {
    const response = await handleLine(request(7, "ping"), NO_ENV);
    assert.ok(response);
    assert.deepEqual(response.result, {});
});

test("malformed request ids are refused before tool dispatch", async () => {
    for (const id of [null, 1.5, true, false, {}, [], 1e400]) {
        const line = id === Infinity
            ? '{"jsonrpc":"2.0","id":1e400,"method":"tools/call","params":{"name":"viewer_close"}}'
            : JSON.stringify({ jsonrpc: "2.0", id, method: "tools/call", params: { name: "viewer_close" } });
        const response = await handleLine(line, NO_ENV);
        assert.equal(response?.error?.code, -32600);
        assert.equal(response?.id, null);
    }
});

test("tool arguments are checked against the advertised schema before dispatch", async () => {
    const cases: { name: string; arguments?: unknown }[] = [
        { name: "viewer_close", arguments: [] },
        { name: "viewer_close", arguments: null },
        { name: "viewer_close", arguments: "viewer-1" },
        { name: "viewer_close", arguments: { pane: 1 } },
        { name: "viewer_close", arguments: { panes: "viewer-1" } },
        { name: "viewer_open" },
        { name: "viewer_open", arguments: { url: "README.md", newPane: "false" } },
        { name: "viewer_open", arguments: { url: false } },
        { name: "viewer_focus", arguments: {} },
        { name: "viewer_split", arguments: { orientation: "diagonal" } },
    ];
    for (const params of cases) {
        const response = await handleLine(request(42, "tools/call", params), NO_ENV);
        assert.equal(response?.error?.code, -32602, JSON.stringify(params));
        assert.equal(response?.id, 42);
    }
});

test("non-object envelope params are invalid even for parameterless methods", async () => {
    for (const params of [null, true, 42, "hello", []]) {
        const response = await handleLine(request(42, "ping", params), NO_ENV);
        assert.equal(response?.error?.code, -32602);
        assert.equal(response?.id, 42);
    }
});

test("requests using notification-only methods are refused instead of left unanswered", async () => {
    for (const method of [
        "notifications/initialized", "notifications/cancelled", "notifications/roots/list_changed",
    ]) {
        const response = await handleLine(request(42, method), NO_ENV);
        assert.equal(response?.id, 42);
        assert.equal(response?.error?.code, -32600);
    }
});

test("a tool call reaches the control socket and returns the desktop's data", async () => {
    // The whole agent-facing path: MCP tool call -> control client -> socket ->
    // listener -> relay, then the desktop's answer back out as tool text.
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-mcp-"));
    try {
        // The MCP server takes the socket from the ENVIRONMENT (a tool call
        // carries no path) and refuses when it is absent, so the pane env must
        // carry the very path this listener bound — which is what the client
        // exports into each pane of ITS window.
        const socketPath = mintControlSocketPath({ XDG_RUNTIME_DIR: dir, HOME: dir });
        const env: NodeJS.ProcessEnv = {
            OMP_DEV_SESSION_ID: "dev-1",
            OMP_TERMINAL_ID: "term-1",
            [CONTROL_SOCKET_ENV]: socketPath,
            XDG_RUNTIME_DIR: dir,
            HOME: dir,
        };
        let relayedId = "";
        const listener = await startControlListener((command) => {
            relayedId = command.commandId;
            // Answer as the desktop would, on the next turn, so the producer is
            // genuinely waiting rather than being settled inline.
            queueMicrotask(() => {
                listener.settle({
                    commandId: command.commandId,
                    ok: true,
                    data: { paneId: "viewer-2", url: "file:///srv/README.md" },
                });
            });
            return true;
        }, socketPath);
        try {
            const response = await handleLine(
                request(8, "tools/call", {
                    name: "viewer_open",
                    arguments: { url: "README.md", newPane: true },
                }),
                env,
            );
            assert.ok(response);
            const { text, isError } = toolText(response.result);
            assert.equal(isError, false);
            assert.deepEqual(JSON.parse(text), {
                paneId: "viewer-2",
                url: "file:///srv/README.md",
            });
            assert.equal(relayedId, "vc-1");
        } finally {
            listener.close();
        }
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("a refused command becomes an isError tool result carrying the code", async () => {
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-mcp-"));
    try {
        const socketPath = mintControlSocketPath({ XDG_RUNTIME_DIR: dir, HOME: dir });
        const env: NodeJS.ProcessEnv = {
            OMP_DEV_SESSION_ID: "dev-other",
            OMP_TERMINAL_ID: "term-1",
            [CONTROL_SOCKET_ENV]: socketPath,
            XDG_RUNTIME_DIR: dir,
            HOME: dir,
        };
        let refusalCode = "not_active_session";
        const listener = await startControlListener((command) => {
            queueMicrotask(() => {
                listener.settle({
                    commandId: command.commandId,
                    ok: false,
                    error: {
                        code: refusalCode,
                        message: refusalCode === "unsaved_changes"
                            ? "That pane has unsaved edits."
                            : "That Dev Session is not the one open in CodeHarbor.",
                    },
                });
            });
            return true;
        }, socketPath);
        try {
            for (const code of ["not_active_session", "unsaved_changes"]) {
                refusalCode = code;
                const response = await handleLine(
                    request(9, "tools/call", { name: "viewer_reload", arguments: { pane: "viewer-1" } }),
                    env,
                );
                assert.ok(response);
                const { text, isError } = toolText(response.result);
                assert.equal(isError, true);
                // The CODE is what a skill tells the model to branch on, so it
                // must survive both control-channel validators and MCP text.
                assert.match(text, new RegExp(code));
            }
        } finally {
            listener.close();
        }
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("every tool names a real control operation", () => {
    // The tool table and the control ops are two spellings of one list; a
    // mismatch would only surface when a model happened to call the broken tool.
    for (const tool of TOOLS) {
        assert.ok(tool.op.length > 0, tool.name);
        assert.match(tool.name, /^viewer_[a-z]+$/);
    }
});

test("no tool schema accepts unknown properties", () => {
    // A model that invents an argument should be told, not silently obeyed with
    // the argument dropped.
    for (const tool of TOOLS) {
        assert.equal(
            "additionalProperties" in tool.inputSchema
                ? tool.inputSchema.additionalProperties
                : undefined,
            false,
            tool.name,
        );
    }
});

test("the listener closes each producer connection with its single reply", async () => {
    // If it did not, an agent's tool would hang holding a descriptor per call.
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-mcp-"));
    try {
        const socketPath = path.join(dir, "control.sock");
        const listener = await startControlListener((command) => {
            queueMicrotask(() => listener.settle({ commandId: command.commandId, ok: true }));
            return true;
        }, socketPath);
        try {
            const reply = await new Promise<string>((resolve, reject) => {
                const probe = net.createConnection(socketPath);
                probe.setEncoding("utf8");
                let buffer = "";
                probe.on("error", reject);
                probe.on("connect", () => {
                    probe.write(
                        `${JSON.stringify({ devSessionId: "d", terminalId: "t", op: "list" })}\n`,
                    );
                });
                // The data handler is REQUIRED, not incidental: a socket whose
                // readable side nobody consumes never emits 'end', so it would
                // never close and this case would hang rather than fail.
                probe.on("data", (chunk: string) => {
                    buffer += chunk;
                });
                probe.on("close", () => resolve(buffer));
            });
            assert.match(reply, /"ok":true/);
        } finally {
            listener.close();
        }
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("closing stdin does not discard an answer that is still in flight", async () => {
    // REGRESSION. Every scripted use — and a harness that batches one turn's
    // requests — writes its lines and closes the pipe. Exiting on stdin 'close'
    // killed the process while the control-socket round trip was outstanding, so
    // the tool calls produced NOTHING: no result, no error, no exit code that
    // said why. Caught only by running the real server end to end.
    //
    // Driven as a child process, because the bug is in runStdio's exit policy and
    // an in-process handleLine() call cannot see it.
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-mcp-"));
    try {
        const socketPath = mintControlSocketPath({ XDG_RUNTIME_DIR: dir, HOME: dir });
        const env: NodeJS.ProcessEnv = {
            ...process.env,
            OMP_DEV_SESSION_ID: "dev-1",
            OMP_TERMINAL_ID: "term-1",
            [CONTROL_SOCKET_ENV]: socketPath,
            XDG_RUNTIME_DIR: dir,
            HOME: dir,
        };
        const listener = await startControlListener((command) => {
            // Answer on a LATER turn of the event loop, as the desktop really
            // does: the answer cannot arrive before stdin has already closed.
            setTimeout(() => {
                listener.settle({
                    commandId: command.commandId,
                    ok: true,
                    data: { paneId: "viewer-7" },
                });
            }, 25).unref();
            return true;
        }, socketPath);
        try {
            const serverPath = fileURLToPath(new URL("../src/mcp/server.ts", import.meta.url));
            const child = spawn(process.execPath, [serverPath], { env });
            let out = "";
            child.stdout.setEncoding("utf8");
            child.stdout.on("data", (chunk: string) => {
                out += chunk;
            });
            child.stdin.end(
                `${JSON.stringify({
                    jsonrpc: "2.0",
                    id: 1,
                    method: "tools/call",
                    params: { name: "viewer_focus", arguments: { pane: "viewer-7" } },
                })}\n`,
            );
            const code = await new Promise<number | null>((resolve) => {
                child.on("close", (status) => resolve(status));
            });
            assert.equal(code, 0);
            assert.match(out, /viewer-7/);
        } finally {
            listener.close();
        }
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("stdin EOF waits for backpressured stdout responses to flush", { timeout: 10000 }, async () => {
    const serverUrl = new URL("../src/mcp/server.ts", import.meta.url).href;
    const child = spawn(process.execPath, [
        "--input-type=module",
        "-e",
        `import { runStdio } from ${JSON.stringify(serverUrl)};
         process.stdin.once("close", () => process.send("input-closed"));
         runStdio();`,
    ], { stdio: ["pipe", "pipe", "pipe", "ipc"] });
    let output = "";
    let errors = "";
    child.stdout!.setEncoding("utf8");
    child.stdout!.on("data", (chunk: string) => { output += chunk; });
    child.stdout!.pause();
    child.stderr!.setEncoding("utf8");
    child.stderr!.on("data", (chunk: string) => { errors += chunk; });
    const inputClosed = Promise.withResolvers<void>();
    child.once("message", () => inputClosed.resolve());
    child.once("error", inputClosed.reject);
    child.once("exit", () => inputClosed.reject(new Error("MCP exited before announcing stdin EOF")));
    const exited = Promise.withResolvers<number | null>();
    child.once("close", exited.resolve);
    try {
        const count = 2000;
        child.stdin!.end(
            Array.from({ length: count }, (_, id) => request(id, "tools/list")).join("\n") + "\n",
        );
        // A real EOF notification over IPC is the barrier: all requests have
        // been read, while stdout is deliberately still blocked.
        await inputClosed.promise;
        child.stdout!.resume();
        assert.equal(await exited.promise, 0, errors);
        const replies = output.trim().split("\n").map((line) => JSON.parse(line));
        assert.equal(replies.length, count);
        assert.equal(new Set(replies.map((reply) => reply.id)).size, count);
    } finally {
        child.kill();
    }
});

test("a symlinked MCP server answers stdio requests (RI2)", () => {
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-mcp-link-"));
    try {
        const link = path.join(dir, "codeharbor-mcp.ts");
        symlinkSync(fileURLToPath(new URL("../src/mcp/server.ts", import.meta.url)), link);
        const result = spawnSync(process.execPath, [link], {
            env: NO_ENV,
            input: `${request(73, "ping")}\n`,
            encoding: "utf8",
            timeout: 10_000,
        });
        assert.equal(result.error, undefined);
        assert.equal(result.status, 0, result.stderr);
        assert.deepEqual(JSON.parse(result.stdout), { jsonrpc: "2.0", id: 73, result: {} });
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("MCP cancellation closes the pending producer and suppresses its response", { timeout: 5000 }, async () => {
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-mcp-cancel-"));
    const socketPath = path.join(dir, "control.sock");
    const received = Promise.withResolvers<net.Socket>();
    const sockets = new Set<net.Socket>();
    const server = net.createServer((socket) => {
        sockets.add(socket);
        socket.once("data", () => received.resolve(socket));
    });
    server.listen(socketPath);
    await once(server, "listening");
    const session = createMcpSession({
        OMP_DEV_SESSION_ID: "d", OMP_TERMINAL_ID: "t",
        [CONTROL_SOCKET_ENV]: socketPath,
    });
    try {
        const pending = session.handleLine(request(1, "tools/call", { name: "viewer_list" }));
        const socket = await received.promise;
        const closed = once(socket, "close");
        const duplicate = await session.handleLine(request(1, "tools/call", { name: "viewer_list" }));
        assert.equal(duplicate?.error?.code, -32600);
        assert.equal(await session.handleLine(JSON.stringify({
            jsonrpc: "2.0", method: "notifications/cancelled", params: { requestId: 1 },
        })), null);
        assert.equal(await pending, null);
        await closed;
        assert.deepEqual((await session.handleLine(request(2, "ping")))?.result, {});
    } finally {
        session.close();
        for (const socket of sockets) socket.destroy();
        server.close();
        rmSync(dir, { recursive: true, force: true });
    }
});

test("MCP bounds outstanding viewer waits without dispatching overflow requests", { timeout: 5000 }, async () => {
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-mcp-limit-"));
    const socketPath = path.join(dir, "control.sock");
    const sockets = new Set<net.Socket>();
    const received = Promise.withResolvers<void>();
    let dispatches = 0;
    const server = net.createServer((socket) => {
        sockets.add(socket);
        socket.once("data", () => {
            dispatches += 1;
            if (dispatches === MAX_MCP_INFLIGHT) received.resolve();
        });
    });
    server.listen(socketPath);
    await once(server, "listening");
    const session = createMcpSession({
        OMP_DEV_SESSION_ID: "d", OMP_TERMINAL_ID: "t",
        [CONTROL_SOCKET_ENV]: socketPath,
    });
    try {
        const pending = Array.from({ length: MAX_MCP_INFLIGHT }, (_, id) =>
            session.handleLine(request(id, "tools/call", { name: "viewer_list" })));
        await received.promise;
        const overflow = await session.handleLine(
            request(MAX_MCP_INFLIGHT, "tools/call", { name: "viewer_close" }),
        );
        assert.ok(overflow?.error);
        assert.equal(dispatches, MAX_MCP_INFLIGHT);
        assert.deepEqual((await session.handleLine(request(1000, "ping")))?.result, {});
        session.close();
        assert.ok((await Promise.all(pending)).every((response) => response === null));
    } finally {
        session.close();
        for (const socket of sockets) socket.destroy();
        server.close();
        rmSync(dir, { recursive: true, force: true });
    }
});

test("overlong MCP input exits unsuccessfully instead of reporting a clean shutdown", () => {
    const serverPath = fileURLToPath(new URL("../src/mcp/server.ts", import.meta.url));
    const child = spawnSync(process.execPath, [serverPath], {
        input: "x".repeat(MAX_MCP_LINE_BYTES + 1),
        encoding: "utf8",
        timeout: 5000,
    });
    assert.equal(child.error, undefined);
    assert.equal(child.status, 1);
    assert.match(child.stderr, /input line exceeded/);
});

test("a stalled MCP output pipe cannot accumulate unbounded responses", { timeout: 10000 }, async () => {
    const serverPath = fileURLToPath(new URL("../src/mcp/server.ts", import.meta.url));
    const child = spawn(process.execPath, [serverPath]);
    let errors = "";
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk: string) => { errors += chunk; });
    child.stdin.on("error", () => {});
    const exited = once(child, "exit");
    const errorsEnded = once(child.stderr, "end");
    try {
        // An unknown method is echoed in its refusal. Keep every input frame
        // legal while leaving stdout unread and exceeding the output budget.
        const method = "x".repeat(MAX_MCP_LINE_BYTES / 2);
        const count = Math.ceil(MAX_MCP_PENDING_OUTPUT_BYTES / method.length) + 8;
        child.stdin.end(Array.from({ length: count }, (_, id) => request(id, method)).join("\n") + "\n");
        const [status] = await exited;
        await errorsEnded;
        assert.equal(status, 1);
        assert.match(errors, /pending output exceeded/);
    } finally {
        child.kill();
        child.stdout.destroy();
    }
});

test("MCP stdio returns deeply nested viewer data as valid bounded tool text (NIN6)", { timeout: 10000 }, async () => {
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-mcp-deep-"));
    const socketPath = path.join(dir, "control.sock");
    const depth = 10000;
    const data = JSON.parse('{"n":'.repeat(depth) + "0" + "}".repeat(depth)) as Record<string, unknown>;
    const listener = await startControlListener((command) => {
        queueMicrotask(() => listener.settle({ commandId: command.commandId, ok: true, data }));
        return true;
    }, socketPath);
    const child = spawn(process.execPath, [
        fileURLToPath(new URL("../src/mcp/server.ts", import.meta.url)),
    ], {
        env: {
            ...process.env,
            OMP_DEV_SESSION_ID: "d", OMP_TERMINAL_ID: "t",
            [CONTROL_SOCKET_ENV]: socketPath,
        },
        timeout: 5000,
    });
    let output = "";
    let errors = "";
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => { output += chunk; });
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk: string) => { errors += chunk; });
    try {
        child.stdin.end(`${request(1, "tools/call", { name: "viewer_list" })}\n`);
        const [status] = await once(child, "close");
        assert.equal(status, 0, errors);
        assert.equal(errors, "");
        assert.ok(Buffer.byteLength(output) <= MAX_MCP_LINE_BYTES);
        const response = JSON.parse(output);
        assert.equal(response.id, 1);
        assert.equal(response.error, undefined);
        const result = toolText(response.result);
        assert.equal(result.isError, false);
        let value: unknown = JSON.parse(result.text);
        for (let i = 0; i < depth; i += 1) value = (value as Record<string, unknown>).n;
        assert.equal(value, 0);
    } finally {
        child.kill();
        listener.close();
        rmSync(dir, { recursive: true, force: true });
    }
});
