import type { AgentState } from "../events.ts";
import type { NativeEvent } from "./types.ts";

// The event vocabulary shared by the Oh My Pi and Pi harnesses (SPEC 6.2), and
// the SPEC 6.5 mapping from it onto CodeHarbor agent states:
//   session_start        -> starting
//   agent_start          -> running
//   tool_call: ask       -> waiting_input
//   tool_result: ask     -> running
//   agent_end / settled  -> idle_unseen
//   session_shutdown     -> stopped
//   agent or hook error  -> error
//
// ONE mapping, two thin adapters (adapters/oh-my-pi.ts, adapters/pi.ts). It
// used to be one mapping written out twice, byte for byte, with a comment on
// each copy saying the duplication was deliberate "so the two can diverge" and
// a test whose whole job was to fail when somebody edited one copy alone. That
// is a test defending a hazard the code did not need to have: the two harnesses
// speak the same vocabulary, so the shared thing is the mapping and the
// per-harness thing is the adapter identity. If they ever do diverge, the
// divergence is a case in ONE function or an adapter that stops calling it,
// which is a smaller and far more visible change than keeping two files in
// lockstep by hand.
export function mapPiFamilyEvent(native: NativeEvent): AgentState | null {
    const type = typeof native.type === "string" ? native.type : "";
    // PRECEDENCE — a shutdown outranks the error flag, and everything else is
    // outranked by it.
    //
    // `session_shutdown` is a terminal, OBSERVED fact: the harness said its
    // session is over, and nothing further will ever arrive for this terminal.
    // `error` describes an agent that is still there and broken. Letting the
    // flag win produced a row that stayed red for a session that no longer
    // exists, and it is trivially reached by the mistake the flag invites: a
    // producer that exports OMP_ERROR=1 once and forgets to unset it turns
    // every later firing — including the shutdown that would have cleaned the
    // row up — into another error. `stopped` is the terminal's last word, so it
    // is the one state that must not be maskable.
    if (type === "session_shutdown") return "stopped";
    if (native.error === true) return "error";
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
        case "error":
            return "error";
        default:
            return null;
    }
}

/** Auxiliary metadata both harnesses carry: the tool name, when there is one. */
export function piFamilyMetadata(native: NativeEvent): Record<string, unknown> | undefined {
    return typeof native.tool === "string" ? { tool: native.tool } : undefined;
}
