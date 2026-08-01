// Cross-language drift gate for the RPC method-name contract.
//
// Every wire method name exists twice: once in remote/src/rpc-types.ts (the
// authoritative TypeScript contract the server dispatches on) and once as an
// `inline constexpr auto kMethod*` in src/remote/RpcTypes.h (the C++ client's
// mirror). Nothing in either language's own unit suite can see the other copy,
// so a rename on ONE side used to pass both suites and only surface as a
// runtime "method not found" against a live server.
//
// This test closes that hole the same way schema.test.ts closes the
// schema.sql/WORKSPACE_SCHEMA_VERSION hole: read the sibling-language source
// and assert parity. It is deliberately a SET comparison in both directions —
// a rename shows up as one missing name plus one unexpected name, an addition
// on one side alone shows up as an unexpected name.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import {
    RPC_DATABASE_BUSY,
    RPC_METHODS,
    RPC_REVISION_MISMATCH,
    RPC_TMUX_METHODS,
    RPC_WORKSPACE_METHODS,
    RPC_PING_METHOD,
    RPC_WATCH_EVENT_NOTIFICATION,
    RPC_WATCH_EVENTS_LOST_NOTIFICATION,
} from "../src/rpc-types.ts";
import {
    RPC_SCHEMA_VERSION,
    MAX_LINE_BYTES,
    dispatch,
} from "../src/codeharbord.ts";
import { WORKSPACE_METHODS } from "../src/workspace.ts";
import { fileMethods } from "../src/files.ts";
import { TMUX_METHODS } from "../src/tmux.ts";

const headerPath = fileURLToPath(
    new URL("../../src/remote/RpcTypes.h", import.meta.url),
);
const header = readFileSync(headerPath, "utf8");

// `inline constexpr auto kSomething = "value";` — the one form RpcTypes.h uses
// for wire names. Capturing the C++ identifier too lets a failure name the
// exact constant to fix. Parsed once: the header does not change mid-run.
const CONSTANT_RE = /^inline constexpr auto (k\w+)\s*=\s*"([^"]*)";/gm;

const CPP_CONSTANTS = new Map<string, string>();
for (const [, name, value] of header.matchAll(CONSTANT_RE)) {
    CPP_CONSTANTS.set(name, value);
}

// Every C++ string constant whose value belongs to `group.` — the mirror of one
// TypeScript method table.
function headerGroup(group: string): string[] {
    return [...CPP_CONSTANTS.values()]
        .filter((v) => v.startsWith(`${group}.`))
        .sort();
}

function tsGroup(table: Record<string, string>): string[] {
    return Object.values(table).sort();
}

// The header is the C++ client's only source of wire names, so it must parse.
// A zero-match regex would make every set comparison below trivially pass.
test("RpcTypes.h yields a non-trivial set of wire-name constants", () => {
    assert.ok(
        CPP_CONSTANTS.size >= 25,
        `parsed only ${CPP_CONSTANTS.size} constants from RpcTypes.h — the ` +
            `regex no longer matches the header's declaration form`,
    );
});

test("file.* method names match between rpc-types.ts and RpcTypes.h", () => {
    // The two file.* NOTIFICATION names share the prefix but are not request
    // methods, so they are excluded here and pinned on their own below.
    const cpp = headerGroup("file").filter(
        (v) =>
            v !== RPC_WATCH_EVENT_NOTIFICATION &&
            v !== RPC_WATCH_EVENTS_LOST_NOTIFICATION,
    );
    assert.deepEqual(cpp, tsGroup(RPC_METHODS));
});

test("tmux.* method names match between rpc-types.ts and RpcTypes.h", () => {
    assert.deepEqual(headerGroup("tmux"), tsGroup(RPC_TMUX_METHODS));
});

test("workspace.* method names match between rpc-types.ts and RpcTypes.h", () => {
    assert.deepEqual(headerGroup("workspace"), tsGroup(RPC_WORKSPACE_METHODS));
});

