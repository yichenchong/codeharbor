#pragma once

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
    // Never hold an unterminated ANSI string indefinitely: malformed remote
    // output must not make the pending buffer grow without bound. A complete
    // OSC 52 payload is normally only a few KiB; 64 KiB leaves generous room
    // while making the failure mode for a malicious stream bounded.
    static constexpr int kMaxPendingEscapeBytes = 64 * 1024;
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
    // Upper bound on output that has been emitted to the renderer but not yet
    // acknowledged as consumed (see acknowledgeOutput()). Past it, flushes are
    // retained in the SAME rolling buffer the hidden case uses instead of being
    // emitted, which is what stops a runaway remote process (`yes`, a `cat` of a
    // large log) from queueing an unbounded amount of data in the WebChannel
    // transport and inside Chromium.
    //
    // 512 KiB, chosen from both ends:
    //   * It must never stall ordinary work. A full-screen redraw is the
    //     largest single thing a terminal does, and the worst case is bounded:
    //     a 300x100 grid is 30k cells, and even at ~10 bytes of SGR per cell
    //     that is ~300 KB. Interactive echo and a compiler's diagnostics are
    //     orders of magnitude below that. So a user never waits on an
    //     acknowledgement for anything they would notice.
    //   * It must actually bound memory. 512 KiB is a quarter of the rolling
    //     buffer, so the worst case per pane is that buffer plus this window
    //     plus one release batch — a few megabytes, not "however fast the
    //     remote can print".
    static constexpr int kMaxUnacknowledgedBytes = 512 * 1024;
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
    // retained buffer, as one batch when the credit window has room for all of
    // it and otherwise as successive batches driven by acknowledgeOutput().
    //
    // A visibility change also resets the unacknowledged-output account: a
    // renderer that is not listening owes nothing, and the renderer that
    // becomes visible (a fresh page after a reload, say) never saw whatever the
    // previous one was handed.
    void setViewVisible(bool visible);

    // Feed raw output drained from the SSH PTY channel into the buffer. Bytes
    // are coalesced and released via flushReady() per the SPEC 5.5 policy, and
    // withheld once too much of what was already released is unacknowledged
    // (see acknowledgeOutput()).
    void ingestOutput(const QByteArray &bytes);

    // The renderer consumed `bytes` of what flushReady() handed it (SPEC 5.4).
    // This is the OTHER half of the flow control: flushReady() is free to run
    // ahead of the renderer only up to kMaxUnacknowledgedBytes, and this is what
    // brings the outstanding amount back down and releases what was retained
    // meanwhile.
    //
    // The count is in the same PTY bytes flushReady() emitted, not in decoded
    // characters, because ch::TerminalBridge tells the page the byte weight of
    // each batch and the page echoes it back — an exact round trip that cannot
    // drift on output that is not valid UTF-8.
    //
    // Acknowledgements are ADVISORY, and deliberately so: they arrive from a
    // page that may be wedged, crashed, or lying. Over-acknowledging only
    // relaxes this pane's own flow control, under-acknowledging (or never
    // acknowledging at all) degrades to exactly the hidden-pane behaviour —
    // the bounded rolling buffer with oldest-first eviction. Neither stalls the
    // read pump: ingestOutput() keeps draining the transport either way.
    void acknowledgeOutput(qint64 bytes);
    // Bytes emitted to the renderer and not yet acknowledged.
    qint64 unacknowledgedBytes() const;
    // The renderer was REPLACED (a page reload, a re-mount). Forget what the
    // previous one owed and release whatever is retained to the new one.
    //
    // setViewVisible() does this too, and covers the ordinary reload, where the
    // outgoing page reports hidden on its way out. This exists for the reload
    // that does NOT: a page that vanishes without a word leaves the controller
    // believing it is visible, so the mount handshake of its replacement is not
    // a visibility CHANGE and would otherwise hand the new renderer the old
    // one's debt — and with it a pane that never receives another byte.
    void resetOutputAcknowledgements();

    // Attach the PTY byte transport for this pane (SPEC 5.1/5.3), mirroring
    // CodeharbordClient::setTransport()/AgentStatusMonitor::setTransport(). In
    // production this is a ch::SshChannelDevice running a remote PTY;
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

    // Bytes retained and not yet released to the view: everything flushed while
    // the pane was hidden, plus everything withheld from a visible pane that is
    // too far behind on acknowledgements. One buffer, one eviction rule, both
    // cases.
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

    // Attach-or-create command for the tmux session `target`, rooted at
    // workingDir (SPEC 5.2):
    //   tmux new-session -A -s '<target>' -c '<workingDir>' \; \
    //       set-option -t '=<target>:' mouse on
    //
    // `target` is NOT minted here, and deliberately no longer can be. It is the
    // `tmuxTarget` column of this pane's row in the server's `terminal_panes`
    // table, minted once by codeharbord from that row's UUID id (see
    // mintTmuxTarget in remote/src/workspace.ts) and read back by
    // ch::TerminalFactory. This class used to derive it as
    // `ch_<devSessionId>_<terminalId>` from the LAYOUT pane id, which recycles
    // per Dev Session — so two client machines sharing a Dev Session each
    // re-minted `terminal-2` and the second one attached the first one's
    // still-running shell. There is now exactly one minting site, and it is on
    // the server. This function's only job is to build the command.
    //
    // The second tmux command switches mouse reporting on for THIS SESSION ONLY
    // (never `-g`, which would reconfigure every tmux session the user owns), so
    // a wheel turn reaches tmux's own scrollback instead of being translated
    // into cursor keys by the renderer. See the implementation for the full
    // reasoning and the tmux 3.6 verification.
    static QString tmuxNewSessionCommand(const QString &target, const QString &workingDir);

