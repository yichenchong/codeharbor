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
import { fileURLToPath } from "node:url";
import { isEntrypoint } from "./tools/entrypoint.ts";
import { adapterFor } from "./adapters/index.ts";
import { nativeString } from "./adapters/types.ts";
import {
    isEventIdentifier,
    isHarness,
    isPlainObject,
    makeEvent,
    resolveSocketPath,
    type AgentEvent,
} from "./events.ts";
import { JsonDepthLimitError, JsonSizeLimitError, stringifyJson } from "./json.ts";
import {
    BRIDGE_SUBSCRIBE_PROTOCOL,
    BridgeSubscribers,
    bridgeRelayIdleMs,
    parseBridgeSubscription,
    runBridgeProxy,
} from "./bridge-relay.ts";

// Upper bound on one line arriving on the bridge socket. A bridge message is a
// handful of short identifiers plus an optional one-line summary, so a
// megabyte is already several orders of magnitude of headroom; anything past
// it is a producer that lost its newline, not a real event. Deliberately much
// smaller than codeharbord's 16 MiB transport cap: that one has to carry whole
// file contents, this one never does.
export const MAX_BRIDGE_LINE_BYTES = 1024 * 1024;

// Qt's QJsonDocument accepts at most 1024 nested containers, including the
// AgentEvent envelope. Deeper optional metadata must not hide the state event.
export const MAX_BRIDGE_JSON_DEPTH = 1024;

/**
 * Does one serialized JSONL message fit inside the relay's line bound?
 *
 * CALLER REQUIREMENT: pass the JSON text WITHOUT its trailing newline —
 * `JSON.stringify(message)`, never `` `${JSON.stringify(message)}\n` ``. The
 * newline is FRAMING and is deliberately outside the bound, because that is
 * what the input framer measures: it counts the bytes before the newline and
 * never the newline itself. Handing this the framed line makes the caller one
 * byte stricter than the relay, which is exactly the disagreement this function
 * exists to remove.
 *
 * Exported so a PRODUCER — the harness hooks, which serialize their own message
 * and write it themselves — can ask the relay whether a line will be accepted
 * instead of re-deriving the rule from MAX_BRIDGE_LINE_BYTES and its own idea
 * of the framing. Two copies of that arithmetic is precisely the drift this
 * predicate exists to prevent, and a producer that guesses wrong writes a line
 * the relay is guaranteed to refuse while reporting success. A producer that
 * gets `false` should shed its OPTIONAL fields (the free-text summary, the
 * metadata bag) and ask again, rather than write a line that cannot arrive.
 *
 * It also fixes an off-by-one the two in-tree callers disagreed on: makeStreamSink
 * measured the framed line (payload PLUS newline) against the same constant the
 * framer applies to the payload alone, so a message of exactly
 * MAX_BRIDGE_LINE_BYTES was refused on the way out and accepted on the way in.
 */
export function bridgeLineFits(payload: string): boolean {
    return Buffer.byteLength(payload) <= MAX_BRIDGE_LINE_BYTES;
}

const BRIDGE_LOCK_SUFFIX = ".lock";

function bridgeLockPath(socketPath: string): string {
    return `${socketPath}${BRIDGE_LOCK_SUFFIX}`;
}

function unlinkOwnedPath(filePath: string, identity: fs.Stats): void {
    try {
        const current = fs.lstatSync(filePath);
        if (current.dev === identity.dev && current.ino === identity.ino) {
            fs.unlinkSync(filePath);
        }
    } catch {
        // Already removed, replaced, or no longer accessible.
    }
}

