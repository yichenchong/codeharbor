// SPEC 6.6 — coarse activity detection for the "generic" harness.
//
// The generic harness has no lifecycle-event adapter (see adapters/index.ts),
// so a coarse agent state can only be derived from terminal output activity:
// recent output means the agent is running; a quiet stretch means it has gone
// idle; no output yet means it is still starting. The detector is the derivation
// only — it holds no clock and reads no stream, so it produces states only for a
// caller that feeds it note() from a terminal's output and publishes state().
//
// The detector is deterministic — every time value is passed in, never read
// from a clock — so it is trivially testable and reproducible.

import type { AgentState } from "../events.ts";

/** Default quiet window after which an agent with no output is deemed idle. */
export const DEFAULT_IDLE_THRESHOLD_MS = 2000;

export class FallbackActivityDetector {
    #lastOutputAtMs: number | null = null;
    readonly #idleThresholdMs: number;

    constructor(idleThresholdMs: number = DEFAULT_IDLE_THRESHOLD_MS) {
        this.#idleThresholdMs = idleThresholdMs;
    }

    /** Record terminal output observed at `outputAtMs` (monotonic-ish ms). */
    note(outputAtMs: number): void {
        if (this.#lastOutputAtMs === null || outputAtMs > this.#lastOutputAtMs) {
            this.#lastOutputAtMs = outputAtMs;
        }
    }

    /**
     * Coarse agent state at `nowMs`: "starting" before any output, "running"
     * when the last output is within the idle threshold, else "idle".
     */
    state(nowMs: number): AgentState {
        if (this.#lastOutputAtMs === null) return "starting";
        return nowMs - this.#lastOutputAtMs < this.#idleThresholdMs ? "running" : "idle";
    }
}
