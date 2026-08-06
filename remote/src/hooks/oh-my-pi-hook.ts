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
//     session_start    -> node /path/to/oh-my-pi-hook.ts session_start
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
// OMP_METADATA optionally carries a JSON object of free-form fields (a model
// name, a turn counter, a run id) through to the client untouched; the bridge
// merges it over the adapter's own derived metadata (see parseHookMetadata).
// OMP_SUMMARY and OMP_METADATA are the only parts of the message the hook will
// ever give up: if the serialized line would overrun the bound the relay
// enforces, they are dropped — the bag first, then the summary if that was not
// enough — so the state change itself still gets through, and the loss is
// reported on stderr (see emitHookEvent).
// SPEC 6.4: a broken producer must never take down the agent. Any failure to
// connect or write is swallowed (logged to stderr) and the process exits 0.

import net from "node:net";
// pathToFileURL, not string concatenation: a script path containing a space (or
// any character a URL must percent-encode) would never compare equal to
// import.meta.url, silently turning the CLI entry point below into a no-op.
import { pathToFileURL } from "node:url";
import type { NativeEvent } from "../adapters/index.ts";
import { BRIDGE_MESSAGE_VERSION, bridgeLineFits } from "../bridge.ts";
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
 *
 * `version` declares which revision of THIS shape the message is written in.
 * The relay treats it as optional — an absent version means "the current
 * revision", which is what keeps hooks installed before the field existed
 * working unchanged — but a hook shipped from this tree always states it. The
 * hook is the one piece of the chain that lives outside the project's update
 * cycle: a user pastes its path into their assistant's configuration and it may
 * keep running out of a months-old checkout for as long as that configuration
 * lives. Without a declared revision, a future field rename reaches the relay
 * as a message that parses, fails a guard and produces nothing — which looks
 * exactly like an event that carried no state change. With one, the mismatch is
 * a nameable condition instead of a silent nothing.
 */
export interface BridgeMessage {
    version: typeof BRIDGE_MESSAGE_VERSION;
    harness: "oh-my-pi";
    devSessionId: string;
    terminalId: string;
    native: NativeEvent;
    summary?: string;
    metadata?: Record<string, unknown>;
}

/**
 * Decode $OMP_METADATA: a JSON object of free-form auxiliary fields the harness
 * wants carried alongside the event (a model name, a turn counter, a run id).
 * Returns undefined for absent, blank, unparseable, or non-object input.
 *
 * Free-form and opaque ON PURPOSE. The bridge merges it over the adapter's own
 * derived metadata and the client parses it as a plain JSON object
 * (AgentEvent.metadata), so a harness can add a field without a change on
 * either side. That is what the bag is for; before this it could only be
 * populated by a programmatic caller of toBridgeMessage(), which no installed
 * hook is, so the declared field was unreachable from the one place hooks are
 * actually configured.
 *
 * Never throws and never rejects the EVENT: metadata is optional, so bad
 * metadata costs the metadata and nothing else (SPEC 6.4 — a broken producer
 * must not break the agent). main() reports the loss on stderr, where the
 * misconfiguration is actionable.
 *
 * A JSON array is rejected: the field is typed as a record on the wire and in
 * the client's QJsonObject, and an array would be silently dropped downstream.
 */
export function parseHookMetadata(raw: string | undefined): Record<string, unknown> | undefined {
    if (raw === undefined || raw.trim() === "") return undefined;
    let parsed: unknown;
    try {
        parsed = JSON.parse(raw);
    } catch {
        return undefined;
    }
    if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) return undefined;
    return parsed as Record<string, unknown>;
}

/**
 * Read a HookInput from process argv/env. The native event name is the first
 * positional argument (argv[2]), falling back to $OMP_HOOK_EVENT; session
 * coordinates, tool and the free-form $OMP_METADATA bag come from the
 * environment so the same script serves every hook.
 */
