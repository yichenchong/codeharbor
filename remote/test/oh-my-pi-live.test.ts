// The guard the unit tests structurally cannot be: it runs the REAL `omp`
// binary with the REAL extension module and asserts that a finished agent
// really does report completion to a REAL bridge socket.
//
// It exists because the integration shipped unusable and every test agreed with
// it. The producer was a command-line script documented as being installed into
// a "hook config" of the form `agent_end -> node oh-my-pi-hook.ts agent_end`.
// Oh My Pi has no such configuration — it loads an extension MODULE and calls
// pi.on(...) — so nothing ever invoked the script. Every unit test passed,
// because each one called the producer itself; the only thing missing was the
// harness, which is exactly the piece no unit test supplies. A pane running a
// real agent therefore sat at "running" forever, including long after the run
// had finished, and the first person to notice was a user.
//
// So this file supplies the harness. It asserts the end-to-end fact the sidebar
// depends on — a run that ends is reported as ended — and it does it through
// the shipped path: `omp --hook=<module>`, the module's own socket resolution
// (the advertised CODEHARBOR_AGENT_SOCKET), and real adapter mapping.
//
// Opt in with CH_LIVE_OMP=1 and provide model credentials in environment
// variables. An ordinary test run must not spend money or touch an installed
// agent's profile/history just because `omp` happens to be on PATH. Even an
// opted-in run uses private state and disables tools and discovered extensions.

import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync, spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { mkdirSync } from "node:fs";
import path from "node:path";

import { processBridgeLine } from "../src/bridge.ts";
import { AGENT_SOCKET_ENV, type AgentState } from "../src/events.ts";
import { startFakeBridge } from "./fake-bridge.ts";

function ompAvailable(): boolean {
    try {
        execFileSync("omp", ["--version"], { stdio: "ignore" });
        return true;
    } catch {
        return false;
    }
}

const SKIP = process.env.CH_LIVE_OMP !== "1"
    ? "set CH_LIVE_OMP=1 to run the real model integration"
    : ompAvailable() ? false : "no omp binary on this host";

// A real model call: generous enough that a slow provider does not fail the
// suite, and bounded so a wedged run cannot hang it.
const RUN_TIMEOUT_MS = 180_000;

test("a real omp agent reports its whole lifecycle to the bridge", { skip: SKIP }, async () => {
    const bridge = await startFakeBridge();
    try {
        const extension = fileURLToPath(
            new URL("../src/hooks/oh-my-pi-extension.ts", import.meta.url),
        );
        const home = path.join(bridge.dir, "home");
        const data = path.join(bridge.dir, "data");
        const state = path.join(bridge.dir, "state");
        const tmux = path.join(bridge.dir, "tmux");
        const tmp = path.join(bridge.dir, "tmp");
        for (const directory of [home, data, state, tmux, tmp]) {
            mkdirSync(directory, { mode: 0o700 });
        }
        // API credentials may be inherited, but writable state and event routes
        // must not point at the developer's ordinary running agent or daemon.
        const child = spawn("omp", [
            "-p", "Reply with exactly: pong", `--hook=${extension}`,
            "--no-session", "--no-tools", "--no-extensions", "--no-skills",
            "--no-rules", "--no-context-files", "--no-title", "--no-lsp",
        ], {
            cwd: bridge.dir,
            env: {
                ...process.env,
                HOME: home,
                PWD: bridge.dir,
                XDG_RUNTIME_DIR: bridge.dir,
                XDG_DATA_HOME: data,
                XDG_STATE_HOME: state,
                XDG_CONFIG_HOME: path.join(home, ".config"),
                XDG_CACHE_HOME: path.join(home, ".cache"),
                CODEHARBOR_DB: path.join(bridge.dir, "workspace.sqlite"),
                [AGENT_SOCKET_ENV]: bridge.socketPath,
                CODEHARBOR_CONTROL_SOCKET: undefined,
                PI_CONFIG_DIR: ".omp",
                PI_CONFIG_FILES: undefined,
                PI_CODING_AGENT_DIR: path.join(home, "agent"),
                PI_CODING_AGENT_SESSION_DIR: path.join(home, "agent", "sessions"),
                PI_NATIVES_DIR: path.join(home, ".omp", "natives"),
                // Empty is intentional: undefined enables legacy PI_PROFILE.
                OMP_PROFILE: "",
                PI_PROFILE: undefined,
                OMP_AUTH_BROKER_SNAPSHOT_CACHE: path.join(home, ".cache", "broker-snapshot.enc"),
                OMP_AUTORESEARCH_DB_DIR: path.join(data, "autoresearch"),
                OMP_WORKTREE_DIR: path.join(home, ".omp", "wt"),
                OMP_GITHUB_CACHE_DB: path.join(home, ".cache", "github-cache.db"),
                CLAUDE_CONFIG_DIR: path.join(home, ".claude"),
                DEBUG_CURSOR_LOG: path.join(state, "cursor-debug.jsonl"),
                GITLAB_DUO_WORKFLOW_TRACE_FILE: path.join(state, "gitlab-workflow.jsonl"),
                PI_TUI_WRITE_LOG: undefined,
                HISTFILE: "/dev/null",
                TMUX: undefined,
                TMUX_TMPDIR: tmux,
                TMPDIR: tmp,
                OMP_DEV_SESSION_ID: "sess-live",
                OMP_TERMINAL_ID: "term-live",
            },
            stdio: ["ignore", "pipe", "pipe"],
        });
        const exited = Promise.withResolvers<number | null>();
        let stderr = "";
        child.stderr.on("data", (chunk: Buffer) => {
            stderr += chunk.toString("utf8");
        });
        child.stdout.resume();
        const kill = setTimeout(() => child.kill("SIGKILL"), RUN_TIMEOUT_MS);
        child.on("close", (code) => {
            clearTimeout(kill);
            exited.resolve(code);
        });
        const code = await exited.promise;
        assert.equal(code, 0, `omp exited ${code}: ${stderr}`);

        const states: AgentState[] = [];
        const natives: string[] = [];
        for (const line of bridge.lines) {
            const event = processBridgeLine(line);
            if (event === null) continue;
            states.push(event.state);
            natives.push(event.event);
            assert.equal(event.harness, "oh-my-pi");
            assert.equal(event.devSessionId, "sess-live");
            assert.equal(event.terminalId, "term-live");
        }
        const report = `states=${states.join(",")} events=${natives.join(",")}`;

        // A run that ends must SAY it ended. This one assertion is the whole
        // reason the file exists: without it the chain can be broken end to end
        // — as it was — while every other test in the suite passes.
        assert.ok(states.includes("idle_unseen"), `no completion reported: ${report}`);
        // And it must be a real lifecycle, not one lucky line: the pane goes
        // starting -> running before it finishes.
        assert.equal(states[0], "starting", report);
        assert.equal(natives[0], "session_start", report);
        assert.ok(states.includes("running"), report);
        // Ordering matters as much as presence — a completion reported before
        // the work would leave the row idle for the rest of the run.
        assert.ok(
            states.indexOf("running") < states.lastIndexOf("idle_unseen"),
            report,
        );
        // Nothing in a healthy run may reach the client as an error: `error`
        // paints the terminal row red and is the state a user reads as "your
        // agent broke".
        assert.ok(!states.includes("error"), report);
    } finally {
        await bridge.close();
    }
});
