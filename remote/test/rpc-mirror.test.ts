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
import { EventEmitter } from "node:events";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import {
    RPC_DATABASE_BUSY,
    RPC_INTERNAL_ERROR,
    RPC_METHODS,
    RPC_REVISION_MISMATCH,
    RPC_RESOURCE_LIMIT,
    RPC_SERVER_INFO_METHOD,
    RPC_TMUX_METHODS,
    RPC_WORKSPACE_METHODS,
    RPC_PING_METHOD,
    RPC_WATCH_EVENT_NOTIFICATION,
    RPC_WATCH_EVENTS_LOST_NOTIFICATION,
} from "../src/rpc-types.ts";
import {
    RPC_SCHEMA_VERSION,
    RPC_SERVER_VERSION,
    responseLine,
    MAX_LINE_BYTES,
    MAX_PENDING_WATCH_EVENTS,
    createLineFramer,
    createWatchNotificationRelay,
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
    // server.info is a built-in in codeharbord.ts's static method map rather
    // than a spread-in table, but it still has a shared TypeScript constant.
    assert.equal(
        CPP_CONSTANTS.get("kMethodServerInfo"),
        RPC_SERVER_INFO_METHOD,
    );
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
        RPC_SERVER_INFO_METHOD,
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

// Same contract, third error code. RPC_RESOURCE_LIMIT is what a file.listDirectory
// on a huge directory, or a file.watch past the subscription cap, answers instead
// of serializing a reply nobody can carry. That refusal is the client's
// protection: a listing written out anyway exceeded CodeharbordClient's 16 MiB
// unframed-line cap, and going over that cap does not fail one reply, it drops
// the whole transport — every terminal and editor on that connection with it. If
// the two sides drift on this number, the code that keeps that from happening
// stops being recognisable as one.
test("the resource-limit error code matches between TypeScript and C++", () => {
    const declared = /^inline constexpr int kResourceLimit\s*=\s*(-?\d+);/m.exec(header);
    assert.ok(declared, "RpcTypes.h must declare kResourceLimit");
    assert.equal(Number(declared[1]), RPC_RESOURCE_LIMIT);
    assert.ok(
        RPC_RESOURCE_LIMIT <= -32000 && RPC_RESOURCE_LIMIT >= -32099,
        `${RPC_RESOURCE_LIMIT} is outside the reserved server-error range`,
    );
    // Three distinct conditions, three distinct codes: "the file changed under
    // you", "retry, the database was busy", and "too big, nothing was done" are
    // different answers and the client must be able to tell them apart.
    assert.notEqual(RPC_RESOURCE_LIMIT, RPC_REVISION_MISMATCH);
    assert.notEqual(RPC_RESOURCE_LIMIT, RPC_DATABASE_BUSY);
});

// The fourth error code, and the only one of the four the JSON-RPC 2.0
// specification itself fixes rather than this project. It is pinned for the
// same reason as the three above and because RpcTypes.h itself says the two
// copies must not drift: the desktop client tells "the server broke" apart from
// the three retryable/refusable answers purely by this number, so a typo in
// either copy turns a genuine server fault into an unrecognised code the client
// reports as something else — or, worse, silently matches one of the others.
test("the internal-error code matches between TypeScript and C++", () => {
    const declared = /^inline constexpr int kInternalError\s*=\s*(-?\d+);/m.exec(header);
    assert.ok(declared, "RpcTypes.h must declare kInternalError");
    assert.equal(Number(declared[1]), RPC_INTERNAL_ERROR);
    // The reserved pre-defined code, NOT one of the implementation-defined
    // -32000..-32099 server codes: an implementation is free to choose those
    // three, and is not free to choose this one.
    assert.equal(RPC_INTERNAL_ERROR, -32603);
    for (const other of [RPC_REVISION_MISMATCH, RPC_DATABASE_BUSY, RPC_RESOURCE_LIMIT]) {
        assert.notEqual(RPC_INTERNAL_ERROR, other);
    }
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

test("the line framer rejects an oversized frame whose newline is in the same chunk", () => {
    const lines: string[] = [];
    let overflows = 0;
    const feed = createLineFramer(
        (line) => lines.push(line),
        () => {
            overflows += 1;
        },
    );
    feed(Buffer.concat([Buffer.alloc(MAX_LINE_BYTES + 1, 0x61), Buffer.from("\nok\n")]));
    assert.equal(overflows, 1);
    assert.deepEqual(lines, ["ok"]);
});

test("watch relay coalescing cannot exceed its byte bound", () => {
    let blocked = true;
    const lines: string[] = [];
    const out = Object.assign(new EventEmitter(), {
        write(line: string): boolean {
            lines.push(line);
            return !blocked;
        },
    }) as unknown as NodeJS.WritableStream & EventEmitter;
    const live = new Set<string>(["sub"]);
    const relay = createWatchNotificationRelay(out, (id) => live.has(id));
    relay.stall();
    relay.deliver({ subscriptionId: "sub", path: "/same", event: "modified" });
    for (let i = 0; i < MAX_PENDING_WATCH_EVENTS; i += 1) {
        const id = `sub-${i}`;
        live.add(id);
        relay.deliver({
            subscriptionId: id,
            path: "/" + "x".repeat(4000) + `-${i}`,
            event: "modified",
        });
    }
    // A larger replacement for an existing key must be reported as loss, not
    // allowed to push the serialized queue past its byte cap.
    relay.deliver({
        subscriptionId: "sub",
        path: "/same",
        event: "modified",
        revision: "r".repeat(10_000),
    });
    assert.ok(relay.pendingCount() <= MAX_PENDING_WATCH_EVENTS);
    blocked = false;
    out.emit("drain");
    assert.ok(
        lines.some(
            (line) => JSON.parse(line).method === "file.watchEventsLost",
        ),
        "the client must be told that the oversized replacement was dropped",
    );
});

// The version server.info reports is written out twice: as RPC_SERVER_VERSION
// in remote/src/codeharbord.ts and as the "version" field of
// remote/package.json. Only the package.json copy is rewritten by the release
// script, which is how the constant once sat at 0.1.0 while the tag said 0.1.8
// and every deployed server reported a version three releases stale. That
// number is not cosmetic: the client shows it to the USER verbatim in its
// "Server too old: codeharbord <version> speaks ..." message, so a stale one
// names the wrong release to upgrade.
test("the reported server version matches remote/package.json", () => {
    const packagePath = fileURLToPath(new URL("../package.json", import.meta.url));
    const declared = JSON.parse(readFileSync(packagePath, "utf8")) as { version?: string };
    assert.equal(
        RPC_SERVER_VERSION,
        declared.version,
        "codeharbord.ts's RPC_SERVER_VERSION and remote/package.json's version must be bumped together",
    );
});

test("a response is serialized as one newline-terminated JSON line", () => {
    assert.equal(
        responseLine({ jsonrpc: "2.0", id: 1, result: { pong: true } }),
        '{"jsonrpc":"2.0","id":1,"result":{"pong":true}}\n',
    );
});

// An answer bigger than one transport frame used to be written out anyway, and
// BOTH ends of this protocol drop the connection on an over-cap frame: a single
// oversized reply cost the user every terminal and every editor on that
// connection, with nothing on screen saying why. It must instead become a small
// refusal that keeps the request id — so the client's pending call completes,
// the message names the limit, and the session survives.
test("an over-cap response is refused instead of written as a doomed frame", () => {
    const line = responseLine({
        jsonrpc: "2.0",
        id: 7,
        result: { blob: "x".repeat(MAX_LINE_BYTES + 1) },
    });
    assert.ok(
        Buffer.byteLength(line) <= MAX_LINE_BYTES,
        "the refusal itself must fit in a frame",
    );
    const decoded = JSON.parse(line);
    assert.equal(decoded.id, 7);
    assert.equal(decoded.error.code, RPC_RESOURCE_LIMIT);
    assert.match(decoded.error.message, /transport frame limit/);
});
test("an oversized response id is replaced with null in the bounded refusal", () => {
    const line = responseLine({
        jsonrpc: "2.0",
        id: "x".repeat(MAX_LINE_BYTES),
        result: { method: "unknown" },
    });
    assert.ok(Buffer.byteLength(line) <= MAX_LINE_BYTES);
    const decoded = JSON.parse(line);
    assert.equal(decoded.id, null);
    assert.equal(decoded.error.code, RPC_RESOURCE_LIMIT);
});

// The two directions must agree on WHAT the cap measures, to the byte. The
// outbound check used to weigh the payload PLUS its framing newline while the
// inbound framer weighs only the payload, so a reply of exactly MAX_LINE_BYTES
// was accepted on the way in and refused on the way out — two comments claiming
// one rule, differing by one byte. Both now measure the payload alone.
test("the transport cap measures the payload, not the framing newline", () => {
    // Grow the result until the serialized payload is EXACTLY the cap.
    const envelope = JSON.stringify({ jsonrpc: "2.0", id: 1, result: { b: "" } });
    const exact = "x".repeat(MAX_LINE_BYTES - Buffer.byteLength(envelope));
    const line = responseLine({ jsonrpc: "2.0", id: 1, result: { b: exact } });
    // Delivered, not refused: the payload is at the cap, and the newline that
    // frames it is not part of what the cap bounds.
    assert.equal(Buffer.byteLength(line), MAX_LINE_BYTES + 1);
    assert.equal(JSON.parse(line).result.b.length, exact.length);

    // The inbound framer accepts the same payload, which is the whole point.
    const framed: string[] = [];
    let overflows = 0;
    const feed = createLineFramer((l) => framed.push(l), () => {
        overflows += 1;
    });
    feed(Buffer.from(line));
    assert.equal(overflows, 0);
    assert.equal(framed.length, 1);

    // One byte more is over the cap in both directions.
    const over = responseLine({ jsonrpc: "2.0", id: 1, result: { b: `${exact}x` } });
    assert.equal(JSON.parse(over).error.code, RPC_RESOURCE_LIMIT);
});

// A payload JSON.stringify cannot render (a reference cycle, a BigInt) still
// has to produce a line. The client correlates replies by request id, so a
// request answered with nothing holds that call open until the heartbeat tears
// the whole transport down over one unserializable result.
test("an unserializable response still answers the request", () => {
    const cyclic: Record<string, unknown> = {};
    cyclic.self = cyclic;
    const decoded = JSON.parse(
        responseLine({ jsonrpc: "2.0", id: "abc", result: cyclic }),
    );
    assert.equal(decoded.id, "abc");
    assert.equal(decoded.error.code, RPC_INTERNAL_ERROR);
    assert.match(decoded.error.message, /could not be serialized/);
});

// The last piece of the server.info payload the client stores and acts on, and
// the only one that is a PATH: `recoveryDir` is where the editor writes its
// crash-recovery snapshots, on the SERVER. The client appends its per-pane id
// and sends the result back as a file.writeFile path, and files.ts hands a
// relative path straight to the filesystem — so a relative recoveryDir puts the
// snapshots under whatever directory this daemon happened to be launched in, and
// a daemon started from anywhere else looks for them somewhere different. That
// is silently no crash recovery, discovered only after a crash. $XDG_DATA_HOME
// is DEFINED to be absolute, so a relative one is ignored rather than joined —
// the same rule resolveSocketPath applies to $XDG_RUNTIME_DIR.
test("server.info reports an absolute recoveryDir even for a relative XDG_DATA_HOME", async () => {
    // server.info reads its serverId from the workspace database; point the
    // lazily-opened default connection at a throwaway file rather than the real
    // one, exactly as test/adapters.test.ts does.
    const dbDir = mkdtempSync(path.join(os.tmpdir(), "codeharbord-mirror-"));
    const previousDb = process.env.CODEHARBOR_DB;
    const previousDataHome = process.env.XDG_DATA_HOME;
    process.env.CODEHARBOR_DB = path.join(dbDir, "codeharbor.sqlite");
    const recoveryDirOf = async (): Promise<string> => {
        const response = await dispatch({
            jsonrpc: "2.0",
            id: 1,
            method: RPC_SERVER_INFO_METHOD,
        });
        assert.ok(response !== null && "result" in response);
        const { recoveryDir } = response.result as { recoveryDir?: unknown };
        assert.equal(typeof recoveryDir, "string");
        return recoveryDir as string;
    };
    try {
        // An absolute value is honoured verbatim...
        process.env.XDG_DATA_HOME = path.join(dbDir, "data");
        assert.equal(
            await recoveryDirOf(),
            path.join(dbDir, "data", "codeharbor", "recovery"),
        );

        // ...and a relative one is ignored, not joined onto the daemon's cwd.
        // Read per request, so this reassignment must take effect.
        const absoluteFallback = path.join(
            os.homedir(),
            ".local",
            "share",
            "codeharbor",
            "recovery",
        );
        for (const relative of ["share", "./share", "..", ""]) {
            process.env.XDG_DATA_HOME = relative;
            assert.equal(await recoveryDirOf(), absoluteFallback, relative);
        }

        delete process.env.XDG_DATA_HOME;
        assert.equal(await recoveryDirOf(), absoluteFallback);
    } finally {
        if (previousDb === undefined) delete process.env.CODEHARBOR_DB;
        else process.env.CODEHARBOR_DB = previousDb;
        if (previousDataHome === undefined) delete process.env.XDG_DATA_HOME;
        else process.env.XDG_DATA_HOME = previousDataHome;
        rmSync(dbDir, { recursive: true, force: true });
    }
});