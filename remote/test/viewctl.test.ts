import { test } from "node:test";
import assert from "node:assert/strict";
import { Writable } from "node:stream";
import { mkdtempSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";

import { startControlListener } from "../src/control.ts";
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
