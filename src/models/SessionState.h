#pragma once

#include <QString>

namespace ch {

// Terminal connection lifecycle (SPEC 5.6). Only the states shipping code
// actually produces are listed: TerminalFactory drives Unloaded (initial) ->
// OpeningChannel -> AttachingTmux -> Ready -> Disconnected plus Error. The
// session-level connection progress (connecting/authenticating/reconnecting)
// is tracked once for the whole application on ch::SessionBootstrap::State and
// ch::SshConnectionPool::State — a single pane never observes it — so it is
// deliberately NOT mirrored here.
enum class TerminalState {
    Unloaded,
    OpeningChannel,
    AttachingTmux,
    Ready,
    Disconnected,
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
    // Reachable via a truncated over-size read: a file larger than
    // EditorController::kMaxEditableReadBytes comes back as a PREFIX, which
    // settles here rather than Clean because no save can be issued from a
    // buffer that is only part of the file (see EditorController::open() /
    // reload()). Read-only-ness for a WRITABLE file is modelled separately as
    // the boolean EditorController::readOnly, not as this state.
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
// flag that is set wins. "Disconnected" is reserved for a terminal that was
// previously live and then explicitly reported a lost connection; an empty
// session, an unopened pane and a pane still opening have no loss to report and
// therefore fall back to Idle instead of claiming a disconnect.
SessionRowState aggregateRowState(bool anyError,
                                  bool anyWaitingInput,
                                  bool anyRunning,
                                  bool anyFinishedUnseen,
                                  bool anyConnected,
                                  bool anyDisconnected = false);

} // namespace ch
