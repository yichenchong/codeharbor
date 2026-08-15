// Viewer control channel (agent -> desktop). A coding agent runs inside a
// CodeHarbor terminal pane on the SERVER; the viewer panes it wants to drive
// live in the Qt process on the user's desktop. This module is the server half
// of the only path between them.
//
// Shape of the path, end to end:
//
//   agent (MCP server / codeharbor-view CLI)
//     -> this Unix socket, one JSON request line per connection
//     -> codeharbord relays it as an id-less `viewer.command` JSON-RPC
//        notification on the RPC channel it already owns
//     -> the client's ch::ViewerCommandService hands it to Main.qml, which
//        drives the real ViewerRegion
//     -> the client answers with a `viewer.commandResult` REQUEST
//     -> settle() writes that answer back on the producer's still-open socket.
//
// It deliberately reuses the RPC channel rather than the agent-status bridge:
// the bridge is a one-way status relay whose stdin is a lifetime watchdog
// (`cat >/dev/null` in SessionBootstrap::bridgeCommand), so nothing can travel
// client -> server on it, and an unanswerable command is worse than none. The
// RPC channel already carries server -> client notifications (file.watchEvent)
// and client -> server requests, so request/response needs no new transport.
//
// Like every producer boundary in this codebase (SPEC 6.4), a broken or hostile
// producer must never take the daemon down: every guard below answers with a
// structured refusal and closes one connection, and nothing here throws into
// the socket's own event handlers.

import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";

import { isEventIdentifier, isPlainObject } from "./events.ts";

/**
 * Format revision of the PRODUCER-to-daemon message, i.e. the line an agent's
 * tool writes into the control socket.
 *
 * OPTIONAL ON THE WIRE, and that is a compatibility guarantee rather than an
 * oversight — the same rule BRIDGE_MESSAGE_VERSION states in bridge.ts. An
 * ABSENT version means "the current revision" forever; only a message that
 * DECLARES a revision this daemon does not implement is refused. A user pastes
 * an MCP server path into an assistant's configuration once, and that path may
 * keep pointing at a months-old checkout for as long as the configuration
 * lives.
 */
export const CH_CONTROL_VERSION = 1 as const;

/**
 * The viewer operations an agent may ask for. Closed set: an unlisted token is
 * refused here rather than forwarded, so the client never has to decide what an
 * op it has never heard of should do to the user's layout.
 */
export const CONTROL_OPS = ["list", "open", "close", "split", "focus", "reload"] as const;
export type ControlOp = (typeof CONTROL_OPS)[number];

/**
 * Refusal vocabulary. These tokens are the agent-facing contract — an MCP tool
 * result carries them verbatim — so they are stable strings rather than
 * numbers, and every one of them names a condition an agent can act on:
 *
 *   bad_request         the request itself is malformed; fix and retry
 *   busy                too many commands in flight; retry shortly
 *   timeout             the desktop never answered; it may be gone
 *   not_active_session  the pane's Dev Session is not the one on screen
 *   unknown_pane        no such viewer pane in that session's layout
 *   failed              the desktop tried and could not
 */
export const CONTROL_ERROR_CODES = [
    "bad_request",
    "busy",
    "timeout",
    "not_active_session",
    "unknown_pane",
    "failed",
] as const;
export type ControlErrorCode = (typeof CONTROL_ERROR_CODES)[number];

/** One request line on the control socket. */
export interface ControlRequest {
    version?: number;
    devSessionId: string;
    terminalId: string;
    op: ControlOp;
    args?: Record<string, unknown>;
}

/** The single response line written back on the same connection. */
export interface ControlResponse {
    version: typeof CH_CONTROL_VERSION;
    ok: boolean;
    data?: Record<string, unknown>;
    error?: { code: ControlErrorCode; message: string };
}

// Upper bound on one line arriving on the control socket. A control request is
// a handful of short identifiers plus a URL, so 64 KiB is already several
// orders of magnitude of headroom; anything past it is a producer that lost its
// newline. Deliberately far smaller than the bridge's 1 MiB (which may carry a
// summary) and than the RPC transport's 16 MiB (which carries file contents).
export const MAX_CONTROL_LINE_BYTES = 64 * 1024;

