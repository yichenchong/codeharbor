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

// Behaves like the real `tmux list-sessions -F <format>`: it reads the LITERAL
// text the code put in front of the first `#{...}` directive and echoes it back
// at the head of every record, exactly as tmux expands a format string. That
// literal is the per-call record marker, so a test cannot accidentally pass by
// hard-coding one — and the adversarial cases below can prove that a line
// WITHOUT it is rejected.
function listingRunner(records: string[]): { run: CommandRunner; calls: string[][] } {
    const calls: string[][] = [];
    return {
        calls,
        run: async (argv) => {
            calls.push(argv);
            const format = argv[argv.indexOf("-F") + 1] ?? "";
            const marker = format.slice(0, format.indexOf("#{"));
            return OK(records.map((record) => marker + record).join("\n") + "\n");
        },
    };
}

// What `tmux list-sessions -F LIST_SESSIONS_FORMAT` actually emits: windows,
// created (unix seconds), attached flag, then the name — which holds spaces
// verbatim. The colon here is deliberate parser hardening: tmux 3.6 rewrites
// `:` to `_` at creation, but that normalization is the SERVER's choice and
// not something the parser may lean on (older tmux, or a name that arrived by
// another route). A colon must land in `name`, never split a field.
const REALISTIC_RECORDS = [
    "3\t1753372800\t1\tcodeharbor",
    "1\t1753376400\t0\tmy session: staging",
    "12\t1753380000\t0\tagent-42",
];
const REALISTIC_OUTPUT = [...REALISTIC_RECORDS, ""].join("\n");

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
    const tmux = listingRunner(REALISTIC_RECORDS);
    const sessions = await listSessions(tmux.run);

    assert.equal(tmux.calls.length, 1);
    const [subcommand, flag, format] = tmux.calls[0];
    assert.equal(subcommand, "list-sessions");
    assert.equal(flag, "-F");
    assert.ok(format.endsWith("\t" + LIST_SESSIONS_FORMAT), format);
    assert.equal(sessions.length, 3);
    assert.equal(sessions[1].name, "my session: staging");
});

// SECURITY: the record marker is what makes a remote-controlled session name
// unable to forge a listing entry, so it must be unguessable AND fresh for
// every call — a fixed marker leaks in the first listing the attacker sees.
test("the record marker is non-empty and different on every listing", async () => {
    const first = listingRunner(REALISTIC_RECORDS);
    const second = listingRunner(REALISTIC_RECORDS);
    await listSessions(first.run);
    await listSessions(second.run);

    const markerOf = (calls: string[][]) => {
        const format = calls[0][2];
        return format.slice(0, format.indexOf("#{"));
    };
    const a = markerOf(first.calls);
    const b = markerOf(second.calls);
    assert.ok(a.length > 12, `marker too short to be unguessable: ${JSON.stringify(a)}`);
    assert.ok(a.endsWith("\t"), "the marker must be its own field");
    assert.ok(!a.slice(0, -1).includes("\t"), "the marker may not contain the field delimiter");
    assert.notEqual(a, b, "the marker must be freshly generated per call");
});

// SECURITY: a session name is attacker-controlled data echoed back inside the
// listing. If it could carry a raw newline plus three tabs it would inject a
// whole extra record — an invented session the client would then show, adopt or
// kill. tmux escapes newlines in names today, but the parser must not depend on
// that: an unmarked line is not a record.
test("a session name carrying a raw newline cannot forge a listing entry", async () => {
    const tmux = listingRunner([
        "1\t1753372800\t0\tinnocent\n9\t1753380000\t1\tFORGED-admin-session",
        "2\t1753372900\t0\treal",
    ]);
    const sessions = await listSessions(tmux.run);

    assert.deepEqual(
        sessions.map((session) => session.name),
        ["innocent", "real"],
        "only marked lines are records; the injected continuation line is dropped",
    );
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
    const tmux = listingRunner(REALISTIC_RECORDS);
    assert.deepEqual(await sessionExists({ name: "codeharbor" }, tmux.run), { exists: true });
    assert.deepEqual(await sessionExists({ name: "my session: staging" }, tmux.run), { exists: true });
    assert.deepEqual(await sessionExists({ name: "codeharb" }, tmux.run), { exists: false });
    assert.deepEqual(await sessionExists({ name: "absent" }, tmux.run), { exists: false });
});

// SECURITY: tmux's own `-t` target grammar does prefix and fnmatch matching, so
// asking tmux "does codeharb* exist" would answer yes. sessionExists compares
// against the listing instead, and must therefore treat glob metacharacters and
// tmux's `=`/`$` target sigils as ORDINARY CHARACTERS of the name — otherwise a
// caller could probe for, or later act on, a session it did not name.
test("sessionExists never glob- or sigil-matches another session", async () => {
    const tmux = listingRunner(REALISTIC_RECORDS);
    for (const probe of ["*", "codeharb*", "codeharbor?", "[c]odeharbor", "=codeharbor", "$0"]) {
        assert.deepEqual(
            await sessionExists({ name: probe }, tmux.run),
            { exists: false },
            `"${probe}" must not match any real session`,
        );
    }
    // ...and the probe never reaches tmux's argv at all: the listing is the
    // only thing consulted, so there is nothing for tmux to interpret.
    for (const argv of tmux.calls)
        assert.deepEqual(argv.slice(0, 2), ["list-sessions", "-F"]);
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

// SECURITY: two distinct ways a hostile name could hit the wrong session.
// (a) An option-looking name: `-a`, `-C`, `--`. Without the `=` sigil the name
//     is the first character of the argv element and tmux's getopt would read
//     it as a FLAG (`kill-session -a` kills every OTHER session).
// (b) A glob: tmux's target grammar falls back to prefix then fnmatch matching,
//     so a bare `-t 'ch_*'` destroys whatever it happens to hit. `=` pins the
//     lookup to an exact name.
test("an option-shaped or glob name cannot be read as a flag or match another session", async () => {
    for (const name of ["-a", "-C", "--", "-t", "*", "ch_*_term", "?", "[abc]"]) {
        const tmux = stubRunner(OK(""));
        await killSession({ name }, tmux.run);
        assert.deepEqual(
            tmux.calls[0],
            ["kill-session", "-t", `=${name}`],
            `"${name}" must reach tmux as one exact-match target, not as an option or pattern`,
        );
        // The `=` sigil is what makes it neither: it is the first character, so
        // getopt sees no leading `-`, and it disables prefix/fnmatch matching.
        assert.ok(tmux.calls[0][2].startsWith("="));
    }
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

// tmux's output arrives over an SSH channel, which may translate line endings.
// A trailing CR must be stripped before the fields are read, or it lands inside
// the session NAME and every exact-name comparison (sessionExists, adopt, kill)
// silently stops matching.
test("a CRLF listing parses the same as an LF one", () => {
    assert.deepEqual(parseSessions("3\t1753372800\t1\tcodeharbor\r\n"), [
        { name: "codeharbor", windows: 3, created: 1753372800, attached: true },
    ]);
});

test("a MARKED line with too few fields is skipped, not half-parsed", async () => {
    // The marker proves tmux emitted the line, but a truncated record still has
    // no name to report: it must be dropped rather than yielding an entry with
    // an empty or undefined name that a later kill could act on.
    const tmux = listingRunner(["1\t1753372800\t0", "2\t1753372900\t0\treal"]);
    assert.deepEqual(
        (await listSessions(tmux.run)).map((session) => session.name),
        ["real"],
    );
});
