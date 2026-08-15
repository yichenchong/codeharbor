// The viewer control channel, end to end on the remote side: an agent's tool
// writes a request into a Unix socket, codeharbord relays it, the desktop's
// answer comes back on that same socket.
//
// Every case here drives the REAL listener over a real socket in a temp
// directory. The path from a producer's bytes to a settled promise has four
// pieces (path resolution, lock, framing/validation, correlation) and a mock at
// any of those seams would leave the seam itself untested — which is precisely
// where an agent's command silently vanishing would come from.
//
// No sleeps and no polling: the harness turns each relayed command into a
// promise, so every case awaits the signal the code itself produces.

import { test } from "node:test";
import assert from "node:assert/strict";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";

import {
    CH_CONTROL_VERSION,
    CONTROL_OPS,
    MAX_CONTROL_INFLIGHT,
    MAX_CONTROL_LINE_BYTES,
    parseControlRequest,
    resolveControlSocketPath,
    startControlListener,
    type ControlListener,
    type ControlResponse,
    type ViewerCommand,
} from "../src/control.ts";
import { sendControlRequest } from "../src/control-client.ts";

function tempDir(): string {
    return mkdtempSync(path.join(os.tmpdir(), "ch-control-"));
}

/** True when parseControlRequest answered with a refusal rather than a request. */
function isRefusal(value: object): value is ControlResponse {
    return "ok" in value;
}

interface Harness {
    listener: ControlListener;
    relayed: ViewerCommand[];
    env: NodeJS.ProcessEnv;
    /** Resolves with the Nth relayed command (1-based), whenever it arrives. */
    command(ordinal: number): Promise<ViewerCommand>;
    dispose(): void;
}

async function harness(options: { emit?: (c: ViewerCommand) => boolean } = {}): Promise<Harness> {
    const dir = tempDir();
    const socketPath = path.join(dir, "control.sock");
    const relayed: ViewerCommand[] = [];
    // One waiter per ordinal. Registered before or after the command arrives —
    // command() resolves immediately for one already relayed — so no case has to
    // race the listener.
    const waiters = new Map<number, (c: ViewerCommand) => void>();
    const listener = await startControlListener((command) => {
        relayed.push(command);
        const waiter = waiters.get(relayed.length);
        if (waiter) {
            waiters.delete(relayed.length);
            waiter(command);
        }
        return options.emit ? options.emit(command) : true;
    }, socketPath);
    return {
        listener,
        relayed,
        env: {
            OMP_DEV_SESSION_ID: "dev-1",
            OMP_TERMINAL_ID: "term-1",
            XDG_RUNTIME_DIR: dir,
            HOME: dir,
        },
        command(ordinal) {
            const already = relayed[ordinal - 1];
            if (already) return Promise.resolve(already);
            return new Promise<ViewerCommand>((resolve) => waiters.set(ordinal, resolve));
        },
        dispose() {
            listener.close();
            rmSync(dir, { recursive: true, force: true });
        },
    };
}

/** Send one raw line on the socket and resolve with the single reply line. */
function rawExchange(socketPath: string, payload: string): Promise<string> {
    return new Promise<string>((resolve, reject) => {
        const socket = net.createConnection(socketPath);
        socket.setEncoding("utf8");
        let buffer = "";
        socket.on("error", reject);
        socket.on("connect", () => socket.write(payload));
        socket.on("data", (chunk: string) => {
            buffer += chunk;
            const newline = buffer.indexOf("\n");
            if (newline >= 0) {
                socket.destroy();
                resolve(buffer.slice(0, newline));
            }
        });
    });
}

// ---------------------------------------------------------------------------
// Socket path. Both ends resolve through one function, and a relative
// $XDG_RUNTIME_DIR must be IGNORED rather than joined: joining it makes the path
// depend on each process's working directory, so the daemon would bind one path
// and an agent's tool — started wherever the agent happens to run — would look
// for another. Every command would vanish with nothing reporting it.
// ---------------------------------------------------------------------------

