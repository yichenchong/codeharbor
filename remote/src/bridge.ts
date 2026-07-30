// codeharbor-bridge (SPEC 6.3): a small remote helper that listens on a Unix
// socket for harness-native events, maps them to CodeHarbor agent states via
// the adapter registry, and relays validated AgentEvents as JSONL on stdout.
// SSH forwards that stream to the client's AgentStatusMonitor.
//
// Harness adapters must never block or break the agent if CodeHarbor is
// unavailable (SPEC 6.4): a broken line is dropped, never thrown.

import net from "node:net";
import readline from "node:readline";
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

// Wire format for a single line arriving on the bridge socket.
interface BridgeMessage {
    harness: unknown;
    devSessionId: unknown;
    terminalId: unknown;
    native: unknown;
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

    let decoded: BridgeMessage;
    try {
        decoded = JSON.parse(trimmed) as BridgeMessage;
    } catch {
        return null;
    }

    if (
        !isHarness(decoded.harness) ||
        // Non-blank identifiers, checked HERE because this relay builds its
        // events with makeEvent and never runs them through validateEvent: this
        // is the only gate on the socket-to-stdout path. An empty id yields a
        // structurally valid event filed under a Dev Session that does not exist
        // (see isEventIdentifier). The `typeof` half stays because it is what
        // narrows the field to `string` for the makeEvent call below.
        typeof decoded.devSessionId !== "string" ||
        typeof decoded.terminalId !== "string" ||
        !isEventIdentifier(decoded.devSessionId) ||
        !isEventIdentifier(decoded.terminalId) ||
        // An array would pass a bare `typeof === "object"` check and then read
        // back as a native event with no `type`, i.e. a silent no-op.
        !isPlainObject(decoded.native)
    ) {
        return null;
    }

    const adapter = adapterFor(decoded.harness);
    if (!adapter) return null;

    const native = decoded.native as Record<string, unknown>;
    const state = adapter.map(native);
    if (state === null) return null;

    const nativeName = native.type ?? native.hook;
    return makeEvent({
        harness: decoded.harness,
        devSessionId: decoded.devSessionId,
        terminalId: decoded.terminalId,
        state,
        event: typeof nativeName === "string" ? nativeName : "unknown",
        summary: typeof decoded.summary === "string" ? decoded.summary : undefined,
        metadata: isPlainObject(decoded.metadata) ? decoded.metadata : undefined,
    });
}

/**
 * A sink for mapped events. Receives the source socket so it can apply
 * back-pressure: when the output stalls the sink pauses the socket the event
 * arrived on and resumes it once the output drains (RR24).
 */
export type EventSink = (event: AgentEvent, source: net.Socket) => void;

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
    out.on("drain", () => {
        for (const socket of paused) socket.resume();
        paused.clear();
    });
    return (event, source) => {
        const ok = out.write(`${JSON.stringify(event)}\n`);
        if (!ok) {
            source.pause();
            paused.add(source);
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
    sink: EventSink = makeStreamSink(process.stdout),
): Promise<net.Server> {
    const server = net.createServer((socket) => {
        const lines = readline.createInterface({ input: socket });
        lines.on("line", (line) => {
            const event = processBridgeLine(line);
            if (event) sink(event, socket);
        });
        socket.on("error", () => socket.destroy());
    });
    // Ensure the socket's parent directory exists (the ~/.cache fallback may
    // not, SPEC 6.3) and is private (0700) so an unrelated user cannot connect
    // to the socket even before the post-listen chmod tightens it.
    fs.mkdirSync(path.dirname(socketPath), { recursive: true, mode: 0o700 });
    try {
        fs.chmodSync(path.dirname(socketPath), 0o700);
    } catch {
        // Directory already existed with other perms we cannot change; the
        // 0600 socket chmod below is still the primary guard.
    }
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
            probe.on("connect", () => {
                probe.destroy();
                resolve(true);
            });
            probe.on("error", () => {
                probe.destroy();
                resolve(false);
            });
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
            server.removeListener("error", onError);
            // Restrict the socket to the owning user (0600); the default
            // net.Server socket honours only umask, which may leave it
            // group/world-accessible.
            try {
                fs.chmodSync(socketPath, 0o600);
            } catch {
                // Best-effort: some platforms ignore socket permissions.
            }
            resolve();
        });
    });
    // Report post-listen server errors without crashing: a server that errors
    // after it is bound should not take the process down.
    server.on("error", (err) => {
        process.stderr.write(`codeharbor-bridge: ${err.message}\n`);
    });
    return server;
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
    const socketPath = resolveSocketPath();
    try {
        const server = await startBridge(socketPath);
        process.stderr.write(`codeharbor-bridge listening on ${socketPath}\n`);
        // Without a handler, SIGINT/SIGTERM terminate the process outright and
        // leave the socket file behind, so the next run has to treat a socket
        // that may still look live as stale. Close the listener and remove the
        // socket ourselves, then exit immediately: waiting for open connections
        // to drain would let one stuck producer block shutdown forever.
        const shutdown = (): void => {
            server.close();
            try {
                if (fs.lstatSync(socketPath).isSocket()) fs.unlinkSync(socketPath);
            } catch {
                // Already unlinked by server.close(), or never created.
            }
            process.exit(0);
        };
        process.on("SIGINT", shutdown);
        process.on("SIGTERM", shutdown);
    } catch (err) {
        // A live bridge already owns the socket (RR12), or listen failed.
        process.stderr.write(`${(err as Error).message}\n`);
        process.exit(1);
    }
}
