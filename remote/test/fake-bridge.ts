// A bridge stand-in for tests that need a REAL producer to write to a REAL
// Unix socket: the shipped producers connect, write one JSONL line and close,
// and a stub that replaces that transport would stop defending the part of the
// chain that has actually broken before (a producer nothing ever invoked, a
// line nobody was listening for). Shared by the extension unit tests and the
// live `omp` test so the two agree on what the bridge end looks like.

import net from "node:net";
import os from "node:os";
import fs from "node:fs";
import path from "node:path";

/**
 * A bridge stand-in: a real Unix socket listener that records every JSONL line
 * a producer writes, and counts connections so a test can assert that NOTHING
 * connected at all.
 */
export interface FakeBridge {
    dir: string;
    socketPath: string;
    lines: string[];
    connections: number;
    waitForLines(count: number): Promise<void>;
    close(): Promise<void>;
}

export async function startFakeBridge(): Promise<FakeBridge> {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ch-omp-ext-"));
    // The same name resolveSocketPath() derives from XDG_RUNTIME_DIR, so a
    // producer handed this directory in its environment lands here.
    const socketPath = path.join(dir, "codeharbor.sock");
    // Awaited on arrival, never on a duration: the tests assert on lines the
    // producer wrote, and a sleep long enough to be reliable under load is both
    // slower and worse at saying what actually failed.
    let waiting: { count: number; resolve: () => void } | null = null;
    const bridge: FakeBridge = {
        dir,
        socketPath,
        lines: [],
        connections: 0,
        waitForLines(count) {
            if (bridge.lines.length >= count) return Promise.resolve();
            const { promise, resolve } = Promise.withResolvers<void>();
            waiting = { count, resolve };
            return promise;
        },
        close() {
            const { promise, resolve } = Promise.withResolvers<void>();
            server.close(() => {
                fs.rmSync(dir, { recursive: true, force: true });
                resolve();
            });
            return promise;
        },
    };
    const server = net.createServer((socket) => {
        bridge.connections += 1;
        let buffer = "";
        socket.on("data", (chunk) => {
            buffer += chunk.toString("utf8");
            let newline = buffer.indexOf("\n");
            while (newline >= 0) {
                bridge.lines.push(buffer.slice(0, newline));
                buffer = buffer.slice(newline + 1);
                newline = buffer.indexOf("\n");
            }
            if (waiting !== null && bridge.lines.length >= waiting.count) {
                waiting.resolve();
                waiting = null;
            }
        });
    });
    const listening = Promise.withResolvers<void>();
    server.listen(socketPath, listening.resolve);
    await listening.promise;
    return bridge;
}
