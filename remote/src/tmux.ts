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

// Wall-clock ceiling for one tmux invocation. Every caller awaits its runner and
// the dispatcher awaits the handler, so a tmux that never answers (an
// unresponsive server socket, a stuck NFS-mounted socket directory) would hang
// the RPC method — and the client's session list — forever. Ten seconds is far
// beyond any healthy `list-sessions`.
//
// Hitting it is a REAL failure and is surfaced as one, unlike rule 1's two
// absences: a hung tmux is not a host without sessions, and answering "no
// sessions" would invite the client to create a second session alongside the
// live one it could not see. execFileRunner names the timeout in `stderr` so
// the surfaced message says so instead of "exit code -1".
const TMUX_TIMEOUT_MS = 10_000;

// Ceiling on the bytes one tmux invocation may print. A listing of every
// session on a busy host is kilobytes; eight megabytes is a producer that has
// gone wrong, and execFile needs a bound or it buffers without one.
const TMUX_MAX_OUTPUT_BYTES = 8 * 1024 * 1024;

// --- tmux target names (SPEC 5.2) -------------------------------------------
//
// A tmux TARGET is not free-form text. Every `-t` argument is read with the
// grammar `session:window.pane`, so a `:` or a `.` inside a name is STRUCTURE:
// `-t 'ch_a:0'` addresses window 0 of session `ch_a`, not a session called
// `ch_a:0`. tmux does not even let such a session exist — session_check_name()
// rewrites both characters to `_` at creation time. Verified against the tmux
// 3.6 on this host: `tmux new-session -s 'ch_a.b:c'` produces a session whose
// `#{session_name}` reads back `ch_a_b_c`. A stored target carrying either
// character therefore names a session that CANNOT exist, so the exact-match
// form (`-t '=<target>'`, which attach and kill both use) misses it outright,
// while an un-anchored form resolves to whatever session the prefix happens to
// hit — somebody else's shell.
//
// The accepted set is a WHITELIST rather than a blacklist of those two
// characters, because several more are meaningful in a target position: `$`,
// `@` and `%` introduce tmux's session/window/pane ids, `=` is its exact-match
// prefix, `*` and `?` are its fnmatch wildcards, a leading `-` reads as an
// option, and control characters come back vis-escaped in tmux's output (see
// the listing parser below). Everything CodeHarbor mints is `ch_` plus two
// UUIDs, which fits this set with room to spare.
// The FIRST character is restricted further: `^[A-Za-z0-9_]` excludes a leading
// `-`, which the prose above claims to exclude but a plain `[A-Za-z0-9_-]+`
// happily accepted. A stored target such as `-d` survives every `-t '=<target>'`
// call site (the `=` shields it), but it is handed to tmux UNSHIELDED wherever a
// name is created rather than targeted — `tmux new-session -A -s <target>`
// (TerminalController::tmuxNewSessionCommand) — and one getopt away from being
// read as a flag. Nothing CodeHarbor mints starts with `-`, so this costs
// nothing and closes the gap between the rule and the regex.
const TMUX_TARGET_SAFE = /^[A-Za-z0-9_][A-Za-z0-9_-]*$/;

// Longest target accepted. Nothing in tmux's grammar hangs on it; it is a bound
// so an absurd name cannot be stored, shipped through a command line and then
// truncated somewhere downstream into a DIFFERENT session's name. `ch_` plus
// two 36-character UUIDs is 75 characters, so this leaves ample headroom.
export const TMUX_TARGET_MAX_LENGTH = 200;

/**
 * Is `target` usable verbatim as a tmux session name and as an exact-match
 * `-t '=<target>'` argument? See TMUX_TARGET_SAFE for what "usable" excludes.
 */
export function isSafeTmuxTarget(target: string): boolean {
    return (
        target.length > 0 &&
        target.length <= TMUX_TARGET_MAX_LENGTH &&
        TMUX_TARGET_SAFE.test(target)
    );
}

/**
 * Apply tmux's OWN session-name normalization to `name`: `:` and `.` become
 * `_`, exactly as session_check_name() does when the session is created.
 *
 * This is deliberately a REWRITE and not a rejection, and it has exactly one
 * caller: the schema v3 migration repairing targets already in the database. A
 * stored `ch_a.b` never named a real session — tmux created `ch_a_b` — so the
 * rewrite does not retarget the pane, it makes the stored value finally name
 * the session that has been there all along. New values coming in over RPC are
 * REJECTED instead (see Workspace.createTerminalPane): there the caller still
 * exists to be told, and silently handing back a target different from the one
 * it asked for would leave it attaching to one session and killing another.
 */
