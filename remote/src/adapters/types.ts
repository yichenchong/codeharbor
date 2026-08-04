import type { AgentState, Harness } from "../events.ts";

// A harness-native event as received on the bridge socket. Which field names
// the harness's own event depends on the harness: Oh My Pi and Pi use `type`,
// Claude Code uses `hook` (it has lifecycle hooks, not an event stream). Both
// are declared so a reader of an adapter does not have to guess which key is
// load-bearing. `error` is declared for the same reason: it is not part of any
// harness's event vocabulary but a separate marker a producer may set on ANY
// firing (SPEC 6.5 precedence), and every adapter reads it. Every other field
// is harness-specific.
export type NativeEvent = Record<string, unknown> & {
    type?: unknown;
    hook?: unknown;
    error?: unknown;
};

/**
 * Read a string field out of a native event: the trimmed value, or "" when the
 * field is absent or is not a string.
 *
 * Trimming is load-bearing, not tidiness. Every adapter decides state by
 * comparing these fields against exact names (`"session_shutdown"`, `"ask"`,
 * `"SessionEnd"`), and producers are shell hook configurations that interpolate
 * variables: one stray trailing newline or CR from a command substitution or a
 * CRLF-authored config turns `"ask"` into `"ask\n"`, which matches nothing and
 * silently downgrades a prompt the user must answer into an ordinary running
 * event. "" is returned rather than undefined so callers compare against the
 * exact names without a null check; no real event name is blank.
 *
 * Shared by all adapters so the normalization cannot drift between them.
 */
export function nativeString(value: unknown): string {
    return typeof value === "string" ? value.trim() : "";
}

export interface HarnessAdapter {
    readonly harness: Harness;
    /**
     * Map a harness-native event to a CodeHarbor agent state, or null when the
     * event carries no state transition and should be ignored.
     */
    map(native: NativeEvent): AgentState | null;
    /**
     * Derive auxiliary event metadata from the native event. The bridge
     * attaches the returned object to the AgentEvent; return undefined when the
     * event carries no metadata.
     */
    metadata?(native: NativeEvent): Record<string, unknown> | undefined;
}
