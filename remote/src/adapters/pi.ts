import type { AgentState } from "../events.ts";
import type { HarnessAdapter, NativeEvent } from "./types.ts";

// Pi shares Oh My Pi's event vocabulary (SPEC 6.2). The mapping is currently
// identical; kept as a distinct adapter so the two can diverge without touching
// callers. See docs/PLAN.md workstream A.
export const piAdapter: HarnessAdapter = {
    harness: "pi",
    map(native: NativeEvent): AgentState | null {
        if (native.error === true) return "error";
        const type = typeof native.type === "string" ? native.type : "";
        const tool = typeof native.tool === "string" ? native.tool : undefined;
        switch (type) {
            case "session_start":
                return "starting";
            case "agent_start":
                return "running";
            case "tool_call":
                return tool === "ask" ? "waiting_input" : "running";
            case "tool_result":
                return tool === "ask" ? "running" : null;
            case "agent_end":
            case "settled":
                return "idle_unseen";
            case "session_shutdown":
                return "stopped";
            case "error":
                return "error";
            default:
                return null;
        }
    },
    metadata(native: NativeEvent): Record<string, unknown> | undefined {
        return typeof native.tool === "string" ? { tool: native.tool } : undefined;
    },
};