export function tmuxSafeName(name: string): string {
    return name.replace(/[:.]/g, "_");
}

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
            // The binary is overridable so the live suite can point at a
            // wrapper or a non-PATH tmux; unset it and this is plain `tmux`
            // resolved through PATH. Read per invocation rather than captured
            // at import time, so a value exported after this module was loaded
            // is honoured instead of silently ignored.
            process.env.CODEHARBOR_TMUX ?? "tmux",
            argv,
            {
                encoding: "utf8",
                maxBuffer: TMUX_MAX_OUTPUT_BYTES,
                timeout: TMUX_TIMEOUT_MS,
            },
            (err, stdout, stderr) => {
                if (err === null) {
                    resolve({ code: 0, stdout, stderr });
                    return;
                }
                // execFile reports a non-zero exit as a numeric `code` and a
                // spawn failure (ENOENT for a missing tmux) as a string errno.
                //
                // Two more outcomes are neither, and both used to collapse onto
                // the same shape as "could not spawn" and produce the useless
                // message `tmux list-sessions failed: exit code -1`:
                //
                //   * a TIMEOUT: execFile kills the child, so there is no exit
                //     code at all and `stderr` is usually empty;
                //   * OUTPUT PAST maxBuffer: execFile also kills the child, and
                //     tags the error ERR_CHILD_PROCESS_STDIO_MAXBUFFER. Reported
                //     as a timeout it sent whoever had to debug it hunting an
                //     unresponsive tmux over a listing that was merely enormous.
                //
                // Naming both also stops isMissingTmux from ever reading a hung
                // or over-talkative tmux as an absent binary.
                const overflowed = err.code === "ERR_CHILD_PROCESS_STDIO_MAXBUFFER";
                const timedOut =
                    !overflowed && err.killed === true && typeof err.code !== "number";
                let detail: string;
                if (overflowed) {
                    detail = `tmux produced more than ${TMUX_MAX_OUTPUT_BYTES} bytes of output`;
                } else if (timedOut) {
                    detail = `tmux did not respond within ${TMUX_TIMEOUT_MS}ms`;
                } else {
                    detail = stderr ?? err.message;
                }
                resolve({
                    code: typeof err.code === "number" ? err.code : SPAWN_FAILED,
                    stdout: stdout ?? "",
                    stderr: detail,
                });
            },
        );
    });

// A stub runner may throw where the real one resolves. Absorbing that here
// gives every caller ONE shape to branch on (a CommandResult, never an
// exception); whether the failure is then an absence or a real error is
// isMissingTmux/isNoServer's decision, exactly as for a runner that resolved.
async function run(runner: CommandRunner, argv: string[]): Promise<CommandResult> {
    try {
        return await runner(argv);
    } catch (err) {
        return {
            code: SPAWN_FAILED,
            stdout: "",
            stderr: err instanceof Error ? err.message : String(err),
        };
    }
}

function isMissingTmux(result: CommandResult): boolean {
    return (
        result.code === SPAWN_FAILED &&
        /(?:\bENOENT\b|\btmux\b.*\b(?:not found|cannot find)\b)/i.test(result.stderr)
    );
}

// "No tmux server" has TWO spellings, and only one of them says "server".
// Newer tmux prints "no server running on /tmp/tmux-1000/default", but when the
// socket file does not exist at all it instead reports the failed connect:
// "error connecting to /tmp/tmux-1000/default (No such file or directory)".
// That second form is the ordinary state of a machine where tmux has simply
// never been started — which is exactly what a continuous-integration runner
// looks like — so treating it as a real failure turns "no sessions yet" into an
// internal error and breaks the very first listing on a fresh host.
function isNoServer(result: CommandResult): boolean {
    if (/no (?:tmux )?server running\b/i.test(result.stderr)) return true;
    return /error connecting to .*\(No such file or directory\)/i.test(result.stderr);
}