// How long a relayed command may go unanswered by the desktop before the
// producer is told `timeout`. The agent's own turn is blocked for exactly this
// long, so it is bounded and short: the client's work is a QML tree edit, not
// I/O. A desktop that is merely slow loses the answer, never the command.
export const CONTROL_REQUEST_TIMEOUT_MS = 5000;

// Ceiling on commands awaiting a desktop answer at once. Past it a producer is
// told `busy` instead of being queued: each pending entry pins a socket and a
// timer, and an agent that fires faster than the user's UI can settle wants
// back-pressure, not an unbounded queue of layout edits.
export const MAX_CONTROL_INFLIGHT = 32;

// Ceiling on accepted producer connections. Every real producer connects,
// writes one line and ends, so this only ever binds on an abandoned or hung
// one; without it enough such corpses make accept() fail with EMFILE and the
// only symptom is that agent pane control silently stops working.
export const MAX_CONTROL_CONNECTIONS = 64;

// How long a freshly accepted producer may stay completely silent. Disarmed by
// the FIRST byte, so a producer still writing is never mistaken for an idle
// one. Same guard, same reasoning as BRIDGE_HANDSHAKE_TIMEOUT_MS.
export const CONTROL_HANDSHAKE_TIMEOUT_MS = 30_000;

const CONTROL_LOCK_SUFFIX = ".lock";

/**
 * Resolve the control socket path: `$XDG_RUNTIME_DIR/codeharbor-control.sock`
 * when the runtime directory is set, otherwise
 * `~/.cache/codeharbor/control.sock`.
 *
 * The rules are deliberately identical to resolveSocketPath() in events.ts,
 * including the two branches having DIFFERENT basenames, so the two sockets can
 * never be confused for one another and both ends of each pair resolve through
 * one function.
 *
 * A RELATIVE $XDG_RUNTIME_DIR is ignored rather than honoured: the variable is
 * defined to be absolute, and joining a relative one produces a path that
 * depends on the process's working directory — the daemon would bind it under
 * its own directory and an agent's tool, started wherever the agent happens to
 * be running, would look for it under a different one. Every command would
 * silently vanish. The home directory comes from the SAME `env`, so a caller
 * that passes an environment gets a self-consistent answer rather than its own
 * runtime directory and somebody else's home.
 */
export function resolveControlSocketPath(env: NodeJS.ProcessEnv = process.env): string {
    const runtimeDir = env.XDG_RUNTIME_DIR;
    if (runtimeDir && path.isAbsolute(runtimeDir)) {
        return path.join(runtimeDir, "codeharbor-control.sock");
    }
    const home = env.HOME;
    const base = home && path.isAbsolute(home) ? home : os.homedir();
    return path.join(base, ".cache", "codeharbor", "control.sock");
}

function controlLockPath(socketPath: string): string {
    return `${socketPath}${CONTROL_LOCK_SUFFIX}`;
}

/**
 * Take exclusive ownership of the control socket path, or throw.
 *
 * A stale Unix socket INODE is indistinguishable from a live one: bind() fails
 * with EADDRINUSE either way, and unlinking it unconditionally would let a
 * second daemon steal the path from a first that is serving commands. The lock
 * file carries the owner's PID, so a crashed owner's leftovers can be told from
 * a live one and reclaimed. Same algorithm as acquireBridgeLock in bridge.ts;
 * kept separate rather than shared because the two differ in their message and
 * their lifetime, and that file is under test.
 */