test("an absolute XDG_RUNTIME_DIR gives the runtime-dir socket", () => {
    assert.equal(
        resolveControlSocketPath({ XDG_RUNTIME_DIR: "/run/user/1000", HOME: "/home/u" }),
        "/run/user/1000/codeharbor-control.sock",
    );
});

test("a relative XDG_RUNTIME_DIR falls back to the home cache path", () => {
    assert.equal(
        resolveControlSocketPath({ XDG_RUNTIME_DIR: "relative/dir", HOME: "/home/u" }),
        "/home/u/.cache/codeharbor/control.sock",
    );
});

test("the control socket is never the agent-status socket", () => {
    // Two endpoints under one runtime directory. If the basenames ever collided,
    // a status hook's line would reach the control listener (refused as a bad
    // request) while pane commands reached the status relay.
    assert.equal(
        path.basename(resolveControlSocketPath({ XDG_RUNTIME_DIR: "/run/user/1000" })),
        "codeharbor-control.sock",
    );
});

// ---------------------------------------------------------------------------
// Request validation. Pure and total: a hostile producer gets a structured
// refusal, never an exception into a socket handler (which would be an uncaught
// exception that kills the daemon and every terminal and editor with it).
// ---------------------------------------------------------------------------

test("parseControlRequest normalizes a minimal request", () => {
    const parsed = parseControlRequest(
        JSON.stringify({ devSessionId: "d", terminalId: "t", op: "list" }),
    );
    assert.equal(isRefusal(parsed), false);
    assert.deepEqual(parsed, {
        version: CH_CONTROL_VERSION,
        devSessionId: "d",
        terminalId: "t",
        op: "list",
        args: {},
    });
});

test("parseControlRequest refuses every malformed shape without throwing", () => {
    const cases: { line: string; why: string }[] = [
        { line: "", why: "empty line" },
        { line: "not json", why: "unparseable" },
        // JSON.parse("null") succeeds; without the plain-object guard every field
        // access after it would throw inside the socket's own data handler.
        { line: "null", why: "bare null" },
        { line: "[]", why: "array" },
        { line: "42", why: "primitive" },
        {
            line: JSON.stringify({ version: 99, devSessionId: "d", terminalId: "t", op: "list" }),
            why: "declared future version",
        },
        { line: JSON.stringify({ devSessionId: "  ", terminalId: "t", op: "list" }), why: "blank session" },
        { line: JSON.stringify({ devSessionId: "d", terminalId: "", op: "list" }), why: "blank terminal" },
        { line: JSON.stringify({ devSessionId: "d", terminalId: "t", op: "explode" }), why: "unknown op" },
        { line: JSON.stringify({ devSessionId: "d", terminalId: "t" }), why: "no op" },
        {
            line: JSON.stringify({ devSessionId: "d", terminalId: "t", op: "open", args: [] }),
            why: "array args",
        },
    ];
    for (const testCase of cases) {
        const parsed = parseControlRequest(testCase.line);
        assert.equal(isRefusal(parsed), true, testCase.why);
        if (!isRefusal(parsed)) continue;
        assert.equal(parsed.ok, false, testCase.why);
        assert.equal(parsed.error?.code, "bad_request", testCase.why);
    }
});

test("an ABSENT version means the current revision", () => {
    // The compatibility guarantee: a tool installed before the version field
    // existed keeps working forever. Only a DECLARED unknown revision is refused.
    const parsed = parseControlRequest(
        JSON.stringify({
            devSessionId: "d",
            terminalId: "t",
            op: "focus",
            args: { pane: "viewer-1" },
        }),
    );
    assert.equal(isRefusal(parsed), false);
});

// ---------------------------------------------------------------------------
// The whole round trip.
// ---------------------------------------------------------------------------

