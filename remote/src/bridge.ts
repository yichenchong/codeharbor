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
import { adapterFor } from "./adapters/index.ts";
import {
    isHarness,
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
        typeof decoded.devSessionId !== "string" ||
        typeof decoded.terminalId !== "string" ||
        typeof decoded.native !== "object" ||
        decoded.native === null
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
        metadata:
            typeof decoded.metadata === "object" && decoded.metadata !== null
                ? (decoded.metadata as Record<string, unknown>)
                : undefined,
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

if (import.meta.url === `file://${process.argv[1]}`) {
    const socketPath = resolveSocketPath();
    startBridge(socketPath);
    process.stderr.write(`codeharbor-bridge listening on ${socketPath}\n`);
}
