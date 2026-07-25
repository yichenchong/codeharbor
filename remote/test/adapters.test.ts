import { test } from "node:test";
import assert from "node:assert/strict";
import { adapterFor, ohMyPiAdapter } from "../src/adapters/index.ts";
import { processBridgeLine } from "../src/bridge.ts";
import { dispatch, handleLine, RPC_METHOD_NOT_FOUND, RPC_SCHEMA_VERSION } from "../src/codeharbord.ts";

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

test("codeharbord dispatch answers introspection and rejects unknown methods", async () => {
    const info = await dispatch({ jsonrpc: "2.0", id: 1, method: "server.info" });
    assert.deepEqual(info, {
        jsonrpc: "2.0",
        id: 1,
        result: { name: "codeharbord", version: "0.1.0", schemaVersion: RPC_SCHEMA_VERSION },
    });

    const ping = await dispatch({ jsonrpc: "2.0", id: "a", method: "ping" });
    assert.deepEqual(ping, { jsonrpc: "2.0", id: "a", result: { pong: true } });

    const missing = await dispatch({ jsonrpc: "2.0", id: 2, method: "does.not.exist" });
    assert.ok(missing);
    assert.equal("error" in missing && missing.error.code, RPC_METHOD_NOT_FOUND);
});

test("codeharbord handleLine reports parse errors", async () => {
    const res = await handleLine("{not valid");
    assert.ok(res);
    assert.ok("error" in res);
    assert.equal(res.id, null);
});
