import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
    adapterFor,
    claudeCodeAdapter,
    ohMyPiAdapter,
    piAdapter,
    type NativeEvent,
} from "../src/adapters/index.ts";
import { HARNESSES } from "../src/events.ts";
import { processBridgeLine } from "../src/bridge.ts";
import {
    createLineFramer,
    dispatch,
    handleLine,
    MAX_LINE_BYTES,
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

    // The type is not present at runtime: malformed bridge input can still
    // contain a property name inherited from Object.prototype.
    const inheritedName = "toString" as Parameters<typeof adapterFor>[0];
    assert.equal(adapterFor(inheritedName), undefined);
});

test("processBridgeLine builds a validated event from a native message", () => {
    const line = JSON.stringify({
        harness: "oh-my-pi",
        devSessionId: "sess-1",
        terminalId: "term-1",
        native: { type: "tool_call", tool: "ask" },
        // Explicit wire metadata overrides the adapter-derived `tool`.
        metadata: { tool: "explicit", toolName: "ask" },
    });
    const event = processBridgeLine(line);
    assert.ok(event);
    assert.equal(event.state, "waiting_input");
    assert.equal(event.event, "tool_call");
    assert.equal(event.devSessionId, "sess-1");
    assert.deepEqual(event.metadata, { tool: "explicit", toolName: "ask" });
});

test("oh-my-pi adapter derives metadata from the native event", () => {
    assert.deepEqual(ohMyPiAdapter.metadata?.({ type: "tool_call", tool: "ask" }), { tool: "ask" });
    assert.equal(ohMyPiAdapter.metadata?.({ type: "agent_start" }), undefined);
});

test("processBridgeLine attaches adapter-derived metadata (RR25)", () => {
    const line = JSON.stringify({
        harness: "oh-my-pi",
        devSessionId: "sess-1",
        terminalId: "term-1",
        native: { type: "tool_call", tool: "ask" },
    });
    const event = processBridgeLine(line);
    assert.ok(event);
    assert.deepEqual(event.metadata, { tool: "ask" });
});

test("processBridgeLine drops malformed, unknown-harness, and no-op lines", () => {
    assert.equal(processBridgeLine(""), null);
    assert.equal(processBridgeLine("{bad json"), null);
    // `null` and `[]` PARSE — JSON.parse does not reject them — so the field
    // reads that follow used to throw a TypeError inside the socket's 'line'
    // handler, where nothing catches it: four bytes on the wire killed the
    // whole bridge process.
    assert.equal(processBridgeLine("null"), null);
    assert.equal(processBridgeLine("[]"), null);
    assert.equal(processBridgeLine("42"), null);
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
    // The three original fields stay byte-compatible; serverId and recoveryDir
    // (SPEC 11.3, the remote crash-recovery data dir) are additive.
    assert.equal(info.name, "codeharbord");
    // `version` is the RELEASE version and the client shows it to the user
    // verbatim ("Server too old: codeharbord <version> ..."). It lives in
    // codeharbord.ts by hand while the release script only rewrites the
    // manifests, so pin it to remote/package.json — pinning the literal instead
    // is what let every server report 0.1.0 three releases after the fact.
    const manifest = JSON.parse(
        readFileSync(fileURLToPath(new URL("../package.json", import.meta.url)), "utf8"),
    ) as { version: string };
    assert.equal(info.version, manifest.version);
    assert.equal(info.schemaVersion, RPC_SCHEMA_VERSION);
    assert.deepEqual(
        Object.keys(info).sort(),
        ["name", "recoveryDir", "schemaVersion", "serverId", "version"],
    );
    // recoveryDir is an absolute server path ending in the recovery directory.
    assert.equal(typeof info.recoveryDir, "string");
    assert.match(String(info.recoveryDir), /codeharbor[/\\]recovery$/);

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
    assert.equal(claudeCodeAdapter.map({ hook: "UserPromptExpansion" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "PreToolUse" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "PostToolUse" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "PostToolUseFailure" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "PostToolBatch" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "PreCompact" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "PostCompact" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "SubagentStart" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "SubagentStop" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "TeammateIdle" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "TaskCreated" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "TaskCompleted" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "MessageDisplay" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "ElicitationResult" }), "running");
    assert.equal(claudeCodeAdapter.map({ hook: "PermissionRequest" }), "waiting_input");
    assert.equal(claudeCodeAdapter.map({ hook: "Elicitation" }), "waiting_input");
    assert.equal(claudeCodeAdapter.map({ hook: "PermissionDenied" }), "error");
    assert.equal(claudeCodeAdapter.map({ hook: "StopFailure" }), "error");
    assert.equal(claudeCodeAdapter.map({ hook: "Notification" }), "waiting_input");
    assert.equal(
        claudeCodeAdapter.map({ hook: "Notification", notification_type: "permission_prompt" }),
        "waiting_input",
    );
    assert.equal(
        claudeCodeAdapter.map({ hook: "Notification", notification_type: "idle_prompt" }),
        "waiting_input",
    );
    assert.equal(
        claudeCodeAdapter.map({ hook: "Notification", notification_type: "elicitation_dialog" }),
        "waiting_input",
    );
    assert.equal(
        claudeCodeAdapter.map({ hook: "Notification", notification_type: "agent_needs_input" }),
        "waiting_input",
    );
    assert.equal(
        claudeCodeAdapter.map({
            hook: "Notification",
            notification_type: "elicitation_complete",
        }),
        "running",
    );
    assert.equal(
        claudeCodeAdapter.map({
            hook: "Notification",
            notification_type: "elicitation_response",
        }),
        "running",
    );
    assert.equal(
        claudeCodeAdapter.map({ hook: "Notification", notification_type: "auth_success" }),
        null,
    );
    assert.equal(
        claudeCodeAdapter.map({ hook: "Notification", notification_type: 7 }),
        null,
    );
    assert.equal(claudeCodeAdapter.map({ hook: "Stop" }), "idle_unseen");
    // SubagentStop fires when a Task-tool subagent finishes while the MAIN agent
    // is still working. It is covered above and must map to running rather than
    // a completion, which would arm the unseen-completion badge mid-turn.
    assert.equal(claudeCodeAdapter.map({ hook: "SessionEnd" }), "stopped");
    assert.equal(claudeCodeAdapter.map({ hook: "Anything", error: true }), "error");
    assert.equal(claudeCodeAdapter.map({ hook: "Unrecognized" }), null);
    // Claude Code names its events under `hook`; `type` is the Oh My Pi / Pi
    // key, so a type-keyed event is not a Claude Code event and maps to nothing.
    assert.equal(claudeCodeAdapter.map({ type: "SessionStart" }), null);
});

