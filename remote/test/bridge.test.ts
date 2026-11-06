import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { startBridge } from "../src/bridge.ts";

test("stale lock reclamation preserves a replacement published during the owner probe", async (t) => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ch-bridge-lock-"));
    const socketPath = path.join(dir, "events.sock");
    const lockPath = `${socketPath}.lock`;
    const replacement = path.join(dir, "replacement.lock");
    fs.writeFileSync(lockPath, `${process.pid}\n`);
    fs.writeFileSync(replacement, `${process.pid}\n`);
    const replacementIdentity = fs.lstatSync(replacement);
    const kill = process.kill;
    let replaced = false;
    let server: net.Server | undefined;
    // The first probe represents a dead owner. Before it returns, a competing
    // starter publishes its live replacement; the next probe uses our real PID.
    t.mock.method(process, "kill", (pid: number, signal?: NodeJS.Signals | number): true => {
        if (!replaced && pid === process.pid && signal === 0) {
            replaced = true;
            fs.renameSync(replacement, lockPath);
            throw Object.assign(new Error("previous owner has exited"), { code: "ESRCH" });
        }
        return kill(pid, signal);
    });
    try {
        await assert.rejects(async () => {
            server = await startBridge(socketPath, () => {});
        }, /address already in use/);
        assert.equal(replaced, true);
        const current = fs.lstatSync(lockPath);
        assert.equal(current.dev, replacementIdentity.dev);
        assert.equal(current.ino, replacementIdentity.ino);
        assert.equal(fs.existsSync(socketPath), false);
    } finally {
        if (server) {
            const closed = Promise.withResolvers<void>();
            server.close(() => closed.resolve());
            await closed.promise;
        }
        fs.rmSync(dir, { recursive: true, force: true });
    }
});
