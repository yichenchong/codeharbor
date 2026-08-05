import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";

import {
    LIST_SESSIONS_FORMAT,
    execFileRunner,
    SPAWN_FAILED,
    TMUX_TARGET_MAX_LENGTH,
    isSafeTmuxTarget,
    tmuxSafeName,
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

// The other thing tmux says when there is no server. When the socket file has
// never been created, tmux does not mention a server at all: it reports the
// failed connect. That is the ordinary state of a machine where tmux has never
// been started, so it must read as "no sessions", not as a server failure.
test("a missing tmux socket also yields an empty result, not an error", async () => {
    const stderr = "error connecting to /tmp/tmux-1001/default (No such file or directory)";
    const tmux = stubRunner({ code: 1, stdout: "", stderr });
    assert.deepEqual(await listSessions(tmux.run), []);
    const killTmux = stubRunner({ code: 1, stdout: "", stderr });
    assert.deepEqual(await killSession({ name: "codeharbor" }, killTmux.run), {});
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
    assert.deepEqual(await killSession({ name: "my session staging" }, tmux.run), {});
    assert.deepEqual(tmux.calls, [["kill-session", "-t", "=my session staging"]]);
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
test("killSession rejects tmux target separators instead of killing another session", async () => {
    const tmux = stubRunner(OK(""));
    // A raw control character is refused for the same reason: tmux vis-escapes
    // it in its own output, so the stored name never matches the session tmux
    // actually made, and an unanchored match can land on a different one.
    for (const name of ["other:0", "other.0", "   ", "bell\u0007", "\u007f", "nl\nx"]) {
        await assert.rejects(
            () => killSession({ name }, tmux.run),
            /tmux-safe session name|non-empty string name/,
            name,
        );
    }
    assert.deepEqual(tmux.calls, [], "unsafe targets must not reach tmux");
});

test("unexpected tmux failures are surfaced", async () => {
    const failed = stubRunner({ code: 1, stdout: "", stderr: "permission denied" });
    await assert.rejects(() => listSessions(failed.run), /tmux list-sessions failed/);

    const killFailed = stubRunner({ code: 1, stdout: "", stderr: "permission denied" });
    await assert.rejects(() => killSession({ name: "codeharbor" }, killFailed.run), /tmux kill-session failed/);
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

// Number.parseInt is a PREFIX scanner: it consumes what it can and ignores the
// rest, so "3abc" became 3 and "1753372800junk" became a valid timestamp. A
// record whose numeric fields are not numbers is not a record this format
// produced; accepting it and silently discarding the tail means the listing the
// client acts on (attach, kill) was assembled from data the parser did not
// actually understand.
test("a partly-numeric field is skipped, not read as its numeric prefix", () => {
    const good = { name: "real", windows: 3, created: 1753372800, attached: true };
    // Sanity: the well-formed record this varies from does parse.
    assert.deepEqual(parseSessions("3\t1753372800\t1\treal"), [good]);

    for (const line of [
        "3abc\t1753372800\t1\tsneaky", // trailing garbage on the window count
        "3\t1753372800junk\t1\tsneaky", // trailing garbage on the timestamp
        " 3\t1753372800\t1\tsneaky", // leading whitespace parseInt skips
        "+3\t1753372800\t1\tsneaky", // a sign parseInt accepts
        "3.7\t1753372800\t1\tsneaky", // a fraction truncated to 3
        "0x10\t1753372800\t1\tsneaky", // parseInt(_, 10) reads this as 0
        "\t1753372800\t1\tsneaky", // an empty numeric field
        "-1\t1753372800\t1\tsneaky", // a negative window count parseInt accepts
        "3\t-1753372800\t1\tsneaky", // a negative timestamp parseInt accepts
    ]) {
        assert.deepEqual(parseSessions(line), [], line);
    }

    // `#{?session_attached,1,0}` emits exactly "1" or "0". Anything else was
    // read as `=== "1"` — false — so a malformed record was reported as a real,
    // detached session that a later kill could act on.
    assert.deepEqual(parseSessions("3\t1753372800\tyes\tsneaky"), []);
    assert.deepEqual(parseSessions("3\t1753372800\t\tsneaky"), []);

    // A timestamp past 2^53 passes a digits-only check but rounds to a
    // different instant the moment it becomes a JS number.
    assert.deepEqual(parseSessions("3\t99999999999999999999\t1\tsneaky"), []);
});

// --- target names (SPEC 5.2) ------------------------------------------------
//
// A terminal pane's tmux target is minted once, on the server, and every client
// then uses it verbatim as a session name and as a `-t '=<target>'` argument.
// Whether a given string can survive that round trip is tmux's grammar, not a
// matter of taste, so it gets its own tests.

test("isSafeTmuxTarget accepts what the server mints", () => {
    // The real shape: `ch_` plus two UUIDs. Nothing else the product produces
    // ever reaches tmux as a session name.
    assert.equal(
        isSafeTmuxTarget("ch_2f1c9a30-4c1b-4e3e-8d0e-6a1f9b2c3d4e_8b7a6c5d-4e3f-2a1b-9c8d-7e6f5a4b3c2d"),
        true,
    );
    assert.equal(isSafeTmuxTarget("ch_a"), true);
    assert.equal(isSafeTmuxTarget("ch-a_B9"), true);
    assert.equal(isSafeTmuxTarget("x".repeat(TMUX_TARGET_MAX_LENGTH)), true);
});

test("isSafeTmuxTarget refuses anything tmux reads as structure", () => {
    // `:` and `.` are the session/window/pane separators, so `-t 'ch_a:0'`
    // addresses window 0 of session `ch_a`. tmux will not even let a session
    // hold them: it rewrites both to `_` at creation time.
    assert.equal(isSafeTmuxTarget("ch_a:0"), false);
    assert.equal(isSafeTmuxTarget("ch_a.1"), false);
    // `$`, `@` and `%` introduce tmux's own session/window/pane ids, `=` is the
    // exact-match prefix, and `*`/`?` are its fnmatch wildcards — each of them
    // makes a target resolve to something other than the name as typed.
    for (const bad of ["$3", "@1", "%2", "=ch_a", "ch_*", "ch_?"]) {
        assert.equal(isSafeTmuxTarget(bad), false, `expected ${bad} to be unsafe`);
    }
    // Whitespace, control characters and the empty name have no business in a
    // session name either; the empty one is what tmux itself rejects outright.
    for (const bad of ["", " ", "ch a", "ch\ta", "ch\na", "ch\u0007a"]) {
        assert.equal(isSafeTmuxTarget(bad), false, `expected ${JSON.stringify(bad)} to be unsafe`);
    }
    // A bound, so a target cannot be stored here and truncated into a different
    // session's name somewhere downstream.
    assert.equal(isSafeTmuxTarget("x".repeat(TMUX_TARGET_MAX_LENGTH + 1)), false);
    assert.equal(isSafeTmuxTarget("-d"), false);
});

// This mirrors tmux's session_check_name(), verified against tmux 3.6:
// `tmux new-session -s 'ch_a.b:c'` creates a session whose #{session_name} is
// `ch_a_b_c`. Repairing a stored target this way is therefore not a retarget —
// it is the stored value finally naming the session tmux made.
test("tmuxSafeName rewrites exactly the two characters tmux rewrites", () => {
    assert.equal(tmuxSafeName("ch_a.b:c"), "ch_a_b_c");
    assert.equal(tmuxSafeName("ch_plain"), "ch_plain");
    assert.equal(isSafeTmuxTarget(tmuxSafeName("ch_a.b:c")), true);
    // It repairs the tmux grammar and nothing else: a name carrying a space is
    // still not a target this server will accept, it is simply not this
    // function's business.
    assert.equal(tmuxSafeName("ch a.b"), "ch a_b");
});

// execFile reports two very different outcomes with neither an exit code nor
// an errno: a timeout, and output past its maxBuffer. They used to collapse
// onto one message, so a listing that was merely enormous was reported as
// "tmux did not respond within 10000ms" — which sends whoever has to debug it
// hunting an unresponsive tmux server that is not there. Driven through a real
// child process because that classification only exists in execFileRunner.
test("output past the execFile buffer is reported as output, not as a timeout", async () => {
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-tmux-runner-"));
    const fake = path.join(dir, "tmux");
    // Nine megabytes: comfortably past the eight-megabyte execFile buffer, and
    // small enough that the child finishes far inside the ten-second timeout,
    // so the test cannot pass for the wrong reason.
    writeFileSync(
        fake,
        `#!/bin/sh\nexec ${process.execPath} -e "process.stdout.write('a'.repeat(9*1024*1024))"\n`,
        { mode: 0o755 },
    );
    const previous = process.env.CODEHARBOR_TMUX;
    process.env.CODEHARBOR_TMUX = fake;
    try {
        // Reading the override per invocation is itself the contract here: a
        // binary captured at import time would ignore this assignment and the
        // test would silently run the host's real tmux.
        const result = await execFileRunner(["list-sessions"]);
        assert.equal(result.code, SPAWN_FAILED);
        assert.match(result.stderr, /produced more than \d+ bytes of output/);
        assert.doesNotMatch(result.stderr, /did not respond/);
        // And it stays a REAL failure: neither "no tmux installed" nor "no
        // server running", both of which would answer with an empty listing and
        // invite the client to start a second session beside the live one.
        await assert.rejects(
            () => listSessions(execFileRunner),
            /tmux list-sessions failed: tmux produced more than/,
        );
    } finally {
        if (previous === undefined) delete process.env.CODEHARBOR_TMUX;
        else process.env.CODEHARBOR_TMUX = previous;
        rmSync(dir, { recursive: true, force: true });
    }
});

// The other end of the same classification, and the one that decides what a
// machine WITHOUT tmux sees. execFile reports a failure to spawn by setting a
// string errno on the error and handing back an EMPTY stderr — not a null one —
// so the runner has to fall back to the error's own message to keep the errno
// text. It used to keep the empty string instead, isMissingTmux then matched
// nothing, and the very first listing on a tmux-less host failed with
// "tmux list-sessions failed: exit code -1" instead of answering "no sessions".
test("a tmux that cannot be spawned reports the errno, and lists as empty", async () => {
    const dir = mkdtempSync(path.join(os.tmpdir(), "ch-tmux-absent-"));
    const missing = path.join(dir, "definitely-not-tmux");
    const previous = process.env.CODEHARBOR_TMUX;
    process.env.CODEHARBOR_TMUX = missing;
    try {
        const result = await execFileRunner(["list-sessions"]);
        assert.equal(result.code, SPAWN_FAILED);
        assert.match(result.stderr, /ENOENT/);
        // Rule 1: absence is not failure.
        assert.deepEqual(await listSessions(execFileRunner), []);
        assert.deepEqual(await killSession({ name: "ch_absent" }, execFileRunner), {});
    } finally {
        if (previous === undefined) delete process.env.CODEHARBOR_TMUX;
        else process.env.CODEHARBOR_TMUX = previous;
        rmSync(dir, { recursive: true, force: true });
    }
});
