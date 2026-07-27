import { test } from "node:test";
import assert from "node:assert/strict";
import net from "node:net";
import os from "node:os";
import fs from "node:fs";
import path from "node:path";
import {
    emitHookEvent,
    toBridgeMessage,
    readHookInput,
    type HookInput,
} from "../src/hooks/oh-my-pi-hook.ts";
import { FallbackActivityDetector } from "../src/adapters/fallback.ts";
import { resolveSocketPath } from "../src/events.ts";
import { processBridgeLine } from "../src/bridge.ts";

test("toBridgeMessage wraps the raw native event without mapping", () => {
    const message = toBridgeMessage({
        event: "tool_call",
        devSessionId: "sess-1",
        terminalId: "term-1",
        tool: "ask",
    });
    assert.equal(message.harness, "oh-my-pi");
    assert.equal(message.devSessionId, "sess-1");
    assert.equal(message.terminalId, "term-1");
    // The hook emits the raw native event; mapping is the bridge's job.
    assert.deepEqual(message.native, { type: "tool_call", tool: "ask" });

    // Unknown/no-op events are still wrapped verbatim (bridge decides the no-op).
    const noop = toBridgeMessage({ event: "noop", devSessionId: "s", terminalId: "t" });
    assert.deepEqual(noop.native, { type: "noop" });

    // Errors surface via native.error.
    const errored = toBridgeMessage({
        event: "tool_call",
        devSessionId: "s",
        terminalId: "t",
        error: true,
    });
    assert.equal(errored.native.error, true);
});

test("readHookInput reads event from argv and coordinates from env", () => {
    const input = readHookInput(["node", "hook.ts", "tool_call"], {
        OMP_DEV_SESSION_ID: "sess-9",
        OMP_TERMINAL_ID: "term-9",
        OMP_TOOL: "ask",
        OMP_ERROR: "true",
    } as NodeJS.ProcessEnv);
    assert.equal(input.event, "tool_call");
    assert.equal(input.devSessionId, "sess-9");
    assert.equal(input.terminalId, "term-9");
    assert.equal(input.tool, "ask");
    assert.equal(input.error, true);
});

// Emit a hook event over a real socket and return the raw JSONL line the bridge
// would receive. Exercises resolveSocketPath via XDG_RUNTIME_DIR (SPEC 6.3).
async function emitAndReceive(input: HookInput): Promise<string> {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ch-hook-"));
    const socketPath = resolveSocketPath({ XDG_RUNTIME_DIR: dir } as NodeJS.ProcessEnv);

    const received = Promise.withResolvers<string>();
    const server = net.createServer((socket) => {
        const chunks: Buffer[] = [];
        socket.on("data", (chunk) => chunks.push(chunk));
        socket.on("end", () => received.resolve(Buffer.concat(chunks).toString("utf8")));
    });
    const listening = Promise.withResolvers<void>();
    server.listen(socketPath, () => listening.resolve());
    await listening.promise;

    try {
        await emitHookEvent(input, socketPath);
        return await received.promise;
    } finally {
        const closed = Promise.withResolvers<void>();
        server.close(() => closed.resolve());
        await closed.promise;
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

test("emitHookEvent -> bridge: tool_call ask relays a waiting_input AgentEvent", async () => {
    const raw = await emitAndReceive({
        event: "tool_call",
        devSessionId: "sess-1",
        terminalId: "term-1",
        tool: "ask",
    });
    assert.ok(raw.endsWith("\n"), "line must be newline-terminated JSONL");

    // The line is a raw BridgeMessage, not a pre-mapped AgentEvent.
    const decoded = JSON.parse(raw.trim());
    assert.equal(decoded.harness, "oh-my-pi");
    assert.equal(decoded.devSessionId, "sess-1");
    assert.equal(decoded.terminalId, "term-1");
    assert.deepEqual(decoded.native, { type: "tool_call", tool: "ask" });

    // End-to-end: the bridge maps the native event to an AgentEvent.
    const event = processBridgeLine(raw);
    assert.ok(event, "bridge must map the message to an AgentEvent");
    assert.equal(event.harness, "oh-my-pi");
    assert.equal(event.state, "waiting_input");
    assert.equal(event.event, "tool_call");
    assert.equal(event.devSessionId, "sess-1");
    assert.equal(event.terminalId, "term-1");
});

test("emitHookEvent -> bridge: agent_start relays a running AgentEvent", async () => {
    const raw = await emitAndReceive({
        event: "agent_start",
        devSessionId: "sess-2",
        terminalId: "term-2",
    });

    const decoded = JSON.parse(raw.trim());
    assert.deepEqual(decoded.native, { type: "agent_start" });

    const event = processBridgeLine(raw);
    assert.ok(event, "bridge must map the message to an AgentEvent");
    assert.equal(event.state, "running");
    assert.equal(event.event, "agent_start");
    assert.equal(event.devSessionId, "sess-2");
    assert.equal(event.terminalId, "term-2");
});

test("emitHookEvent -> bridge: unknown native event is a no-op (null)", async () => {
    const raw = await emitAndReceive({
        event: "noop",
        devSessionId: "sess-3",
        terminalId: "term-3",
    });

    // The hook still emits a valid BridgeMessage line...
    const decoded = JSON.parse(raw.trim());
    assert.equal(decoded.harness, "oh-my-pi");
    assert.deepEqual(decoded.native, { type: "noop" });

    // ...but the bridge maps the unknown native event to no state transition.
    assert.equal(processBridgeLine(raw), null);
});

test("FallbackActivityDetector: starting, then running within threshold, idle after", () => {
    const detector = new FallbackActivityDetector(2000);
    // No output yet.
    assert.equal(detector.state(0), "starting");

    detector.note(1000);
    // Output 500ms ago -> within threshold -> running.
    assert.equal(detector.state(1500), "running");
    // Boundary: exactly at threshold is idle (strict less-than window).
    assert.equal(detector.state(3000), "idle");
    // Well past threshold -> idle.
    assert.equal(detector.state(5000), "idle");

    // Fresh output revives the running state.
    detector.note(5000);
    assert.equal(detector.state(5100), "running");
});
