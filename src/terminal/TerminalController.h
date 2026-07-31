#pragma once

#include "Ids.h"
#include "SessionState.h"

#include <QByteArray>
// QIODevice must be COMPLETE, not forward-declared: QPointer<T> instantiates
// QWeakPointer<QObject>(T*, bool), which is constrained on
// is_convertible<T*, QObject*> and silently fails for an incomplete T. Qt 6.10
// happens to accept the forward declaration; 6.6 does not.
#include <QIODevice>
#include <QObject>
#include <QPointer>
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
    // How far past the raw eviction point appendHidden() may look for a point
    // it can safely resume the replay from (a line feed, else a UTF-8 character
    // boundary), so the retained buffer never starts inside a multi-byte
    // character or an escape sequence. Bounded so the scan stays cheap and so a
    // pathological stream with no line feed at all still evicts something.
    // 4 KiB is a couple of full-width terminal lines: far more than any escape
    // sequence, far less than the buffer it trims.
    static constexpr int kHiddenResyncWindowBytes = 4 * 1024;
    // Upper bound on the SILENT part of an attach (SPEC 5.6). A pane leaves
    // OpeningChannel/AttachingTmux when the remote PTY produces its first bytes
    // — that is what "tmux has drawn itself" looks like from this side, and it is
    // TerminalFactory::attach() that makes the transition. The failure this bound
    // exists for is a tmux attach that neither draws nor ends: a tmux server
    // blocked on its socket, or a login shell stalled in an rc file on an
    // unresponsive network mount. Those produce no bytes AND no end-of-stream, so
    // without a bound the pane advertises "attaching_tmux" for as long as the
    // application runs and the user cannot tell a slow attach from a dead one.
    // On expiry the pane moves to Error, which is the state
    // src/qml/TerminalPaneView.qml shows a reason and a Retry button for.
    //
    // One minute, and deliberately not less: it has to cover a cold tmux server
    // start plus a login shell over a slow link, and reporting a working pane as
    // failed is worse than the wait. It is the same budget the live gates
    // (tst_liveterminal, tst_liveterminalfactory, tst_coldstart) already allow an
    // attach before they give up on it.
    static constexpr int kAttachTimeoutMs = 60000;

    explicit TerminalController(QObject *parent = nullptr);

    TerminalState state() const;

    // The bound above is policy, not physics: a host on a slow link may widen
    // it, and a unit test compresses it to milliseconds instead of spending a
    // real minute of suite time. 0 disables the bound entirely and restores the
    // "wait for the first byte forever" behaviour, mirroring
    // ch::SessionBootstrap::setConnectTimeoutMs(0) in src/app. Negative values
    // are clamped to 0. Changing it while a pane is already attaching restarts
    // the window from now.
    void setAttachTimeoutMs(int ms);
    int attachTimeoutMs() const;

    bool viewVisible() const;
    // Toggle renderer visibility (SPEC 5.4). While hidden, flushes accumulate
    // into the rolling buffer instead of the view; becoming visible replays the
    // retained buffer once as a single batch.
    void setViewVisible(bool visible);

    // Feed raw output drained from the SSH PTY channel into the buffer. Bytes
    // are coalesced and released via flushReady() per the SPEC 5.5 policy.
    void ingestOutput(const QByteArray &bytes);

    // Attach the PTY byte transport for this pane (SPEC 5.1/5.3), mirroring
    // CodeharbordClient::setTransport()/AgentStatusMonitor::setTransport(). In
    // production this is a ch::SshChannelDevice opened with ChannelKind::Pty;
    // the parameter stays a plain QIODevice so the controller is exercisable
    // against a QBuffer/QLocalSocket with no SSH session in sight.
    //
    // Everything the transport emits is fed through ingestOutput(), so the
    // SPEC 5.5 coalescing and the SPEC 5.4 hidden buffer apply unchanged.
    // Ownership stays with the caller; the pointer is weak, so a transport
    // destroyed under a live controller detaches instead of dangling. Passing
    // nullptr detaches without touching the pending/hidden buffers, so a
    // reconnect resumes into the same pane.
    void setTransport(QIODevice *transport);
    QIODevice *transport() const;

    // Forward renderer keystrokes to the remote PTY (SPEC 5.1). False when no
    // writable transport is attached or the write was short.
    bool sendInput(const QByteArray &bytes);

    // Record the renderer geometry and push it to the remote PTY (SPEC 5.1).
    // Returns true only when the window-change actually reached a real PTY
    // channel; a non-PTY transport (or none) records the size and returns
    // false. The recorded size is re-applied by setTransport(), so the PTY
    // opened by a reconnect inherits the current pane geometry.
    bool resize(int cols, int rows);
    // Last geometry passed to resize(); 0 until it is called.
    int columns() const;
    int rows() const;

    // Bytes retained while hidden and not yet replayed to the view.
    const QByteArray &hiddenBuffer() const;

    // Drive the lifecycle state machine (SPEC 5.6); emits stateChanged() only on
    // an actual transition.
    //
    // src/terminal only ever produces OpeningChannel -> AttachingTmux -> Ready
    // -> Disconnected (TerminalFactory::attach(), onTransportFinished()) plus
    // Error. Session-level connection progress (connecting/authenticating/
    // reconnecting) is tracked once for the whole application on
    // ch::SessionBootstrap::State and ch::SshConnectionPool::State, never on a
    // pane, so TerminalState does not carry those values at all.
    void setState(TerminalState next);

    // True for the states in which the pane actually has a live channel, and so
    // the only states a dropped connection may move a pane OUT of (SPEC 5.6).
    // Public and shared on purpose: onTransportFinished() and
    // TerminalFactory::detach() both have to classify a pane this way, and two
    // hand-written copies of the list would drift apart unnoticed.
    static bool isLiveState(TerminalState state);

    // Stable tmux target for a pane: ch_<devSessionId>_<terminalId> (SPEC 5.2).
    // Stable IDs are used rather than user-facing display names.
    static QString tmuxTarget(const DevSessionId &devSession, const TerminalId &terminal);
    // Attach-or-create command for the pane's tmux target rooted at workingDir
    // (SPEC 5.2):
    //   tmux new-session -A -s '<target>' -c '<workingDir>' \; \
    //       set-option -t '=<target>:' mouse on
    // The second tmux command switches mouse reporting on for THIS SESSION ONLY
    // (never `-g`, which would reconfigure every tmux session the user owns), so
    // a wheel turn reaches tmux's own scrollback instead of being translated
    // into cursor keys by the renderer. See the implementation for the full
    // reasoning and the tmux 3.6 verification.
    static QString tmuxNewSessionCommand(const DevSessionId &devSession,
                                         const TerminalId &terminal,
                                         const QString &workingDir);

