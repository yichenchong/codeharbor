// codeharbor-bridge (SPEC 6.3): a small remote helper that listens on a Unix
// socket for harness-native events, maps them to CodeHarbor agent states via
// the adapter registry, and relays validated AgentEvents as JSONL on stdout.
// SSH forwards that stream to the client's AgentStatusMonitor.
//
// Harness adapters must never block or break the agent if CodeHarbor is
// unavailable (SPEC 6.4): a broken line is dropped, never thrown.

import net from "node:net";
import fs from "node:fs";
import path from "node:path";
// pathToFileURL, not string concatenation: a script path containing a space (or
// any character a URL must percent-encode) would never compare equal to
// import.meta.url, silently turning the CLI entry point below into a no-op.
import { pathToFileURL } from "node:url";
import { adapterFor } from "./adapters/index.ts";
import {
    isEventIdentifier,
    isHarness,
    isPlainObject,
    makeEvent,
    resolveSocketPath,
    type AgentEvent,
} from "./events.ts";

// Upper bound on one line arriving on the bridge socket. A bridge message is a
// handful of short identifiers plus an optional one-line summary, so a
// megabyte is already several orders of magnitude of headroom; anything past
// it is a producer that lost its newline, not a real event. Deliberately much
// smaller than codeharbord's 16 MiB transport cap: that one has to carry whole
// file contents, this one never does.
export const MAX_BRIDGE_LINE_BYTES = 1024 * 1024;

// Wire format for a single line arriving on the bridge socket. Every field is
// optional and `unknown`: this describes what a producer is SUPPOSED to send,
// and a decoded JSON object is free to omit or mistype any of it — which is
// what the guards in processBridgeLine are for.
interface BridgeMessage {
    harness?: unknown;
    devSessionId?: unknown;
    terminalId?: unknown;
    native?: unknown;
    summary?: unknown;
    metadata?: unknown;
}

/**
 * Convert one raw socket line into an AgentEvent, or null when the line is
 * malformed, targets an unknown/adapterless harness, or carries no state
 * transition. Pure and total: never throws.
 */
export function processBridgeLine(line: string): AgentEvent | null {
    const trimmed = line.trim();
    if (trimmed.length === 0) return null;

    let decoded: unknown;
    try {
        decoded = JSON.parse(trimmed);
    } catch {
        return null;
    }
    // A bare `null` line PARSES — JSON.parse("null") is null, not an error — so
    // without this every field access below would throw a TypeError. That
    // TypeError happens inside the socket's 'line' handler, where nothing
    // catches it: one producer sending the four bytes `null` would take the
    // whole bridge process down and with it every harness's status reporting.
    // An array or a primitive is likewise not a message.
    if (!isPlainObject(decoded)) return null;
    const message = decoded as BridgeMessage;

    if (
        !isHarness(message.harness) ||
        // Non-blank identifiers, checked HERE because this relay builds its
        // events with makeEvent and never runs them through validateEvent: this
        // is the only gate on the socket-to-stdout path. An empty id yields a
        // structurally valid event filed under a Dev Session that does not exist
        // (see isEventIdentifier). The `typeof` half stays because it is what
        // narrows the field to `string` for the makeEvent call below.
        typeof message.devSessionId !== "string" ||
        typeof message.terminalId !== "string" ||
        !isEventIdentifier(message.devSessionId) ||
        !isEventIdentifier(message.terminalId) ||
        // An array would pass a bare `typeof === "object"` check and then read
        // back as a native event with no `type`, i.e. a silent no-op.
        !isPlainObject(message.native)
    ) {
        return null;
    }

    try {
        const adapter = adapterFor(message.harness);
        if (!adapter) return null;

        const native = message.native as Record<string, unknown>;

        const state = adapter.map(native);
        if (state === null) return null;

        // Metadata is derived from the native event by the adapter
        // (harness-agnostic), then merged with any explicit metadata a bridge
        // producer put on the wire. Explicit wire fields win over derived
        // ones.
        const derived = adapter.metadata?.(native);
        const explicit = isPlainObject(message.metadata) ? message.metadata : undefined;
        const metadata = (derived || explicit)
            ? { ...(derived ?? {}), ...(explicit ?? {}) }
            : undefined;

        const nativeName = native.type ?? native.hook;
        return makeEvent({
            harness: message.harness,
            devSessionId: message.devSessionId,
            terminalId: message.terminalId,
            state,
            event: typeof nativeName === "string" ? nativeName : "unknown",
            summary: typeof message.summary === "string" ? message.summary : undefined,
            metadata,
        });
    } catch {
        // Adapters are extensions at this boundary. A malformed native payload
        // must be isolated to its producer, not allowed to terminate the relay.
        return null;
    }

}