function acquireControlLock(socketPath: string): string {
    const lockPath = controlLockPath(socketPath);
    for (;;) {
        const tempPath = `${lockPath}.${process.pid}.${Date.now()}.${Math.random()}`;
        try {
            fs.writeFileSync(tempPath, `${process.pid}\n`, {
                encoding: "utf8",
                flag: "wx",
                mode: 0o600,
            });
            try {
                // Publishing the fully written PID with linkSync is atomic: no
                // contender can observe an empty ownership record.
                fs.linkSync(tempPath, lockPath);
                fs.unlinkSync(tempPath);
                return lockPath;
            } catch (err) {
                try {
                    fs.unlinkSync(tempPath);
                } catch {
                    // The temporary file may already have been removed.
                }
                if (!(err instanceof Error) || !("code" in err) || err.code !== "EEXIST") {
                    throw err;
                }
            }
        } catch (err) {
            if (!(err instanceof Error) || !("code" in err) || err.code !== "EEXIST") {
                throw err;
            }
        }
        let ownerPid: number;
        try {
            ownerPid = Number.parseInt(fs.readFileSync(lockPath, "utf8").trim(), 10);
        } catch {
            throw new Error(`codeharbord: control address already in use: ${socketPath}`);
        }
        if (!Number.isInteger(ownerPid) || ownerPid <= 0) {
            throw new Error(`codeharbord: control address already in use: ${socketPath}`);
        }
        let ownerAlive = false;
        try {
            process.kill(ownerPid, 0);
            ownerAlive = true;
        } catch (probeError) {
            // EPERM means the process exists but belongs to someone else, which
            // is very much alive. Only ESRCH ("no such process") is stale.
            ownerAlive =
                probeError instanceof Error &&
                "code" in probeError &&
                probeError.code === "EPERM";
        }
        if (ownerAlive) {
            throw new Error(`codeharbord: control address already in use: ${socketPath}`);
        }
        try {
            fs.unlinkSync(lockPath);
        } catch (unlinkError) {
            if (
                !(unlinkError instanceof Error) ||
                !("code" in unlinkError) ||
                unlinkError.code !== "ENOENT"
            ) {
                throw unlinkError;
            }
        }
    }
}

/** The command a listener hands to its `emit` callback. */
export interface ViewerCommand {
    commandId: string;
    devSessionId: string;
    terminalId: string;
    op: ControlOp;
    args: Record<string, unknown>;
}

/** The desktop's answer to one relayed command. */
export interface ViewerCommandOutcome {
    commandId: string;
    ok: boolean;
    error?: { code: string; message: string };
    data?: Record<string, unknown>;
}

export interface ControlListener {
    /** Stop accepting, answer pending producers, and remove socket and lock. */
    close(): void;
    /**
     * Deliver the desktop's answer for a pending command. Returns false when
     * the id is unknown — already answered, or already timed out — which is NOT
     * an error: the caller must not turn a late answer into a fault.
     */
    settle(outcome: ViewerCommandOutcome): boolean;
    /** Commands awaiting an answer, for tests asserting the bound holds. */
    pendingCount(): number;
    /** The bound socket path, for diagnostics. */
    socketPath: string;
}

function responseLine(response: ControlResponse): string {
    return `${JSON.stringify(response)}\n`;
}

function refusal(code: ControlErrorCode, message: string): ControlResponse {
    return { version: CH_CONTROL_VERSION, ok: false, error: { code, message } };
}

/**
 * Validate one decoded control line. Returns the normalized request, or the
 * refusal to send back. Pure and total: it never throws.
 */
export function parseControlRequest(line: string): ControlRequest | ControlResponse {
    const trimmed = line.trim();
    if (trimmed.length === 0) return refusal("bad_request", "empty request");

    let decoded: unknown;
    try {
        decoded = JSON.parse(trimmed);
    } catch {
        return refusal("bad_request", "request is not valid JSON");
    }
    // A bare `null` line PARSES — JSON.parse("null") is null, not an error — so
    // without this every field access below would throw on it. An array or a
    // primitive is likewise not a request.
    if (!isPlainObject(decoded)) return refusal("bad_request", "request is not a JSON object");

    const version = decoded.version;
    if (version !== undefined && version !== CH_CONTROL_VERSION) {
        return refusal(
            "bad_request",
            `unsupported request version ${JSON.stringify(version)}; this server speaks ${CH_CONTROL_VERSION}`,
        );
    }

    const devSessionId = decoded.devSessionId;
    if (typeof devSessionId !== "string" || !isEventIdentifier(devSessionId)) {
        return refusal(
            "bad_request",
            "devSessionId must be a non-blank string (set OMP_DEV_SESSION_ID)",
        );
    }
    const terminalId = decoded.terminalId;
    if (typeof terminalId !== "string" || !isEventIdentifier(terminalId)) {
        return refusal(
            "bad_request",
            "terminalId must be a non-blank string (set OMP_TERMINAL_ID)",
        );
    }
    const op = decoded.op;
    if (typeof op !== "string" || !(CONTROL_OPS as readonly string[]).includes(op)) {
        return refusal("bad_request", `op must be one of ${CONTROL_OPS.join(", ")}`);
    }
    const rawArgs = decoded.args;
    if (rawArgs !== undefined && !isPlainObject(rawArgs)) {
        return refusal("bad_request", "args must be a JSON object when present");
    }

    return {
        version: CH_CONTROL_VERSION,
        devSessionId,
        terminalId,
        op: op as ControlOp,
        args: rawArgs ?? {},
    };
}

