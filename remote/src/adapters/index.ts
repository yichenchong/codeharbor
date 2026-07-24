import type { Harness } from "../events.ts";
import type { HarnessAdapter } from "./types.ts";
import { ohMyPiAdapter } from "./oh-my-pi.ts";
import { piAdapter } from "./pi.ts";
import { claudeCodeAdapter } from "./claude-code.ts";

// Static registry. "generic" has no adapter (fallback activity detection only,
// SPEC 6.6), so the table is partial over Harness.
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
