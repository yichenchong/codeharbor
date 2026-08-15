// The viewer control channel, end to end on the remote side: an agent's tool
// writes a request into a Unix socket, codeharbord relays it, the desktop's
// answer comes back on that same socket.
//
// Every case here drives the REAL listener over a real socket in a temp
// directory. The path from a producer's bytes to a settled promise has four
// pieces (socket addressing, framing, validation, correlation) and a mock at any
// of those seams would leave the seam itself untested — which is precisely where
// an agent's command silently vanishing, or reaching the wrong window, comes
// from.
//
// No sleeps and no polling: the harness turns each relayed command into a
// promise, so every case awaits the signal the code itself produces.

import { test } from "node:test";
import assert from "node:assert/strict";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { existsSync, mkdirSync, mkdtempSync, rmSync } from "node:fs";
import { spawn } from "node:child_process";
import { setImmediate as once } from "node:timers/promises";

import {
    CH_CONTROL_VERSION,
    CONTROL_OPS,
    CONTROL_SOCKET_ENV,
    MAX_CONTROL_INFLIGHT,
    MAX_CONTROL_LINE_BYTES,
    controlSocketDir,
    mintControlSocketPath,
    parseControlRequest,
    startControlListener,
    sweepStaleControlSockets,
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

async function harness(
    options: {
        emit?: (c: ViewerCommand) => boolean;
        // A directory shared with another harness, so two daemons mint their
        // sockets side by side exactly as two windows of one remote user do.
        // Owned by the caller when given: dispose() then leaves it alone.
        dir?: string;
        devSessionId?: string;
    } = {},
): Promise<Harness> {
    const owned = options.dir === undefined;
    const dir = options.dir ?? tempDir();
    // Minted through the production helper, in the production directory: that is
    // what proves two daemons under ONE $XDG_RUNTIME_DIR get different paths.
    const socketPath = mintControlSocketPath({ XDG_RUNTIME_DIR: dir, HOME: dir });
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
        // The environment a pane of THIS window carries: the identity pair plus
        // the socket its own daemon reported. Every case sends with this env and
        // NO explicit path, because reading the variable is the production
        // routing — a producer that derived the path is the misrouting this
        // addressing exists to remove.
        env: {
            OMP_DEV_SESSION_ID: options.devSessionId ?? "dev-1",
            OMP_TERMINAL_ID: "term-1",
            [CONTROL_SOCKET_ENV]: socketPath,
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
            if (owned) rmSync(dir, { recursive: true, force: true });
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
// Socket addressing. ONE SOCKET PER DAEMON, named with a random token — the
// routing identity, and the whole reason an agent reaches its own window.
// ---------------------------------------------------------------------------

test("the socket directory follows the runtime dir, ignoring a relative one", () => {
    assert.equal(
        controlSocketDir({ XDG_RUNTIME_DIR: "/run/user/1000", HOME: "/home/u" }),
        "/run/user/1000",
    );
    // Joining a relative $XDG_RUNTIME_DIR makes the path depend on each
    // process's working directory, so the daemon would bind one path and an
    // agent's tool would look for another.
    assert.equal(
        controlSocketDir({ XDG_RUNTIME_DIR: "relative/dir", HOME: "/home/u" }),
        "/home/u/.cache/codeharbor",
    );
});

test("every minted socket path is distinct and unguessable", () => {
    // The path IS the routing identity. A pid would be reused — and a live agent
    // keeps the path it was given, because tmux cannot retrofit a running
    // process — so a later daemon inheriting that pid would inherit that agent's
    // commands. A token is never reused: a path that outlives its daemon
    // resolves to nothing instead of to the wrong window.
    const env = { XDG_RUNTIME_DIR: "/run/user/1000" };
    const first = mintControlSocketPath(env);
    const second = mintControlSocketPath(env);
    assert.notEqual(first, second);
    for (const minted of [first, second]) {
        assert.equal(path.dirname(minted), "/run/user/1000");
        assert.match(path.basename(minted), /^codeharbor-control-[0-9a-f]{16}\.sock$/);
    }
});

test("the control socket is never the agent-status socket", () => {
    // Two endpoints under one runtime directory. If the names could collide, a
    // status hook's line would reach the control listener and pane commands would
    // reach the status relay.
    const minted = path.basename(mintControlSocketPath({ XDG_RUNTIME_DIR: "/run/user/1000" }));
    assert.notEqual(minted, "codeharbor.sock");
    assert.match(minted, /^codeharbor-control-/);
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
        // env only, no explicit path: the variable IS the routing.
        const pending = sendControlRequest("open", { url: "file:///tmp/x.md" }, h.env);
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
        const pending = sendControlRequest("close", { pane: "viewer-9" }, h.env);
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
        const pending = sendControlRequest("focus", { pane: "viewer-1" }, h.env);
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
        const pending = sendControlRequest("list", {}, h.env);
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
        const response = await sendControlRequest("list", {}, h.env);
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
        const pending = sendControlRequest("list", {}, h.env);
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
            held.push(sendControlRequest("list", {}, h.env));
        }
        await h.command(MAX_CONTROL_INFLIGHT);
        assert.equal(h.listener.pendingCount(), MAX_CONTROL_INFLIGHT);

        const overflow = await sendControlRequest("list", {}, h.env);
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

// ---------------------------------------------------------------------------
// TWO WINDOWS. This is the defect the per-daemon socket exists to remove: with
// one shared path only the first daemon could bind it, so every other window's
// agent reached the FIRST window's client — told its own plainly-open Dev
// Session was not active, or, with the same session active there, silently
// rearranging the wrong window.
// ---------------------------------------------------------------------------

test("two daemons in ONE runtime directory each serve only their own window", async () => {
    // The exact shipped defect, reproduced as it really happens: two windows of
    // one remote user, so both daemons mint under the SAME $XDG_RUNTIME_DIR. With
    // the old shared path the second could not bind at all and its agents reached
    // the first window.
    const dir = tempDir();
    try {
        const a = await harness({ dir, devSessionId: "dev-a" });
        const b = await harness({ dir, devSessionId: "dev-b" });
        try {
            assert.equal(path.dirname(a.listener.socketPath), dir);
            assert.equal(path.dirname(b.listener.socketPath), dir);
            assert.notEqual(a.listener.socketPath, b.listener.socketPath);

            // NO explicit path: each producer routes purely on the
            // $CODEHARBOR_CONTROL_SOCKET its own window exported, which is the
            // production path and the only thing that can misroute.
            const toA = sendControlRequest("focus", { pane: "viewer-1" }, a.env);
            const toB = sendControlRequest("focus", { pane: "viewer-9" }, b.env);
            await a.command(1);
            await b.command(1);

            // Neither daemon saw the other's command, and each carries its own
            // window's Dev Session rather than the neighbour's.
            assert.equal(a.relayed.length, 1);
            assert.equal(b.relayed.length, 1);
            assert.equal(a.relayed[0].devSessionId, "dev-a");
            assert.equal(b.relayed[0].devSessionId, "dev-b");
            assert.deepEqual(a.relayed[0].args, { pane: "viewer-1" });
            assert.deepEqual(b.relayed[0].args, { pane: "viewer-9" });

            a.listener.settle({ commandId: "vc-1", ok: true, data: { paneId: "viewer-1" } });
            b.listener.settle({ commandId: "vc-1", ok: true, data: { paneId: "viewer-9" } });
            assert.deepEqual((await toA).data, { paneId: "viewer-1" });
            assert.deepEqual((await toB).data, { paneId: "viewer-9" });
        } finally {
            a.dispose();
            b.dispose();
        }
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("a window that closes cannot steer the window that is still open", async () => {
    // The other half of the same defect: an agent whose window is gone keeps the
    // path it was given. It must reach NOTHING — not the daemon that is still
    // serving the surviving window from the same directory.
    const dir = tempDir();
    try {
        const survivor = await harness({ dir, devSessionId: "dev-survivor" });
        try {
            const closing = await harness({ dir, devSessionId: "dev-closing" });
            const orphanedEnv = closing.env;
            closing.dispose(); // that window goes away, socket and all

            const response = await sendControlRequest("close", { pane: "viewer-1" }, orphanedEnv);
            assert.equal(response.ok, false);
            assert.equal(response.error?.code, "failed");
            // The surviving window was never touched.
            assert.equal(survivor.relayed.length, 0);
        } finally {
            survivor.dispose();
        }
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("a producer with no injected socket is refused, never routed by guesswork", async () => {
    // The refusal IS the fix. A fallback to a shared or derived path is exactly
    // how a command reaches the wrong window, so a pane that carries no
    // $CODEHARBOR_CONTROL_SOCKET is told so instead.
    const h = await harness();
    try {
        const response = await sendControlRequest("list", {}, {
            OMP_DEV_SESSION_ID: "dev-1",
            OMP_TERMINAL_ID: "term-1",
            XDG_RUNTIME_DIR: path.dirname(h.listener.socketPath),
        });
        assert.equal(response.ok, false);
        assert.equal(response.error?.code, "bad_request");
        assert.match(response.error?.message ?? "", /CODEHARBOR_CONTROL_SOCKET/);
        // And nothing reached the daemon that happens to be listening next door.
        assert.equal(h.relayed.length, 0);
    } finally {
        h.dispose();
    }
});

test("a path whose daemon is gone reports an unreachable server", async () => {
    // What a long-lived agent sees after its window closed: the token is never
    // reused, so the stale path resolves to nothing rather than to whichever
    // daemon started since.
    const h = await harness();
    const stale = h.listener.socketPath;
    const env = { ...h.env, CODEHARBOR_CONTROL_SOCKET: stale };
    h.dispose();

    const response = await sendControlRequest("list", {}, env);
    assert.equal(response.ok, false);
    assert.equal(response.error?.code, "failed");
    assert.match(response.error?.message ?? "", /cannot reach the CodeHarbor server/);
});

test("the sweep removes an abandoned socket and keeps a live one", async () => {
    const dir = tempDir();
    try {
        const env = { XDG_RUNTIME_DIR: dir, HOME: dir };
        const live = await startControlListener(() => true, mintControlSocketPath(env));
        try {
            // A REAL Unix socket whose owner was KILLED, left on disk: exactly
            // what a SIGKILLed daemon leaves behind, and the reason staleness is
            // decided by CONNECTING rather than by parsing the name.
            //
            // Bound in a CHILD and SIGKILLed, because Node's own server.close()
            // unlinks the socket — a clean shutdown leaves nothing to sweep, which
            // is precisely why the sweep exists only for the unclean case.
            const orphan = mintControlSocketPath(env);
            const victim = spawn(process.execPath, [
                "-e",
                `require("node:net").createServer().listen(${JSON.stringify(orphan)}, () => console.log("up"))`,
            ]);
            await new Promise<void>((resolve, reject) => {
                victim.stdout.on("data", () => resolve());
                victim.on("error", reject);
                victim.on("exit", () => reject(new Error("the victim died before binding")));
            });
            victim.kill("SIGKILL");
            await new Promise<void>((resolve) => victim.on("exit", () => resolve()));
            assert.equal(existsSync(orphan), true, "the orphan socket was not left on disk");

            sweepStaleControlSockets(live.socketPath, env);
            // The probe answers on a later turn: the sweep is deliberately
            // asynchronous so it can never delay a session.
            await once();
            await once();
            assert.equal(existsSync(orphan), false, "the orphan survived the sweep");
            assert.equal(existsSync(live.socketPath), true, "the sweep removed a live socket");
        } finally {
            live.close();
        }
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("the sweep leaves anything that is not a refused socket alone", async () => {
    // Housekeeping must never be able to break a working session. A probe error
    // that is NOT "nobody is listening" — a permission problem on another user's
    // socket, or this process running out of descriptors — says nothing about the
    // owner, and deleting on it would take a live window's pane control away.
    const dir = tempDir();
    try {
        const env = { XDG_RUNTIME_DIR: dir, HOME: dir };
        // A DIRECTORY at a control-socket name: connect() fails with ECONNREFUSED
        // on some platforms and EACCES/ENOTSOCK on others, and either way it is
        // not a stale socket. rmSync would remove it; the sweep must not.
        const decoy = mintControlSocketPath(env);
        mkdirSync(decoy);
        sweepStaleControlSockets(null, env);
        await once();
        await once();
        assert.equal(existsSync(decoy), true, "the sweep removed something it could not identify");
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("closing the listener answers the agents still waiting", async () => {
    const h = await harness();
    try {
        const pending = sendControlRequest("list", {}, h.env);
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