/**
 * A sink for mapped events. Receives the source socket so it can apply
 * back-pressure: when the output stalls the sink pauses the socket the event
 * arrived on and resumes it once the output drains (RR24).
 */
export type EventSink = (event: AgentEvent, source: net.Socket) => void;
interface BridgeLineFramer {
    feed(chunk: Buffer): void;
    // Resume parsing bytes retained from a chunk that arrived just before the
    // source socket was paused for output back-pressure.
    resume(): void;
    close(): void;
}

// A paused socket can still finish delivering the chunk that triggered the
// pause. Keep that remainder bounded too; otherwise one unusually large
// readable chunk would bypass the socket-level back-pressure guard.
export const MAX_BRIDGE_PENDING_BYTES = MAX_BRIDGE_LINE_BYTES + 64 * 1024;

/**
 * Frame bridge input without readline's unbounded unterminated-line buffer.
 * The consumer returns false when it paused the source socket; the framer then
 * retains only the unprocessed suffix and resumes it after the sink drains.
 */
export function createBridgeLineFramer(
    onLine: (line: string) => boolean,
    onOverflow: () => void,
): BridgeLineFramer {
    let held: Buffer[] = [];
    let heldBytes = 0;
    let pending: Buffer[] = [];
    let pendingBytes = 0;
    let blocked = false;
    let closed = false;

    const drop = (): void => {
        if (closed) return;
        closed = true;
        held = [];
        heldBytes = 0;
        pending = [];
        pendingBytes = 0;
        onOverflow();
    };

    // `front` puts the chunk back at the HEAD of the queue. That is the only
    // correct place for the unprocessed tail of a chunk being drained: the rest
    // of the queue arrived AFTER those bytes, so appending the tail instead
    // reorders the stream — and because a tail can end mid-line, the bytes of
    // one event get glued to the bytes of a later one and both lines are lost.
    const retain = (chunk: Buffer, front = false): boolean => {
        if (chunk.length === 0) return true;
        if (pendingBytes + chunk.length > MAX_BRIDGE_PENDING_BYTES) {
            drop();
            return false;
        }
        if (front) pending.unshift(chunk);
        else pending.push(chunk);
        pendingBytes += chunk.length;
        return true;
    };

    const processChunk = (chunk: Buffer): void => {
        let start = 0;
        let nl = chunk.indexOf(0x0a);
        while (nl !== -1 && !closed) {
            const segment = chunk.subarray(start, nl);
            const lineBytes = heldBytes + segment.length;
            if (lineBytes > MAX_BRIDGE_LINE_BYTES) {
                drop();
                return;
            }
            let line: string;
            if (heldBytes === 0) {
                line = segment.toString("utf8");
            } else {
                held.push(segment);
                line = Buffer.concat(held, lineBytes).toString("utf8");
            }
            held = [];
            heldBytes = 0;
            start = nl + 1;
            nl = chunk.indexOf(0x0a, start);
            if (!onLine(line)) {
                blocked = true;
                retain(chunk.subarray(start), true);
                return;
            }
        }
        if (closed || blocked) return;
        if (start < chunk.length) {
            const rest = chunk.subarray(start);
            held.push(rest);
            heldBytes += rest.length;
            if (heldBytes > MAX_BRIDGE_LINE_BYTES) drop();
        }
    };

    const drain = (): void => {
        if (closed || blocked) return;
        while (pending.length > 0 && !closed && !blocked) {
            const chunk = pending.shift() as Buffer;
            pendingBytes -= chunk.length;
            processChunk(chunk);
        }
    };

    return {
        feed(chunk) {
            if (closed) return;
            if (blocked) {
                retain(chunk);
                return;
            }
            processChunk(chunk);
        },
        resume() {
            if (!blocked || closed) return;
            blocked = false;
            drain();
        },
        close() {
            closed = true;
            held = [];
            heldBytes = 0;
            pending = [];
            pendingBytes = 0;
        },
    };
}


