#include "SessionState.h"

namespace ch {

QString toString(TerminalState s)
{
    switch (s) {
    case TerminalState::Unloaded: return QStringLiteral("unloaded");
    case TerminalState::Connecting: return QStringLiteral("connecting");
    case TerminalState::Authenticating: return QStringLiteral("authenticating");
    case TerminalState::OpeningChannel: return QStringLiteral("opening_channel");
    case TerminalState::AttachingTmux: return QStringLiteral("attaching_tmux");
    case TerminalState::Ready: return QStringLiteral("ready");
    case TerminalState::Disconnected: return QStringLiteral("disconnected");
    case TerminalState::Reconnecting: return QStringLiteral("reconnecting");
    case TerminalState::Error: return QStringLiteral("error");
    }
    return QStringLiteral("unknown");
}

// These are the agent-status wire words (SPEC 6.4). They must stay identical, in
// this order, to AGENT_STATES in remote/src/events.ts and to the tokens
// agentStateFromWireStrict() accepts in src/agent/AgentEvent.h; adding a case
// here without adding it there (or the other way round) is caught by
// TstModels::agentStateWireWordsMatchRemoteEventsTs, which reads that TypeScript
// file and compares the whole ordered list.
QString toString(AgentState s)
{
    switch (s) {
    case AgentState::Starting: return QStringLiteral("starting");
    case AgentState::Running: return QStringLiteral("running");
    case AgentState::WaitingInput: return QStringLiteral("waiting_input");
    case AgentState::IdleUnseen: return QStringLiteral("idle_unseen");
    case AgentState::Idle: return QStringLiteral("idle");
    case AgentState::Error: return QStringLiteral("error");
    case AgentState::Stopped: return QStringLiteral("stopped");
    case AgentState::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QString toString(SessionRowState s)
{
    switch (s) {
    case SessionRowState::Error: return QStringLiteral("error");
    case SessionRowState::WaitingForInput: return QStringLiteral("waiting_for_input");
    case SessionRowState::Running: return QStringLiteral("running");
    case SessionRowState::FinishedUnseen: return QStringLiteral("finished_unseen");
    case SessionRowState::Idle: return QStringLiteral("idle");
    case SessionRowState::Disconnected: return QStringLiteral("disconnected");
    }
    return QStringLiteral("disconnected");
}

QString toString(FileState s)
{
    switch (s) {
    case FileState::Loading: return QStringLiteral("loading");
    case FileState::Clean: return QStringLiteral("clean");
    case FileState::Modified: return QStringLiteral("modified");
    case FileState::Saving: return QStringLiteral("saving");
    case FileState::Saved: return QStringLiteral("saved");
    case FileState::ExternallyModified: return QStringLiteral("externally_modified");
    case FileState::Conflict: return QStringLiteral("conflict");
    case FileState::ReadOnly: return QStringLiteral("read_only");
    case FileState::Error: return QStringLiteral("error");
    case FileState::Disconnected: return QStringLiteral("disconnected");
    }
    return QStringLiteral("error");
}

SessionRowState aggregateRowState(bool anyError,
                                  bool anyWaitingInput,
                                  bool anyRunning,
                                  bool anyFinishedUnseen,
                                  bool anyConnected)
{
    if (anyError) return SessionRowState::Error;
    if (anyWaitingInput) return SessionRowState::WaitingForInput;
    if (anyRunning) return SessionRowState::Running;
    if (anyFinishedUnseen) return SessionRowState::FinishedUnseen;
    if (anyConnected) return SessionRowState::Idle;
    return SessionRowState::Disconnected;
}

} // namespace ch
