import type { AgentState, Harness } from "../events.ts";

// A harness-native event as received on the bridge socket. The `type` field
// names the harness's own event; other fields are harness-specific.
export type NativeEvent = Record<string, unknown> & { type?: unknown };

export interface HarnessAdapter {
    readonly harness: Harness;
    /**
     * Map a harness-native event to a CodeHarbor agent state, or null when the
     * event carries no state transition and should be ignored.
     */
    map(native: NativeEvent): AgentState | null;
}
