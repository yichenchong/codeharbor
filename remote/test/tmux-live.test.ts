// The guard the stubbed tmux suite structurally cannot be: it runs the REAL
// `tmux` binary and feeds its REAL output to the shipped parsers.
//
// It exists because of a bug the whole stubbed suite agreed with. Both listing
// formats separated their fields with a TAB, and every fixture in tmux.test.ts
// spelled that tab by hand — so the parsers and the fixtures agreed, and both
// disagreed with tmux. tmux replaces each non-printable byte of a format's
// output with `_` (utf8_sanitize) unless the tmux CLIENT considers itself
// UTF-8-capable, and a client is NOT UTF-8-capable when both of these hold:
//
//   * its locale is not a UTF-8 one (LC_ALL/LC_CTYPE/LANG unset is "POSIX"), and
//   * it is not itself running inside a tmux session (no `TMUX` in its
//     environment, which is tmux's other route to deciding it is UTF-8).
//
// Production hits both: `codeharbord` runs from an SSH exec, which has no LANG
// and no TMUX. So every record failed its `marker + separator` anchor,
// `tmux.listSessions` answered [] for a host full of sessions and
// `tmux.paneActivity` reported every pane dead. A developer checking by hand
// misses it twice over — from a normal UTF-8 shell, and from inside a tmux
// window even in the C locale.
//
// Which is why the runner below reproduces the PRODUCTION environment rather
// than the developer's, and why both halves matter: with `TMUX` left inherited
// this file passes against the tab separator it exists to reject, as it did on
// the first attempt. Verified both ways — under the tab it fails with an empty
// listing, under the shipped separator it passes.
//
// The server is private (`-L`) and starts without user configuration (`-f`),
// so tests neither touch somebody's sessions nor run their startup hooks.
//
// Skipped, never failed, when tmux is absent: a machine without tmux is a
// legitimate place to run the remote suite.

import { test } from "node:test";
import assert from "node:assert/strict";
import { execFile, execFileSync, spawn } from "node:child_process";

import { execFileRunner, killSession, listSessions, paneActivity, SPAWN_FAILED } from "../src/tmux.ts";
import type { CommandResult, CommandRunner } from "../src/tmux.ts";
import type { TerminalPaneTarget } from "../src/rpc-types.ts";

// One private socket per test process, so two concurrent runs cannot see each
// other's sessions.
const SOCKET = `ch-remote-test-${process.pid}`;
const SESSION = `ch_live_parse_${process.pid}`;

function tmuxAvailable(): boolean {
    try {
        execFileSync("tmux", ["-V"], { stdio: "ignore" });
        return true;
    } catch {
        return false;
    }
}

const SKIP = tmuxAvailable() ? false : "no tmux binary on this host";

// A CommandRunner shaped exactly like execFileRunner, except that it addresses
// the private server and strips the environment down to what an SSH exec gives
// the daemon. `TMUX`/`TMUX_PANE` are DELETED rather than overridden: their mere
// presence is what tells tmux it is talking to a UTF-8 terminal, and inheriting
// them from a developer running `npm test` inside tmux would make this guard
// silently accept the very bytes it is here to reject.
const PRODUCTION_ENV: NodeJS.ProcessEnv = {
    ...process.env,
    LC_ALL: "C",
    LANG: "C",
    LANGUAGE: "",
};
delete PRODUCTION_ENV.TMUX;
delete PRODUCTION_ENV.TMUX_PANE;

const runner: CommandRunner = (argv) =>
    new Promise<CommandResult>((resolve) => {
        execFile(
            "tmux",
            ["-L", SOCKET, "-f", "/dev/null", ...argv],
            { encoding: "utf8", env: PRODUCTION_ENV },
            (err, stdout, stderr) => {
                if (err === null) {
                    resolve({ code: 0, stdout, stderr });
                    return;
                }
                resolve({
                    code: typeof err.code === "number" ? err.code : SPAWN_FAILED,
                    stdout: stdout ?? "",
                    stderr: stderr !== "" ? stderr : err.message,
                });
            },
        );
    });

// (no wrapper around `runner`: the argv arrays below are clearer as literals)

