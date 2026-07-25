import { test } from "node:test";
import assert from "node:assert/strict";

import {
    LIST_SESSIONS_FORMAT,
    SPAWN_FAILED,
    parseSessions,
    listSessions,
    sessionExists,
    killSession,
    TMUX_METHODS,
} from "../src/tmux.ts";
import type { CommandResult, CommandRunner } from "../src/tmux.ts";
import { RPC_TMUX_METHODS } from "../src/rpc-types.ts";
import { dispatch, RPC_METHOD_NOT_FOUND, RPC_SCHEMA_VERSION } from "../src/codeharbord.ts";

// Records every argv the code under test hands the runner, so the tests can
// assert that user input travels as ARGV and is never spliced into a shell
// string. `reply` is what tmux would have produced.
function stubRunner(reply: CommandResult): { run: CommandRunner; calls: string[][] } {
    const calls: string[][] = [];
    return {
        calls,
        run: async (argv) => {
            calls.push(argv);
            return reply;
        },
    };
}

const OK = (stdout: string): CommandResult => ({ code: 0, stdout, stderr: "" });

// What `tmux list-sessions -F LIST_SESSIONS_FORMAT` actually emits: windows,
// created (unix seconds), attached flag, then the name — which holds spaces
// verbatim. The colon here is deliberate parser hardening: tmux 3.6 rewrites
// `:` to `_` at creation, but that normalization is the SERVER's choice and
// not something the parser may lean on (older tmux, or a name that arrived by
// another route). A colon must land in `name`, never split a field.
const REALISTIC_OUTPUT = [
    "3\t1753372800\t1\tcodeharbor",
    "1\t1753376400\t0\tmy session: staging",
    "12\t1753380000\t0\tagent-42",
    "",
].join("\n");

test("the list format is machine-readable and puts the name last", () => {
    assert.equal(
        LIST_SESSIONS_FORMAT,
        "#{session_windows}\t#{session_created}\t#{?session_attached,1,0}\t#{session_name}",
    );
});

test("parses a realistic multi-session listing, names with spaces and colons intact", () => {
    assert.deepEqual(parseSessions(REALISTIC_OUTPUT), [
        { name: "codeharbor", windows: 3, created: 1753372800, attached: true },
        { name: "my session: staging", windows: 1, created: 1753376400, attached: false },
        { name: "agent-42", windows: 12, created: 1753380000, attached: false },
    ]);
});

test("a name containing the field delimiter survives, because the name comes last", () => {
    assert.deepEqual(parseSessions("2\t1753372800\t0\tweird\tname"), [
        { name: "weird\tname", windows: 2, created: 1753372800, attached: false },
    ]);
});

test("malformed lines are skipped, not fatal", () => {
    const stdout = ["garbage", "1\tnot-a-number\t0\tbad", "4\t1753372800\t1\tgood", ""].join("\n");
    assert.deepEqual(parseSessions(stdout), [
        { name: "good", windows: 4, created: 1753372800, attached: true },
    ]);
});

test("listSessions invokes tmux with argv and returns parsed sessions", async () => {
    const tmux = stubRunner(OK(REALISTIC_OUTPUT));
    const sessions = await listSessions(tmux.run);

    assert.deepEqual(tmux.calls, [["list-sessions", "-F", LIST_SESSIONS_FORMAT]]);
    assert.equal(sessions.length, 3);
    assert.equal(sessions[1].name, "my session: staging");
});

test("no tmux server running yields an empty result, not an error", async () => {
    const tmux = stubRunner({ code: 1, stdout: "", stderr: "no server running on /tmp/tmux-1000/default" });
    assert.deepEqual(await listSessions(tmux.run), []);
});

test("tmux missing from PATH yields an empty result, not an error", async () => {
    const tmux = stubRunner({ code: SPAWN_FAILED, stdout: "", stderr: "spawn tmux ENOENT" });
    assert.deepEqual(await listSessions(tmux.run), []);
    assert.deepEqual(await sessionExists({ name: "codeharbor" }, tmux.run), { exists: false });
    assert.deepEqual(await killSession({ name: "codeharbor" }, tmux.run), {});
});

