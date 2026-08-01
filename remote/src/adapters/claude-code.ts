import type { AgentState } from "../events.ts";
import type { HarnessAdapter, NativeEvent } from "./types.ts";

// Claude Code exposes lifecycle hooks rather than a native event stream
// (SPEC 6.3). Hook names map onto CodeHarbor states; the Notification hook that
// requests user input maps to waiting_input. See docs/PLAN.md workstream A.
//
// Only `Stop` is a completion. `SubagentStop` fires when a Task-tool subagent
// finishes WHILE THE MAIN AGENT IS STILL WORKING, so treating it as a
// completion would arm the Dev Session's unseen-completion badge and pop an
// "Agent finished" desktop notification for a turn that has not finished —
// once per subagent, on a turn that may spawn a dozen. The main agent is
// running at that moment, and that is what the state says.
export const claudeCodeAdapter: HarnessAdapter = {
    harness: "claude-code",
    map(native: NativeEvent): AgentState | null {
        if (native.error === true) return "error";
        const hook = typeof native.hook === "string" ? native.hook : "";
        switch (hook) {
            case "SessionStart":
                return "starting";
            case "UserPromptSubmit":
            case "PreToolUse":
            case "PostToolUse":
                return "running";
            case "Notification":
                return "waiting_input";
            case "Stop":
                return "idle_unseen";
            case "SubagentStop":
                return "running";
            case "SessionEnd":
                return "stopped";
            default:
                return null;
        }
    },
    metadata(native: NativeEvent): Record<string, unknown> | undefined {
        return typeof native.tool_name === "string" ? { tool: native.tool_name } : undefined;
    },
};
