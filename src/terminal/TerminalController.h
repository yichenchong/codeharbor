#pragma once

#include "Ids.h"
#include "SessionState.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

namespace ch {

// Owns input, output, buffering, state, and reconnect logic for one terminal
// pane, independently of the xterm.js view that may render it (SPEC 5.4). The
// controller stays connected and drains/buffers output even when hidden, so a
// suspended renderer never loses data. See docs/PLAN.md workstream T.
class TerminalController : public QObject {
    Q_OBJECT
public:
    // Output buffering thresholds (SPEC 5.5): coalesce incoming bytes and flush
    // on whichever comes first — a size cap or a short time window.
    static constexpr int kFlushSizeBytes = 8 * 1024;             // 8 KiB
    static constexpr int kFlushIntervalMs = 10;                  // 10 ms
    // Rolling buffer retained while the view is hidden (SPEC 5.4/5.5); oldest
    // bytes are evicted past this cap and tmux history covers anything older.
    static constexpr int kHiddenBufferMaxBytes = 2 * 1024 * 1024; // 2 MiB

    explicit TerminalController(QObject *parent = nullptr);

    TerminalState state() const;

    bool viewVisible() const;
    // Toggle renderer visibility (SPEC 5.4). While hidden, flushes accumulate
    // into the rolling buffer instead of the view; becoming visible replays the
    // retained buffer once as a single batch.
    void setViewVisible(bool visible);

    // Feed raw output drained from the SSH PTY channel into the buffer. Bytes
    // are coalesced and released via flushReady() per the SPEC 5.5 policy.
    void ingestOutput(const QByteArray &bytes);

    // Bytes retained while hidden and not yet replayed to the view.
    const QByteArray &hiddenBuffer() const;

    // Drive the lifecycle state machine (SPEC 5.6); emits stateChanged() only on
    // an actual transition.
    void setState(TerminalState next);

    // Stable tmux target for a pane: ch_<devSessionId>_<terminalId> (SPEC 5.2).
    // Stable IDs are used rather than user-facing display names.
    static QString tmuxTarget(const DevSessionId &devSession, const TerminalId &terminal);
    // Attach-or-create command for the pane's tmux target rooted at workingDir
    // (SPEC 5.2): tmux new-session -A -s '<target>' -c '<workingDir>'.
    static QString tmuxNewSessionCommand(const DevSessionId &devSession,
                                         const TerminalId &terminal,
                                         const QString &workingDir);

    // Retry delay in seconds for the Nth (0-based) automatic reconnect attempt:
    // 1, 2, 5, 10, 30, then 60 thereafter (SPEC 5.6). Manual reconnect bypasses
    // the wait and is not modelled here.
    static int reconnectDelaySeconds(int attempt);

signals:
    void stateChanged(ch::TerminalState state);
    // A coalesced batch of output ready for the visible renderer (SPEC 5.5).
    void flushReady(QByteArray batch);

private:
    void flush();
    void appendHidden(const QByteArray &batch);

    TerminalState m_state = TerminalState::Unloaded;
    bool m_viewVisible = true;
    QByteArray m_pending;   // coalesced output awaiting the next flush
    QByteArray m_hidden;    // rolling buffer retained while hidden
    QTimer m_flushTimer;
};

} // namespace ch

Q_DECLARE_METATYPE(ch::TerminalState)