function acquireBridgeLock(socketPath: string): () => void {
    const lockPath = bridgeLockPath(socketPath);
    for (;;) {
        const tempPath = `${lockPath}.${process.pid}.${Date.now()}.${Math.random()}`;
        try {
            fs.writeFileSync(tempPath, `${process.pid}\n`, {
                encoding: "utf8",
                flag: "wx",
                mode: 0o600,
            });
            try {
                const identity = fs.lstatSync(tempPath);
                // Publishing the fully written PID with linkSync is atomic:
                // no contender can observe an empty ownership record.
                fs.linkSync(tempPath, lockPath);
                fs.unlinkSync(tempPath);
                return () => unlinkOwnedPath(lockPath, identity);
            } catch (err) {
                try {
                    fs.unlinkSync(tempPath);
                } catch {
                    // The temporary file may already have been removed.
                }
                if (
                    !(err instanceof Error) ||
                    !("code" in err) ||
                    err.code !== "EEXIST"
                ) {
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
            const ownerText = fs.readFileSync(lockPath, "utf8").trim();
            ownerPid = Number.parseInt(ownerText, 10);
        } catch {
            throw new Error(
                `codeharbor-bridge: address already in use: ${socketPath}`,
            );
        }
        if (!Number.isInteger(ownerPid) || ownerPid <= 0) {
            throw new Error(
                `codeharbor-bridge: address already in use: ${socketPath}`,
            );
        }
        let ownerAlive = false;
        try {
            process.kill(ownerPid, 0);
            ownerAlive = true;
        } catch (probeError) {
            ownerAlive =
                probeError instanceof Error &&
                "code" in probeError &&
                probeError.code === "EPERM";
        }
        if (ownerAlive) {
            throw new Error(`codeharbor-bridge: address already in use: ${socketPath}`);
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

// How long a freshly accepted producer may stay completely silent. Every real
// producer is a harness hook that connects, writes its one line and ends
// (remote/src/hooks/oh-my-pi-hook.ts), so a connection that has delivered no
// bytes AT ALL within this window is abandoned or hung. Without the bound each
// such connection pins a file descriptor for the lifetime of the bridge, and
// enough of them make accept() fail with EMFILE — at which point no producer
// can connect again and the only symptom is that agent status quietly stops
// updating. The timer is disarmed by the FIRST byte, so a live producer that is
// later paused for output back-pressure is never mistaken for an idle one.
export const BRIDGE_HANDSHAKE_TIMEOUT_MS = 30_000;

// How long a producer may sit on an INCOMPLETE line — bytes delivered with no
// terminating newline yet — before the bridge gives up on it.
//
// BRIDGE_HANDSHAKE_TIMEOUT_MS above covers only the producer that sends nothing
// at all: it is disarmed by the first byte. A producer that writes one byte and
// then hangs used to be reclaimed by nothing whatsoever — it kept its file
// descriptor AND its buffered partial line in the framer for the whole life of
// the bridge process, with MAX_BRIDGE_CONNECTIONS merely capping how many such
// corpses could pile up before accept() stopped answering and agent status
// silently stopped updating for every harness, permanently.
//
// The timer is re-armed on every chunk, so a producer still making progress
// through a long line is never killed. And it is armed ONLY while a partial
// line is held, so the two legitimate reasons a connection is quiet are both
// untouched: a long-lived producer idling between complete lines, and one
// paused for output back-pressure (whose retained bytes live in the framer's
// pending queue, not in its partial-line buffer).
export const BRIDGE_PARTIAL_LINE_TIMEOUT_MS = 30_000;

// Ceiling on producer connections the bridge holds open at once; see the
// server.maxConnections assignment in startBridge for why a handshake timeout
// alone does not bound them.
export const MAX_BRIDGE_CONNECTIONS = 256;

// Format revision of the PRODUCER-to-relay message, i.e. the line a harness
// hook writes into the socket. The relay's own output already carries a version
// the desktop client checks strictly (CH_EVENT_VERSION on AgentEvent); this is
// the same protection for the other half of the path, which had none.
//
// The hook is the component most likely to be out of step: a user pastes its
// path into their assistant's configuration once, and that path may keep
// pointing at a months-old checkout for as long as the configuration lives. A
// future field rename would reach this relay as a message that parses, fails a
// guard, and produces nothing — indistinguishable from an event that simply
// carried no state change. With a declared revision the mismatch is a distinct,
// nameable condition instead.
//
// OPTIONAL ON THE WIRE, and that is a compatibility guarantee, not an
// oversight: every hook installed today sends no `version` at all, and an
// ABSENT version means "the current revision" forever. Only a message that
// declares a revision this relay does not implement is rejected. So raising
// this number is a decision to stop accepting explicitly-old producers, and it
// can never orphan an unversioned one.
export const BRIDGE_MESSAGE_VERSION = 1;

// Wire format for a single line arriving on the bridge socket. Every field is
// optional and `unknown`: this describes what a producer is SUPPOSED to send,
// and a decoded JSON object is free to omit or mistype any of it — which is
// what the guards in processBridgeLine are for.
interface BridgeMessage {
    version?: unknown;
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

    // An ABSENT version is the current revision — that is what keeps every hook
    // installed before this field existed working unchanged. A version that IS
    // declared must be one this relay implements; anything else is a producer
    // speaking a format whose fields this code would misread, so refuse it
    // rather than map it against the wrong shape.
    if (message.version !== undefined && message.version !== BRIDGE_MESSAGE_VERSION) {
        return null;
    }

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

        // The event NAME goes to the client verbatim, so it is normalized the
        // same way the adapters normalize the name they match on: producers are
        // shell hook configurations, and one interpolated variable leaves
        // "agent_start\r\n" here. Untrimmed, the adapter maps the state
        // correctly (it trims) while the client displays and logs the event
        // name with a carriage return glued to it.
        //
        // `||`, not `??`: a BLANK `type` is "this producer did not name its
        // event under `type`", not the event whose name is the empty string, so
        // it must fall through to the Claude Code key rather than shadow it.
        const nativeName = nativeString(native.type) || nativeString(native.hook);
        return makeEvent({
            harness: message.harness,
            devSessionId: message.devSessionId,
            terminalId: message.terminalId,
            state,
            event: nativeName || "unknown",
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
export interface BridgeLineFramer {
    feed(chunk: Buffer): void;
    /**
     * True while bytes of an unterminated line are buffered, i.e. the producer
     * started a line and has not finished it. The caller arms its partial-line
     * watchdog on exactly this condition; see BRIDGE_PARTIAL_LINE_TIMEOUT_MS.
     */
    partial(): boolean;
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
        partial() {
            return heldBytes > 0;
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


/** Encode once for either a direct sink or every reader of the shared relay. */
function encodeBridgeEvent(event: AgentEvent): string | null {
    let candidate = event;
    for (;;) {
        try {
            const payload = stringifyJson(candidate, MAX_BRIDGE_LINE_BYTES, MAX_BRIDGE_JSON_DEPTH);
            if (payload === undefined) throw new TypeError("Agent event is not JSON-serializable");
            return `${payload}\n`;
        } catch (err) {
            if (!(err instanceof JsonSizeLimitError) && !(err instanceof JsonDepthLimitError)) throw err;
            // Optional decoration must not hide the authoritative state event.
            if (candidate === event) candidate = { ...event };
            if (candidate.metadata !== undefined) {
                delete candidate.metadata;
                continue;
            }
            if (candidate.summary !== undefined) {
                delete candidate.summary;
                continue;
            }
            process.stderr.write(
                `codeharbor-bridge: dropping an event that exceeds the ${MAX_BRIDGE_LINE_BYTES}-byte ` +
                `or ${MAX_BRIDGE_JSON_DEPTH}-container frame bound\n`,
            );
            return null;
        }
    }
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
    let failed = false;
    // Keep idle producers too: after a complete event their watchdogs are
    // disarmed, so an output failure must explicitly close them.
    const sources = new Set<net.Socket>();
    const fail = (): void => {
        failed = true;
        for (const socket of sources) socket.destroy();
        sources.clear();
        paused.clear();
    };
    out.on("drain", () => {
        if (failed) return;
        const toResume = [...paused];
        // Clear before resuming. A resumed source may synchronously deliver
        // buffered input, and that write can fill the output again.
        paused.clear();
        for (const socket of toResume) {
            if (!socket.destroyed) socket.resume();
        }
    });
    out.on("error", fail);
    return (event, source) => {
        if (failed) {
            source.destroy();
            return;
        }
        if (!sources.has(source)) {
            sources.add(source);
            source.once("close", () => {
                sources.delete(source);
                paused.delete(source);
            });
        }
        // The line framer normally stops before invoking us again for a paused
        // source. Keep this guard for custom callers so a full output stream
        // can never be fed another event synchronously.
        if (paused.has(source)) return;
        let line: string | null;
        try {
            line = encodeBridgeEvent(event);
        } catch {
            source.destroy();
            return;
        }
        if (line === null) {
            source.destroy();
            return;
        }
        let ok: boolean;
        try {
            ok = out.write(line);
        } catch {
            fail();
            return;
        }
        if (failed || source.destroyed) return;
        if (!ok) {
            source.pause();
            paused.add(source);
        }
    };
}

/** Watchdog bounds for one accepted producer connection. */
export interface BridgeOptions {
    /** Overrides BRIDGE_HANDSHAKE_TIMEOUT_MS. */
    handshakeTimeoutMs?: number;
    /** Overrides BRIDGE_PARTIAL_LINE_TIMEOUT_MS. */
    partialLineTimeoutMs?: number;
    /** Internal shared-reader registration; producers do not use this path. */
    onSubscribe?: (socket: net.Socket, version: unknown) => boolean;
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
 *
 * `options` exists so the two per-connection watchdogs can be driven in a test
 * without waiting out their production defaults, which are deliberately tens of
 * seconds. Omitting it is exactly the production configuration.
 */
export async function startBridge(
    socketPath: string = resolveSocketPath(),
    sink?: EventSink,
    options: BridgeOptions = {},
): Promise<net.Server> {
    const handshakeTimeoutMs = options.handshakeTimeoutMs ?? BRIDGE_HANDSHAKE_TIMEOUT_MS;
    const partialLineTimeoutMs = options.partialLineTimeoutMs ?? BRIDGE_PARTIAL_LINE_TIMEOUT_MS;
    // Built ONCE, up front, rather than on the first event that needs it. The
    // sink is what installs the 'error' listener on the output stream, and an
    // output stream with no 'error' listener turns a failure into an uncaught
    // exception that kills the relay. In production the output is stdout, i.e.
    // an SSH channel the client can close at any moment — including before the
    // first event ever arrives, which is precisely the window a lazily built
    // sink left unguarded.
    const activeSink = sink ?? makeStreamSink(process.stdout);
    const server = net.createServer((socket) => {
        let subscribed = false;
        const lines = createBridgeLineFramer(
            (line) => {
                if (subscribed) {
                    socket.destroy();
                    return false;
                }
                if (options.onSubscribe && line.includes(BRIDGE_SUBSCRIBE_PROTOCOL)) {
                    const request = parseBridgeSubscription(line);
                    if (request) {
                        subscribed = true;
                        try {
                            options.onSubscribe(socket, request.version);
                        } catch {
                            socket.destroy();
                        }
                        return !socket.destroyed;
                    }
                }
                const event = processBridgeLine(line);
                if (event) {
                    try {
                        activeSink(event, socket);
                    } catch {
                        socket.destroy();
                        return false;
                    }
                }
                // A sink may destroy the producer (an oversized mapped event,
                // or a failed write). Stop parsing immediately: the rest of
                // this same chunk must not be relayed for a socket that has
                // already been torn down.
                return !socket.destroyed && !socket.isPaused();
            },
            () => {
                process.stderr.write(
                    `codeharbor-bridge: dropping a producer that exceeded the ${MAX_BRIDGE_LINE_BYTES}-byte frame bound\n`,
                );
                socket.destroy();
            },
        );
        const handshake = setTimeout(() => socket.destroy(), handshakeTimeoutMs);
        handshake.unref();
        // Armed only while the framer holds an unterminated line, re-armed on
        // every advance; see BRIDGE_PARTIAL_LINE_TIMEOUT_MS for why the
        // handshake timer above cannot cover this case.
        let partialWatch: NodeJS.Timeout | undefined;
        const watchPartialLine = (): void => {
            if (partialWatch !== undefined) {
                clearTimeout(partialWatch);
                partialWatch = undefined;
            }
            if (socket.destroyed || !lines.partial()) return;
            partialWatch = setTimeout(() => socket.destroy(), partialLineTimeoutMs);
            partialWatch.unref();
        };
        socket.once("data", () => clearTimeout(handshake));
        socket.on("data", (chunk: Buffer) => {
            if (subscribed) {
                socket.destroy();
                return;
            }
            lines.feed(chunk);
            if (subscribed && lines.partial()) socket.destroy();
            watchPartialLine();
        });
        socket.on("resume", () => {
            // Draining bytes retained during a back-pressure stall can leave a
            // fresh partial line behind, and no 'data' event follows to notice.
            lines.resume();
            watchPartialLine();
        });
        socket.on("close", () => {
            clearTimeout(handshake);
            if (partialWatch !== undefined) clearTimeout(partialWatch);
            lines.close();
        });
        socket.on("error", () => socket.destroy());
    });
    // Hard ceiling on producers held open at once, and a backstop rather than
    // the primary defence: the two per-connection watchdogs above reclaim a
    // producer that sends nothing (BRIDGE_HANDSHAKE_TIMEOUT_MS) and one that
    // stalls part-way through a line (BRIDGE_PARTIAL_LINE_TIMEOUT_MS), but
    // neither touches a producer that is merely idle between complete lines,
    // which is legitimate. Enough simultaneously idle connections would make
    // accept() fail with EMFILE — after which no producer can connect at all
    // and agent status silently stops updating, with no bound on the memory the
    // accepted sockets hold either. A real producer connects, writes one line
    // and ends within milliseconds, so 256 concurrent is orders of magnitude of
    // headroom; past it Node closes the newcomer immediately rather than
    // letting the accumulation take the whole relay down.
    server.maxConnections = MAX_BRIDGE_CONNECTIONS;
    fs.mkdirSync(path.dirname(socketPath), { recursive: true, mode: 0o700 });
    const releaseOwnedLock = acquireBridgeLock(socketPath);
    let lockReleased = false;
    let socketIdentity: fs.Stats | undefined;
    const cleanup = (): void => {
        if (socketIdentity) unlinkOwnedPath(socketPath, socketIdentity);
        releaseLock();
    };
    const releaseLock = (): void => {
        if (lockReleased) return;
        lockReleased = true;
        process.removeListener("exit", cleanup);
        releaseOwnedLock();
    };
    // Keep ownership cleanup with the listener, not the CLI wrapper: an old
    // listener must not unlink the paths of a replacement during a later exit.
    process.once("exit", cleanup);
    server.once("close", releaseLock);
    try {
        let existing: fs.Stats | undefined;
        try {
            existing = fs.lstatSync(socketPath);
        } catch {
            // No existing socket.
        }
        if (existing?.isSocket()) {
            const probeResult = Promise.withResolvers<boolean>();
            const probe = net.createConnection(socketPath);
            let settled = false;
            const finish = (isAlive: boolean, error?: Error): void => {
                if (settled) return;
                settled = true;
                probe.destroy();
                if (error) probeResult.reject(error);
                else probeResult.resolve(isAlive);
            };
            probe.setTimeout(1000, () => finish(
                false,
                new Error(`codeharbor-bridge: socket probe timed out; preserving ${socketPath}`),
            ));
            probe.on("connect", () => finish(true));
            probe.on("error", (error: NodeJS.ErrnoException) => {
                // Permission/resource failures say nothing about whether a
                // listener is alive. Never unlink its endpoint on uncertainty.
                const stale = error.code === "ECONNREFUSED" || error.code === "ENOENT";
                finish(false, stale ? undefined : error);
            });
            const alive = await probeResult.promise;
            if (alive) {
                throw new Error(
                    `codeharbor-bridge: address already in use: a live bridge is listening on ${socketPath}`,
                );
            }
            const current = fs.lstatSync(socketPath);
            if (!current.isSocket() || current.dev !== existing.dev || current.ino !== existing.ino) {
                throw new Error(`codeharbor-bridge: socket changed while probing ${socketPath}`);
            }
            fs.unlinkSync(socketPath);
        }
    } catch (err) {
        releaseLock();
        throw err;
    }
    await new Promise<void>((resolve, reject) => {
        const onError = (err: Error): void => {
            releaseLock();
            reject(err);
        };
        server.once("error", onError);
        server.listen(socketPath, () => {
            try {
                socketIdentity = fs.lstatSync(socketPath);
                fs.chmodSync(socketPath, 0o600);
            } catch (err) {
                server.removeListener("error", onError);
                releaseLock();
                server.close(() => reject(err));
                return;
            }
            server.removeListener("error", onError);
            server.on("error", (err: Error) => {
                process.stderr.write(`codeharbor-bridge: ${err.message}\n`);
            });
            resolve();
        });
    });
    return server;
}

async function runSharedBridgeRelay(socketPath: string): Promise<void> {
    let server: net.Server | undefined;
    let stopping = false;
    const connections = new Set<net.Socket>();
    const stopped = Promise.withResolvers<void>();
    const shutdown = (): void => {
        if (stopping) return;
        stopping = true;
        readers.close();
        for (const socket of connections) socket.destroy();
        if (server) server.close(() => stopped.resolve());
        else stopped.resolve();
    };
    const readers = new BridgeSubscribers(shutdown, { idleTimeoutMs: bridgeRelayIdleMs() });
    const signals = ["SIGHUP", "SIGINT", "SIGTERM"] as const;
    for (const signal of signals) process.on(signal, shutdown);
    try {
        server = await startBridge(socketPath, (event, source) => {
            const line = encodeBridgeEvent(event);
            if (line === null) source.destroy();
            else readers.broadcast(line);
        }, { onSubscribe: (socket, version) => readers.subscribe(socket, version) });
        if (stopping) {
            server.close();
            return;
        }
        server.on("connection", (socket) => {
            connections.add(socket);
            socket.once("close", () => connections.delete(socket));
        });
        process.stderr.write(`codeharbor-bridge relay listening on ${socketPath}\n`);
        await stopped.promise;
    } finally {
        readers.close();
        for (const socket of connections) socket.destroy();
        if (server?.listening) server.close();
        for (const signal of signals) process.removeListener(signal, shutdown);
    }
}

if (isEntrypoint(import.meta.url)) {
    const args = process.argv.slice(2);
    try {
        const socketPath = resolveSocketPath();
        if (args.length === 0) {
            await runBridgeProxy(socketPath, fileURLToPath(import.meta.url));
        } else if (args.length === 1 && args[0] === "--relay") {
            await runSharedBridgeRelay(socketPath);
        } else {
            process.stderr.write("usage: codeharbor-bridge [--relay]\n");
            process.exitCode = 2;
        }
    } catch (err) {
        process.stderr.write(`${err instanceof Error ? err.message : String(err)}\n`);
        process.exitCode = 1;
    }
}
