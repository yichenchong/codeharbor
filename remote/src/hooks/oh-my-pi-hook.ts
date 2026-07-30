// SPEC 6.2 — installable Oh My Pi lifecycle hook.
//
// A user installs this small script into their Oh My Pi harness so that its
// native lifecycle hooks emit CodeHarbor bridge messages to the bridge socket.
// On each hook firing, Oh My Pi runs:
//
//     node oh-my-pi-hook.ts <event>
//
// with the native event name as the positional argument — or, for a harness
// that cannot pass arguments to a hook, in $OMP_HOOK_EVENT — and the session
// coordinates supplied through the environment (see readHookInput). The hook
// does NOT map events itself: it wraps the native event in a BridgeMessage and
// appends it as one JSONL line to the bridge socket resolved by
// resolveSocketPath(). codeharbor-bridge (SPEC 6.3) is the single
// adapter-mapping point — it reads each BridgeMessage, maps `native` to an
// AgentState via the harness adapter, and relays the validated AgentEvent to
// the client's AgentStatusMonitor.
//
// Installation (add to your Oh My Pi hook config, e.g. ~/.config/oh-my-pi/hooks):
//     session_start   -> node /path/to/oh-my-pi-hook.ts session_start
//     agent_start      -> node /path/to/oh-my-pi-hook.ts agent_start
//     tool_call        -> node /path/to/oh-my-pi-hook.ts tool_call     (export OMP_TOOL)
//     tool_result      -> node /path/to/oh-my-pi-hook.ts tool_result   (export OMP_TOOL)
//     agent_end        -> node /path/to/oh-my-pi-hook.ts agent_end
//     session_shutdown -> node /path/to/oh-my-pi-hook.ts session_shutdown
// Each invocation must export OMP_DEV_SESSION_ID and OMP_TERMINAL_ID so the
// event can be attributed to the right terminal.
// Without both of them the hook emits NOTHING and explains why on stderr (see
// missingCoordinates): an event carrying a blank id reaches the client as a
// valid event that belongs to no Dev Session.
// SPEC 6.4: a broken producer must never take down the agent. Any failure to
// connect or write is swallowed (logged to stderr) and the process exits 0.

import net from "node:net";
// pathToFileURL, not string concatenation: a script path containing a space (or
// any character a URL must percent-encode) would never compare equal to
// import.meta.url, silently turning the CLI entry point below into a no-op.
import { pathToFileURL } from "node:url";
import type { NativeEvent } from "../adapters/index.ts";
import { isEventIdentifier, resolveSocketPath } from "../events.ts";

/** Normalized inputs gathered from a native Oh My Pi hook invocation. */
export interface HookInput {
    /** Native Oh My Pi event name (e.g. "session_start", "tool_call"). */
    event: string;
    devSessionId: string;
    terminalId: string;
    /** Tool name for tool_call/tool_result events (e.g. "ask"). */
    tool?: string;
    /** True when the hook fires for an agent/hook error. */
    error?: boolean;
    summary?: string;
    metadata?: Record<string, unknown>;
}

/**
 * Wire format for a single line on the bridge socket (SPEC 6.3). `native`
 * carries the harness-native event; the bridge maps it via the harness adapter.
 */
export interface BridgeMessage {
    harness: "oh-my-pi";
    devSessionId: string;
    terminalId: string;
    native: NativeEvent;
    summary?: string;
    metadata?: Record<string, unknown>;
}

/**
 * Read a HookInput from process argv/env. The native event name is the first
 * positional argument (argv[2]); session coordinates and tool come from the
 * environment so the same script serves every hook.
 */
export function readHookInput(
    argv: readonly string[] = process.argv,
    env: NodeJS.ProcessEnv = process.env,
): HookInput {
    const event = argv[2] ?? env.OMP_HOOK_EVENT ?? "";
    const input: HookInput = {
        event,
        devSessionId: env.OMP_DEV_SESSION_ID ?? "",
        terminalId: env.OMP_TERMINAL_ID ?? "",
    };
    if (env.OMP_TOOL) input.tool = env.OMP_TOOL;
    if (env.OMP_ERROR === "1" || env.OMP_ERROR === "true") input.error = true;
    if (env.OMP_SUMMARY) input.summary = env.OMP_SUMMARY;
    return input;
}

/**
 * Wrap a HookInput in a BridgeMessage carrying the raw native event. Total: it
 * never drops or maps events — the bridge decides whether the native event maps
 * to a state transition or is a no-op.
 */