function commandFailure(operation: string, result: CommandResult): Error {
    const detail = result.stderr.trim() || `exit code ${result.code}`;
    return new Error(`tmux ${operation} failed: ${detail}`);
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

// A whole numeric field of the listing format: digits only, at least one, no
// sign, no whitespace, nothing trailing. tmux emits `session_windows` and
// `session_created` as plain non-negative decimals, so a field that does not
// match is not a field this format produced.
const INTEGER_FIELD = /^\d+$/;

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
        // STRICT field validation, not Number.parseInt's prefix scan. parseInt
        // stops at the first character it cannot use, so "3abc" parsed as 3,
        // "1753372800\u0000junk" as a valid timestamp, and " 7" as 7 — a
        // record whose numeric fields are not numbers was accepted as if they
        // were, with the garbage silently discarded. The whole point of
        // parsing positionally is that fields 0-2 have exactly one shape each;
        // a field that does not have it means the line is not a record this
        // format produced, so drop the line rather than invent a value for it.
        //
        // Number.isSafeInteger is the second half: `session_created` is seconds
        // since the epoch and a 20-digit field passes /^\d+$/ but lands beyond
        // 2^53, where it silently rounds to a different instant.
        if (!INTEGER_FIELD.test(fields[0]) || !INTEGER_FIELD.test(fields[1])) continue;
        // `#{?session_attached,1,0}` emits exactly "1" or "0"; anything else is
        // not this format's third field, and reading it as `=== "1"` would have
        // reported a malformed record as a detached session that a later kill
        // could act on.
        if (fields[2] !== "0" && fields[2] !== "1") continue;
        const windows = Number.parseInt(fields[0], 10);
        const created = Number.parseInt(fields[1], 10);
        const name = fields.slice(3).join("\t");
        if (
            !Number.isSafeInteger(windows) ||
            !Number.isSafeInteger(created) ||
            name === ""
        ) continue;
        sessions.push({
            // Everything past the third tab is the name — see LIST_SESSIONS_FORMAT.
            name,
            windows,
            created,
            attached: fields[2] === "1",
        });
    }
    return sessions;
}

/**
 * List every tmux session on the host. Empty when tmux is missing or no server
 * is running — expected on a fresh box, not an error. tmux exits non-zero and
 * says either "no server running on ..." or, when the socket file was never
 * created at all, "error connecting to ... (No such file or directory)"; both
 * mean the same thing here. Any other non-zero exit is a real failure and is
 * raised, so a permission problem cannot masquerade as an empty host.
 */
export async function listSessions(runner: CommandRunner = execFileRunner): Promise<ListSessionsResult> {
    const marker = randomBytes(12).toString("base64url");
    const result = await run(runner, ["list-sessions", "-F", marker + "\t" + LIST_SESSIONS_FORMAT]);
    if (result.code === 0) return parseSessions(result.stdout, marker);
    if (isMissingTmux(result) || isNoServer(result)) return [];
    throw commandFailure("list-sessions", result);
}

// Validates the one parameter shared by sessionExists and killSession. A blank
// or non-string name is a malformed REQUEST — unlike a missing tmux, it is a
// genuine error and must surface as one rather than silently matching nothing.
// Tagged as invalid params so the dispatcher answers -32602 (the client's
// payload is at fault) rather than -32603 (the server is at fault).
function requireName(params: unknown, method: string): string {
    if (typeof params === "object" && params !== null && "name" in params) {
        const { name } = params;
        if (typeof name === "string" && name.trim() !== "") return name;
    }
    throw new InvalidParamsError(`${method} requires a non-empty string name`);
}
// `=` makes tmux use an exact target, but it does not make `:` or `.` literal:
// tmux still interprets those as window/pane separators. Reject them (and
// control characters) before building a kill target so a malformed name such
// as `other:0` can never kill another session.
function requireKillName(params: unknown): string {
    const name = requireName(params, RPC_TMUX_METHODS.killSession);
    if (/[:.\u0000-\u001f\u007f]/.test(name)) {
        throw new InvalidParamsError(
            `${RPC_TMUX_METHODS.killSession} requires a tmux-safe session name`,
        );
    }
    return name;
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
 * `=` pins the lookup to an exact name, so neither an option-shaped nor a glob
 * name can reach another session. The target grammar still treats `:` and `.`
 * as session/window/pane structure; requireKillName rejects those before the
 * argument is built. Other command failures are surfaced instead of reported
 * as a successful kill.
 */
export async function killSession(
    params: KillSessionParams,
    runner: CommandRunner = execFileRunner,
): Promise<KillSessionResult> {
    const name = requireKillName(params);
    const result = await run(runner, ["kill-session", "-t", `=${name}`]);
    if (
        result.code === 0 ||
        isMissingTmux(result) ||
        isNoServer(result) ||
        /(?:can't find session|no such session|session not found)\b/i.test(result.stderr)
    ) {
        return {};
    }
    throw commandFailure("kill-session", result);
}

// RPC handler table for the `tmux.*` group, keyed by the frozen wire names.
// codeharbord spreads these into its method map and awaits the returned promise.
export const TMUX_METHODS: Record<RpcTmuxMethodName, (params: unknown) => unknown | Promise<unknown>> = {
    [RPC_TMUX_METHODS.listSessions]: () => listSessions(),
    [RPC_TMUX_METHODS.sessionExists]: (params) => sessionExists(params as SessionExistsParams),
    [RPC_TMUX_METHODS.killSession]: (params) => killSession(params as KillSessionParams),
};