test("a runner that rejects outright is still not an RPC error", async () => {
    const exploding: CommandRunner = async () => {
        throw new Error("spawn tmux ENOENT");
    };
    assert.deepEqual(await listSessions(exploding), []);
    assert.deepEqual(await sessionExists({ name: "codeharbor" }, exploding), { exists: false });
});

test("sessionExists matches an exact name and rejects a mere prefix", async () => {
    const tmux = stubRunner(OK(REALISTIC_OUTPUT));
    assert.deepEqual(await sessionExists({ name: "codeharbor" }, tmux.run), { exists: true });
    assert.deepEqual(await sessionExists({ name: "my session: staging" }, tmux.run), { exists: true });
    assert.deepEqual(await sessionExists({ name: "codeharb" }, tmux.run), { exists: false });
    assert.deepEqual(await sessionExists({ name: "absent" }, tmux.run), { exists: false });
});

test("sessionExists is false when the server has no sessions at all", async () => {
    const tmux = stubRunner(OK(""));
    assert.deepEqual(await sessionExists({ name: "codeharbor" }, tmux.run), { exists: false });
});

test("killSession targets the exact name through argv, never a shell string", async () => {
    const tmux = stubRunner(OK(""));
    assert.deepEqual(await killSession({ name: "my session: staging" }, tmux.run), {});
    assert.deepEqual(tmux.calls, [["kill-session", "-t", "=my session: staging"]]);
});

test("killSession passes hostile names verbatim as one argv element", async () => {
    const tmux = stubRunner(OK(""));
    const hostile = "; rm -rf $HOME #";
    await killSession({ name: hostile }, tmux.run);
    assert.deepEqual(tmux.calls[0], ["kill-session", "-t", `=${hostile}`]);
});

test("killing an absent session is a successful no-op", async () => {
    const tmux = stubRunner({ code: 1, stdout: "", stderr: "can't find session: ghost" });
    assert.deepEqual(await killSession({ name: "ghost" }, tmux.run), {});
});

test("a malformed name IS an error, unlike a missing tmux", async () => {
    const tmux = stubRunner(OK(""));
    await assert.rejects(() => sessionExists({ name: "" }, tmux.run), /non-empty string name/);
    await assert.rejects(
        () => killSession({ name: undefined as unknown as string }, tmux.run),
        /non-empty string name/,
    );
    assert.deepEqual(tmux.calls, [], "no tmux invocation for a rejected request");
});

test("the tmux group is registered under its frozen wire names", async () => {
    assert.deepEqual(Object.keys(TMUX_METHODS).sort(), [
        "tmux.killSession",
        "tmux.listSessions",
        "tmux.sessionExists",
    ]);
    assert.equal(RPC_TMUX_METHODS.listSessions, "tmux.listSessions");
    assert.equal(RPC_TMUX_METHODS.sessionExists, "tmux.sessionExists");
    assert.equal(RPC_TMUX_METHODS.killSession, "tmux.killSession");

    // Reaches the dispatcher rather than falling through to method-not-found.
    // Deliberately shape-only: this host may or may not have a tmux server, and
    // either way the call must succeed with an array.
    const response = await dispatch({ jsonrpc: "2.0", id: 1, method: "tmux.listSessions" });
    assert.ok(response !== null && "result" in response, `expected a result, got ${JSON.stringify(response)}`);
    assert.ok(Array.isArray(response.result));

    const unknown = await dispatch({ jsonrpc: "2.0", id: 2, method: "tmux.bogus" });
    assert.ok(unknown !== null && "error" in unknown);
    assert.equal(unknown.error.code, RPC_METHOD_NOT_FOUND);
});

// A floor, not a pin: the tmux group required version 3, and later additive
// bumps (server.info's serverId took it to 4) must not fail this.
test("the schema version was bumped for the tmux group", () => {
    assert.ok(RPC_SCHEMA_VERSION >= 3, `expected >= 3, got ${RPC_SCHEMA_VERSION}`);
});