export function toBridgeMessage(input: HookInput): BridgeMessage {
    const native: NativeEvent = { type: input.event };
    if (input.tool !== undefined) native.tool = input.tool;
    if (input.error) native.error = true;
    const message: BridgeMessage = {
        harness: "oh-my-pi",
        devSessionId: input.devSessionId,
        terminalId: input.terminalId,
        native,
    };
    if (input.summary !== undefined) message.summary = input.summary;
    if (input.metadata !== undefined) message.metadata = input.metadata;
    return message;
}

/**
 * How long the hook waits on the bridge socket before giving up. The agent run
 * is blocked for exactly as long as this hook takes, so the wait is bounded
 * (SPEC 6.4: a broken producer must never break the agent).
 */
export const HOOK_TIMEOUT_MS = 2000;

/**
 * Connect to the bridge socket, append the BridgeMessage as one JSONL line, and
 * close. Resolves with the written message. Rejects on a socket-level failure
 * or after `timeoutMs` of inactivity (both swallowed by main per SPEC 6.4).
 */
export function emitHookEvent(
    input: HookInput,
    socketPath: string = resolveSocketPath(),
    timeoutMs: number = HOOK_TIMEOUT_MS,
): Promise<BridgeMessage> {
    const message = toBridgeMessage(input);
    const { promise, resolve, reject } = Promise.withResolvers<BridgeMessage>();
    const socket = net.createConnection(socketPath);
    // A bridge that accepted the connection but stopped reading, or a socket
    // file whose listener is wedged, produces neither 'connect' nor 'error':
    // without this bound the hook — and the agent waiting on it — would hang.
    socket.setTimeout(timeoutMs, () => {
        socket.destroy();
        reject(new Error(`bridge socket timed out after ${timeoutMs}ms: ${socketPath}`));
    });
    socket.on("error", reject);
    socket.on("connect", () => {
        socket.end(`${JSON.stringify(message)}\n`, () => {
            socket.setTimeout(0);
            resolve(message);
        });
    });
    return promise;
}

const USAGE = "usage: node oh-my-pi-hook.ts <native-event>\n" +
    "  env: OMP_DEV_SESSION_ID, OMP_TERMINAL_ID,\n" +
    "       [OMP_HOOK_EVENT], [OMP_TOOL], [OMP_ERROR], [OMP_SUMMARY]\n";

/**
 * Names the environment variables whose session coordinates are missing or
 * blank, in the order they appear in USAGE. Empty array means the hook is
 * configured well enough to emit.
 *
 * Emitting anyway is the harmful option: an event with an empty devSessionId or
 * terminalId is structurally valid all the way to the desktop client, which
 * files it under a Dev Session that does not exist and — for the states that
 * notify — pops a notification whose body is little more than a slash. Failing
 * HERE, in the shell where the hook was misconfigured, is the only place the
 * problem is actionable.
 */
export function missingCoordinates(input: HookInput): string[] {
    const missing: string[] = [];
    if (!isEventIdentifier(input.devSessionId)) missing.push("OMP_DEV_SESSION_ID");
    if (!isEventIdentifier(input.terminalId)) missing.push("OMP_TERMINAL_ID");
    return missing;
}

/**
 * CLI entry point. Never throws: on any failure it warns to stderr and returns
 * so the calling agent is never disrupted (SPEC 6.4). A misconfigured
 * environment is reported the same way — loudly on stderr, but with a success
 * exit code, because breaking the agent run is never an acceptable outcome.
 */
export async function main(
    argv: readonly string[] = process.argv,
    env: NodeJS.ProcessEnv = process.env,
): Promise<void> {
    const input = readHookInput(argv, env);
    if (!input.event) {
        process.stderr.write(USAGE);
        return;
    }
    const missing = missingCoordinates(input);
    if (missing.length > 0) {
        process.stderr.write(
            `oh-my-pi-hook: not emitting ${input.event}: ${missing.join(" and ")} ` +
                `unset or blank, so the event could not be attributed to a terminal\n${USAGE}`,
        );
        return;
    }
    try {
        await emitHookEvent(input, resolveSocketPath(env));
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        process.stderr.write(`oh-my-pi-hook: ${message}\n`);
    }
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
    // main() already swallows every failure, but its own stderr write can fail
    // (EPIPE) and an unhandled rejection exits non-zero, which a harness may
    // read as a failed hook and surface to the user (SPEC 6.4).
    void main().catch(() => {});
}
