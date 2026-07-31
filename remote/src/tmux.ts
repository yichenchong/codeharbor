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
import { randomBytes } from "node:crypto";

import { RPC_TMUX_METHODS } from "./rpc-types.ts";
import type {
    TmuxSession,
    ListSessionsResult,
    RpcTmuxMethodName,
    SessionExistsParams,
    SessionExistsResult,
    KillSessionParams,
    KillSessionResult,
} from "./rpc-types.ts";
import { InvalidParamsError } from "./validate.ts";

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

// The binary is overridable so the live suite can point at a wrapper or a
// non-PATH tmux; unset it and this is plain `tmux` resolved through PATH.
const TMUX_BINARY = process.env.CODEHARBOR_TMUX ?? "tmux";

// Wall-clock ceiling for one tmux invocation. Every caller awaits its runner and
// the dispatcher awaits the handler, so a tmux that never answers (an
// unresponsive server socket, a stuck NFS-mounted socket directory) would hang
// the RPC method — and the client's session list — forever. Ten seconds is far
// beyond any healthy `list-sessions`, and hitting it lands on rule 1's empty
// result rather than an error.
const TMUX_TIMEOUT_MS = 10_000;

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
            { encoding: "utf8", maxBuffer: 8 * 1024 * 1024, timeout: TMUX_TIMEOUT_MS },
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

// SECURITY: a session name is REMOTE-CONTROLLED data that is echoed back inside
// the listing, so "one line = one record" is an assumption an attacker gets to
// attack. tmux 3.6 does escape a newline in a name to the two characters \ and
// n (session_check_name() vis-encodes VIS_NL|VIS_TAB), which is what stops a
// name like "x\n9\t0\t1\tforged" from injecting a whole extra record today —
// but that is the SERVER's normalization, not an invariant this parser may rest
// on. So every record is prefixed with a per-call, unguessable marker that tmux
// emits as literal format text: a line that does not start with the marker is
// not a record tmux produced for THIS call, and is dropped. A name cannot carry
// a marker it cannot predict, so forging a record is impossible rather than
// merely unlikely.
//
// listSessions mints the marker with base64url so it can never contain a tab
// (the field delimiter) or `#` (which tmux would read as a format directive).

/**
 * Parse the tab-separated output of `tmux list-sessions -F LIST_SESSIONS_FORMAT`.
 * Unparseable lines are skipped rather than throwing: one odd line must not cost
 * the client the whole listing.
 *
 * `marker` is the per-call record anchor described above. When it is non-empty
 * a line MUST begin with `<marker>\t` to count as a record, which is what makes
 * a session name unable to forge one. It defaults to empty only so a test (or a
 * caller holding raw tmux output) can parse an unmarked listing; `listSessions`
 * always supplies one.
 */
export function parseSessions(stdout: string, marker = ""): TmuxSession[] {
    const anchor = marker === "" ? "" : marker + "\t";
    const sessions: TmuxSession[] = [];
    for (const raw of stdout.split("\n")) {
        let line = raw.endsWith("\r") ? raw.slice(0, -1) : raw;
        if (anchor !== "") {
            if (!line.startsWith(anchor)) continue;
            line = line.slice(anchor.length);
        }
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
    const marker = randomBytes(12).toString("base64url");
    const result = await run(runner, ["list-sessions", "-F", marker + "\t" + LIST_SESSIONS_FORMAT]);
    if (result.code !== 0) return [];
    return parseSessions(result.stdout, marker);
}

// Validates the one parameter shared by sessionExists and killSession. A blank
// or non-string name is a malformed REQUEST — unlike a missing tmux, it is a
// genuine error and must surface as one rather than silently matching nothing.
// Tagged as invalid params so the dispatcher answers -32602 (the client's
// payload is at fault) rather than -32603 (the server is at fault).
function requireName(params: unknown, method: string): string {
    if (typeof params === "object" && params !== null && "name" in params) {
        const { name } = params;
        if (typeof name === "string" && name !== "") return name;
    }
    throw new InvalidParamsError(`${method} requires a non-empty string name`);
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
 *
 * Unlike sessionExists this does go through tmux's `-t` target grammar, because
 * there is no listing-only way to destroy a session. `=` pins the lookup to an
 * exact name, so neither an option-shaped nor a glob name can reach another
 * session. The one construct `=` does not neutralize is a `:` inside the name,
 * which the grammar may read as a session/window separator — tmux rewrites `:`
 * out of a session name when the session is created, so such a name should not
 * exist on the host, but this is the reason sessionExists prefers the listing.
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
export const TMUX_METHODS: Record<RpcTmuxMethodName, (params: unknown) => unknown | Promise<unknown>> = {
    [RPC_TMUX_METHODS.listSessions]: () => listSessions(),
    [RPC_TMUX_METHODS.sessionExists]: (params) => sessionExists(params as SessionExistsParams),
    [RPC_TMUX_METHODS.killSession]: (params) => killSession(params as KillSessionParams),
};
