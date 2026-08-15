import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";

import {
    LIST_FIELD_SEPARATOR,
    LIST_SESSIONS_FORMAT,
    LIST_ACTIVITY_FORMAT,
    execFileRunner,
    SPAWN_FAILED,
    TMUX_TARGET_MAX_LENGTH,
    isSafeTmuxTarget,
    tmuxSafeName,
    parseSessions,
    parseWindowActivity,
    listSessions,
    sessionExists,
    killSession,
    paneActivity,
    TMUX_METHODS,
} from "../src/tmux.ts";
import type { CommandResult, CommandRunner, TerminalPaneSource } from "../src/tmux.ts";
import { RPC_TMUX_METHODS } from "../src/rpc-types.ts";
import type { TerminalPaneTarget } from "../src/rpc-types.ts";
import { isInvalidParams } from "../src/validate.ts";
import {
    dispatch,
    RPC_INVALID_PARAMS,
    RPC_METHOD_NOT_FOUND,
    RPC_SCHEMA_VERSION,
} from "../src/codeharbord.ts";

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

// Every fixture line below is assembled from the SHIPPED separator rather than
// spelling one. A fixture that hard-codes bytes real tmux never emits is exactly
// what hid the tab bug for a release: the parsers agreed with the fixtures and
// both disagreed with tmux. Change LIST_FIELD_SEPARATOR and these lines follow
// it; a parser that did not would fail here.
const SEP = LIST_FIELD_SEPARATOR;
const rec = (...fields: string[]): string => fields.join(SEP);

// What `tmux list-sessions -F LIST_SESSIONS_FORMAT` actually emits: windows,
// created (unix seconds), attached flag, then the name — which holds spaces
// verbatim. The colon here is deliberate parser hardening: tmux 3.6 rewrites
// `:` to `_` at creation, but that normalization is the SERVER's choice and
// not something the parser may lean on (older tmux, or a name that arrived by
// another route). A colon must land in `name`, never split a field.
const REALISTIC_RECORDS = [
    rec("3", "1753372800", "1", "codeharbor"),
    rec("1", "1753376400", "0", "my session: staging"),
    rec("12", "1753380000", "0", "agent-42"),
];
const REALISTIC_OUTPUT = [...REALISTIC_RECORDS, ""].join("\n");