signals:
    void stateChanged(ch::TerminalState state);
    // Non-empty output arrived from the remote PTY. Raised on INGEST, before
    // any coalescing, buffering or flow control, and carrying no payload: it
    // reports that the pane is producing, not what it produced.
    //
    // flushReady() cannot answer that question. It is silent for a hidden pane
    // and for a pane held back on acknowledgements, both of which are still
    // very much alive — so a consumer that wants liveness would conclude the
    // opposite of the truth exactly when the user is not looking. This is the
    // one thing every pane does whenever the remote says anything at all.
    //
    // Sole consumer today is ch::TerminalFactory, which forwards it to
    // ch::AgentStatusMonitor::noteTerminalOutput() for SPEC 6.6 activity
    // detection on the adapterless "generic" harness. Keep it payload-free:
    // a consumer that wants the bytes wants flushReady().
    void outputReceived();
    // A coalesced batch of output ready for the visible renderer (SPEC 5.5).
    // Every byte emitted here counts against kMaxUnacknowledgedBytes until the
    // renderer reports it consumed through acknowledgeOutput().
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
    // Hand as much of the retained buffer to the view as the credit window
    // allows, if the view can take it (visible, and not already too far
    // behind). The single place both the hidden->visible replay and the
    // acknowledgement-driven release go through.
    void releaseRetained();
    // First offset at or after `from` that a cut of `buffer` can safely land
    // on: a line feed outside an ANSI string, else the first complete ANSI
    // sequence boundary, else a UTF-8 character boundary. Bounded so the
    // rolling eviction scan stays cheap; retained output is never cut inside
    // an escape sequence.
    static qsizetype resyncBoundary(const QByteArray &buffer, qsizetype from);
    // Largest safe prefix no longer than `maxBytes`. Used by credit-window
    // replay, where moving a boundary FORWARD could exceed the advertised
    // unacknowledged-byte limit.
    static qsizetype safePrefixBoundary(const QByteArray &buffer, qsizetype maxBytes);
    // Length of the trailing bytes of `data` that form an incomplete ANSI
    // escape sequence. The bytes stay pending until the sequence is complete,
    // just like an incomplete UTF-8 character.
    static qsizetype incompleteTrailingEscape(const QByteArray &data);
    // Length of the trailing bytes of `data` that are the START of a multi-byte
    // UTF-8 character whose continuation bytes have not arrived yet: 0..3.
    // Zero when the data ends on a complete character, on ASCII, or on bytes
    // that are not legal UTF-8 at all — a binary stream must never be held.
    static qsizetype incompleteTrailingUtf8(const QByteArray &data);
    void onTransportReadyRead();
    void onTransportFinished();
    void onAttachTimeout();
    // Narrow the transport to the only thing that can carry a window-change and
    // push `cols` x `rows` to it.
    bool applyPtySize(int cols, int rows);

    TerminalState m_state = TerminalState::Unloaded;
    bool m_viewVisible = true;
    // Coalesced output awaiting the next flush. Between flushes this holds at
    // most the 0..3 trailing bytes of a multi-byte character flush() refused
    // to split, plus whatever has arrived since.
    QByteArray m_pending;
    QByteArray m_hidden;    // rolling buffer of output not yet released
    // Bytes handed to the renderer that it has not reported consuming. Signed
    // and 64-bit: acknowledgements come from the page, so the arithmetic must
    // survive a nonsense value without wrapping (it is clamped at zero).
    qint64 m_unacknowledged = 0;
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
