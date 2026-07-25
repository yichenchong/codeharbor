import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { adapterFor, ohMyPiAdapter } from "../src/adapters/index.ts";
import { processBridgeLine } from "../src/bridge.ts";
import { dispatch, handleLine, RPC_METHOD_NOT_FOUND, RPC_SCHEMA_VERSION } from "../src/codeharbord.ts";

// server.info reads its serverId from the workspace database, so point the
// lazily-opened default connection at a throwaway file instead of the real
// ~/.local/share location. Must happen before the first dispatch, not at
// import time of workspace.ts, which opens nothing until first use.
const dbDir = mkdtempSync(path.join(os.tmpdir(), "codeharbord-adapters-"));
process.env.CODEHARBOR_DB = path.join(dbDir, "codeharbor.sqlite");
process.on("exit", () => rmSync(dbDir, { recursive: true, force: true }));

test("oh-my-pi adapter maps the SPEC 6.5 table", () => {
    assert.equal(ohMyPiAdapter.map({ type: "session_start" }), "starting");
    assert.equal(ohMyPiAdapter.map({ type: "agent_start" }), "running");
    assert.equal(ohMyPiAdapter.map({ type: "tool_call", tool: "ask" }), "waiting_input");
    assert.equal(ohMyPiAdapter.map({ type: "tool_call", tool: "read" }), "running");
    assert.equal(ohMyPiAdapter.map({ type: "tool_result", tool: "ask" }), "running");
    assert.equal(ohMyPiAdapter.map({ type: "agent_end" }), "idle_unseen");
    assert.equal(ohMyPiAdapter.map({ type: "settled" }), "idle_unseen");
    assert.equal(ohMyPiAdapter.map({ type: "session_shutdown" }), "stopped");
    assert.equal(ohMyPiAdapter.map({ type: "anything", error: true }), "error");
    assert.equal(ohMyPiAdapter.map({ type: "unrecognized" }), null);
});

test("registry resolves known harnesses and skips generic", () => {
    assert.ok(adapterFor("oh-my-pi"));
    assert.ok(adapterFor("pi"));
    assert.ok(adapterFor("claude-code"));
    assert.equal(adapterFor("generic"), undefined);
});

test("processBridgeLine builds a validated event from a native message", () => {
    const line = JSON.stringify({
        harness: "oh-my-pi",
        devSessionId: "sess-1",
        terminalId: "term-1",
        native: { type: "tool_call", tool: "ask" },
        metadata: { toolName: "ask" },
    });
    const event = processBridgeLine(line);
    assert.ok(event);
    assert.equal(event.state, "waiting_input");
    assert.equal(event.event, "tool_call");
    assert.equal(event.devSessionId, "sess-1");
    assert.deepEqual(event.metadata, { toolName: "ask" });
});

test("processBridgeLine drops malformed, unknown-harness, and no-op lines", () => {
    assert.equal(processBridgeLine(""), null);
    assert.equal(processBridgeLine("{bad json"), null);
    assert.equal(
        processBridgeLine(JSON.stringify({ harness: "nope", devSessionId: "s", terminalId: "t", native: {} })),
        null,
    );
    assert.equal(
        processBridgeLine(JSON.stringify({ harness: "generic", devSessionId: "s", terminalId: "t", native: {} })),
        null,
    );
    assert.equal(
        processBridgeLine(JSON.stringify({ harness: "oh-my-pi", devSessionId: "s", terminalId: "t", native: { type: "noop" } })),
        null,
    );
});

// The result of one server.info round-trip, narrowed rather than cast: dispatch
// returns a success-or-error union and the test asserts on the success arm.
async function serverInfo(id: number): Promise<Record<string, unknown>> {
    const response = await dispatch({ jsonrpc: "2.0", id, method: "server.info" });
    assert.ok(response && "result" in response);
    const { result } = response;
    assert.ok(result && typeof result === "object");
    return result as Record<string, unknown>;
}

test("codeharbord dispatch answers introspection and rejects unknown methods", async () => {
    const info = await serverInfo(1);
    // The three original fields stay byte-compatible; serverId is additive.
    assert.equal(info.name, "codeharbord");
    assert.equal(info.version, "0.1.0");
    assert.equal(info.schemaVersion, RPC_SCHEMA_VERSION);
    assert.deepEqual(Object.keys(info).sort(), ["name", "schemaVersion", "serverId", "version"]);

    const serverId = info.serverId;
    assert.equal(typeof serverId, "string");
    assert.match(
        String(serverId),
        /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/,
    );

    // Repeated calls report the same identity, never a per-call mint.
    assert.equal((await serverInfo(2)).serverId, serverId);

    const ping = await dispatch({ jsonrpc: "2.0", id: "a", method: "ping" });
    assert.deepEqual(ping, { jsonrpc: "2.0", id: "a", result: { pong: true } });

    const missing = await dispatch({ jsonrpc: "2.0", id: 3, method: "does.not.exist" });
    assert.ok(missing);
    assert.equal("error" in missing && missing.error.code, RPC_METHOD_NOT_FOUND);
});

test("codeharbord handleLine reports parse errors", async () => {
    const res = await handleLine("{not valid");
    assert.ok(res);
    assert.ok("error" in res);
    assert.equal(res.id, null);
});
