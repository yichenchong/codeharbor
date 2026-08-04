import type { AgentState } from "../events.ts";
import { nativeString, type HarnessAdapter, type NativeEvent } from "./types.ts";

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
export const claudeCodeAdapter = {
    harness: "claude-code",
    map(native: NativeEvent): AgentState | null {
        const hook = nativeString(native.hook);
        // PRECEDENCE — `SessionEnd` outranks the error flag; see the same rule
        // and the same reasoning in adapters/pi-family.ts. The session is over
        // and observed to be over, so "stopped" is the terminal's last word and
        // must not be maskable by a flag describing a live-but-broken agent —
        // least of all by a producer that set the flag once and never unset it.
        if (hook === "SessionEnd") return "stopped";
        if (native.error === true) return "error";
        switch (hook) {
            case "SessionStart":
                return "starting";
            case "UserPromptSubmit":
            case "UserPromptExpansion":
            case "PreToolUse":
            case "PostToolUse":
            case "PostToolUseFailure":
            case "PostToolBatch":
            case "PreCompact":
            case "PostCompact":
            case "SubagentStart":
            case "SubagentStop":
            case "TaskCreated":
            case "TaskCompleted":
            case "TeammateIdle":
            case "MessageDisplay":
            case "ElicitationResult":
                return "running";
            case "PermissionRequest":
            case "Elicitation":
                return "waiting_input";
            case "PermissionDenied":
            case "StopFailure":
                return "error";
            case "Notification": {
                // Claude Code also emits informational notifications (for
                // example auth_success). Only notifications that indicate a
                // prompt should move the terminal to waiting_input; otherwise
                // an unrelated desktop message can leave a false completion
                // signal in the sidebar. A malformed notification type is
                // ignored rather than treated as a prompt.
                const rawNotificationType = native.notification_type;
                if (rawNotificationType === undefined) return "waiting_input";
                if (typeof rawNotificationType !== "string") return null;
                const notificationType = rawNotificationType.trim();
                if (notificationType === "elicitation_complete"
                    || notificationType === "elicitation_response")
                    return "running";
                return notificationType === "permission_prompt"
                    || notificationType === "idle_prompt"
                    || notificationType === "elicitation_dialog"
                    || notificationType === "agent_needs_input"
                    ? "waiting_input"
                    : null;
            }
            case "Stop":
                return "idle_unseen";
            default:
                return null;
        }
    },
    metadata(native: NativeEvent): Record<string, unknown> | undefined {
        const tool = nativeString(native.tool_name);
        return tool === "" ? undefined : { tool };
    },
    // `satisfies`, not a type annotation: it checks the shape against
    // HarnessAdapter while keeping `harness` a literal type, which is what lets
    // adapters/index.ts require each registry key to equal the harness the
    // adapter claims (a mis-registration is then a compile error, not a stream
    // of events silently attributed to the wrong harness).
} satisfies HarnessAdapter;