/**
 * Start the control listener on `socketPath`.
 *
 * `emit` relays one command to the desktop and answers whether it reached the
 * wire; a false return is reported to the producer as `failed` rather than
 * being left to time out, because a dead output is knowable immediately.
 */
export async function startControlListener(
    emit: (command: ViewerCommand) => boolean,
    socketPath: string = resolveControlSocketPath(),
): Promise<ControlListener> {
    // `async`, so the directory and lock failures below REJECT rather than
    // throwing synchronously out of the call. The daemon starts this with
    // `void startControlListener(...).then(ok, err)`, and a synchronous throw
    // there is an uncaught exception that kills the whole service — over losing a
    // socket race with a second CodeHarbor window, which is a degradation, not a
    // fault.
    //
    // The directory comes BEFORE the lock, not with the bind: the lock file is a
    // SIBLING of the socket, so on a first run — a fresh $XDG_RUNTIME_DIR entry,
    // or a home with no ~/.cache/codeharbor yet — the lock's temp-file write
    // fails with ENOENT and the listener never starts at all. 0700 because the
    // socket it will hold rearranges the user's windows.
    fs.mkdirSync(path.dirname(socketPath), { recursive: true, mode: 0o700 });
    const lockPath = acquireControlLock(socketPath);
    const releaseLock = (): void => {
        try {
            fs.unlinkSync(lockPath);
        } catch {
            // Already gone; nothing to release.
        }
    };

    interface Pending {
        socket: net.Socket;
        timer: NodeJS.Timeout;
    }
    const pending = new Map<string, Pending>();
    let commandCounter = 0;
    let closed = false;

    const answer = (socket: net.Socket, response: ControlResponse): void => {
        // A producer that vanished mid-flight makes this write raise
        // synchronously (EPIPE). Reached from a socket event handler and from a
        // timer, where an escaping exception is an uncaught exception that kills
        // the daemon and with it every terminal and editor of the session.
        try {
            socket.end(responseLine(response));
        } catch {
            socket.destroy();
        }
    };

    const forget = (commandId: string): Pending | undefined => {
        const entry = pending.get(commandId);
        if (!entry) return undefined;
        clearTimeout(entry.timer);
        pending.delete(commandId);
        return entry;
    };

    const server = net.createServer();
    server.maxConnections = MAX_CONTROL_CONNECTIONS;

    server.on("connection", (socket) => {
        socket.setEncoding("utf8");
        // Errors on a producer socket are the producer's problem, never the
        // daemon's: swallow them so a reset connection cannot become an
        // uncaught exception.
        socket.on("error", () => socket.destroy());

        let answered = false;
        let buffer = "";
        let handshake: NodeJS.Timeout | null = setTimeout(() => {
            handshake = null;
            socket.destroy();
        }, CONTROL_HANDSHAKE_TIMEOUT_MS);
        handshake.unref();

        const finish = (response: ControlResponse): void => {
            answered = true;
            answer(socket, response);
        };

        socket.on("data", (chunk: string) => {
            if (answered) return;
            if (handshake) {
                clearTimeout(handshake);
                handshake = null;
            }
            buffer += chunk;
            if (Buffer.byteLength(buffer) > MAX_CONTROL_LINE_BYTES) {
                finish(
                    refusal(
                        "bad_request",
                        `request exceeded ${MAX_CONTROL_LINE_BYTES} bytes without a newline`,
                    ),
                );
                return;
            }
            const newline = buffer.indexOf("\n");
            if (newline < 0) return;
            // ONE request per connection: everything after the first newline is
            // ignored rather than dispatched. The response is written on this
            // same socket, so a second command on it would have no way back.
            const line = buffer.slice(0, newline);
            buffer = "";

            const parsed = parseControlRequest(line);
            if ("ok" in parsed) {
                finish(parsed);
                return;
            }
            if (pending.size >= MAX_CONTROL_INFLIGHT) {
                finish(
                    refusal(
                        "busy",
                        `${MAX_CONTROL_INFLIGHT} viewer commands are already awaiting the desktop`,
                    ),
                );
                return;
            }

            commandCounter += 1;
            const commandId = `vc-${commandCounter}`;
            const timer = setTimeout(() => {
                pending.delete(commandId);
                finish(
                    refusal(
                        "timeout",
                        `the CodeHarbor desktop did not answer within ${CONTROL_REQUEST_TIMEOUT_MS} ms`,
                    ),
                );
            }, CONTROL_REQUEST_TIMEOUT_MS);
            timer.unref();
            pending.set(commandId, { socket, timer });

            // Registered BEFORE emit(): a synchronous relay failure must find
            // the entry to clear, and settle() must be able to answer an
            // implausibly fast desktop.
            let relayed: boolean;
            try {
                relayed = emit({
                    commandId,
                    devSessionId: parsed.devSessionId,
                    terminalId: parsed.terminalId,
                    op: parsed.op,
                    args: parsed.args ?? {},
                });
            } catch {
                relayed = false;
            }
            if (!relayed && forget(commandId)) {
                finish(refusal("failed", "the CodeHarbor desktop is not reachable"));
            }
        });

        socket.on("close", () => {
            if (handshake) {
                clearTimeout(handshake);
                handshake = null;
            }
            // A producer that gave up releases its slot; otherwise the entry
            // (and the in-flight bound it consumes) would live until the
            // timeout fired against a socket nobody is reading.
            for (const [commandId, entry] of [...pending]) {
                if (entry.socket === socket) forget(commandId);
            }
        });
    });

    return new Promise<ControlListener>((resolve, reject) => {
        const fail = (err: Error): void => {
            server.close();
            releaseLock();
            reject(err);
        };
        server.once("error", fail);
        // A STALE socket inode from a crashed owner: the lock above already
        // proved that owner is gone, so the path is ours to reclaim. Without
        // this, listen() fails with EADDRINUSE against a file nobody is serving
        // and viewer control stays dead until someone deletes it by hand.
        try {
            fs.unlinkSync(socketPath);
        } catch {
            // Nothing there, which is the normal case.
        }
        server.listen(socketPath, () => {
            server.removeListener("error", fail);
            // Past listen() a server error is not a startup failure and must not
            // reject a promise that has already resolved.
            server.on("error", () => {});
            try {
                fs.chmodSync(socketPath, 0o600);
            } catch {
                // A platform without socket permissions is not a reason to
                // refuse service; the 0700 directory above still gates it.
            }
            resolve({
                socketPath,
                close() {
                    if (closed) return;
                    closed = true;
                    for (const commandId of [...pending.keys()]) {
                        const entry = forget(commandId);
                        if (entry) {
                            answer(
                                entry.socket,
                                refusal("failed", "the CodeHarbor session is shutting down"),
                            );
                        }
                    }
                    server.close();
                    try {
                        fs.unlinkSync(socketPath);
                    } catch {
                        // Already gone.
                    }
                    releaseLock();
                },
                settle(outcome) {
                    const entry = forget(outcome.commandId);
                    if (!entry) return false;
                    let response: ControlResponse;
                    if (outcome.ok) {
                        response = { version: CH_CONTROL_VERSION, ok: true };
                        if (outcome.data) response.data = outcome.data;
                    } else {
                        // The desktop is the authority on WHY it refused; an
                        // unrecognized code is carried through as `failed`
                        // rather than dropped, so a newer client can never make
                        // a refused command look like a success here.
                        const code = outcome.error?.code ?? "";
                        response = refusal(
                            (CONTROL_ERROR_CODES as readonly string[]).includes(code)
                                ? (code as ControlErrorCode)
                                : "failed",
                            outcome.error?.message ?? "the viewer command failed",
                        );
                    }
                    answer(entry.socket, response);
                    return true;
                },
                pendingCount: () => pending.size,
            });
        });
    });
}
