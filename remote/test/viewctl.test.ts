import { test } from "node:test";
import assert from "node:assert/strict";
import { Writable } from "node:stream";
import { mkdtempSync, rmSync, symlinkSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { once } from "node:events";

import { MAX_CONTROL_LINE_BYTES, startControlListener } from "../src/control.ts";
import { main } from "../src/tools/viewctl.ts";

function capture() {
    let text = "";
    const stream = new Writable({
        write(chunk, _encoding, callback) {
            text += chunk.toString();
            callback();
        },
    });
    return { stream, text: () => text };
}

test("missing, invalid and operation-inappropriate options are usage errors", async () => {
    const cases = [
        ["open"],
        ["split"],
        ["split", "--orientation", "diagonal"],
        ["close", "--url", "README.md"],
        ["list", "--pane", "viewer-1"],
        ["reload", "--new-pane"],
        ["focus", "--kind", "editor"],
        ["open", "--orientation", "horizontal"],
        ["open", "--url"],
    ];
    for (const argv of cases) {
        const out = capture();
        const err = capture();
        assert.equal(await main(argv, {}, out.stream, err.stream), 2, argv.join(" "));
        assert.equal(out.text(), "");
        assert.match(err.text(), /usage: codeharbor-view/);
    }
});

test("valid CLI commands retain applied and refused exit statuses", async () => {
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-viewctl-"));
    const socketPath = path.join(dir, "control.sock");
    const listener = await startControlListener((command) => {
        queueMicrotask(() => listener.settle(command.op === "open"
            ? { commandId: command.commandId, ok: true, data: { paneId: "viewer-2" } }
            : { commandId: command.commandId, ok: false, error: { code: "unknown_pane", message: "No such pane" } }));
        return true;
    }, socketPath);
    const env = {
        OMP_DEV_SESSION_ID: "d",
        OMP_TERMINAL_ID: "t",
        CODEHARBOR_CONTROL_SOCKET: socketPath,
    };
    try {
        const out = capture();
        const err = capture();
        assert.equal(await main(["open", "--url", "README.md", "--new-pane"], env, out.stream, err.stream), 0);
        assert.deepEqual(JSON.parse(out.text()), { paneId: "viewer-2" });
        assert.equal(err.text(), "");
        const refusedOut = capture();
        const refusedErr = capture();
        assert.equal(await main(["close", "--pane", "missing"], env, refusedOut.stream, refusedErr.stream), 1);
        assert.deepEqual(JSON.parse(refusedOut.text()), { code: "unknown_pane", message: "No such pane" });
        assert.match(refusedErr.text(), /No such pane/);
    } finally {
        listener.close();
        rmSync(dir, { recursive: true, force: true });
    }
});

test("a symlinked viewer CLI still runs and reports refusals (RI2)", () => {
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-viewctl-link-"));
    try {
        const link = path.join(dir, "codeharbor-view.ts");
        symlinkSync(fileURLToPath(new URL("../src/tools/viewctl.ts", import.meta.url)), link);
        const result = spawnSync(process.execPath, [link, "list"], {
            env: {},
            encoding: "utf8",
            timeout: 10_000,
        });
        assert.equal(result.error, undefined);
        assert.equal(result.status, 1, result.stderr);
        assert.equal(JSON.parse(result.stdout).code, "bad_request");
        assert.match(result.stderr, /OMP_DEV_SESSION_ID/);
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("broken output streams preserve CLI exit semantics without uncaught errors", async () => {
    const broken = (): Writable => new Writable({
        write(_chunk, _encoding, callback) {
            const error = Object.assign(new Error("broken pipe"), { code: "EPIPE" });
            callback(error);
        },
    });
    const out = capture();
    assert.equal(await main([], {}, out.stream, broken()), 2);
    assert.equal(out.text(), "");
    assert.equal(await main(["list"], {}, broken(), broken()), 1);

    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-viewctl-output-"));
    const socketPath = path.join(dir, "control.sock");
    const listener = await startControlListener((command) => {
        queueMicrotask(() => listener.settle({ commandId: command.commandId, ok: true }));
        return true;
    }, socketPath);
    try {
        assert.equal(await main(["list"], {
            OMP_DEV_SESSION_ID: "d",
            OMP_TERMINAL_ID: "t",
            CODEHARBOR_CONTROL_SOCKET: socketPath,
        }, broken(), capture().stream), 1);
    } finally {
        listener.close();
        rmSync(dir, { recursive: true, force: true });
    }
});

test("the CLI emits a valid bounded deeply nested viewer result (NIN6)", { timeout: 10000 }, async () => {
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-viewctl-deep-"));
    const socketPath = path.join(dir, "control.sock");
    const depth = 10000;
    const data = JSON.parse('{"n":'.repeat(depth) + "0" + "}".repeat(depth)) as Record<string, unknown>;
    const listener = await startControlListener((command) => {
        queueMicrotask(() => listener.settle({ commandId: command.commandId, ok: true, data }));
        return true;
    }, socketPath);
    const child = spawn(process.execPath, [
        fileURLToPath(new URL("../src/tools/viewctl.ts", import.meta.url)), "list",
    ], {
        env: {
            ...process.env,
            OMP_DEV_SESSION_ID: "d", OMP_TERMINAL_ID: "t",
            CODEHARBOR_CONTROL_SOCKET: socketPath,
        },
        timeout: 5000,
    });
    let output = "";
    let errors = "";
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => { output += chunk; });
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk: string) => { errors += chunk; });
    try {
        const [status] = await once(child, "close");
        assert.equal(status, 0, errors);
        assert.equal(errors, "");
        assert.ok(Buffer.byteLength(output) <= MAX_CONTROL_LINE_BYTES + 1);
        let value: unknown = JSON.parse(output);
        for (let i = 0; i < depth; i += 1) value = (value as Record<string, unknown>).n;
        assert.equal(value, 0);
    } finally {
        child.kill();
        listener.close();
        rmSync(dir, { recursive: true, force: true });
    }
});