test("a command reaches the relay and the desktop's answer reaches the caller", async () => {
    const h = await harness();
    try {
        const pending = sendControlRequest(
            "open",
            { url: "file:///tmp/x.md" },
            h.env,
            h.listener.socketPath,
        );
        const relayed = await h.command(1);
        assert.deepEqual(relayed, {
            commandId: "vc-1",
            devSessionId: "dev-1",
            terminalId: "term-1",
            op: "open",
            args: { url: "file:///tmp/x.md" },
        });
        assert.equal(h.listener.pendingCount(), 1);
        // Exactly what the viewer.commandResult handler does with the client's
        // answer.
        assert.equal(
            h.listener.settle({ commandId: "vc-1", ok: true, data: { paneId: "viewer-2" } }),
            true,
        );
        assert.deepEqual(await pending, {
            version: CH_CONTROL_VERSION,
            ok: true,
            data: { paneId: "viewer-2" },
        });
        assert.equal(h.listener.pendingCount(), 0);
    } finally {
        h.dispose();
    }
});

test("a refusal from the desktop reaches the caller with its code", async () => {
    const h = await harness();
    try {
        const pending = sendControlRequest("close", { pane: "viewer-9" }, h.env, h.listener.socketPath);
        await h.command(1);
        h.listener.settle({
            commandId: "vc-1",
            ok: false,
            error: { code: "unknown_pane", message: "No viewer pane named viewer-9." },
        });
        const response = await pending;
        assert.equal(response.ok, false);
        assert.equal(response.error?.code, "unknown_pane");
        assert.match(response.error?.message ?? "", /viewer-9/);
    } finally {
        h.dispose();
    }
});

test("an unrecognized refusal code from a newer client becomes failed, never success", async () => {
    const h = await harness();
    try {
        const pending = sendControlRequest("focus", { pane: "viewer-1" }, h.env, h.listener.socketPath);
        await h.command(1);
        h.listener.settle({
            commandId: "vc-1",
            ok: false,
            error: { code: "invented_by_a_newer_client", message: "nope" },
        });
        const response = await pending;
        assert.equal(response.ok, false);
        assert.equal(response.error?.code, "failed");
    } finally {
        h.dispose();
    }
});

test("settling an unknown or already-settled command is refused, not thrown", async () => {
    const h = await harness();
    try {
        assert.equal(h.listener.settle({ commandId: "vc-404", ok: true }), false);
        const pending = sendControlRequest("list", {}, h.env, h.listener.socketPath);
        await h.command(1);
        assert.equal(h.listener.settle({ commandId: "vc-1", ok: true, data: { panes: [] } }), true);
        // A second answer must not reach a socket already closed with the first.
        assert.equal(h.listener.settle({ commandId: "vc-1", ok: false }), false);
        assert.equal((await pending).ok, true);
    } finally {
        h.dispose();
    }
});

test("a relay that cannot reach the desktop answers failed immediately", async () => {
    // A failed stdout is knowable NOW; leaving the agent to wait out the
    // five-second timeout for an answer that can never arrive is the one outcome
    // worth avoiding.
    const h = await harness({ emit: () => false });
    try {
        const response = await sendControlRequest("list", {}, h.env, h.listener.socketPath);
        assert.equal(response.ok, false);
        assert.equal(response.error?.code, "failed");
        assert.equal(h.listener.pendingCount(), 0);
    } finally {
        h.dispose();
    }
});

test("a malformed request is refused and the listener keeps serving", async () => {
    const h = await harness();
    try {
        const refusal = await rawExchange(h.listener.socketPath, '{"op":"list"}\n');
        assert.match(refusal, /"code":"bad_request"/);
        assert.equal(h.relayed.length, 0);

        // Still alive for the next producer, which is the point.
        const pending = sendControlRequest("list", {}, h.env, h.listener.socketPath);
        await h.command(1);
        h.listener.settle({ commandId: "vc-1", ok: true });
        assert.equal((await pending).ok, true);
    } finally {
        h.dispose();
    }
});

test("a line past the byte bound is refused rather than buffered", async () => {
    const h = await harness();
    try {
        // No newline: a producer that lost it, which is what the bound exists for.
        const refusal = await rawExchange(
            h.listener.socketPath,
            "x".repeat(MAX_CONTROL_LINE_BYTES + 1024),
        );
        assert.match(refusal, /"code":"bad_request"/);
        assert.match(refusal, /without a newline/);
    } finally {
        h.dispose();
    }
});

