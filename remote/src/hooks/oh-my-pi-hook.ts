// SPEC 6.2 — installable Oh My Pi lifecycle hook.
//
// A user installs this small script into their Oh My Pi harness so that its
// native lifecycle hooks emit CodeHarbor bridge messages to the bridge socket.
// On each hook firing, Oh My Pi runs:
//
//     node oh-my-pi-hook.ts <event>
//
// with the native event name as the sole positional argument and the session
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
//
// SPEC 6.4: a broken producer must never take down the agent. Any failure to
// connect or write is swallowed (logged to stderr) and the process exits 0.

import net from "node:net";
import type { NativeEvent } from "../adapters/index.ts";
import { resolveSocketPath } from "../events.ts";

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
 * Connect to the bridge socket, append the BridgeMessage as one JSONL line, and
 * close. Resolves with the written message. Rejects only on a socket-level
 * failure (swallowed by main per SPEC 6.4).
 */
export function emitHookEvent(
    input: HookInput,
    socketPath: string = resolveSocketPath(),
): Promise<BridgeMessage> {
    const message = toBridgeMessage(input);
    const { promise, resolve, reject } = Promise.withResolvers<BridgeMessage>();
    const socket = net.createConnection(socketPath);
    socket.on("error", reject);
    socket.on("connect", () => {
        socket.end(`${JSON.stringify(message)}\n`, () => resolve(message));
    });
    return promise;
}

const USAGE = "usage: node oh-my-pi-hook.ts <native-event>\n" +
    "  env: OMP_DEV_SESSION_ID, OMP_TERMINAL_ID, [OMP_TOOL], [OMP_ERROR], [OMP_SUMMARY]\n";

/**
 * CLI entry point. Never throws: on any failure it warns to stderr and returns
 * so the calling agent is never disrupted (SPEC 6.4).
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
    try {
        await emitHookEvent(input, resolveSocketPath(env));
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        process.stderr.write(`oh-my-pi-hook: ${message}\n`);
    }
}

if (import.meta.url === `file://${process.argv[1]}`) {
    void main();
}