test("singleton wire names match between rpc-types.ts and RpcTypes.h", () => {
    assert.equal(
        CPP_CONSTANTS.get("kWatchEventNotification"),
        RPC_WATCH_EVENT_NOTIFICATION,
    );
    assert.equal(
        CPP_CONSTANTS.get("kWatchEventsLostNotification"),
        RPC_WATCH_EVENTS_LOST_NOTIFICATION,
    );
    // server.info has no TypeScript table (it is a built-in in codeharbord.ts's
    // static method map), so its name is pinned directly.
    assert.equal(CPP_CONSTANTS.get("kMethodServerInfo"), "server.info");
    // Same for the transport keepalive: `ping` is a built-in in that same map,
    // is not an application method, and belongs to no group — see the comment
    // on kMethodPing in RpcTypes.h for why it keeps its bare, ungrouped name.
    assert.equal(CPP_CONSTANTS.get("kMethodPing"), RPC_PING_METHOD);
});

// Catches a name added to the C++ header under a prefix no TypeScript table
// owns — the mirror gaining a method the server can never dispatch.
test("RpcTypes.h declares no wire name outside a known group", () => {
    const known = new Set<string>([
        ...Object.values(RPC_METHODS),
        ...Object.values(RPC_TMUX_METHODS),
        ...Object.values(RPC_WORKSPACE_METHODS),
        RPC_WATCH_EVENT_NOTIFICATION,
        RPC_WATCH_EVENTS_LOST_NOTIFICATION,
        "server.info",
        RPC_PING_METHOD,
    ]);
    const unexpected = [...CPP_CONSTANTS.entries()]
        .filter(([name, value]) => name.startsWith("kMethod") && !known.has(value))
        .map(([name, value]) => `${name} = "${value}"`);
    assert.deepEqual(unexpected, []);
});

// The C++ mirror is only useful if the server actually serves every name in it:
// the handler table and the contract must agree, not merely the two contracts.
test("every workspace.* contract name has a server handler", () => {
    assert.deepEqual(
        Object.keys(WORKSPACE_METHODS).sort(),
        tsGroup(RPC_WORKSPACE_METHODS),
    );
});

// The same gate for the other two groups. `fileMethods` and `TMUX_METHODS` are
// spread into codeharbord's method map verbatim, so a contract name with no key
// here is a method the client is told exists and the server answers with
// "method not found".
test("every file.* and tmux.* contract name has a server handler", () => {
    assert.deepEqual(Object.keys(fileMethods).sort(), tsGroup(RPC_METHODS));
    assert.deepEqual(Object.keys(TMUX_METHODS).sort(), tsGroup(RPC_TMUX_METHODS));
});

// `ping` is not in a spread-in table, so the two tests above cannot see it.
// Dispatch it for real instead: the C++ client's heartbeat drops the transport
// (failing every in-flight call and announcing a disconnect) when consecutive
// probes go unanswered, so a server that answered this with "method not found"
// — an error response, but still a response — would keep the session alive by
// accident, and a server that answered nothing at all would tear down healthy
// sessions every interval.
test("the keepalive method really dispatches to a success result", async () => {
    const response = await dispatch({
        jsonrpc: "2.0",
        id: 1,
        method: RPC_PING_METHOD,
    });
    assert.deepEqual(response, {
        jsonrpc: "2.0",
        id: 1,
        result: { pong: true },
    });
});

// Method names are not the only duplicated contract: the application-level
// error code travels the same wire and is written out twice as well. A server
// that rejects a stale write with a code the client does not recognize turns a
// precise "the file changed under you" conflict into a generic failure.
test("the revision-mismatch error code matches between TypeScript and C++", () => {
    const declared = /^inline constexpr int kRevisionMismatch\s*=\s*(-?\d+);/m.exec(header);
    assert.ok(declared, "RpcTypes.h must declare kRevisionMismatch");
    assert.equal(Number(declared[1]), RPC_REVISION_MISMATCH);
    // JSON-RPC 2.0 reserves -32000..-32099 for implementation-defined server
    // errors; anything outside that band collides with the spec's own codes.
    assert.ok(
        RPC_REVISION_MISMATCH <= -32000 && RPC_REVISION_MISMATCH >= -32099,
        `${RPC_REVISION_MISMATCH} is outside the reserved server-error range`,
    );
});

