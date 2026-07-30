#pragma once

#include <QString>

namespace ch {

// Terminal connection lifecycle (SPEC 5.6).
enum class TerminalState {
    Unloaded,
    Connecting,
    Authenticating,
    OpeningChannel,
    AttachingTmux,
    Ready,
    Disconnected,
    Reconnecting,
    Error,
};

// Agent status (SPEC 6.4). The enumerator ORDER is part of two contracts, not
// just the names: ch::AgentStatusMonitor::stateFor() returns the ordinal as a
// plain int and its agentStateChanged signal carries that same int into QML, and
// toString() below must yield exactly the AGENT_STATES list in
// remote/src/events.ts, entry for entry, in the same order. Reordering or
// inserting an enumerator therefore silently changes what the user interface
// displays for a terminal.
//
// Two mechanical checks guard that, so this is not a comment-only promise:
//   * the static_assert below fails to compile if an enumerator is inserted
//     anywhere before Unknown (which must stay last) without kAgentStateCount
//     being updated to match;
//   * TstModels::agentStateWireWordsMatchRemoteEventsTs reads the literal
//     AGENT_STATES array out of remote/src/events.ts at run time and fails if
//     the two lists differ in length, order, or spelling — which is the only
//     check that can notice an edit made on the TypeScript side.
enum class AgentState {
    Starting,
    Running,
    WaitingInput,
    IdleUnseen,
    Idle,
    Error,
    Stopped,
    Unknown, // must remain the last enumerator; see the static_assert below
};

// Number of AgentState enumerators, i.e. one past the highest valid ordinal.
// Used to walk every enumerator in order.
inline constexpr int kAgentStateCount = 8;

static_assert(static_cast<int>(AgentState::Unknown) == kAgentStateCount - 1,
              "AgentState::Unknown must stay the last enumerator and kAgentStateCount "
              "must equal the number of enumerators. If you added an agent state, add "
              "it before Unknown, bump kAgentStateCount, add its case to "
              "toString(AgentState) in SessionState.cpp, add the same wire word in the "
              "same position to AGENT_STATES in remote/src/events.ts, and add it to "
              "agentStateFromWireStrict() in src/agent/AgentEvent.h.");

// Aggregate Dev Session row precedence (SPEC 4.2). Lower ordinal = higher
// display priority.
enum class SessionRowState {
    Error,
    WaitingForInput,
    Running,
    FinishedUnseen,
    Idle,
    Disconnected,
};

// Remote editable file lifecycle (SPEC 8.2).
enum class FileState {
    Loading,
    Clean,
    Modified,
    Saving,
    Saved,
    ExternallyModified,
    Conflict,
    ReadOnly,
    Error,
    Disconnected,
};

QString toString(TerminalState);
QString toString(AgentState);
QString toString(SessionRowState);
QString toString(FileState);

// Reduce a set of terminal/agent conditions to the highest-priority row state.
// The SPEC 4.2 precedence is Error > WaitingForInput > Running > FinishedUnseen
// > Idle > Disconnected, i.e. the SessionRowState declaration order: the first
// flag that is set wins, and all-false (no terminal in any notable state, which
// includes a session with no terminals at all) yields Disconnected.
SessionRowState aggregateRowState(bool anyError,
                                  bool anyWaitingInput,
                                  bool anyRunning,
                                  bool anyFinishedUnseen,
                                  bool anyConnected);

} // namespace ch