// pi.ts and oh-my-pi.ts used to be byte-for-byte copies of one mapping, kept in
// step by hand and by this test. They now SHARE the mapping (adapters/
// pi-family.ts) and differ only in harness identity, so the strongest form of
// this assertion is available: same function, not merely same answers.
test("pi and oh-my-pi share one mapping rather than a copy of it", () => {
    assert.equal(piAdapter.map, ohMyPiAdapter.map);
    assert.equal(piAdapter.metadata, ohMyPiAdapter.metadata);
    assert.equal(piAdapter.harness, "pi");
    assert.equal(ohMyPiAdapter.harness, "oh-my-pi");
    // The behaviour that identity now guarantees, still spelled out once so a
    // future split cannot quietly drop a case.
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
        assert.deepEqual(
            piAdapter.metadata?.(native),
            ohMyPiAdapter.metadata?.(native),
            `pi and oh-my-pi metadata disagree on ${JSON.stringify(native)}`,
        );
    }
});

// AG-N3. A shutdown is a terminal, observed fact: the session is over and
// nothing more will arrive for this terminal. The error flag describes an agent
// that is still there and broken, and it is not a latch on the client side —
// but a producer that exports OMP_ERROR=1 once and never unsets it turns every
// later firing, INCLUDING the shutdown, into another error. Before the fix that
// left the sidebar row red forever for a session that no longer exists, because
// nothing after the shutdown can ever replace the state. `stopped` is the
// terminal's last word and must not be maskable.
test("a shutdown event outranks the error flag in every adapter (AG-N3)", () => {
    assert.equal(ohMyPiAdapter.map({ type: "session_shutdown", error: true }), "stopped");
    assert.equal(piAdapter.map({ type: "session_shutdown", error: true }), "stopped");
    assert.equal(claudeCodeAdapter.map({ hook: "SessionEnd", error: true }), "stopped");

    // The flag still outranks every NON-terminal event: an agent_end that blew
    // up is an error, not a completion, and that is the arm the live gate
    // drives.
    assert.equal(ohMyPiAdapter.map({ type: "agent_end", error: true }), "error");
    assert.equal(piAdapter.map({ type: "agent_end", error: true }), "error");
    assert.equal(claudeCodeAdapter.map({ hook: "Stop", error: true }), "error");
    // ...and an unrecognised event with the flag set is still an error, since
    // the flag is the only thing in it that carries meaning.
    assert.equal(ohMyPiAdapter.map({ type: "unrecognized", error: true }), "error");
    assert.equal(claudeCodeAdapter.map({ hook: "Unrecognized", error: true }), "error");

    // The shutdown arm is exact, not a prefix or a truthiness test: only the
    // real terminal event outranks the flag.
    assert.equal(ohMyPiAdapter.map({ type: "session_shutdown_pending", error: true }), "error");
    assert.equal(claudeCodeAdapter.map({ hook: "SessionEnded", error: true }), "error");
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

test("createLineFramer splits complete lines across chunk boundaries", () => {
    const lines: string[] = [];
    let overflowed = false;
    const feed = createLineFramer((line) => lines.push(line), () => {
        overflowed = true;
    });
    // A line spanning two chunks, then two lines in one chunk, and a trailing
    // partial that must NOT be emitted until its newline arrives.
    feed(Buffer.from('{"a":1'));
    feed(Buffer.from('}\n{"b":2}\n{"c'));
    assert.deepEqual(lines, ['{"a":1}', '{"b":2}']);
    feed(Buffer.from('":3}\n'));
    assert.deepEqual(lines, ['{"a":1}', '{"b":2}', '{"c":3}']);
    assert.equal(overflowed, false);
});

test("createLineFramer rejects an over-cap newline-less frame", () => {
    const lines: string[] = [];
    let overflowed = false;
    const feed = createLineFramer((line) => lines.push(line), () => {
        overflowed = true;
    });
    // Stream past the cap in chunks with no newline: the framer must not buffer
    // unboundedly — it signals overflow (the caller drops the connection) and
    // emits no line.
    const chunk = Buffer.alloc(1024 * 1024, 0x41); // 1 MiB of 'A', no newline.
    for (let sent = 0; sent <= MAX_LINE_BYTES && !overflowed; sent += chunk.length) {
        feed(chunk);
    }
    assert.equal(overflowed, true, "over-cap input must trigger overflow");
    assert.deepEqual(lines, [], "no line may be emitted from an unterminated frame");
});

// After an overflow the rest of the offending frame is NOT a request. Framing
// it as one hands the dispatcher an arbitrary slice of a payload the peer never
// finished — and re-reports overflow for every further cap's worth of the same
// frame. The framer must discard through the frame's newline and resume there.
test("createLineFramer discards the rest of an over-cap frame instead of framing it", () => {
    const lines: string[] = [];
    let overflows = 0;
    const feed = createLineFramer((line) => lines.push(line), () => {
        overflows += 1;
    });

    const chunk = Buffer.alloc(1024 * 1024, 0x41); // 1 MiB of 'A', no newline.
    for (let sent = 0; sent <= MAX_LINE_BYTES; sent += chunk.length) {
        feed(chunk);
    }
    assert.equal(overflows, 1);

    // More of the same frame: still one overflow, still no line.
    for (let i = 0; i < 4; i += 1) feed(chunk);
    assert.equal(overflows, 1, "one oversized frame must be reported once");
    assert.deepEqual(lines, []);

    // The tail of the frame, then a real request behind it: the tail is
    // dropped, the request behind it is framed normally.
    feed(Buffer.from('trailing garbage\n{"a":1}\n'));
    assert.deepEqual(lines, ['{"a":1}']);
    assert.equal(overflows, 1);
});

// RA5. The registry maps a harness NAME onto an adapter, and the bridge labels
// every relayed event with the name that arrived on the wire — never with the
// adapter's own `harness` field. So filing an adapter under the wrong key is
// invisible at runtime: events would simply be attributed to a harness that did
// not produce them. The registry's value type now forbids it at compile time
// (see adapters/index.ts); this is the runtime half of the same contract.
test("every registered adapter is filed under the harness it claims (RA5)", () => {
    for (const harness of HARNESSES) {
        const adapter = adapterFor(harness);
        if (harness === "generic") {
            assert.equal(adapter, undefined, "generic has no adapter (SPEC 6.6)");
            continue;
        }
        assert.ok(adapter, `no adapter registered for ${harness}`);
        assert.equal(adapter.harness, harness);
    }

    // Names inherited from Object.prototype are not registrations. A malformed
    // bridge message can contain any string, and a plain lookup would hand back
    // a Function for these.
    for (const inherited of ["toString", "constructor", "valueOf", "__proto__"]) {
        assert.equal(
            adapterFor(inherited as Parameters<typeof adapterFor>[0]),
            undefined,
            `${inherited} must not resolve to an adapter`,
        );
    }
});

// AG7. Cross-language drift gate for the harness NAMES, the same shape as the
// agent wire-state gate in test/events.test.ts. The set of coding assistants a
// status event may come from exists twice and neither language's suite can see
// the other copy: HARNESSES in src/events.ts here, and the membership test
// isHarnessWire() in the desktop client's src/agent/AgentEvent.h. The client
// DROPS an event whose harness name it does not recognise, so adding or
// renaming an assistant on one side alone makes every status event that
// assistant produces vanish at the client edge — no state, no badge, no
// notification, no error, and, until this test, no failing check either.
//
// (The client's own suite carries the other direction: it reads HARNESSES out
// of this file and drives a real event from each name through the parser. This
// one is the exact set comparison, which is what catches a name that exists
// only on the C++ side.)
test("harness names match src/agent/AgentEvent.h (AG7)", () => {
    const headerPath = fileURLToPath(new URL("../../src/agent/AgentEvent.h", import.meta.url));
    const header = readFileSync(headerPath, "utf8");
    // The membership test's body only: the file's comments name harnesses in
    // prose, and other functions in it quote unrelated string literals.
    const body = /isHarnessWire\(QStringView h\)\s*\{([\s\S]*?)\n\}/.exec(header);
    assert.ok(body, "AgentEvent.h must declare isHarnessWire(QStringView)");
    // Hyphens are part of a harness name ("oh-my-pi", "claude-code").
    const cppNames = [...body[1].matchAll(/u"([a-z-]+)"/g)].map((match) => match[1]);
    assert.ok(cppNames.length > 0, "no harness names parsed out of the C++ membership test");
    assert.deepEqual(cppNames.toSorted(), [...HARNESSES].toSorted());
});

// RA3. Producers are shell hook configurations that interpolate variables, so a
// native field routinely arrives with a trailing newline or CR ("ask\n" from a
// command substitution, "agent_start\r\n" from a CRLF-authored config). Every
// adapter decides state by comparing these fields against exact names, so an
// untrimmed value matches nothing: the agent's state would silently freeze at
// whatever it was, and a `tool_call: ask` — the one event meaning "the user
// must answer something" — would be downgraded to an ordinary running event.
test("adapters tolerate surrounding whitespace in native fields (RA3)", () => {
    assert.equal(ohMyPiAdapter.map({ type: "agent_start\r\n" }), "running");
    assert.equal(ohMyPiAdapter.map({ type: " session_shutdown " }), "stopped");
    assert.equal(piAdapter.map({ type: "tool_call\n", tool: " ask\n" }), "waiting_input");
    assert.equal(claudeCodeAdapter.map({ hook: " SessionEnd\n" }), "stopped");
    assert.equal(claudeCodeAdapter.map({ hook: "Stop\r\n" }), "idle_unseen");
    assert.equal(
        claudeCodeAdapter.map({ hook: "Notification", notification_type: " idle_prompt\n" }),
        "waiting_input",
    );
    // AG4. A blank type is the same statement as no type at all. The producer
    // is a shell hook config interpolating a variable, so an unset one leaves
    // "" (or a bare newline) in the payload; reading that as an unrecognised
    // notification type would drop the prompt that an absent type raises.
    for (const blank of ["", "   ", "\n", "\r\n", "\t"]) {
        assert.equal(
            claudeCodeAdapter.map({ hook: "Notification", notification_type: blank }),
            "waiting_input",
            `blank notification_type ${JSON.stringify(blank)} must read as absent`,
        );
    }

    // The normalization reaches the metadata too, so the client never displays
    // a tool name with a newline glued to it.
    assert.deepEqual(piAdapter.metadata?.({ type: "tool_call", tool: " ask\n" }), { tool: "ask" });
    assert.deepEqual(
        claudeCodeAdapter.metadata?.({ hook: "PreToolUse", tool_name: "Bash\n" }),
        { tool: "Bash" },
    );

    // A blank tool name is no tool name: the bag must be absent rather than
    // carry an empty string the client would render as a nameless tool.
    assert.equal(piAdapter.metadata?.({ type: "tool_call", tool: "   " }), undefined);
    assert.equal(claudeCodeAdapter.metadata?.({ hook: "PreToolUse", tool_name: "" }), undefined);
    assert.equal(claudeCodeAdapter.metadata?.({ hook: "PreToolUse", tool_name: 7 }), undefined);
});

// The SPEC 6.5 table maps `tool_result: ask` to running and says nothing about
// any other tool result, which is deliberate: the agent was already running
// before a non-ask tool returned, so the result of one carries no transition
// and must be a no-op rather than a state write. Only the shared-mapping test
// covered this case, and it only asserted that the two pi adapters AGREE — they
// would still agree if both regressed.
test("a non-ask tool_result carries no state transition", () => {
    assert.equal(ohMyPiAdapter.map({ type: "tool_result", tool: "read" }), null);
    assert.equal(ohMyPiAdapter.map({ type: "tool_result" }), null);
    assert.equal(piAdapter.map({ type: "tool_result", tool: "read" }), null);
    // The error flag still outranks it: a tool result that blew up is an error.
    assert.equal(ohMyPiAdapter.map({ type: "tool_result", tool: "read", error: true }), "error");
});

// AG3. The error marker is the JSON boolean `true` and nothing else. It arrives
// on a wire built by shell hook configurations, where every value starts life
// as a string: the hook script is the piece that turns OMP_ERROR=1 or "true"
// into a real boolean (readHookInput), and a hand-written producer that puts
// the STRING "false" or the number 0 there is writing the shape a truthiness
// test gets wrong in both directions. `=== true` is the only reading that
// cannot turn an ordinary event into a red sidebar row, or drop a real error.
test("the error marker must be the boolean true, never merely truthy (AG3)", () => {
    for (const marker of ["true", "1", 1, "yes", {}, [], "false", 0, "", null]) {
        const label = JSON.stringify(marker) ?? String(marker);
        assert.equal(
            ohMyPiAdapter.map({ type: "agent_start", error: marker }),
            "running",
            `oh-my-pi treated error=${label} as an error`,
        );
        assert.equal(
            piAdapter.map({ type: "agent_start", error: marker }),
            "running",
            `pi treated error=${label} as an error`,
        );
        assert.equal(
            claudeCodeAdapter.map({ hook: "UserPromptSubmit", error: marker }),
            "running",
            `claude-code treated error=${label} as an error`,
        );
        // ...and it does not resurrect an event that maps to nothing either.
        assert.equal(ohMyPiAdapter.map({ type: "unrecognized", error: marker }), null, label);
        assert.equal(claudeCodeAdapter.map({ hook: "Unrecognized", error: marker }), null, label);
    }
    // The one value that IS the marker.
    assert.equal(ohMyPiAdapter.map({ type: "agent_start", error: true }), "error");
    assert.equal(claudeCodeAdapter.map({ hook: "UserPromptSubmit", error: true }), "error");
});

// AG3. Every field of a native event is typed `unknown` because it comes
// straight out of JSON.parse on a producer's line. A number, null or an object
// where the event NAME belongs must map to nothing rather than be coerced into
// a name: nativeString() answers "" for a non-string, and "" matches no arm in
// any adapter. The same goes for the tool name, where a non-string must mean
// "no tool" — so a tool_call is the plain running arm, not the prompt arm, and
// the metadata bag stays absent rather than carrying a stringified number.
test("a non-string native event name maps to nothing (AG3)", () => {
    for (const name of [7, null, true, {}, ["session_start"], undefined]) {
        const label = JSON.stringify(name) ?? String(name);
        assert.equal(ohMyPiAdapter.map({ type: name }), null, `oh-my-pi type=${label}`);
        assert.equal(piAdapter.map({ type: name }), null, `pi type=${label}`);
        assert.equal(claudeCodeAdapter.map({ hook: name }), null, `claude-code hook=${label}`);
    }
    assert.equal(ohMyPiAdapter.map({ type: "tool_call", tool: 7 }), "running");
    assert.equal(ohMyPiAdapter.metadata?.({ type: "tool_call", tool: 7 }), undefined);
    assert.equal(claudeCodeAdapter.map({ hook: "Notification", notification_type: null }), null);
});

// AG3. The two vocabularies must not cross. Oh My Pi and Pi name their event
// under `type`, Claude Code under `hook`, and NativeEvent declares both keys —
// so a message carrying both (a producer relaying for two harnesses, or a
// copy-paste in a hook config) is structurally legal. Each adapter must read
// only its own key: reading the other one attributes a state to a harness that
// never reported it, and the bridge labels the relayed event with the WIRE
// harness, so nothing downstream could notice.
test("each adapter reads only its own event-name key (AG3)", () => {
    const both = { type: "session_shutdown", hook: "SessionStart" };
    assert.equal(ohMyPiAdapter.map(both), "stopped");
    assert.equal(piAdapter.map(both), "stopped");
    assert.equal(claudeCodeAdapter.map(both), "starting");

    // And the metadata keys are per-harness too: `tool` for the pi family,
    // `tool_name` for Claude Code. Neither may pick up the other's.
    const bothTools = { type: "tool_call", tool: "ask", tool_name: "Bash" };
    assert.deepEqual(ohMyPiAdapter.metadata?.(bothTools), { tool: "ask" });
    assert.deepEqual(claudeCodeAdapter.metadata?.(bothTools), { tool: "Bash" });
});
