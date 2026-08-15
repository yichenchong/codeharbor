// SPEC 6.2 — the Oh My Pi producer that Oh My Pi can actually load.
//
// Installed by pointing the harness at this file:
//
//     omp --hook=/path/to/codeharbor/remote/src/hooks/oh-my-pi-extension.ts
//
// (`--hook` is an alias for `--extension`; a path placed in the harness's own
// extension configuration works the same way.) The harness imports the module,
// calls its default export with its extension API, and the factory registers
// pi.on(...) handlers on the runtime event bus. Each forwarded firing becomes
// one BridgeMessage line on the CodeHarbor bridge socket, built by the shared
// producer code in ./bridge-emit.ts, and codeharbor-bridge (SPEC 6.3) is still
// the single point that maps a native event to an AgentState.
//
// WHY THIS FILE EXISTS. The sibling CLI script (./oh-my-pi-hook.ts) documented
// an installation of the form `session_start -> node oh-my-pi-hook.ts
// session_start` in a harness hook configuration. Oh My Pi has no such
// configuration: its extensibility surface loads a module that default-exports
// a factory, and it never runs a shell command for an event. So nothing ever
// invoked the script, no event ever reached the bridge, and a pane that ran an
// agent stayed "running" in the sidebar forever — including long after the
// agent had finished. The mapping and the monitor were right the whole time;
// the chain simply had no producer at its head. This is that producer.
//
// SPEC 6.4 GOVERNS EVERY LINE BELOW: a broken CodeHarbor must not take down,
// slow down, or talk over the agent. Concretely, a handler here returns
// immediately without awaiting the socket, never throws (a throw from a
// `tool_call` handler is not swallowed by the harness — it BLOCKS the tool
// call, so a stale socket would stop the agent from running tools at all), and
// never writes to stderr, which in an agent process is the screen the user is
// reading.

import { resolveSocketPath } from "../events.ts";
import { emitHookEvent, missingCoordinates, HOOK_TIMEOUT_MS, type HookInput } from "./bridge-emit.ts";

/**
 * The part of the Oh My Pi extension API this file consumes, declared here
 * rather than imported from the harness package.
 *
 * The module is loaded out of a CodeHarbor checkout by whatever `omp` the user
 * has installed, and that checkout has no dependency on the harness — importing
 * its types would make this producer fail to load (or fail to typecheck) purely
 * because the agent it serves is not a build dependency of the daemon. A narrow
 * structural interface states exactly the surface we rely on, so a harness that
 * keeps `on(event, handler)` keeps working and one that changes it fails
 * against a declaration a reader can find, instead of against `any`.
 */
export interface OhMyPiHookApi {
    on(event: string, handler: (event: unknown) => unknown): void;
}

/**
 * The lifecycle events forwarded to the bridge: every native event that SPEC
 * 6.5 maps to a state, and nothing else. The harness emits considerably more
 * (turn_start, turn_end, auto_compaction_*, …); forwarding those would spend a
 * socket connection each to produce a mapping of null.
 *
 * `tool_result` is here even though only the `ask` tool makes it mean anything,
 * because it is the event that ENDS a waiting_input: without it a pane that
 * asked the user a question and got an answer stays "waiting for you" for the
 * rest of the run.
 */
export const FORWARDED_EVENTS = [
    "session_start",
    "agent_start",
    "tool_call",
    "tool_result",
    "agent_end",
    "session_shutdown",
] as const;

/**
 * Translate one harness event object into the producer's normalized input.
 *
 * WHERE THE `toolName` -> `tool` TRANSLATION LIVES, AND WHY HERE. The harness
 * spells the tool name `toolName`; every CodeHarbor consumer — mapPiFamilyEvent,
 * piFamilyMetadata, the AgentEvent metadata the client renders — reads `tool`,
 * and so does the CLI producer's OMP_TOOL. Teaching the mapping both spellings
 * would put a harness's vocabulary into the shared pi-family adapter and leave
 * two field names live on the wire forever, so every future consumer would have
 * to know both or silently miss half the events. Normalizing at the producer
 * keeps exactly ONE spelling on the wire and confines the harness's spelling to
 * this function, which is the only code that ever sees a harness event object.
 *
 * Both spellings are accepted as INPUT for the symmetric reason: a harness that
 * renames the field one way or the other must not silently stop raising
 * waiting_input. Missing this translation entirely is not a degraded mode but a
 * dead feature — `tool_call` with the `ask` tool is the only thing that ever
 * produces waiting_input, so without it "the agent is waiting for you" could
 * never appear at all.
 */