/**
 * Default sink: relay events as JSONL on `out` (stdout in production),
 * honouring back-pressure. When out.write() reports a full buffer it pauses
 * every source socket that fed a line and resumes them all on the next 'drain',
 * so a stalled consumer at the far end of the SSH channel throttles the
 * producers instead of queueing events in memory without bound. Events stay
 * ordered: writes are synchronous and in arrival order, and a paused socket
 * delivers no further lines until drain.
 */
export function makeStreamSink(out: NodeJS.WritableStream): EventSink {
    const paused = new Set<net.Socket>();
    // Sockets that already carry the cleanup listener below. A long-lived
    // producer can be paused and resumed many times, and attaching a fresh
    // 'close' listener on every pause piles them up on the same socket: Node
    // warns at eleven ("possible EventEmitter memory leak") and every one of
    // them is retained until the socket finally closes. Weak so a closed socket
    // is still collectable.
    const hooked = new WeakSet<net.Socket>();
    out.on("drain", () => {
        const toResume = [...paused];
        // Clear before resuming. A resumed source may synchronously deliver
        // buffered input, and that write can fill the output again.
        paused.clear();
        for (const socket of toResume) {
            if (!socket.destroyed) socket.resume();
        }
    });
    out.on("error", () => {
        // There can be no useful drain after an output failure. Destroying
        // paused producers releases the set and prevents the bridge from
        // retaining every socket forever.
        for (const socket of paused) socket.destroy();
        paused.clear();
    });
    return (event, source) => {
        // The line framer normally stops before invoking us again for a paused
        // source. Keep this guard for custom callers so a full output stream
        // can never be fed another event synchronously.
        if (paused.has(source)) return;
        let ok: boolean;
        try {
            ok = out.write(`${JSON.stringify(event)}\n`);
        } catch {
            source.destroy();
            return;
        }
        if (!ok) {
            source.pause();
            paused.add(source);
            // Forget a producer that disconnects while the output is still
            // stalled. Without this the set holds a reference to every socket
            // paused since the last 'drain', and if the consumer at the far end
            // of the SSH channel never drains again — the exact situation that
            // caused the pause — that set (and the sockets in it) is never
            // released for as long as the bridge runs.
            if (!hooked.has(source)) {
                hooked.add(source);
                source.once("close", () => paused.delete(source));
            }
        }
    };
}

/**
 * Start the bridge server on `socketPath`, relaying mapped events to `sink`
 * (defaults to stdout). Resolves with the listening server once it is bound.
 *
 * Rejects if a LIVE bridge already owns the socket: an async connect probe
 * distinguishes a socket a bridge is still listening on (connect succeeds ->
 * refuse with 'address already in use') from a stale one left by a dead run
 * (connect refused/absent -> unlink and take it over). A non-socket at the path
 * is never clobbered, so a mistyped path pointing at a regular file makes
 * listen() fail rather than deleting the file.
 */
