// R-server tmux session discovery (SPEC 10.2, docs/PLAN.md). Implements the
// `tmux.*` method group so the client can LIST and ADOPT tmux sessions that
// already exist on the host instead of assuming its own naming scheme.
//
// Two rules shape this module:
//
//  1. Absence is not failure. A host with no tmux binary, or with tmux present
//     but no server running, is the normal state of a fresh box. Both yield an
//     empty/false RESULT — never a thrown RPC error. Only a malformed request
//     (a missing/blank session name) throws.
//
//  2. No shell. Every tmux invocation goes through execFile with an argv array,
//     so a session name can contain spaces, quotes, `$`, or `;` without any
//     interpolation risk. Names are additionally passed to `-t` with tmux's `=`
//     exact-match prefix, which stops a leading `-` from being read as an option
//     and stops fnmatch/prefix matching from hitting the wrong session.
//
// The command runner is injectable (CommandRunner) so the unit tests drive
// realistic tmux output without a live server; the real thing is exercised by
// the live suite.

import { execFile } from "node:child_process";

import { RPC_TMUX_METHODS } from "./rpc-types.ts";
import type {
    TmuxSession,
    ListSessionsResult,
    SessionExistsParams,
    SessionExistsResult,
    KillSessionParams,
    KillSessionResult,
} from "./rpc-types.ts";

/** Outcome of one tmux invocation. `code` is -1 when the binary could not be spawned at all. */
export interface CommandResult {
    code: number;
    stdout: string;
    stderr: string;
}

/** Runs `tmux <argv>`; resolves (never rejects) so callers branch on `code`. */
export type CommandRunner = (argv: string[]) => Promise<CommandResult>;

/** Exit code used when tmux is not on PATH (or otherwise unspawnable). */
export const SPAWN_FAILED = -1;

const TMUX_BINARY = process.env.CODEHARBOR_TMUX ?? "tmux";

// Machine-readable listing format. The NAME COMES LAST on purpose: a tmux
// session name may contain the field delimiter (and spaces, and `:`), so the
// numeric/boolean fields are parsed positionally from the front and everything
// after the third tab is the name, verbatim. Screen-scraping tmux's human
// output (`name: 3 windows (created ...)`) would be ambiguous for such names.
export const LIST_SESSIONS_FORMAT =
    "#{session_windows}\t#{session_created}\t#{?session_attached,1,0}\t#{session_name}";

export const execFileRunner: CommandRunner = (argv) =>
    new Promise<CommandResult>((resolve) => {
        execFile(
            TMUX_BINARY,
            argv,
            { encoding: "utf8", maxBuffer: 8 * 1024 * 1024 },
            (err, stdout, stderr) => {
                if (err === null) {
                    resolve({ code: 0, stdout, stderr });
                    return;
                }
                // execFile reports a non-zero exit as a numeric `code` and a
                // spawn failure (ENOENT for a missing tmux) as a string errno.
                resolve({
                    code: typeof err.code === "number" ? err.code : SPAWN_FAILED,
                    stdout: stdout ?? "",
                    stderr: stderr ?? err.message,
                });
            },
        );
    });

// A stub runner may throw where the real one resolves; absorbing that here
// keeps rule 1 (absence is not failure) true for every caller.
async function run(runner: CommandRunner, argv: string[]): Promise<CommandResult> {
    try {
        return await runner(argv);
    } catch (err) {
        return { code: SPAWN_FAILED, stdout: "", stderr: err instanceof Error ? err.message : String(err) };
    }
}

/**
 * Parse the tab-separated output of `tmux list-sessions -F LIST_SESSIONS_FORMAT`.
 * Unparseable lines are skipped rather than throwing: one odd line must not cost
 * the client the whole listing.
 */
export function parseSessions(stdout: string): TmuxSession[] {
    const sessions: TmuxSession[] = [];
    for (const raw of stdout.split("\n")) {
        const line = raw.endsWith("\r") ? raw.slice(0, -1) : raw;
        if (line === "") continue;
        const fields = line.split("\t");
        if (fields.length < 4) continue;
        const windows = Number.parseInt(fields[0], 10);
        const created = Number.parseInt(fields[1], 10);
        if (!Number.isFinite(windows) || !Number.isFinite(created)) continue;
        sessions.push({
            // Everything past the third tab is the name — see LIST_SESSIONS_FORMAT.
            name: fields.slice(3).join("\t"),
            windows,
            created,
            attached: fields[2] === "1",
        });
    }
    return sessions;
}

/**
 * List every tmux session on the host. Empty when tmux is missing or no server
 * is running (tmux exits non-zero with "no server running on ..." — expected on
 * a fresh box, not an error).
 */
export async function listSessions(runner: CommandRunner = execFileRunner): Promise<ListSessionsResult> {
    const result = await run(runner, ["list-sessions", "-F", LIST_SESSIONS_FORMAT]);
    if (result.code !== 0) return [];
    return parseSessions(result.stdout);
}

// Validates the one parameter shared by sessionExists and killSession. A blank
// or non-string name is a malformed REQUEST — unlike a missing tmux, it is a
// genuine error and must surface as one rather than silently matching nothing.
function requireName(params: unknown, method: string): string {
    if (typeof params === "object" && params !== null && "name" in params) {
        const { name } = params;
        if (typeof name === "string" && name !== "") return name;
    }
    throw new Error(`${method} requires a non-empty string name`);
}

/**
 * Exact-name existence check. Derived from the listing rather than
 * `tmux has-session -t`, because tmux's target grammar treats `:` as the
 * session/window separator — a listing comparison is unambiguous for every
 * name tmux can actually hold. False when tmux is missing or no server runs.
 */
export async function sessionExists(
    params: SessionExistsParams,
    runner: CommandRunner = execFileRunner,
): Promise<SessionExistsResult> {
    const name = requireName(params, RPC_TMUX_METHODS.sessionExists);
    const sessions = await listSessions(runner);
    return { exists: sessions.some((session) => session.name === name) };
}

/**
 * Kill one session by exact name. Idempotent: killing an absent session, or
 * calling this where tmux is not installed, is a successful no-op.
 */
export async function killSession(
    params: KillSessionParams,
    runner: CommandRunner = execFileRunner,
): Promise<KillSessionResult> {
    const name = requireName(params, RPC_TMUX_METHODS.killSession);
    await run(runner, ["kill-session", "-t", `=${name}`]);
    return {};
}

// RPC handler table for the `tmux.*` group, keyed by the frozen wire names.
// codeharbord spreads these into its method map and awaits the returned promise.
export const TMUX_METHODS: Record<string, (params: unknown) => unknown | Promise<unknown>> = {
    [RPC_TMUX_METHODS.listSessions]: () => listSessions(),
    [RPC_TMUX_METHODS.sessionExists]: (params) => sessionExists(params as SessionExistsParams),
    [RPC_TMUX_METHODS.killSession]: (params) => killSession(params as KillSessionParams),
};
