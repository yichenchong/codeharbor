// SPEC 6.2/6.3 — the shared producer half of the agent-status chain.
//
// Everything here is the part that is identical no matter HOW a lifecycle event
// reached us: the BridgeMessage shape, the socket path, the bounded write, and
// the never-throw diagnostics. Two entry points call it, and they exist because
// the harness offers two different ways in:
//
//   * hooks/oh-my-pi-extension.ts — an Oh My Pi extension module, loaded with
//     `omp --hook=<path>`, registering pi.on(...) handlers in the agent process.
//     This is the mechanism the harness actually has.
//   * hooks/oh-my-pi-hook.ts — a command-line script, `node oh-my-pi-hook.ts
//     <event>`, for a harness (or a test fixture) that can only shell out.
//
// Both produce the SAME line on the wire, because they share this file. When
// they did not — when the CLI script was the only producer and nothing in the
// harness could invoke it — the whole integration was unreachable: the sidebar
// showed a pane as "running" for as long as the pane lived, because no event
// ever left the agent. Keeping the wire construction in one place is what makes
// "the extension works" and "the CLI works" the same claim.

import net from "node:net";
import type { NativeEvent } from "../adapters/index.ts";
import { BRIDGE_MESSAGE_VERSION, bridgeLineFits } from "../bridge.ts";
import { isEventIdentifier, resolveSocketPath } from "../events.ts";

/** Normalized inputs gathered from a native Oh My Pi lifecycle event. */
export interface HookInput {
    /** Native Oh My Pi event name (e.g. "session_start", "tool_call"). */
    event: string;
    devSessionId: string;
    terminalId: string;
    /** Tool name for tool_call/tool_result events (e.g. "ask"). */
    tool?: string;
    /** True when the hook fires for an agent/hook error. */
    error?: boolean;
    /**
     * Oh My Pi's own `willContinue` flag, carried only by `agent_end`: true
     * means the agent is starting more work, not that the run is over. It is
     * forwarded verbatim so that the single mapping point — mapPiFamilyEvent —
     * can tell a real completion from a mid-run boundary. Deciding it here
     * would put a state decision in a producer, which SPEC 6.3 reserves for the
     * bridge.
     *
     * Only the extension entry point can set it: the CLI script's environment
     * protocol has no variable for it, because the harness that shells out to
     * that script never had the flag to pass in the first place.
     */
    willContinue?: boolean;
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
 * Wrap a HookInput in a BridgeMessage carrying the raw native event. Total: it
 * never drops or maps events — the bridge decides whether the native event maps
 * to a state transition or is a no-op.
 */
export function toBridgeMessage(input: HookInput): BridgeMessage {
    const native: NativeEvent = { type: input.event };
    if (input.tool !== undefined) native.tool = input.tool;
    if (input.error) native.error = true;
    if (input.willContinue !== undefined) native.willContinue = input.willContinue;
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
 * How long a producer waits on the bridge socket before giving up. The agent is
 * blocked for at most this long by the CLI script, so the wait is bounded
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
 * `timeoutMs` of inactivity (every caller swallows the rejection per SPEC 6.4).
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
            // nothing further to give up. Say so — a rejection reaches the
            // caller, which prints one actionable line and carries on — rather
            // than write a line the relay is guaranteed to refuse.
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

/**
 * Names the environment variables whose session coordinates are missing or
 * blank, in the order they appear in the CLI usage text. Empty array means the
 * producer is configured well enough to emit.
 *
 * Emitting anyway is the harmful option: an event with an empty devSessionId or
 * terminalId is structurally valid all the way to the desktop client, which
 * files it under a Dev Session that does not exist and — for the states that
 * notify — pops a notification whose body is little more than a slash. Failing
 * HERE, at the producer, is the only place the problem is actionable.
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
export function writeStderr(message: string): void {
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