export async function startBridge(
    socketPath: string = resolveSocketPath(),
    sink?: EventSink,
): Promise<net.Server> {
    // Delay the default sink until the bridge has actually bound and accepted
    // a producer. A failed live-socket probe must not leak a stdout drain
    // listener on every retry.
    let activeSink = sink;
    const server = net.createServer((socket) => {
        const lines = createBridgeLineFramer(
            (line) => {
                const event = processBridgeLine(line);
                if (event) {
                    try {
                        if (!activeSink) activeSink = makeStreamSink(process.stdout);
                        activeSink(event, socket);
                    } catch {
                        // A sink is an extension boundary. A failed output
                        // must disconnect only this producer, not crash the
                        // bridge serving every other harness.
                        socket.destroy();
                        return false;
                    }
                }
                return !socket.isPaused();
            },
            () => {
                process.stderr.write(
                    `codeharbor-bridge: dropping a producer that exceeded the ${MAX_BRIDGE_LINE_BYTES}-byte frame bound\n`,
                );
                socket.destroy();
            },
        );
        socket.on("data", lines.feed);
        socket.on("resume", lines.resume);
        socket.on("close", lines.close);
        socket.on("error", () => socket.destroy());
    });
    // Ensure the socket's parent directory exists (the ~/.cache fallback may
    // not, SPEC 6.3). The mode applies when mkdir creates a directory; do not
    // chmod an existing caller-owned directory such as /tmp.
    fs.mkdirSync(path.dirname(socketPath), { recursive: true, mode: 0o700 });
    // Only touch an existing entry if it is actually a socket, so a mistyped
    // path pointing at a regular file is never clobbered (listen() then fails).
    let existing = false;
    try {
        existing = fs.lstatSync(socketPath).isSocket();
    } catch {
        // Nothing at socketPath (ENOENT); nothing to probe or clear.
    }
    if (existing) {
        // Probe liveness: a bridge still listening answers the connect, so
        // refuse rather than orphan it. A refused/absent connection (a dead
        // run's leftover) means the socket is stale -> unlink and take over.
        const alive = await new Promise<boolean>((resolve) => {
            const probe = net.createConnection(socketPath);
            let settled = false;
            const finish = (isAlive: boolean): void => {
                if (settled) return;
                settled = true;
                probe.destroy();
                resolve(isAlive);
            };
            probe.setTimeout(1000, () => finish(false));
            probe.on("connect", () => finish(true));
            probe.on("error", () => finish(false));
        });
        if (alive) {
            throw new Error(
                `codeharbor-bridge: address already in use: a live bridge is listening on ${socketPath}`,
            );
        }
        fs.unlinkSync(socketPath);
    }
    await new Promise<void>((resolve, reject) => {
        const onError = (err: Error): void => reject(err);
        server.once("error", onError);
        server.listen(socketPath, () => {
            // Restrict the socket to the owning user (0600); the default
            // net.Server socket honours only umask, which may leave it
            // group/world-accessible. Refuse to run if this security boundary
            // cannot be established on the host.
            try {
                fs.chmodSync(socketPath, 0o600);
            } catch (err) {
                server.removeListener("error", onError);
                server.close(() => reject(err));
                return;
            }
            server.removeListener("error", onError);
            // Past listen(), the ONLY listener for 'error' is gone — and an
            // EventEmitter 'error' with no listener THROWS. A listening server
            // can still emit one (an accept() failure such as EMFILE), which
            // would take down a bridge that had been serving fine for hours,
            // and with it every harness's status reporting. Report and keep
            // running: the failed accept cost one producer, not the service.
            server.on("error", (err: Error) => {
                process.stderr.write(`codeharbor-bridge: ${err.message}\n`);
            });
            resolve();
        });
    });
    return server;
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
    const socketPath = resolveSocketPath();
    try {
        const server = await startBridge(socketPath);
        process.stderr.write(`codeharbor-bridge listening on ${socketPath}\n`);
        // Without handlers, SIGHUP/SIGINT/SIGTERM terminate the process
        // outright and leave the socket file behind, so the next run has to
        // treat a socket that may still look live as stale. Close the listener
        // and remove the socket ourselves, then exit immediately: waiting for
        // open connections to drain would let one stuck producer block
        // shutdown forever.
        const shutdown = (): void => {
            server.close();
            try {
                if (fs.lstatSync(socketPath).isSocket()) fs.unlinkSync(socketPath);
            } catch {
                // Already unlinked by server.close(), or never created.
            }
            process.exit(0);
        };
        process.on("SIGHUP", shutdown);
        process.on("SIGINT", shutdown);
        process.on("SIGTERM", shutdown);
    } catch (err) {
        // A live bridge already owns the socket (RR12), or listen failed.
        process.stderr.write(`${err instanceof Error ? err.message : String(err)}\n`);
        process.exit(1);
    }
}