signals:
    void stateChanged(ch::TerminalState state);
    // A coalesced batch of output ready for the visible renderer (SPEC 5.5).
    void flushReady(QByteArray batch);
    // kAttachTimeoutMs elapsed with the pane still in OpeningChannel or
    // AttachingTmux. The pane has ALREADY been moved to Error when this is
    // emitted; the signal exists because the controller has no user-facing
    // message channel of its own — ch::TerminalFactory::error() is the one the
    // pane's chrome listens to, so the factory turns this into the sentence the
    // user reads.
    void attachTimedOut();

private:
    void flush();
    void appendHidden(const QByteArray &batch);
    void onTransportReadyRead();
    void onTransportFinished();
    void onAttachTimeout();
    // Narrow the transport to the only thing that can carry a window-change and
    // push `cols` x `rows` to it.
    bool applyPtySize(int cols, int rows);

    TerminalState m_state = TerminalState::Unloaded;
    bool m_viewVisible = true;
    QByteArray m_pending;   // coalesced output awaiting the next flush
    QByteArray m_hidden;    // rolling buffer retained while hidden
    QTimer m_flushTimer;
    // Watchdog for the silent part of an attach; armed and disarmed by
    // setState() so every host that drives the state machine is bounded, not
    // only TerminalFactory::attach().
    QTimer m_attachTimer;
    int m_attachTimeoutMs = kAttachTimeoutMs;
    // QPointer, not a raw pointer: a caller-owned transport may be destroyed
    // while the controller outlives it, and setTransport()'s disconnect() on
    // the stale pointer would then be a use-after-free.
    QPointer<QIODevice> m_transport = nullptr;
    int m_columns = 0;      // last renderer geometry; 0 = never reported
    int m_rows = 0;
};

} // namespace ch

Q_DECLARE_METATYPE(ch::TerminalState)
