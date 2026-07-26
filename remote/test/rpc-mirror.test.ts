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
    RPC_METHODS,
    RPC_TMUX_METHODS,
    RPC_WORKSPACE_METHODS,
    RPC_WATCH_EVENT_NOTIFICATION,
} from "../src/rpc-types.ts";
import { WORKSPACE_METHODS } from "../src/workspace.ts";

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
    // The watch-event NOTIFICATION shares the file. prefix but is not a request
    // method, so it is excluded here and pinned on its own below.
    const cpp = headerGroup("file").filter(
        (v) => v !== RPC_WATCH_EVENT_NOTIFICATION,
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
    // server.info has no TypeScript table (it is a built-in in codeharbord.ts's
    // static method map), so its name is pinned directly.
    assert.equal(CPP_CONSTANTS.get("kMethodServerInfo"), "server.info");
});

// Catches a name added to the C++ header under a prefix no TypeScript table
// owns — the mirror gaining a method the server can never dispatch.
test("RpcTypes.h declares no wire name outside a known group", () => {
    const known = new Set<string>([
        ...Object.values(RPC_METHODS),
        ...Object.values(RPC_TMUX_METHODS),
        ...Object.values(RPC_WORKSPACE_METHODS),
        RPC_WATCH_EVENT_NOTIFICATION,
        "server.info",
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
