import type { AgentState } from "../events.ts";
import type { HarnessAdapter, NativeEvent } from "./types.ts";

// Oh My Pi is the highest-priority integration (SPEC 6.2). Mapping per SPEC 6.5:
//   session_start        -> starting
//   agent_start          -> running
//   tool_call: ask       -> waiting_input
//   tool_result: ask     -> running
//   agent_end / settled  -> idle_unseen
//   session_shutdown     -> stopped
//   agent or hook error  -> error
export const ohMyPiAdapter: HarnessAdapter = {
    harness: "oh-my-pi",
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
};