export function readHookInput(
    argv: readonly string[] = process.argv,
    env: NodeJS.ProcessEnv = process.env,
): HookInput {
    // A BLANK positional argument means "no event name given", not "the event
    // is the empty string". Shell hook configurations routinely interpolate a
    // variable into the argument (`node oh-my-pi-hook.ts "$OMP_EVENT"`), and an
    // unset variable hands us an empty argv[2]; `??` would accept that empty
    // string as the answer and never consult OMP_HOOK_EVENT, so the documented
    // environment fallback would be dead for the exact configuration that needs
    // it. Whitespace-only is the same case, and if BOTH sources are blank the
    // event stays empty and main() prints usage instead of emitting an event
    // whose type no adapter can ever map.
    const event = (argv[2] ?? "").trim() || (env.OMP_HOOK_EVENT ?? "").trim();
    // The session coordinates are trimmed for the same reason as everything
    // else read out of this environment, but the consequence of NOT trimming
    // them is the worst of the lot. A CRLF-authored hook config (or a value
    // read out of a file) leaves "sess-1\r" here. That still has real content,
    // so the missing-coordinates guard below is satisfied and the event goes
    // out; the client then files it under the Dev Session literally named
    // "sess-1\r", which does not exist and never will. The client deliberately
    // stores identifiers verbatim — the server mints them and the client must
    // echo back exactly what it was handed — so normalising here, at the
    // producer, is the only place it can happen. Nothing anywhere reports the
    // loss: the sidebar simply never updates.
    const input: HookInput = {
        event,
        devSessionId: (env.OMP_DEV_SESSION_ID ?? "").trim(),
        terminalId: (env.OMP_TERMINAL_ID ?? "").trim(),
    };
    // Trimmed for the same reason as the event name above: these come from a
    // shell hook configuration, and a command substitution or a CRLF-authored
    // config routinely leaves a trailing newline on the value. An OMP_TOOL of
    // "ask\n" is not the tool named "ask" as far as the adapter's exact match
    // is concerned, so the prompt the user must answer would be relayed as an
    // ordinary running event and never raise waiting_input. A value that is
    // only whitespace is no value at all and is dropped.
    const tool = (env.OMP_TOOL ?? "").trim();
    if (tool) input.tool = tool;
    const errorFlag = (env.OMP_ERROR ?? "").trim();
    if (errorFlag === "1" || errorFlag === "true") input.error = true;
    const summary = (env.OMP_SUMMARY ?? "").trim();
    if (summary) input.summary = summary;
    const metadata = parseHookMetadata(env.OMP_METADATA);
    if (metadata !== undefined) input.metadata = metadata;
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
        version: BRIDGE_MESSAGE_VERSION,
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
 * close. Resolves with the message AS WRITTEN — which is not necessarily the
 * message `input` describes: an over-long summary or metadata bag is dropped
 * before the line goes out (see below), and a caller that wants to report the
 * loss compares what it asked for against what it got back. Rejects on a
 * socket-level failure, on a message no trimming can make deliverable, or after
 * `timeoutMs` of inactivity (all swallowed by main per SPEC 6.4).
 */
export function emitHookEvent(
    input: HookInput,
    socketPath: string = resolveSocketPath(),
    timeoutMs: number = HOOK_TIMEOUT_MS,
): Promise<BridgeMessage> {
    let message = toBridgeMessage(input);
    const { promise, resolve, reject } = Promise.withResolvers<BridgeMessage>();
    let line: string;
    let socket: net.Socket;
    try {
        // Serialize BEFORE connecting, inside the guard. Doing it in the
        // 'connect' handler instead puts JSON.stringify on an event-emitter
        // callback stack, where a throw is an uncaught exception rather than a
        // rejection: it would escape this promise entirely and exit the hook
        // process non-zero, which a harness may read as a failed hook
        // (SPEC 6.4). The bag in `metadata` is producer-supplied, so a value
        // stringify refuses — a BigInt, a getter that throws — is its input,
        // not ours.
        let payload = JSON.stringify(message);
        // The relay bounds one line and DROPS THE WHOLE PRODUCER CONNECTION
        // when a message overruns it — the hook writes its entire line in one
        // go, so an oversized message trips the relay's unterminated-line guard
        // before any newline is seen. Writing it anyway would therefore cost
        // the event AND the connection while this function reported success.
        //
        // The two fields that can plausibly get that big are the two OPTIONAL
        // ones, and both are decoration: a human-readable summary and a
        // free-form bag. Sacrificing them keeps the STATE TRANSITION — the
        // thing the sidebar actually runs on — deliverable. bridgeLineFits is
        // asked rather than re-deriving the bound here, so the two ends cannot
        // drift; it measures the payload alone, because the framing newline is
        // deliberately outside the bound.
        //
        // Given up ONE AT A TIME, least useful first. The metadata bag is
        // machine-readable auxiliary data the client merely carries; the summary
        // is the line a human reads in the sidebar. Dropping both together
        // whenever either is too big threw away readable text to make room for a
        // bag nobody asked to see, and it made the per-field loss report in
        // main() — which names OMP_SUMMARY and OMP_METADATA separately, and
        // conjugates its verb for one field or two — unable to ever print its
        // single-field form.
        if (!bridgeLineFits(payload)) {
            if (message.metadata !== undefined) {
                const withoutMetadata: BridgeMessage = { ...message };
                delete withoutMetadata.metadata;
                message = withoutMetadata;
                payload = JSON.stringify(message);
            }
            if (!bridgeLineFits(payload) && message.summary !== undefined) {
                const withoutSummary: BridgeMessage = { ...message };
                delete withoutSummary.summary;
                message = withoutSummary;
                payload = JSON.stringify(message);
            }
            // Still too long with nothing optional left: the event name, the
            // tool name or an identifier is itself megabyte-sized, so there is
            // nothing further to give up. Say so — a rejection reaches main(),
            // which prints one actionable line and exits 0 — rather than write
            // a line the relay is guaranteed to refuse.
            if (!bridgeLineFits(payload)) {
                throw new Error(
                    `event ${input.event} is too large for the bridge even without its `
                        + `summary and metadata (${Buffer.byteLength(payload)} bytes)`,
                );
            }
        }
        line = `${payload}\n`;
        // createConnection validates its argument synchronously (an empty or
        // non-string path throws right here). This function promises to REPORT
        // failures by rejecting; letting one escape synchronously would bypass
        // a caller's .catch() and surface as an unhandled throw inside the
        // agent's hook invocation (SPEC 6.4).
        socket = net.createConnection(socketPath);
    } catch (err) {
        reject(err instanceof Error ? err : new Error(String(err)));
        return promise;
    }
    // A bridge that accepted the connection but stopped reading, or a socket
    // file whose listener is wedged, produces neither 'connect' nor 'error':
    // without this bound the hook — and the agent waiting on it — would hang.
    socket.setTimeout(timeoutMs, () => {
        socket.destroy();
        reject(new Error(`bridge socket timed out after ${timeoutMs}ms: ${socketPath}`));
    });
    socket.on("error", reject);
    // Last-resort bound on the wait. Every other way this promise settles is an
    // event the socket may simply never emit: a connection destroyed without an
    // 'error' — a peer that resets between our write being queued and its
    // flush, or a stream the runtime tears down — fires neither 'error' nor the
    // end-of-write callback, and destroying a socket also cancels the
    // inactivity watchdog above. Nothing would then settle this promise, and
    // the hook, and the agent run blocked on it, would wait forever, which is
    // the one outcome SPEC 6.4 forbids. 'close' is the event a destroyed socket
    // always emits; on the success path it arrives after resolve(), where
    // rejecting an already-settled promise is a no-op.
    socket.on("close", () => {
        reject(new Error(`bridge socket closed before the event was written: ${socketPath}`));
    });
    socket.on("connect", () => {
        socket.end(line, () => {
            // Disarm the watchdog: the write succeeded, and a promise that has
            // resolved must never reject afterwards.
            socket.setTimeout(0);
            resolve(message);
            // Then close outright. The line is already handed to the kernel —
            // a Unix stream socket delivers buffered bytes to the peer even
            // after the sender closes — and we never read a reply, so nothing
            // is lost. Merely half-closing leaves the socket (and therefore
            // this process, and therefore the blocked agent) alive until the
            // PEER decides to close: with the watchdog disarmed, a peer that
            // keeps its half open holds the hook open forever, which is the
            // exact outcome SPEC 6.4 forbids.
            socket.destroy();
        });
    });
    return promise;
}

const USAGE = "usage: node oh-my-pi-hook.ts <native-event>\n" +
    "  env: OMP_DEV_SESSION_ID, OMP_TERMINAL_ID,\n" +
    "       [OMP_HOOK_EVENT], [OMP_TOOL], [OMP_ERROR], [OMP_SUMMARY],\n" +
    "       [OMP_METADATA] (a JSON object of free-form fields)\n";

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
 * Write diagnostics without allowing a closed stderr pipe to reject the hook.
 * Hook failures must be best-effort: an agent can close or redirect stderr
 * independently of the bridge, and reporting that failure must not turn a
 * successful hook invocation into a non-zero process exit.
 */
function writeStderr(message: string): void {
    const onError = (): void => {};
    try {
        process.stderr.once("error", onError);
        process.stderr.write(message, () => {
            process.stderr.off("error", onError);
        });
    } catch {
        // Remove only this write's guard if write() failed synchronously.
        process.stderr.off("error", onError);
        // The diagnostics channel is optional; never break the harness for it.
    }
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
        writeStderr(USAGE);
        return;
    }
    const missing = missingCoordinates(input);
    if (missing.length > 0) {
        writeStderr(
            `oh-my-pi-hook: not emitting ${input.event}: ${missing.join(" and ")} ` +
                `unset or blank, so the event could not be attributed to a terminal\n${USAGE}`,
        );
        return;
    }
    // Metadata is optional, so bad metadata never blocks the event — but it is
    // also invisible once dropped, and the harness author who wrote the JSON is
    // standing right here. Say so, and still emit.
    if ((env.OMP_METADATA ?? "").trim() !== "" && input.metadata === undefined) {
        writeStderr(
            "oh-my-pi-hook: ignoring OMP_METADATA: not a JSON object, so the " +
                "event is emitted without it\n",
        );
    }
    try {
        const written = await emitHookEvent(input, resolveSocketPath(env));
        // The state transition was delivered, but part of what the harness
        // asked to send was sacrificed to keep the line inside the relay's
        // bound. Same reasoning as the OMP_METADATA warning above: a silently
        // dropped field is invisible, and the person who set it is standing in
        // the shell this hook was launched from.
        const dropped: string[] = [];
        if (input.summary !== undefined && written.summary === undefined)
            dropped.push("OMP_SUMMARY");
        if (input.metadata !== undefined && written.metadata === undefined)
            dropped.push("OMP_METADATA");
        if (dropped.length > 0) {
            writeStderr(
                `oh-my-pi-hook: ${input.event} exceeded the bridge line bound, so `
                    + `${dropped.join(" and ")} ${dropped.length > 1 ? "were" : "was"} `
                    + "dropped; the state change was still delivered\n",
            );
        }
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        writeStderr(`oh-my-pi-hook: ${message}\n`);
    }
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
    // main() already swallows every failure, but its own stderr write can fail
    // (EPIPE) and an unhandled rejection exits non-zero, which a harness may
    // read as a failed hook and surface to the user (SPEC 6.4).
    void main().catch(() => {});
}
