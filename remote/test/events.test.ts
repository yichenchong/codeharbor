import { test } from "node:test";
import assert from "node:assert/strict";
import {
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

test("resolveSocketPath prefers XDG_RUNTIME_DIR, else falls back to cache", () => {
    assert.equal(
        resolveSocketPath({ XDG_RUNTIME_DIR: "/run/user/1000" }),
        "/run/user/1000/codeharbor.sock",
    );
    const fallback = resolveSocketPath({});
    assert.match(fallback, /\.cache\/codeharbor\/events\.sock$/);
});
