// SPEC 6.2 — command-line producer of Oh My Pi lifecycle events.
//
// This script turns ONE lifecycle firing into one CodeHarbor bridge message. It
// is run as:
//
//     node oh-my-pi-hook.ts <event>
//
// with the native event name as the positional argument — or, for a caller that
// cannot pass arguments, in $OMP_HOOK_EVENT — and the session coordinates
// supplied through the environment (see readHookInput). It does NOT map events
// itself: it wraps the native event in a BridgeMessage and appends it as one
// JSONL line to the bridge socket resolved by resolveSocketPath().
// codeharbor-bridge (SPEC 6.3) is the single adapter-mapping point — it reads
// each BridgeMessage, maps `native` to an AgentState via the harness adapter,
// and relays the validated AgentEvent to the client's AgentStatusMonitor.
//
// WHICH PRODUCER TO INSTALL. Oh My Pi itself cannot call this script: its
// extensibility surface loads a .ts/.js module that default-exports a factory
// and registers pi.on(...) handlers (`omp --hook=<module>`), and it has no
// "run this shell command on this event" configuration at all. So for a real
// `omp` agent the producer is hooks/oh-my-pi-extension.ts, which forwards the
// same BridgeMessage from inside the agent process. This script stays because
// it is the producer for anything that can only shell out — a wrapper script,
// another harness with command hooks, and the live test fixture
// tests/live/fake-omp-agent.sh that drives the whole chain end to end. Both
// producers build their wire message with the same code (hooks/bridge-emit.ts),
// so neither can drift from the other.
//
// Every invocation needs OMP_DEV_SESSION_ID and OMP_TERMINAL_ID in its
// environment so the event can be attributed to the right terminal. For a pane
// CodeHarbor created, both are already there: the client exports them into the
// tmux session (see tmuxNewSessionCommand in src/terminal/TerminalController.cpp),
// so an agent started inside that pane inherits them and nothing has to be set
// by hand. A hook run outside such a pane — by hand, or in a tmux session that
// was already running before CodeHarbor attached to it — still has to export
// them itself.
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

// pathToFileURL, not string concatenation: a script path containing a space (or
// any character a URL must percent-encode) would never compare equal to
// import.meta.url, silently turning the CLI entry point below into a no-op.
import { pathToFileURL } from "node:url";
import { resolveSocketPath } from "../events.ts";
import {
    emitHookEvent,
    missingCoordinates,
    writeStderr,
    type HookInput,
} from "./bridge-emit.ts";

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

const USAGE = "usage: node oh-my-pi-hook.ts <native-event>\n" +
    "  env: OMP_DEV_SESSION_ID, OMP_TERMINAL_ID,\n" +
    "       [OMP_HOOK_EVENT], [OMP_TOOL], [OMP_ERROR], [OMP_SUMMARY],\n" +
    "       [OMP_METADATA] (a JSON object of free-form fields)\n";

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
