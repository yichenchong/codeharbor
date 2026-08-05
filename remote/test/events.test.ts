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
    assert.equal(validateEvent({ ...good, timestamp: "" }), false);
    assert.equal(validateEvent({ ...good, timestamp: "2026-08-03T12:00:00Z" }), false);
    assert.equal(validateEvent({ ...good, timestamp: "2026-13-03T12:00:00.000Z" }), false);
    assert.equal(validateEvent({ ...good, timestamp: "2026-02-29T12:00:00.000Z" }), false);
    // A day count is checked against the month, and February against the leap
    // rule in both directions: 2024 has a 29th, 2026 does not (above), and no
    // April has a 31st.
    assert.equal(validateEvent({ ...good, timestamp: "2024-02-29T12:00:00.000Z" }), true);
    assert.equal(validateEvent({ ...good, timestamp: "2026-04-31T12:00:00.000Z" }), false);
    // The time-of-day fields are range-checked too: 24:00 is not an hour.
    assert.equal(validateEvent({ ...good, timestamp: "2026-08-03T24:00:00.000Z" }), false);
    assert.equal(
        validateEvent({ ...good, timestamp: "2026-08-03T12:00:00.000+00:00" }),
        true,
    );
    assert.equal(validateEvent({ ...good, metadata: new Date() }), false);
    // `event` is the harness's own event name and is required to be a string:
    // the client renders it, and a number or an absent value would reach the
    // sidebar as "undefined".
    assert.equal(validateEvent({ ...good, event: 42 }), false);
    assert.equal(validateEvent({ ...good, event: undefined }), false);
    // `summary` is optional but, when present, is also text the client shows.
    assert.equal(validateEvent({ ...good, summary: 7 }), false);
    assert.equal(validateEvent({ ...good, summary: "all done" }), true);
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
    assert.equal(parseEventLine(null), null);
    assert.equal(parseEventLine(42), null);
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
    // An empty variable is the same as an unset one.
    assert.equal(resolveSocketPath({ XDG_RUNTIME_DIR: "" }), fallback);
    // A RELATIVE runtime directory is ignored. Honouring it would make the
    // socket path depend on each process's working directory, so the bridge
    // would bind one path and a hook started elsewhere would look for another —
    // every status event lost, with nothing logged anywhere.
    assert.equal(resolveSocketPath({ XDG_RUNTIME_DIR: "run/user" }), fallback);
    assert.equal(resolveSocketPath({ XDG_RUNTIME_DIR: "." }), fallback);
    // The home directory is read from the SAME environment as the runtime
    // directory. Taking it from the real process instead would make the answer
    // only half-overridable, so a hook started with a different HOME and the
    // bridge would look for the socket in two different places.
    assert.equal(
        resolveSocketPath({ HOME: "/home/someone" }),
        "/home/someone/.cache/codeharbor/events.sock",
    );
    // An unset or relative HOME falls back to the real home directory.
    assert.equal(resolveSocketPath({ HOME: "relative/home" }), fallback);
    assert.equal(resolveSocketPath({ HOME: "" }), fallback);
    // XDG_RUNTIME_DIR still wins over HOME.
    assert.equal(
        resolveSocketPath({ XDG_RUNTIME_DIR: "/run/user/7", HOME: "/home/someone" }),
        "/run/user/7/codeharbor.sock",
    );
});

// The bridge reads JSONL off a socket, and a producer that ends its lines with
// CRLF is not a malformed producer — the trailing carriage return is framing,
// not payload. Dropping those events would make CodeHarbor silently useless for
// that harness.
test("parseEventLine tolerates surrounding whitespace and a trailing CR", () => {
    const event = makeEvent({
        harness: "generic",
        devSessionId: "s",
        terminalId: "t",
        state: "running",
        event: "tick",
    });
    assert.deepEqual(parseEventLine(`${JSON.stringify(event)}\r`), event);
    assert.deepEqual(parseEventLine(`  ${JSON.stringify(event)}  `), event);
});

// Valid JSON that is not an event object at all. Each of these decodes without
// throwing, so only the structural check stands between them and the client.
test("parseEventLine rejects valid JSON that is not an event object", () => {
    for (const line of ["[]", "42", '"a string"', "null", "true", '[{"version":1}]']) {
        assert.equal(parseEventLine(line), null, line);
    }
});

// The timestamp grammar accepts a numeric timezone offset, so the offset's own
// fields have to be real ones: "+00:60" and "+99:99" match the shape and name
// no instant. Date.parse is what rules them out, and this pins that it does.
test("validateEvent rejects an impossible timezone offset", () => {
    const good = makeEvent({
        harness: "pi",
        devSessionId: "s",
        terminalId: "t",
        state: "idle",
        event: "done",
        timestamp: "2026-08-03T12:00:00.000Z",
    });
    assert.equal(validateEvent(good), true);
    for (const offset of ["+00:60", "+24:00", "+99:99", "-13:70"]) {
        assert.equal(
            validateEvent({ ...good, timestamp: `2026-08-03T12:00:00.000${offset}` }),
            false,
            offset,
        );
    }
    // A real offset still passes, in both directions.
    for (const offset of ["+05:30", "-08:00"]) {
        assert.equal(
            validateEvent({ ...good, timestamp: `2026-08-03T12:00:00.000${offset}` }),
            true,
            offset,
        );
    }
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
