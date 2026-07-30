import { test } from "node:test";
import assert from "node:assert/strict";
import net from "node:net";
import os from "node:os";
import fs from "node:fs";
import path from "node:path";
import {
    emitHookEvent,
    main,
    missingCoordinates,
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

test("readHookInput falls back to OMP_HOOK_EVENT and ignores a false OMP_ERROR", () => {
    const fromEnv = readHookInput(["node", "hook.ts"], {
        OMP_HOOK_EVENT: "agent_end",
        OMP_DEV_SESSION_ID: "sess-1",
        OMP_TERMINAL_ID: "term-1",
    } as NodeJS.ProcessEnv);
    assert.equal(fromEnv.event, "agent_end");

    // Only "1" and "true" mean error; anything else leaves the flag unset so the
    // bridge does not map an ordinary event to the error state.
    const notAnError = readHookInput(["node", "hook.ts", "agent_start"], {
        OMP_ERROR: "0",
    } as NodeJS.ProcessEnv);
    assert.equal(notAnError.error, undefined);
    // Missing coordinates degrade to empty strings rather than throwing: the
    // hook must never fail the agent run (SPEC 6.4).
    assert.equal(notAnError.devSessionId, "");
    assert.equal(notAnError.terminalId, "");
});

// Capture stderr for the duration of `body`. The hook reports its failures
// there, and a test that let them through would print noise into the run.
async function captureStderr(body: () => Promise<void>): Promise<string[]> {
    const written: string[] = [];
    const real = process.stderr.write.bind(process.stderr);
    process.stderr.write = ((chunk: string | Uint8Array): boolean => {
        written.push(String(chunk));
        return true;
    }) as unknown as typeof process.stderr.write;
    try {
        await body();
    } finally {
        process.stderr.write = real;
    }
    return written;
}

test("a missing bridge socket fails fast and main swallows it (SPEC 6.4)", async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ch-hook-dead-"));
    const env = {
        XDG_RUNTIME_DIR: dir,
        OMP_DEV_SESSION_ID: "sess-1",
        OMP_TERMINAL_ID: "term-1",
    } as NodeJS.ProcessEnv;
    try {
        // Nothing ever listened on this path, so the connect fails outright
        // instead of leaving the agent's hook invocation hanging.
        await assert.rejects(
            emitHookEvent(
                { event: "agent_start", devSessionId: "sess-1", terminalId: "term-1" },
                resolveSocketPath(env),
            ),
        );

        // The CLI turns that failure into one stderr line and returns normally:
        // an unavailable CodeHarbor must never break the agent run.
        const logged = await captureStderr(async () => {
            await main(["node", "oh-my-pi-hook.ts", "agent_start"], env);
        });
        assert.equal(logged.length, 1);
        assert.match(logged[0], /^oh-my-pi-hook: /);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

test("main prints usage and touches no socket when the event name is missing", async () => {
    const logged = await captureStderr(async () => {
        await main(["node", "oh-my-pi-hook.ts"], {} as NodeJS.ProcessEnv);
    });
    assert.equal(logged.length, 1);
    assert.match(logged[0], /^usage: /);
});

// A hook whose environment lacks the session coordinates must NOT emit: the
// event would be structurally valid all the way to the desktop client, which
// would file it under a Dev Session that does not exist and, for a notifying
// state, pop a notification whose body is little more than a slash. It must
// still not break the agent run (SPEC 6.4), so the failure is a stderr message
// and a normal return — never a throw.
test("main refuses to emit when the session coordinates are missing", async () => {
    // A REAL listener on the socket the hook would resolve, counting inbound
    // connections: the assertion is "the hook never even dialled", not merely
    // "no error was raised". No wait is needed — main() returns synchronously
    // from the refusal path, before any connect could be attempted.
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ch-hook-blank-"));
    const env = { XDG_RUNTIME_DIR: dir } as NodeJS.ProcessEnv;
    let connections = 0;
    const server = net.createServer(() => {
        connections += 1;
    });
    const listening = Promise.withResolvers<void>();
    server.listen(resolveSocketPath(env), () => listening.resolve());
    await listening.promise;
    try {
        const logged = await captureStderr(async () => {
            await main(["node", "oh-my-pi-hook.ts", "agent_start"], env);
        });
        assert.equal(logged.length, 1);
        assert.match(logged[0], /^oh-my-pi-hook: not emitting agent_start: /);
        // Both variables are named, so the user knows exactly what to export.
        assert.match(logged[0], /OMP_DEV_SESSION_ID and OMP_TERMINAL_ID/);

        // A blank (whitespace-only) value is as unroutable as an unset one, and
        // only the offending variable is named.
        const blank = await captureStderr(async () => {
            await main(["node", "oh-my-pi-hook.ts", "agent_start"], {
                XDG_RUNTIME_DIR: dir,
                OMP_DEV_SESSION_ID: "sess-1",
                OMP_TERMINAL_ID: "   ",
            } as NodeJS.ProcessEnv);
        });
        assert.equal(blank.length, 1);
        // The usage reminder that follows names BOTH variables, so the check
        // that only the offending one was reported reads the first line alone.
        const firstLine = blank[0].split("\n")[0];
        assert.match(firstLine, /OMP_TERMINAL_ID unset or blank/);
        assert.doesNotMatch(firstLine, /OMP_DEV_SESSION_ID/);

        assert.equal(connections, 0);
    } finally {
        const closed = Promise.withResolvers<void>();
        server.close(() => closed.resolve());
        await closed.promise;
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

test("missingCoordinates names only the coordinates that are unusable", () => {
    const full: HookInput = { event: "agent_start", devSessionId: "s", terminalId: "t" };
    assert.deepEqual(missingCoordinates(full), []);
    assert.deepEqual(missingCoordinates({ ...full, devSessionId: "" }), ["OMP_DEV_SESSION_ID"]);
    assert.deepEqual(missingCoordinates({ ...full, terminalId: "\t" }), ["OMP_TERMINAL_ID"]);
    assert.deepEqual(missingCoordinates({ ...full, devSessionId: "", terminalId: "" }), [
        "OMP_DEV_SESSION_ID",
        "OMP_TERMINAL_ID",
    ]);
});