// Same contract, second error code. A workspace write that lost the race for
// the database write lock reports RPC_DATABASE_BUSY instead of -32603, so the
// client is not told the server malfunctioned over a retryable collision. No
// C++ call site branches on it YET — it reaches the user through the generic
// error path — but the constant is mirrored now so the two sides cannot drift
// before one does.
test("the database-busy error code matches between TypeScript and C++", () => {
    const declared = /^inline constexpr int kDatabaseBusy\s*=\s*(-?\d+);/m.exec(header);
    assert.ok(declared, "RpcTypes.h must declare kDatabaseBusy");
    assert.equal(Number(declared[1]), RPC_DATABASE_BUSY);
    assert.ok(
        RPC_DATABASE_BUSY <= -32000 && RPC_DATABASE_BUSY >= -32099,
        `${RPC_DATABASE_BUSY} is outside the reserved server-error range`,
    );
    // Two distinct conditions must never collapse onto one code: the client
    // special-cases -32001 as "the file changed under you".
    assert.notEqual(RPC_DATABASE_BUSY, RPC_REVISION_MISMATCH);
});

// The third copy of the schema-version contract. RPC_SCHEMA_VERSION lives in
// remote/src/codeharbord.ts and is reported by server.info; the client refuses
// to adopt a server whose reported schemaVersion is below
// AppController::kMinimumServerSchemaVersion (src/app/AppController.h). The two
// numbers are kept in LOCKSTEP, and this test pins them to each other, because
// each bump so far added a method group or an info field the client actually
// uses:
//   * floor above the server  -> every connection fails the compatibility gate
//     and the user gets an empty sidebar, the exact failure that gate explains;
//   * server above the floor  -> the client accepts a server whose newer
//     contract it was never taught, and the mismatch surfaces later as a
//     method-not-found or a missing field on a live connection.
// If a future bump is ever genuinely one-sided (a server capability no client
// needs), relax this to an inequality deliberately, here, with a reason.
test("the server's schemaVersion equals the client's declared minimum", () => {
    const controllerPath = fileURLToPath(
        new URL("../../src/app/AppController.h", import.meta.url),
    );
    // A header that no longer declares the constant in this form must FAIL, not
    // quietly skip: a regex that matches nothing would turn this gate into a
    // no-op exactly when the C++ side has been reshaped and needs checking.
    const declared = /kMinimumServerSchemaVersion\s*=\s*(\d+);/.exec(
        readFileSync(controllerPath, "utf8"),
    );
    assert.ok(
        declared,
        `AppController.h (${controllerPath}) must declare ` +
            "kMinimumServerSchemaVersion as `= <integer>;`",
    );
    assert.equal(
        RPC_SCHEMA_VERSION,
        Number(declared[1]),
        `codeharbord reports schemaVersion ${RPC_SCHEMA_VERSION} but the client ` +
            `declares its minimum as ${declared[1]}: bump both together`,
    );
});

// A fourth cross-language contract: the maximum bytes one input line may reach
// before the transport is dropped. The C++ client (kMaxLineBytes in
// src/remote/CodeharbordClient.cpp) and the server (MAX_LINE_BYTES in
// codeharbord.ts) must agree, or one end tolerates a frame the other rejects.
test("the remote line cap matches the C++ kMaxLineBytes", () => {
    const clientPath = fileURLToPath(
        new URL("../../src/remote/CodeharbordClient.cpp", import.meta.url),
    );
    const source = readFileSync(clientPath, "utf8");
    // `constexpr int kMaxLineBytes = 16 * 1024 * 1024;` — a product of integer
    // factors, not a single literal, so capture the RHS and multiply it out. A
    // regex that no longer matches must FAIL, not silently skip the check.
    const declared = /constexpr\s+int\s+kMaxLineBytes\s*=\s*([^;]+);/.exec(source);
    assert.ok(
        declared,
        `CodeharbordClient.cpp (${clientPath}) must declare kMaxLineBytes as \`= <expr>;\``,
    );
    const cppValue = declared[1]
        .split("*")
        .map((factor) => Number(factor.trim()))
        .reduce((product, factor) => product * factor, 1);
    assert.equal(
        MAX_LINE_BYTES,
        cppValue,
        `codeharbord caps a line at ${MAX_LINE_BYTES} but the C++ client caps ` +
            `at ${cppValue}: keep both at 16 MiB`,
    );
});
