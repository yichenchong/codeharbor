import type { Harness } from "../events.ts";
import type { HarnessAdapter } from "./types.ts";
import { ohMyPiAdapter } from "./oh-my-pi.ts";
import { piAdapter } from "./pi.ts";
import { claudeCodeAdapter } from "./claude-code.ts";

// Static registry. "generic" has no adapter and never will: it names a harness
// that publishes no lifecycle events, so there is nothing to map. Such a pane
// gets its agent state from terminal-output activity instead, derived on the
// CLIENT in ch::AgentStatusMonitor (SPEC 6.6) — the daemon has no per-pane
// source of terminal output. So the table is partial over Harness, and
// adapterFor() answering undefined is the whole of this side's handling.
const registry: Partial<Record<Harness, HarnessAdapter>> = {
    "oh-my-pi": ohMyPiAdapter,
    pi: piAdapter,
    "claude-code": claudeCodeAdapter,
};

export function adapterFor(harness: Harness): HarnessAdapter | undefined {
    return registry[harness];
}

export type { HarnessAdapter, NativeEvent } from "./types.ts";
export { ohMyPiAdapter, piAdapter, claudeCodeAdapter };
