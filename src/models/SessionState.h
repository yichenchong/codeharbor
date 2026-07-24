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

// Agent status (SPEC 6.4). Kept in sync with the wire enum used by the remote
// bridge in remote/src/events.ts.
enum class AgentState {
    Starting,
    Running,
    WaitingInput,
    IdleUnseen,
    Idle,
    Error,
    Stopped,
    Unknown,
};

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
SessionRowState aggregateRowState(bool anyError,
                                  bool anyWaitingInput,
                                  bool anyRunning,
                                  bool anyFinishedUnseen,
                                  bool anyConnected);

} // namespace ch