export function toHookInput(
    eventName: string,
    payload: unknown,
    devSessionId: string,
    terminalId: string,
): HookInput {
    // The event NAME comes from the registration, not from the payload's own
    // `type`: the name is what we asked to be called for, so it cannot be
    // absent or disagree, and a payload shape change can never turn a forwarded
    // event into one the adapter maps to nothing.
    const input: HookInput = { event: eventName, devSessionId, terminalId };
    const fields: Record<string, unknown> =
        typeof payload === "object" && payload !== null
            ? (payload as Record<string, unknown>)
            : {};
    const toolName = typeof fields.toolName === "string" ? fields.toolName : "";
    const tool = toolName || (typeof fields.tool === "string" ? fields.tool : "");
    // Trimmed for the same reason the CLI producer trims OMP_TOOL: the adapter
    // decides waiting_input by an exact match against "ask", and "ask " matches
    // nothing.
    if (tool.trim()) input.tool = tool.trim();
    // Only `agent_end` carries willContinue, and only `true` is load-bearing —
    // see the precedence rule in adapters/pi-family.ts. Forwarded verbatim; the
    // bridge, not the producer, decides what it means.
    if (typeof fields.willContinue === "boolean") input.willContinue = fields.willContinue;
    // `isError` on a tool_result is deliberately NOT turned into native.error.
    // That flag means "this tool call failed" — a grep that matched nothing, a
    // build that did not compile — which is ordinary agent work, whereas
    // native.error paints the whole terminal row red as a broken agent. Mapping
    // one onto the other would leave a healthy session showing an error for
    // every failed command it ran.
    return input;
}

/**
 * Forwards harness events to the bridge socket without ever making the agent
 * wait for one.
 */
export interface EventForwarder {
    /** Queue one harness event. Returns immediately; never throws. */
    forward(eventName: string, payload: unknown): void;
    /**
     * Resolve once every queued write has finished or failed. Awaited only at
     * session shutdown, where the agent has nothing left to be slowed down by
     * and the alternative is losing the terminal state to process exit.
     */
    flush(): Promise<void>;
}

/**
 * Build the forwarder for this process, or null when the agent is not running
 * in a CodeHarbor pane.
 *
 * The coordinates are read ONCE, here, rather than per event: they come from
 * the process environment, which a pane exports at creation and nothing later
 * changes. An agent started outside CodeHarbor has neither, and for it this
 * module must be completely inert — no socket, no diagnostic, no per-event
 * work. Returning null makes that the absence of registered handlers rather
 * than a check (and a possible message) on every single firing, which is the
 * difference between "harmless" and "prints a line about CodeHarbor every time
 * you run a tool".
 */
export function createForwarder(
    env: NodeJS.ProcessEnv = process.env,
    socketPath: string = resolveSocketPath(env),
    timeoutMs: number = HOOK_TIMEOUT_MS,
): EventForwarder | null {
    const devSessionId = (env.OMP_DEV_SESSION_ID ?? "").trim();
    const terminalId = (env.OMP_TERMINAL_ID ?? "").trim();
    if (missingCoordinates({ event: "", devSessionId, terminalId }).length > 0) return null;
    // One write at a time, in the order the events fired. Each event opens its
    // own short-lived connection, so firing several at once would let the relay
    // accept them in any order and report, say, agent_end before the tool_call
    // that preceded it — and the client's monitor takes the last state it is
    // told. Chaining also bounds this producer to a single file descriptor no
    // matter how fast the agent works.
    //
    // The chain link never rejects: emitHookEvent's rejection is absorbed right
    // here, because an unhandled rejection inside the agent's process is
    // exactly the "broken CodeHarbor takes down the agent" outcome SPEC 6.4
    // forbids. Nothing is reported: stderr in an agent process is the user's
    // screen, this would fire once per event for as long as the bridge is down,
    // and the failure is already visible (and diagnosable) at the bridge, which
    // is the side that knows whether it is running.
    let queue: Promise<void> = Promise.resolve();
    return {
        forward(eventName, payload) {
            const input = toHookInput(eventName, payload, devSessionId, terminalId);
            queue = queue.then(
                () => emitHookEvent(input, socketPath, timeoutMs).then(
                    () => {},
                    () => {},
                ),
            );
        },
        flush() {
            return queue;
        },
    };
}

/**
 * Extension entry point: the default export the harness calls with its API.
 *
 * Registers nothing at all when the agent is not in a CodeHarbor pane, so the
 * cost of having this module installed while working outside CodeHarbor is one
 * environment read at startup.
 */
export default function ohMyPiExtension(
    pi: OhMyPiHookApi,
    env: NodeJS.ProcessEnv = process.env,
): void {
    const forwarder = createForwarder(env);
    if (forwarder === null) return;
    for (const eventName of FORWARDED_EVENTS) {
        pi.on(eventName, (event: unknown): undefined => {
            // Synchronous and total. Returning a promise would make the harness
            // await this handler — on `tool_call` that is a delay in front of
            // every tool the agent runs — and returning a value at all would
            // feed the harness's result contract (a `tool_call` handler's
            // return can BLOCK the call). Undefined means "no opinion".
            forwarder.forward(eventName, event);
            return undefined;
        });
    }
    // The last event of the process is also the one most easily lost. Every
    // other write has the rest of the run to drain; this one races the agent's
    // exit, and a lost session_shutdown leaves the sidebar row parked in
    // whatever state came before it. Awaiting the drain here is free — the
    // session is over, there is no turn left to slow down — and it is bounded,
    // because each queued write carries its own timeout.
    pi.on("session_shutdown", (): Promise<void> => forwarder.flush());
}
