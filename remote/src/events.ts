// Internal agent-status event schema (SPEC 6.4). This is the wire contract
// between harness adapters, codeharbor-bridge, and the client's
// AgentStatusMonitor. Keep the AgentState union in sync with the C++ enum in
// src/models/SessionState.h (ch::AgentState) and with the wire-token mapping in
// src/agent/AgentEvent.h (agentStateFromWireStrict); remote/test/events.test.ts
// parses that C++ header and fails if the two token lists drift apart.

import os from "node:os";
import path from "node:path";

export const CH_EVENT_VERSION = 1 as const;

export const AGENT_STATES = [
    "starting",
    "running",
    "waiting_input",
    "idle_unseen",
    "idle",
    "error",
    "stopped",
    "unknown",
] as const;

export type AgentState = (typeof AGENT_STATES)[number];

export const HARNESSES = ["generic", "oh-my-pi", "pi", "claude-code"] as const;
export type Harness = (typeof HARNESSES)[number];

export interface AgentEvent {
    version: typeof CH_EVENT_VERSION;
    /** ISO-8601 timestamp with milliseconds. */
    timestamp: string;
    harness: Harness;
    devSessionId: string;
    terminalId: string;
    state: AgentState;
    /** Harness-native event name that produced this state (e.g. "ask_started"). */
    event: string;
    summary?: string;
    metadata?: Record<string, unknown>;
}

export interface MakeEventInput {
    harness: Harness;
    devSessionId: string;
    terminalId: string;
    state: AgentState;
    event: string;
    summary?: string;
    metadata?: Record<string, unknown>;
    /** Override the timestamp; defaults to now. Primarily for tests. */
    timestamp?: string;
}

export function makeEvent(input: MakeEventInput): AgentEvent {
    const event: AgentEvent = {
        version: CH_EVENT_VERSION,
        timestamp: input.timestamp ?? new Date().toISOString(),
        harness: input.harness,
        devSessionId: input.devSessionId,
        terminalId: input.terminalId,
        state: input.state,
        event: input.event,
    };
    if (input.summary !== undefined) event.summary = input.summary;
    if (input.metadata !== undefined) event.metadata = input.metadata;
    return event;
}

export function isAgentState(value: unknown): value is AgentState {
    return typeof value === "string" && (AGENT_STATES as readonly string[]).includes(value);
}

export function isHarness(value: unknown): value is Harness {
    return typeof value === "string" && (HARNESSES as readonly string[]).includes(value);
}

/**
 * True for a usable Dev Session or terminal identifier: a string with at least
 * one non-whitespace character.
 *
 * An empty (or whitespace-only) identifier is worse than a malformed event. It
 * is structurally valid, so the bridge relays it and the client accepts it, but
 * it names no row: the event is filed under a Dev Session that does not exist
 * and no sidebar entry can ever display it. If its state is "waiting for input"
 * or "finished with unseen output" the client also raises a desktop
 * notification whose body falls back to "<devSessionId> / <terminalId>" — a
 * near-empty, unactionable notification. Dropping the event at the edge is the
 * only outcome the user can make sense of.
 */
export function isEventIdentifier(value: unknown): value is string {
    return typeof value === "string" && value.trim().length > 0;
}

/**
 * True for a decoded JSON object (not null, not an array). `typeof x ===
 * "object"` alone is true for both of those, which is the classic JSON
 * validation footgun.
 */
export function isPlainObject(value: unknown): value is Record<string, unknown> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

/** Structural validation of a decoded object against the event contract. */
export function validateEvent(value: unknown): value is AgentEvent {
    if (typeof value !== "object" || value === null) return false;
    const e = value as Record<string, unknown>;
    return (
        e.version === CH_EVENT_VERSION &&
        typeof e.timestamp === "string" &&
        isHarness(e.harness) &&
        // Non-blank, not merely a string: see isEventIdentifier for why an empty
        // identifier is an unroutable event rather than a harmless one.
        isEventIdentifier(e.devSessionId) &&
        isEventIdentifier(e.terminalId) &&
        isAgentState(e.state) &&
        typeof e.event === "string" &&
        (e.summary === undefined || typeof e.summary === "string") &&
        // metadata is a JSON object: `typeof` alone would also admit an array,
        // which the C++ side reads with QJsonValue::toObject() and silently
        // turns into an empty object.
        (e.metadata === undefined || isPlainObject(e.metadata))
    );
}

/**
 * Parse one JSONL line into a validated AgentEvent, or null if the line is
 * blank, malformed, or fails validation. Never throws: a broken producer must
 * not take down the relay (SPEC 6.4).
 */
export function parseEventLine(line: string): AgentEvent | null {
    const trimmed = line.trim();
    if (trimmed.length === 0) return null;
    let decoded: unknown;
    try {
        decoded = JSON.parse(trimmed);
    } catch {
        return null;
    }
    return validateEvent(decoded) ? decoded : null;
}

/**
 * Resolve the bridge socket path (SPEC 6.3): prefer $XDG_RUNTIME_DIR, else fall
 * back to ~/.cache/codeharbor/events.sock.
 */
export function resolveSocketPath(env: NodeJS.ProcessEnv = process.env): string {
    const runtimeDir = env.XDG_RUNTIME_DIR;
    if (runtimeDir && runtimeDir.length > 0) {
        return path.join(runtimeDir, "codeharbor.sock");
    }
    return path.join(os.homedir(), ".cache", "codeharbor", "events.sock");
}
