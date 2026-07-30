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
 * Start the bridge server on `socketPath`, relaying mapped events to `sink`
 * (defaults to stdout). Returns the listening server.
 */
export function startBridge(
    socketPath: string = resolveSocketPath(),
    sink: (event: AgentEvent) => void = (event) => process.stdout.write(`${JSON.stringify(event)}\n`),
): net.Server {
    const server = net.createServer((socket) => {
        const lines = readline.createInterface({ input: socket });
        lines.on("line", (line) => {
            const event = processBridgeLine(line);
            if (event) sink(event);
        });
        socket.on("error", () => socket.destroy());
    });
    server.on("error", (err) => {
        process.stderr.write(`codeharbor-bridge: ${err.message}\n`);
    });
    // Ensure the socket's parent directory exists (the ~/.cache fallback may
    // not, SPEC 6.3) and clear a stale socket left by a previous run. The
    // directory is private (0700) so an unrelated user cannot connect to the
    // socket even before the post-listen chmod tightens the socket itself.
    fs.mkdirSync(path.dirname(socketPath), { recursive: true, mode: 0o700 });
    try {
        fs.chmodSync(path.dirname(socketPath), 0o700);
    } catch {
        // Directory already existed with other perms we cannot change; the
        // 0600 socket chmod below is still the primary guard.
    }
    // Only remove a stale entry if it is actually a socket, so a mistyped path
    // pointing at a regular file is never clobbered (listen() will then fail).
    // Known limitation: a socket file with a LIVE bridge still listening on it
    // is indistinguishable from a stale one without an async connect probe, so
    // starting a second bridge on the same path orphans the first instead of
    // failing with EADDRINUSE. One bridge per host per user is the design.
    try {
        if (fs.lstatSync(socketPath).isSocket()) {
            fs.unlinkSync(socketPath);
        }
    } catch {
        // Nothing at socketPath (ENOENT); nothing to clear.
    }
    server.listen(socketPath, () => {
        // Restrict the socket to the owning user (0600); the default net.Server
        // socket honours only umask, which may leave it group/world-accessible.
        try {
            fs.chmodSync(socketPath, 0o600);
        } catch {
            // Best-effort: some platforms ignore socket permissions.
        }
    });
    return server;
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
    const socketPath = resolveSocketPath();
    const server = startBridge(socketPath);
    process.stderr.write(`codeharbor-bridge listening on ${socketPath}\n`);
    // Without a handler, SIGINT/SIGTERM terminate the process outright and
    // leave the socket file behind, so the next run has to treat a socket that
    // may still look live as stale. Close the listener and remove the socket
    // ourselves, then exit immediately: waiting for open connections to drain
    // would let one stuck producer block shutdown forever.
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
}
