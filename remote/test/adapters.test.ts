import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import {
    adapterFor,
    claudeCodeAdapter,
    ohMyPiAdapter,
    piAdapter,
    type NativeEvent,
} from "../src/adapters/index.ts";
import { processBridgeLine } from "../src/bridge.ts";
import {
    dispatch,
    handleLine,
    RPC_INVALID_PARAMS,
    RPC_INVALID_REQUEST,
    RPC_METHOD_NOT_FOUND,
    RPC_SCHEMA_VERSION,
} from "../src/codeharbord.ts";

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
    // A blank identifier is dropped HERE too: the bridge builds its events with
    // makeEvent and never calls validateEvent, so this is the only gate between
    // a misconfigured producer and an event the client files under a Dev Session
    // that does not exist.
    assert.equal(
        processBridgeLine(JSON.stringify({ harness: "oh-my-pi", devSessionId: "", terminalId: "t", native: { type: "agent_start" } })),
        null,
    );
    assert.equal(
        processBridgeLine(JSON.stringify({ harness: "oh-my-pi", devSessionId: "s", terminalId: "  ", native: { type: "agent_start" } })),
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

test("claude-code adapter maps its lifecycle hook names", () => {
    assert.equal(claudeCodeAdapter.map({ hook: "SessionStart" }), "starting");
    assert.equal(claudeCodeAdapter.map({ hook: "UserPromptSubmit" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "PreToolUse" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "PostToolUse" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "Notification" }), "waiting_input");
    assert.equal(claudeCodeAdapter.map({ hook: "Stop" }), "idle_unseen");
    assert.equal(claudeCodeAdapter.map({ hook: "SubagentStop" }), "idle_unseen");
    assert.equal(claudeCodeAdapter.map({ hook: "SessionEnd" }), "stopped");
    assert.equal(claudeCodeAdapter.map({ hook: "Anything", error: true }), "error");
    assert.equal(claudeCodeAdapter.map({ hook: "Unrecognized" }), null);
    // Claude Code names its events under `hook`; `type` is the Oh My Pi / Pi
    // key, so a type-keyed event is not a Claude Code event and maps to nothing.
    assert.equal(claudeCodeAdapter.map({ type: "SessionStart" }), null);
});

// pi.ts documents itself as "currently identical" to oh-my-pi.ts. That claim is
// only true while nobody edits one file alone, which is exactly what this
// asserts — including the no-op and error cases, not just the happy path.
test("pi adapter mirrors the oh-my-pi mapping", () => {
    const natives: NativeEvent[] = [
        { type: "session_start" },
        { type: "agent_start" },
        { type: "tool_call", tool: "ask" },
        { type: "tool_call", tool: "read" },
        { type: "tool_result", tool: "ask" },
        { type: "tool_result", tool: "read" },
        { type: "agent_end" },
        { type: "settled" },
        { type: "session_shutdown" },
        { type: "error" },
        { type: "session_start", error: true },
        { type: "unrecognized" },
        {},
    ];
    for (const native of natives) {
        assert.equal(
            piAdapter.map(native),
            ohMyPiAdapter.map(native),
            `pi and oh-my-pi disagree on ${JSON.stringify(native)}`,
        );
    }
    assert.equal(piAdapter.harness, "pi");
});

// JSON-RPC 2.0 conformance for the request envelope. Every rejection must be a
// response with id null: the spec requires null whenever the id could not be
// determined, and a client that cannot match a reply to a call would otherwise
// leak the pending call forever.
test("dispatch rejects anything that is not a JSON-RPC 2.0 request object", async () => {
    const malformed: unknown[] = [
        {},
        { jsonrpc: "1.0", id: 1, method: "ping" },
        { jsonrpc: "2.0", id: 1 },
        { jsonrpc: "2.0", id: 1, method: 42 },
        { jsonrpc: "2.0", id: true, method: "ping" },
        // Batches (an array of requests) are deliberately unsupported.
        [{ jsonrpc: "2.0", id: 1, method: "ping" }],
        "ping",
        null,
    ];
    for (const value of malformed) {
        const response = await dispatch(value);
        assert.ok(
            response !== null && "error" in response,
            `expected an error response for ${JSON.stringify(value)}`,
        );
        assert.equal(response.error.code, RPC_INVALID_REQUEST);
        assert.equal(response.id, null);
    }
});

test("dispatch echoes a null id and rejects non-structured params", async () => {
    // null is a legal JSON-RPC id and must be echoed back, not mistaken for the
    // absent id that marks a notification.
    assert.deepEqual(await dispatch({ jsonrpc: "2.0", id: null, method: "ping" }), {
        jsonrpc: "2.0",
        id: null,
        result: { pong: true },
    });

    // params must be a structured value (object or array), never a primitive.
    const primitive = await dispatch({ jsonrpc: "2.0", id: 7, method: "ping", params: 5 });
    assert.ok(primitive !== null && "error" in primitive);
    assert.equal(primitive.error.code, RPC_INVALID_PARAMS);
    assert.equal(primitive.id, 7);

    // An unknown method outranks bad params: the client is told the method is
    // gone rather than sent to fix arguments for a method that never existed.
    const missing = await dispatch({ jsonrpc: "2.0", id: 8, method: "no.such", params: 5 });
    assert.ok(missing !== null && "error" in missing);
    assert.equal(missing.error.code, RPC_METHOD_NOT_FOUND);

    // An array IS a structured value, so it reaches the handler.
    const array = await dispatch({ jsonrpc: "2.0", id: 9, method: "ping", params: [] });
    assert.ok(array !== null && "result" in array);

    // A notification carrying bad params stays silent: notifications never get
    // a response, not even an error one.
    assert.equal(await dispatch({ jsonrpc: "2.0", method: "ping", params: 5 }), null);
});

test("handleLine answers a batch line with a single Invalid Request", async () => {
    const response = await handleLine('[{"jsonrpc":"2.0","id":1,"method":"ping"}]');
    assert.ok(response !== null && "error" in response);
    assert.equal(response.error.code, RPC_INVALID_REQUEST);
    assert.equal(response.id, null);
});