test(
    "the real tmux binary's listing parses: a detached session is found, named and dated",
    { skip: SKIP },
    async (t) => {
        // `-d` so nothing is attached, which is also the state the pane-activity
        // method exists for.
        const created = await runner(["new-session", "-d", "-s", SESSION]);
        assert.equal(created.code, 0, `could not create the session: ${created.stderr}`);
        t.after(async () => {
            await runner(["kill-server"]);
        });

        // Output with NO client attached — the property the whole activity
        // subsystem rests on. `#{window_activity}` must advance for it.
        const typed = await runner([
            "send-keys",
            "-t",
            `=${SESSION}:`,
            "printf 'live\\n'",
            "Enter",
        ]);
        assert.equal(typed.code, 0, `could not produce output: ${typed.stderr}`);

        // (a) listSessions against real output. The session MUST be found: an
        // empty listing here is precisely the production failure this file was
        // written for, and it used to be what a sanitizing server produced.
        const sessions = await listSessions(runner);
        const mine = sessions.filter((session) => session.name === SESSION);
        assert.equal(
            mine.length,
            1,
            `real tmux listing did not parse; got ${JSON.stringify(sessions)}`,
        );
        assert.equal(mine[0].attached, false);
        assert.ok(mine[0].windows >= 1, JSON.stringify(mine[0]));
        // Seconds since the epoch, from tmux and not invented here.
        assert.ok(mine[0].created > 1_600_000_000, JSON.stringify(mine[0]));

        // (b) paneActivity against real output, joined against a pane row whose
        // target is that session. A NUMBER must come back: `null` is the daemon
        // saying it could not date the pane, which is what the tab bug produced
        // for every pane on the host.
        const pane: TerminalPaneTarget = {
            devSessionId: "ds-live",
            terminalId: "t-live",
            target: SESSION,
        };
        const result = await paneActivity({ devSessionIds: ["ds-live"] }, () => [pane], runner);
        assert.equal(result.panes.length, 1);
        const [reported] = result.panes;
        assert.equal(reported.target, SESSION);
        assert.equal(reported.alive, true, JSON.stringify(reported));
        assert.equal(reported.attached, false, JSON.stringify(reported));
        assert.equal(
            typeof reported.lastActivityMs,
            "number",
            `the pane could not be dated: ${JSON.stringify(reported)}`,
        );
        // Milliseconds on the server's own clock, and the reading is of THIS
        // session's output: it cannot predate the server we just started, and it
        // cannot be in the future.
        assert.ok(
            reported.lastActivityMs !== null && reported.lastActivityMs <= result.nowMs,
            `activity ${reported.lastActivityMs} is after nowMs ${result.nowMs}`,
        );
        assert.ok(
            reported.lastActivityMs !== null
                && result.nowMs - reported.lastActivityMs < 5 * 60 * 1000,
            `activity ${reported.lastActivityMs} is implausibly old for a session created just now`,
        );

        // A target naming no session is dead with a null date, never a session
        // quiet since the epoch — the same listing, so this cannot pass by the
        // listing having failed wholesale.
        const absent = await paneActivity(
            {},
            () => [{ devSessionId: "ds-live", terminalId: "t-gone", target: `${SESSION}_gone` }],
            runner,
        );
        assert.equal(absent.panes[0].alive, false);
        assert.equal(absent.panes[0].lastActivityMs, null);
    },
);

test(
    "the production runner preserves Unicode session identity in the C locale",
    { skip: SKIP },
    async (t) => {
        const unicode = "日本語";
        const underscores = "______";
        t.after(async () => {
            await runner(["kill-server"]);
        });
        // These names collide in tmux's sanitized C-locale output.
        for (const name of [unicode, underscores]) {
            const created = await runner(["-u", "new-session", "-d", "-s", name]);
            assert.equal(created.code, 0, created.stderr);
        }

        const keys = ["LC_ALL", "LANG", "TMUX", "TMUX_PANE", "CODEHARBOR_TMUX"];
        const previous = new Map(keys.map((key) => [key, process.env[key]]));
        process.env.LC_ALL = "C";
        process.env.LANG = "C";
        process.env.CODEHARBOR_TMUX = "tmux";
        delete process.env.TMUX;
        delete process.env.TMUX_PANE;
        const productionRunner: CommandRunner = (argv) =>
            execFileRunner(["-L", SOCKET, ...argv]);
        try {
            const sessions = await listSessions(productionRunner);
            assert.deepEqual(sessions.map((session) => session.name).sort(), [underscores, unicode]);
            await killSession({ name: unicode }, productionRunner);
            const remaining = await listSessions(productionRunner);
            assert.deepEqual(remaining.map((session) => session.name), [underscores]);
        } finally {
            for (const [key, value] of previous) {
                if (value === undefined) delete process.env[key];
                else process.env[key] = value;
            }
        }
    },
);

