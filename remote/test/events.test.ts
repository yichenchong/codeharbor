import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import {
    AGENT_STATES,
    CH_EVENT_VERSION,
    makeEvent,
    parseEventLine,
    validateEvent,
    resolveSocketPath,
} from "../src/events.ts";

test("makeEvent stamps version and defaults timestamp", () => {
    const e = makeEvent({
        harness: "oh-my-pi",
        devSessionId: "s1",
        terminalId: "t1",
        state: "running",
        event: "agent_start",
    });
    assert.equal(e.version, CH_EVENT_VERSION);
    assert.equal(e.harness, "oh-my-pi");
    assert.equal(e.state, "running");
    assert.match(e.timestamp, /^\d{4}-\d{2}-\d{2}T/);
    assert.equal(e.summary, undefined);
    assert.equal(e.metadata, undefined);
});

test("makeEvent preserves optional summary and metadata", () => {
    const e = makeEvent({
        harness: "pi",
        devSessionId: "s1",
        terminalId: "t1",
        state: "waiting_input",
        event: "ask_started",
        summary: "waiting",
        metadata: { toolName: "ask" },
    });
    assert.equal(e.summary, "waiting");
    assert.deepEqual(e.metadata, { toolName: "ask" });
});

test("validateEvent accepts a well-formed event and rejects bad ones", () => {
    const good = makeEvent({
        harness: "claude-code",
        devSessionId: "s",
        terminalId: "t",
        state: "idle",
        event: "Stop",
    });
    assert.equal(validateEvent(good), true);

    assert.equal(validateEvent({ ...good, version: 2 }), false);
    assert.equal(validateEvent({ ...good, state: "bogus" }), false);
    assert.equal(validateEvent({ ...good, harness: "unknown-harness" }), false);
    // `metadata` is a JSON object; an array is not one, even though `typeof`
    // says "object" for both.
    assert.equal(validateEvent({ ...good, metadata: [] }), false);
    assert.equal(validateEvent({ ...good, metadata: { toolName: "ask" } }), true);
    assert.equal(validateEvent(null), false);
    assert.equal(validateEvent("string"), false);
});

test("parseEventLine round-trips valid JSONL and rejects junk", () => {
    const e = makeEvent({
        harness: "oh-my-pi",
        devSessionId: "s",
        terminalId: "t",
        state: "starting",
        event: "session_start",
    });
    const line = JSON.stringify(e);
    assert.deepEqual(parseEventLine(line), e);

    assert.equal(parseEventLine(""), null);
    assert.equal(parseEventLine("   "), null);
    assert.equal(parseEventLine("{not json"), null);
    assert.equal(parseEventLine('{"version":1}'), null);
});

// An empty identifier is not a harmless field: the event is otherwise
// well-formed, so the bridge relays it and the client accepts it, files it under
// a Dev Session that does not exist, and — for the notifying states — raises a
// desktop notification whose body is little more than " / ". Reject it here, at
// the edge, where the producer can still be blamed.
test("validateEvent rejects a blank Dev Session or terminal identifier", () => {
    const good = makeEvent({
        harness: "oh-my-pi",
        devSessionId: "sess-1",
        terminalId: "term-1",
        state: "waiting_input",
        event: "tool_call",
    });
    assert.equal(validateEvent(good), true);

    assert.equal(validateEvent({ ...good, devSessionId: "" }), false);
    assert.equal(validateEvent({ ...good, terminalId: "" }), false);
    // Whitespace-only is the same failure wearing a costume: it names no row.
    assert.equal(validateEvent({ ...good, devSessionId: "   " }), false);
    assert.equal(validateEvent({ ...good, terminalId: "\t\n" }), false);
    assert.equal(validateEvent({ ...good, devSessionId: undefined }), false);

    // The same rule applies through the JSONL entry point the relay uses.
    assert.equal(parseEventLine(JSON.stringify({ ...good, terminalId: "" })), null);
});

test("resolveSocketPath prefers XDG_RUNTIME_DIR, else falls back to cache", () => {
    assert.equal(
        resolveSocketPath({ XDG_RUNTIME_DIR: "/run/user/1000" }),
        "/run/user/1000/codeharbor.sock",
    );
    const fallback = resolveSocketPath({});
    assert.match(fallback, /\.cache\/codeharbor\/events\.sock$/);
});

// Cross-language drift gate, the same shape as rpc-mirror.test.ts: the agent
// wire-state tokens (SPEC 6.4) exist twice — AGENT_STATES here and
// agentStateFromWireStrict() in src/agent/AgentEvent.h — and neither language's
// own suite can see the other copy. A token added or renamed on one side alone
// would surface only at runtime, as a live event the client silently drops.
test("agent wire-state tokens match src/agent/AgentEvent.h", () => {
    const headerPath = fileURLToPath(new URL("../../src/agent/AgentEvent.h", import.meta.url));
    const header = readFileSync(headerPath, "utf8");
    // Body of the strict parser only: the lenient wrapper below it holds no
    // tokens of its own, and the file's comments mention token names in prose.
    const body = /agentStateFromWireStrict\(QStringView s\)\s*\{([\s\S]*?)\n\}/.exec(header);
    assert.ok(body, "AgentEvent.h must declare agentStateFromWireStrict(QStringView)");
    const cppTokens = [...body[1].matchAll(/u"([a-z_]+)"/g)].map((match) => match[1]);
    assert.ok(cppTokens.length > 0, "no wire tokens parsed out of the C++ mapping");
    assert.deepEqual(cppTokens.toSorted(), [...AGENT_STATES].toSorted());
});