test("the list format is machine-readable and puts the name last", () => {
    assert.equal(
        LIST_SESSIONS_FORMAT,
        "#{session_windows} #{session_created} #{?session_attached,1,0} #{session_name}",
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
    assert.deepEqual(parseSessions(rec("2", "1753372800", "0", "weird name")), [
        { name: "weird name", windows: 2, created: 1753372800, attached: false },
    ]);
});

// THE LESSON, not merely the fix. Both listing formats used a TAB, and a
// tab-separated listing is something the tmux we run in production NEVER emits:
// tmux passes a non-printable byte through a format only when the CLIENT
// considers itself UTF-8-capable, and replaces it with `_` otherwise
// (utf8_sanitize). A client is not UTF-8-capable when its locale is not a UTF-8
// one AND it is not running inside a tmux session — which is exactly the daemon,
// launched by an SSH exec with neither LANG nor TMUX set. So every record failed
// its `marker + separator` anchor: parseSessions returned NOTHING for a host
// full of sessions and paneActivity reported every pane dead. It looked fine by
// hand because a UTF-8 shell, or any shell inside a tmux window, gets the tabs
// verbatim. See LIST_FIELD_SEPARATOR for the measurements, and
// tmux-live.test.ts for the guard that runs real tmux in the production
// environment.
//
// So a TAB-separated line must NOT parse. If someone reintroduces a tab
// separator, these two assertions are what says no — and the third is the price
// of a printable separator, paid and pinned: the session name comes LAST, so a
// name full of separators still round-trips verbatim.
test("a TAB-separated line is not a record, because a sanitizing tmux never sends one", () => {
    assert.deepEqual(parseSessions("3\t1753372800\t1\tcodeharbor"), []);
    assert.equal(parseWindowActivity("1786805795\t0\tch_a_1").size, 0);

    const spaced = parseWindowActivity(rec("1786805795", "0", "my session: staging"));
    assert.deepEqual([...spaced.keys()], ["my session: staging"]);
    assert.equal(spaced.get("my session: staging")?.lastActivityMs, 1786805795000);
});

test("malformed lines are skipped, not fatal", () => {
    const stdout = [
        "garbage",
        rec("1", "not-a-number", "0", "bad"),
        rec("4", "1753372800", "1", "good"),
        "",
    ].join("\n");
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
    assert.ok(format.endsWith(SEP + LIST_SESSIONS_FORMAT), format);
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
    assert.ok(a.endsWith(SEP), "the marker must be its own field");
    assert.ok(!a.slice(0, -1).includes(SEP), "the marker may not contain the field delimiter");
    assert.notEqual(a, b, "the marker must be freshly generated per call");
});

// SECURITY: a session name is attacker-controlled data echoed back inside the
// listing. If it could carry a raw newline plus three tabs it would inject a
// whole extra record — an invented session the client would then show, adopt or
// kill. tmux escapes newlines in names today, but the parser must not depend on
// that: an unmarked line is not a record.
test("a session name carrying a raw newline cannot forge a listing entry", async () => {
    const tmux = listingRunner([
        rec("1", "1753372800", "0", "innocent")
            + "\n"
            + rec("9", "1753380000", "1", "FORGED-admin-session"),
        rec("2", "1753372900", "0", "real"),
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

// The other absence on the kill path: tmux is installed but no server is
// running, so the session the caller wants gone is already gone. Same rule 1 as
// the listing — an absence is a successful no-op, not a failure.
test("killing a session with no tmux server running is a successful no-op", async () => {
    const noServer = stubRunner({
        code: 1,
        stdout: "",
        stderr: "no server running on /tmp/tmux-1000/default",
    });
    assert.deepEqual(await killSession({ name: "ghost" }, noServer.run), {});

    const noSocket = stubRunner({
        code: 1,
        stdout: "",
        stderr: "error connecting to /tmp/tmux-1000/default (No such file or directory)",
    });
    assert.deepEqual(await killSession({ name: "ghost" }, noSocket.run), {});
});

// SECURITY: tmux's `=` exact-match prefix is not a general quoting mechanism.
// A LEADING `$` is tmux's session-ID form and is resolved BEFORE the
// exact-name lookup, so `=` does not shield it. Verified against tmux 3.6:
// with sessions `victim` ($0) and a second session literally named `$0`,
// `tmux kill-session -t '=$0'` killed `victim` and left `$0` alive. A target of
// `$0` therefore destroys an unrelated session and everything running in it,
// while tmux.sessionExists — which compares against the listing — reports that
// no such session exists. The name must be refused before the target is built.
test("killSession refuses a name tmux would read as a session ID", async () => {
    const tmux = stubRunner(OK(""));
    for (const name of ["$0", "$1", "$", "$victim"]) {
        await assert.rejects(
            () => killSession({ name }, tmux.run),
            /tmux-safe session name/,
            name,
        );
    }
    assert.deepEqual(tmux.calls, [], "an ID-shaped target must not reach tmux");

    // Only the FIRST character carries the ID meaning: tmux matches `a$0`
    // literally, so refusing it would make a legitimately named session
    // un-killable.
    const literal = stubRunner(OK(""));
    assert.deepEqual(await killSession({ name: "a$0" }, literal.run), {});
    assert.deepEqual(literal.calls, [["kill-session", "-t", "=a$0"]]);
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
        "tmux.paneActivity",
        "tmux.sessionExists",
    ]);
    assert.equal(RPC_TMUX_METHODS.listSessions, "tmux.listSessions");
    assert.equal(RPC_TMUX_METHODS.sessionExists, "tmux.sessionExists");
    assert.equal(RPC_TMUX_METHODS.killSession, "tmux.killSession");
    assert.equal(RPC_TMUX_METHODS.paneActivity, "tmux.paneActivity");

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
    assert.deepEqual(parseSessions(rec("3", "1753372800", "1", "codeharbor") + "\r\n"), [
        { name: "codeharbor", windows: 3, created: 1753372800, attached: true },
    ]);
});

test("a MARKED line with too few fields is skipped, not half-parsed", async () => {
    // The marker proves tmux emitted the line, but a truncated record still has
    // no name to report: it must be dropped rather than yielding an entry with
    // an empty or undefined name that a later kill could act on.
    const tmux = listingRunner([
        rec("1", "1753372800", "0"),
        rec("2", "1753372900", "0", "real"),
    ]);
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
    assert.deepEqual(parseSessions(rec("3", "1753372800", "1", "real")), [good]);

    for (const line of [
        rec("3abc", "1753372800", "1", "sneaky"), // trailing garbage on the window count
        rec("3", "1753372800junk", "1", "sneaky"), // trailing garbage on the timestamp
        rec("\t3", "1753372800", "1", "sneaky"), // leading whitespace parseInt skips
        rec("+3", "1753372800", "1", "sneaky"), // a sign parseInt accepts
        rec("3.7", "1753372800", "1", "sneaky"), // a fraction truncated to 3
        rec("0x10", "1753372800", "1", "sneaky"), // parseInt(_, 10) reads this as 0
        rec("", "1753372800", "1", "sneaky"), // an empty numeric field
        rec("-1", "1753372800", "1", "sneaky"), // a negative window count parseInt accepts
        rec("3", "-1753372800", "1", "sneaky"), // a negative timestamp parseInt accepts
    ]) {
        assert.deepEqual(parseSessions(line), [], line);
    }

    // `#{?session_attached,1,0}` emits exactly "1" or "0". Anything else was
    // read as `=== "1"` — false — so a malformed record was reported as a real,
    // detached session that a later kill could act on.
    assert.deepEqual(parseSessions(rec("3", "1753372800", "yes", "sneaky")), []);
    assert.deepEqual(parseSessions(rec("3", "1753372800", "", "sneaky")), []);

    // A timestamp past 2^53 passes a digits-only check but rounds to a
    // different instant the moment it becomes a JS number.
    assert.deepEqual(parseSessions(rec("3", "99999999999999999999", "1", "sneaky")), []);
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

// --- tmux.paneActivity -------------------------------------------------------
//
// The method exists because a Dev Session the user is not looking at has no
// client attached, so the client sees no PTY bytes from it and its panes settle
// to Idle forever. Everything below therefore turns on one distinction: "tmux
// says this pane has been quiet since T" versus "we do not know". The second
// one MUST NOT be reported as the first.

// The panes the workspace would hand the scan. Two Dev Sessions, so the filter
// has something to narrow.
const ACTIVITY_PANES: TerminalPaneTarget[] = [
    { devSessionId: "ds-a", terminalId: "t-a1", target: "ch_a_1" },
    { devSessionId: "ds-a", terminalId: "t-a2", target: "ch_a_2" },
    { devSessionId: "ds-b", terminalId: "t-b1", target: "ch_b_1" },
];

// Stands in for the workspace database, and records the ids it was asked for so
// a test can prove the filter reached the store rather than being applied (or
// forgotten) afterwards.
function paneSource(panes: TerminalPaneTarget[] = ACTIVITY_PANES): {
    source: TerminalPaneSource;
    asked: string[][];
} {
    const asked: string[][] = [];
    return {
        asked,
        source: (devSessionIds) => {
            asked.push([...devSessionIds]);
            if (devSessionIds.length === 0) return panes;
            return panes.filter((pane) => devSessionIds.includes(pane.devSessionId));
        },
    };
}

test("the activity format asks for window_activity, with the name last", () => {
    assert.equal(
        LIST_ACTIVITY_FORMAT,
        "#{window_activity} #{?session_attached,1,0} #{session_name}",
    );
    // `#{pane_activity}` does not exist on tmux 3.6 and expands to nothing in an
    // otherwise successful listing, which is the silent failure this whole
    // method is built around. Asking for it would report every pane idle.
    assert.doesNotMatch(LIST_ACTIVITY_FORMAT, /pane_activity/);
});

test("one tmux invocation lists every window on the server", async () => {
    const stub = listingRunner([rec("1786805795", "0", "ch_a_1")]);
    const panes = paneSource();
    await paneActivity({}, panes.source, stub.run);
    assert.equal(stub.calls.length, 1);
    const argv = stub.calls[0];
    assert.equal(argv[0], "list-windows");
    // `-a` is what makes this one call rather than one per session: without it
    // tmux lists only the current session's windows.
    assert.ok(argv.includes("-a"));
    assert.match(argv[argv.indexOf("-F") + 1], /#\{window_activity\}/);
});

test("activity is reported in milliseconds, taking the newest window", async () => {
    // ch_a_1 has three windows: tmux lists them in index order, so the answer
    // must be the MAXIMUM and not the first or the last line seen. A user
    // working in window 2 of a session whose window 0 is idle is otherwise
    // reported as idle.
    const stub = listingRunner([
        rec("1786805795", "0", "ch_a_1"),
        rec("1786805999", "0", "ch_a_1"),
        rec("1786805800", "0", "ch_a_1"),
        rec("1786800000", "1", "ch_a_2"),
    ]);
    const panes = paneSource();
    const result = await paneActivity({}, panes.source, stub.run);
    const byTerminal = new Map(result.panes.map((pane) => [pane.terminalId, pane]));
    assert.deepEqual(byTerminal.get("t-a1"), {
        devSessionId: "ds-a",
        terminalId: "t-a1",
        target: "ch_a_1",
        lastActivityMs: 1786805999_000,
        attached: false,
        alive: true,
    });
    // The attached flag rides along, and seconds are scaled here too.
    assert.deepEqual(byTerminal.get("t-a2"), {
        devSessionId: "ds-a",
        terminalId: "t-a2",
        target: "ch_a_2",
        lastActivityMs: 1786800000_000,
        attached: true,
        alive: true,
    });
    // nowMs is the SERVER's clock, read AFTER the listing, and it is the value
    // the client subtracts a timestamp from. It must be a real wall clock and
    // not a monotonic counter or a zero, or every age would be nonsense.
    const spread = Math.abs(result.nowMs - Date.now());
    assert.ok(spread < 5_000, `nowMs is not a wall clock: off by ${spread}ms`);
});

// THE advisory case. An unrecognised `#{...}` does not fail a tmux listing: the
// run succeeds and the field arrives EMPTY. Reporting that as 0 would date the
// pane to 1970 and mark it permanently idle on any future tmux whose format
// names differ; reporting it as `now` would mark it permanently busy. Both would
// be silent. The pane is still ALIVE — we simply have no evidence about it.
test("an empty activity field is unknown, not the epoch", async () => {
    const stub = listingRunner([
        rec("", "0", "ch_a_1"),
        rec("\t\t", "1", "ch_a_2"),
        rec("not-a-number", "0", "ch_b_1"),
    ]);
    const panes = paneSource();
    const result = await paneActivity({}, panes.source, stub.run);
    for (const pane of result.panes) {
        assert.equal(pane.lastActivityMs, null, `${pane.target} invented a timestamp`);
        assert.equal(pane.alive, true, `${pane.target} was dropped instead of reported`);
    }
    assert.notEqual(result.panes[0].lastActivityMs, 0);
    // The rest of the record still parses: an unknown activity field costs the
    // caller the timestamp, not the session.
    assert.equal(result.panes[1].attached, true);
});

test("a pane whose target names no live session is reported dead, not idle", async () => {
    const stub = listingRunner([rec("1786805795", "1", "ch_a_1")]);
    const panes = paneSource();
    const result = await paneActivity({}, panes.source, stub.run);
    const dead = result.panes.filter((pane) => pane.target !== "ch_a_1");
    assert.equal(dead.length, 2);
    for (const pane of dead) {
        assert.deepEqual(
            { lastActivityMs: pane.lastActivityMs, attached: pane.attached, alive: pane.alive },
            { lastActivityMs: null, attached: false, alive: false },
            `${pane.target} was not reported as dead`,
        );
    }
});

// Rule 1 again: absence is not failure. The client polls this on a timer, so a
// host whose tmux server is simply not running must be ANSWERED — otherwise the
// user collects one error per tick for having no sessions yet.
test("no tmux server is answered with dead panes rather than thrown", async () => {
    const noServer: CommandResult = {
        code: 1,
        stdout: "",
        stderr: "no server running on /tmp/tmux-1000/default\n",
    };
    const panes = paneSource();
    const result = await paneActivity({}, panes.source, stubRunner(noServer).run);
    assert.equal(result.panes.length, ACTIVITY_PANES.length);
    assert.ok(result.panes.every((pane) => !pane.alive && pane.lastActivityMs === null));

    // Neither is a host without the binary at all.
    const missing: CommandResult = { code: SPAWN_FAILED, stdout: "", stderr: "spawn tmux ENOENT" };
    const absent = await paneActivity({}, paneSource().source, stubRunner(missing).run);
    assert.ok(absent.panes.every((pane) => !pane.alive));

    // A REAL failure still surfaces: a permission problem must not read as a
    // server full of dead panes.
    const denied: CommandResult = { code: 1, stdout: "", stderr: "permission denied" };
    await assert.rejects(
        () => paneActivity({}, paneSource().source, stubRunner(denied).run),
        /tmux list-windows failed: permission denied/,
    );
});

test("devSessionIds narrows the answer, and absence means every pane", async () => {
    const stub = listingRunner([rec("1786805795", "0", "ch_b_1")]);
    const filtered = paneSource();
    const result = await paneActivity({ devSessionIds: ["ds-b"] }, filtered.source, stub.run);
    assert.deepEqual(filtered.asked, [["ds-b"]]);
    assert.deepEqual(
        result.panes.map((pane) => pane.terminalId),
        ["t-b1"],
    );

    // Absent, null and empty all mean "every pane the workspace knows": the
    // client's periodic sweep sends no filter at all.
    for (const params of [undefined, null, {}, { devSessionIds: [] }]) {
        const all = paneSource();
        const answer = await paneActivity(params, all.source, listingRunner([]).run);
        assert.deepEqual(all.asked, [[]], `params ${JSON.stringify(params)} invented a filter`);
        assert.equal(answer.panes.length, ACTIVITY_PANES.length);
    }
});

test("a malformed devSessionIds is invalid params, not an empty filter", async () => {
    const rejected: unknown[] = [
        { devSessionIds: "ds-a" },
        { devSessionIds: [1] },
        { devSessionIds: ["ds-a", ""] },
        { devSessionIds: ["   "] },
        { devSessionIds: {} },
        [],
        "ds-a",
    ];
    for (const params of rejected) {
        const panes = paneSource();
        await assert.rejects(
            () => paneActivity(params, panes.source, listingRunner([]).run),
            (err: unknown) => isInvalidParams(err),
            `${JSON.stringify(params)} was accepted`,
        );
        // Validation happens BEFORE anything is listed, so a bad request never
        // reaches the database or spawns tmux.
        assert.deepEqual(panes.asked, []);
    }
});

test("tmux.paneActivity is dispatchable and reports bad params as -32602", async () => {
    // Params only: a well-formed call would open the workspace database, which
    // this unit test has no business creating. The param guard runs before the
    // pane source, so this exercises the dispatcher's wiring without one.
    const response = await dispatch({
        jsonrpc: "2.0",
        id: 1,
        method: RPC_TMUX_METHODS.paneActivity,
        params: { devSessionIds: "ds-a" },
    });
    assert.ok(response !== null && "error" in response, JSON.stringify(response));
    assert.equal(response.error.code, RPC_INVALID_PARAMS);
});

test("the schema version was bumped for tmux.paneActivity", () => {
    // A v7 server answers "method not found" and leaves the client with no way
    // to know an unattended pane is busy, so the mismatch has to be caught at
    // the compatibility gate instead of showing up as a wrong sidebar badge.
    assert.ok(RPC_SCHEMA_VERSION >= 8, `expected >= 8, got ${RPC_SCHEMA_VERSION}`);
});