test(
    "killing a semicolon-suffixed session never kills its shorter namesake",
    { skip: SKIP },
    async (t) => {
        t.after(async () => {
            await runner(["kill-server"]);
        });
        const base = `${SESSION}_semicolon`;
        const created = await runner(["new-session", "-d", "-s", base]);
        assert.equal(created.code, 0, created.stderr);

        // The suffixed session does not exist. tmux's argv parser used to strip
        // the final semicolon and kill the live shorter name instead.
        await killSession({ name: `${base};` }, runner);
        assert.deepEqual((await listSessions(runner)).map((session) => session.name), [base]);

        // A backslash quotes the final semicolon for tmux's command-list parser;
        // the actual session name stored by the server contains no backslash.
        const literal = await runner(["new-session", "-d", "-s", `${base}\\;`]);
        assert.equal(literal.code, 0, literal.stderr);
        assert.deepEqual(
            (await listSessions(runner)).map((session) => session.name).sort(),
            [base, `${base};`].sort(),
        );
        await killSession({ name: `${base};` }, runner);
        assert.deepEqual((await listSessions(runner)).map((session) => session.name), [base]);
    },
);

test(
    "listing-to-kill preserves literal backslash, separator and target-sigil names",
    { skip: SKIP },
    async (t) => {
        t.after(async () => {
            await runner(["kill-server"]);
        });
        const sentinel = `${SESSION}_identity`;
        const created = await runner(["new-session", "-d", "-s", sentinel]);
        assert.equal(created.code, 0, created.stderr);

        // tmux normalizes backslashes at creation time. The exact name to kill
        // is the one discovery returns, not the string passed to new-session.
        for (const requested of [
            `${sentinel}\\;`,
            `${sentinel}\\\\;`,
            `${sentinel}\\`,
            `${sentinel};;`,
            `=${sentinel}`,
            "@0",
            "%0",
            `${sentinel}*`,
            "-a",
        ]) {
            const added = await runner([
                "new-session", "-d", "-s", requested.replace(/;$/, "\\;"),
            ]);
            assert.equal(added.code, 0, added.stderr);
            const sessions = await listSessions(runner);
            const candidate = sessions.filter((session) => session.name !== sentinel);
            assert.equal(candidate.length, 1, JSON.stringify(sessions));
            await killSession({ name: candidate[0].name }, runner);
            assert.deepEqual(
                (await listSessions(runner)).map((session) => session.name),
                [sentinel],
                `killing ${JSON.stringify(candidate[0].name)} changed another session`,
            );
        }
    },
);

test(
    "a missing session named like an attached client cannot kill that client's session",
    { skip: SKIP, timeout: 15_000 },
    async (t) => {
        const sentinel = `${SESSION}_client_fallback`;
        t.after(async () => {
            await runner(["kill-server"]);
        });
        const created = await runner([
            "new-session", "-d", "-s", sentinel, "sleep 60",
        ]);
        assert.equal(created.code, 0, created.stderr);

        // Control mode supplies a real attached tmux client without touching a
        // user's tty. It still participates in cmd_find_get_session's fallback
        // from a missing exact session name to a client name.
        const client = spawn(
            "tmux",
            ["-L", SOCKET, "-f", "/dev/null", "-C", "attach-session", "-t", `=${sentinel}`],
            { env: PRODUCTION_ENV, timeout: 10_000 },
        );
        t.after(() => { client.kill(); });
        client.stderr.resume();
        const ready = Promise.withResolvers<void>();
        let output = "";
        const onData = (chunk: Buffer) => {
            output = (output + chunk.toString()).slice(-4096);
            if (output.includes("%session-changed ")) {
                client.stdout.removeListener("data", onData);
                ready.resolve();
            }
        };
        client.stdout.on("data", onData);
        client.once("error", ready.reject);
        client.once("exit", (code) => ready.reject(new Error(`control client exited: ${code}`)));
        await ready.promise;
        const clients = await runner(["list-clients", "-F", "#{client_name}"]);
        assert.equal(clients.code, 0, clients.stderr);
        const names = clients.stdout.trim().split("\n");
        assert.equal(names.length, 1, clients.stdout);
        assert.notEqual(names[0], sentinel);
        await killSession({ name: names[0] }, runner);
        assert.deepEqual(
            (await listSessions(runner)).map((session) => session.name),
            [sentinel],
            "an absent session name must not resolve through an attached client",
        );
    },
);