test("past the in-flight bound a producer is told busy instead of being queued", async () => {
    const h = await harness();
    try {
        const held: Promise<ControlResponse>[] = [];
        for (let i = 0; i < MAX_CONTROL_INFLIGHT; i += 1) {
            held.push(sendControlRequest("list", {}, h.env, h.listener.socketPath));
        }
        await h.command(MAX_CONTROL_INFLIGHT);
        assert.equal(h.listener.pendingCount(), MAX_CONTROL_INFLIGHT);

        const overflow = await sendControlRequest("list", {}, h.env, h.listener.socketPath);
        assert.equal(overflow.ok, false);
        assert.equal(overflow.error?.code, "busy");
        // The held commands are untouched: back-pressure refuses the NEW one
        // rather than dropping work already in flight.
        assert.equal(h.listener.pendingCount(), MAX_CONTROL_INFLIGHT);
        for (let i = 0; i < MAX_CONTROL_INFLIGHT; i += 1) {
            h.listener.settle({ commandId: `vc-${i + 1}`, ok: true });
        }
        await Promise.all(held);
    } finally {
        h.dispose();
    }
});

test("a second listener cannot steal a live socket path", async () => {
    const h = await harness();
    try {
        await assert.rejects(
            () => startControlListener(() => true, h.listener.socketPath),
            /control address already in use/,
        );
    } finally {
        h.dispose();
    }
});

test("a crashed owner's lock is reclaimed rather than blocking forever", async () => {
    const dir = tempDir();
    try {
        const socketPath = path.join(dir, "control.sock");
        // A REAL pid that has really exited, obtained by reaping a child: the
        // liveness probe is process.kill(pid, 0), and no constant is reliably
        // dead (pid 0 signals the process group and succeeds; pid 1 is init).
        const child = spawnSync(process.execPath, ["-e", ""]);
        assert.equal(child.status, 0);
        assert.ok(child.pid !== undefined && child.pid > 1);
        writeFileSync(`${socketPath}.lock`, `${child.pid}\n`);
        // Without reclamation one crash would leave viewer control dead until a
        // human deleted the file.
        const listener = await startControlListener(() => true, socketPath);
        assert.equal(listener.socketPath, socketPath);
        listener.close();
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("closing the listener answers the agents still waiting", async () => {
    const h = await harness();
    try {
        const pending = sendControlRequest("list", {}, h.env, h.listener.socketPath);
        await h.command(1);
        h.listener.close();
        const response = await pending;
        assert.equal(response.ok, false);
        assert.equal(response.error?.code, "failed");
        assert.match(response.error?.message ?? "", /shutting down/);
    } finally {
        h.dispose();
    }
});

test("no socket at all is a named refusal, not a thrown error", async () => {
    const dir = tempDir();
    try {
        const response = await sendControlRequest(
            "list",
            {},
            { OMP_DEV_SESSION_ID: "d", OMP_TERMINAL_ID: "t" },
            path.join(dir, "absent.sock"),
        );
        assert.equal(response.ok, false);
        assert.equal(response.error?.code, "failed");
        assert.match(response.error?.message ?? "", /cannot reach the CodeHarbor server/);
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("missing pane coordinates are refused before any socket work", async () => {
    // An agent started in a tmux session that predates CodeHarbor has no
    // coordinates. A command sent anyway would be structurally valid and
    // unroutable, so it is refused here with an explanation a person can act on.
    const response = await sendControlRequest("list", {}, { HOME: "/home/u" }, "/nonexistent.sock");
    assert.equal(response.ok, false);
    assert.equal(response.error?.code, "bad_request");
    assert.match(response.error?.message ?? "", /OMP_DEV_SESSION_ID/);
});

test("the op vocabulary is exactly what the client implements", () => {
    // Main.qml's runViewerCommand and ch::ViewerCommandService::isKnownOp carry
    // the same list. Pinning the daemon's copy keeps an op from being added on one
    // side alone and reaching a client that cannot answer it.
    assert.deepEqual([...CONTROL_OPS], ["list", "open", "close", "split", "focus", "reload"]);
});
