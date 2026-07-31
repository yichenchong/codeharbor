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
import { PassThrough } from "node:stream";
import { resolveSocketPath, type AgentEvent } from "../src/events.ts";
import {
    processBridgeLine,
    startBridge,
    makeStreamSink,
    MAX_BRIDGE_LINE_BYTES,
} from "../src/bridge.ts";

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

test("startBridge rejects when a live bridge already owns the socket (RR12)", async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ch-bridge-live-"));
    const socketPath = path.join(dir, "bridge.sock");
    const first = await startBridge(socketPath, () => {});
    try {
        // A second bridge on the same live socket must fail loudly instead of
        // orphaning the first (whose events would silently stop arriving).
        await assert.rejects(
            startBridge(socketPath, () => {}),
            /address already in use/,
        );
    } finally {
        const closed = Promise.withResolvers<void>();
        first.close(() => closed.resolve());
        await closed.promise;
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

test("startBridge unlinks a stale socket left by a dead bridge (RR12)", async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ch-bridge-stale-"));
    const socketPath = path.join(dir, "bridge.sock");
    // Manufacture a stale socket: bind a server, hard-link its socket inode to
    // a second name, then close the server (which unlinks only the name it
    // bound). The linked name is still a socket file, but nothing listens on
    // its inode, so a connect probe is refused — exactly a dead run's leftover.
    const donorPath = path.join(dir, "donor.sock");
    const donor = net.createServer();
    const donorUp = Promise.withResolvers<void>();
    donor.listen(donorPath, () => donorUp.resolve());
    await donorUp.promise;
    fs.linkSync(donorPath, socketPath);
    const donorDown = Promise.withResolvers<void>();
    donor.close(() => donorDown.resolve());
    await donorDown.promise;
    assert.ok(fs.lstatSync(socketPath).isSocket(), "leftover must be a socket file");

    let server: net.Server | undefined;
    try {
        server = await startBridge(socketPath, () => {});
        assert.ok(server.listening, "startBridge must take over a stale socket");
    } finally {
        if (server) {
            const closed = Promise.withResolvers<void>();
            server.close(() => closed.resolve());
            await closed.promise;
        }
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

// A stand-in for a producer socket that records the three things the sink does
// to one: pause it, resume it, and subscribe to its 'close' so a producer that
// disconnects mid-stall can be forgotten.
function fakeSource(): {
    socket: net.Socket;
    paused: boolean;
    pauses: number;
    resumes: number;
    close: () => void;
} {
    const state = {
        paused: false,
        pauses: 0,
        resumes: 0,
        close: (): void => {},
        socket: undefined as unknown as net.Socket,
    };
    state.socket = {
        pause() {
            state.paused = true;
            state.pauses += 1;
        },
        resume() {
            state.paused = false;
            state.resumes += 1;
        },
        once(event: string, handler: () => void) {
            if (event === "close") state.close = handler;
        },
    } as unknown as net.Socket;
    return state;
}

const RELAY_EVENT = {
    harness: "oh-my-pi",
    state: "running",
    event: "agent_start",
    devSessionId: "s",
    terminalId: "t",
} as unknown as AgentEvent;

test("makeStreamSink pauses the source on a full buffer and resumes on drain (RR24)", async () => {
    // A tiny highWaterMark forces write() to report a full buffer on the first
    // event; nothing consumes the readable side until we resume it below.
    const out = new PassThrough({ highWaterMark: 1 });
    const sink = makeStreamSink(out);
    const source = fakeSource();

    // Write until the output buffer is full: the sink must then pause the source.
    let guard = 0;
    while (!source.paused && guard++ < 10) sink(RELAY_EVENT, source.socket);
    assert.equal(source.paused, true, "a full output buffer must pause the source");

    // Further events while still stalled must not re-pause an already paused
    // producer (and must not re-subscribe to its 'close').
    const pausesAfterFirst = source.pauses;
    sink(RELAY_EVENT, source.socket);
    assert.equal(source.pauses, pausesAfterFirst, "an already paused source is not paused again");

    // Draining the output (consume the readable side) must resume the source.
    const drained = Promise.withResolvers<void>();
    out.on("drain", () => drained.resolve());
    out.resume();
    await drained.promise;
    assert.equal(source.paused, false, "the source must resume once the output drains");
});

// A producer that disconnects while the output is still stalled must be
// forgotten. Otherwise the sink holds a reference to every socket paused since
// the last drain — and when the consumer at the far end of the SSH channel
// never drains again, which is exactly why they were paused, that set is never
// released for the lifetime of the bridge.
test("makeStreamSink forgets a source that disconnects while the output is stalled", async () => {
    const out = new PassThrough({ highWaterMark: 1 });
    const sink = makeStreamSink(out);
    const source = fakeSource();

    let guard = 0;
    while (!source.paused && guard++ < 10) sink(RELAY_EVENT, source.socket);
    assert.equal(source.paused, true, "a full output buffer must pause the source");

    // The producer goes away before the output ever drains.
    source.close();

    const drained = Promise.withResolvers<void>();
    out.on("drain", () => drained.resolve());
    out.resume();
    await drained.promise;
    assert.equal(source.resumes, 0, "a disconnected source must not be resumed");
});

// readline has no maximum line length, so a producer that streams bytes and
// never sends a newline used to grow the bridge's memory until the process
// died — taking every other harness's status reporting down with it. The
// connection must be dropped instead, and a well-formed producer on another
// connection must keep working.
test("startBridge drops a producer that never sends a newline", async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ch-bridge-cap-"));
    const socketPath = path.join(dir, "bridge.sock");
    // Await the relayed event itself rather than polling: the sink IS the
    // signal that the bridge finished processing the good producer's line.
    const relayed = Promise.withResolvers<AgentEvent>();
    const server = await startBridge(socketPath, (event) => relayed.resolve(event));
    try {
        const flood = net.createConnection(socketPath);
        const floodClosed = Promise.withResolvers<void>();
        const floodUp = Promise.withResolvers<void>();
        flood.on("close", () => floodClosed.resolve());
        // The server destroys the connection mid-write, which surfaces as
        // EPIPE/ECONNRESET on this end. That IS the expected outcome here.
        flood.on("error", () => {});
        flood.once("connect", () => floodUp.resolve());
        await floodUp.promise;
        // Twice the cap with no newline at all.
        const junk = Buffer.alloc(256 * 1024, 0x61);
        for (let sent = 0; sent < MAX_BRIDGE_LINE_BYTES * 2; sent += junk.length) {
            flood.write(junk);
        }
        await floodClosed.promise;

        // The server survived and still serves a well-behaved producer.
        const good = net.createConnection(socketPath);
        const goodUp = Promise.withResolvers<void>();
        good.once("connect", () => goodUp.resolve());
        await goodUp.promise;
        good.end(
            `${JSON.stringify({
                harness: "oh-my-pi",
                devSessionId: "s1",
                terminalId: "t1",
                native: { type: "agent_start" },
            })}\n`,
        );
        assert.equal((await relayed.promise).state, "running");
    } finally {
        const closed = Promise.withResolvers<void>();
        server.close(() => closed.resolve());
        await closed.promise;
        fs.rmSync(dir, { recursive: true, force: true });
    }
});
