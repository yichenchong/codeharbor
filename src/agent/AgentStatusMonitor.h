#pragma once

#include "AgentEvent.h"
#include "SessionState.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QIODevice>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

namespace ch {

// Consumes the newline-delimited AgentEvent JSONL stream from the SSH
// agent-status channel (SPEC 6.4) and maintains per-terminal agent state plus a
// per-Dev-Session "unseen completion" flag. The transport is any QIODevice so
// the monitor is exercisable without SSH: production wires the dedicated SSH
// status channel, tests wire a QBuffer or QLocalSocket pair.
//
// Threading: single-threaded, driven by the transport's readyRead on the owning
// thread's event loop. Malformed or blank lines are skipped; a broken producer
// must not take down the client.
class AgentStatusMonitor : public QObject {
    Q_OBJECT
public:
    explicit AgentStatusMonitor(QObject* parent = nullptr);

    // Bind the transport carrying the JSONL status stream. Ownership stays with
    // the caller. Rebinding rewires readyRead and drops any half-received line
    // (bytes from a dead producer must never be spliced onto a new one's first
    // frame); accumulated per-session state is preserved across the swap, since
    // it is the user's — which terminals are busy, which Dev Sessions have an
    // unseen completion — and not the wire's. Passing the device already bound
    // is a no-op, buffer included; a reconnect always supplies a NEW device.
    //
    // Anything already buffered on the new device is drained SYNCHRONOUSLY
    // before this returns, so agentStateChanged/unseenChanged/notify can all
    // fire from inside the call. A caller that installs its own connections
    // must do so BEFORE binding the transport or it will miss those events.
    void setTransport(QIODevice* transport);
    QIODevice* transport() const { return m_transport; }

    // Clear the unseen-completion flag for a Dev Session (the user viewed it).
    // Emits unseenChanged(devSessionId, false) iff the flag was set. The
    // terminals' raw agent states are untouched: a terminal that finished stays
    // at IdleUnseen, and the display layer downgrades it (see
    // AppController::rebuildRows). A later idle_unseen event re-arms the flag
    // even if the raw state never left IdleUnseen in between.
    Q_INVOKABLE void markSeen(const QString& devSessionId);

    // Current agent state for a (devSessionId, terminalId) pair as an
    // int-valued ch::AgentState; AgentState::Unknown if never observed.
    Q_INVOKABLE int stateFor(const QString& devSessionId,
                             const QString& terminalId) const;

    // Whether the Dev Session has an unseen completion (a terminal reached
    // idle_unseen and markSeen has not been called since). Per Dev Session, not
    // global and not per terminal: any one terminal finishing flags the whole
    // Dev Session, and one markSeen clears it for all of them (SPEC 4.2 folds
    // the terminals into a single sidebar row state).
    Q_INVOKABLE bool hasUnseen(const QString& devSessionId) const;

    // Drop all accumulated state for every Dev Session NOT in
    // `liveDevSessionIds`. The unit of eviction is a whole Dev Session subtree:
    // each removed Dev Session's per-terminal agent states and its unseen flag
    // go together. Called after the sidebar list is rebuilt from the
    // authoritative server tree, so a Dev Session the server no longer lists is
    // dropped wholesale — NEVER on a mere terminal close, which would destroy
    // the finished-with-unseen-output signal the sidebar badge derives from. A
    // removed Dev Session has no live row, so nothing is emitted.
    void retainDevSessions(const QSet<QString>& liveDevSessionIds);

    // ---- SPEC 6.6: activity detection for the adapterless "generic" harness
    //
    // The "generic" harness has no lifecycle adapter — adapterFor() in
    // remote/src/adapters/index.ts deliberately answers nothing for it — so the
    // bridge relays no event for one and everything above this line produces
    // NOTHING at all for a generic pane, end to end. The only observable a
    // generic harness offers is its terminal output.
    //
    // That output already exists on THIS side of the wire, in
    // ch::TerminalController, one subsystem over. So the derivation lives here,
    // fed by ch::TerminalFactory, which is the single place the client learns
    // both halves of a pane's identity (its Dev Session and its terminal_panes
    // row) and owns the PTY channel the bytes arrive on. Doing it remotely
    // instead would need either a per-pane server-side tmux output tap or a
    // client-to-daemon stream duplicating every terminal byte back over SSH —
    // an enormous amount of machinery to answer "has this pane printed anything
    // recently", which the client can already see.
    //
    // Three inputs, in the order a pane supplies them.

    // Record which harness a pane runs, from its `terminal_panes.harness`
    // column. Only the literal "generic" opts a pane into activity detection:
    // a pane with no harness configured is a plain shell, and inferring "an
    // agent is running" from a shell's output would light up every terminal in
    // the sidebar. Registering a new generic pane starts tracking it at
    // AgentState::Unknown and emits nothing; repeated registration preserves
    // its derived state. Switching away from generic clears a previous
    // generic-derived or stale lifecycle state to Unknown and emits that
    // transition, without creating a row for an adapter-driven pane that has
    // never spoken.
    Q_INVOKABLE void setTerminalHarness(const QString& devSessionId,
                                        const QString& terminalId,
                                        const QString& harness);

    // A PTY channel was attached to this pane: observation begins now, and any
    // output age remembered from a previous attach is discarded with the
    // channel that produced it. For a generic pane this is SPEC 6.6's "no
    // output observed yet" arm, i.e. Starting. For every other harness it only
    // refreshes the liveness clock the silence timeout below reads.
    Q_INVOKABLE void noteTerminalAttached(const QString& devSessionId,
                                          const QString& terminalId);

    // Output was observed on this pane. For a generic pane that is SPEC 6.6's
    // "output within the idle threshold" arm: Running, falling back to Idle
    // once the pane has been quiet for kFallbackIdleThresholdMs. For every
    // harness it also refutes the silence timeout: a pane that is printing is
    // demonstrably alive, whatever its last lifecycle event claimed.
    //
    // Cheap by construction (two hash lookups and a clock read) because it sits
    // on the terminal output path: the CONTENT of the output is never examined,
    // only the fact that some arrived.
    Q_INVOKABLE void noteTerminalOutput(const QString& devSessionId,
                                        const QString& terminalId);

    // Quiet window after which a generic pane with no output is Idle (SPEC 6.6).
    static constexpr int kFallbackIdleThresholdMs = 2000;

    // Silence window after which a pane that CLAIMS to be working is demoted to
    // Unknown. A harness that is killed — or whose host reboots — emits no
    // shutdown event, so without this the pane's last claim ("running") is kept
    // for the lifetime of the application and the sidebar reports work that
    // stopped hours ago.
    //
    // Only Starting and Running age. They are the two states that assert a live
    // agent process is doing something right now, and they are the two the user
    // reads as "come back later". WaitingInput and IdleUnseen are NOT aged: they
    // are the user's to-do list, they stay true for as long as nobody acts on
    // them, and expiring them would delete exactly the signal this subsystem
    // exists to raise. Idle/Stopped/Error/Unknown assert no liveness at all.
    //
    // The demotion target is Unknown, never Idle. The client genuinely does not
    // know whether a silent agent died or is thinking; Unknown says that, and
    // Idle would be a claim it cannot support. Terminal output refutes the
    // silence (see noteTerminalOutput), so the window only elapses for a pane
    // that has said nothing on either channel.
    //
    // Fifteen minutes, deliberately generous: a single long tool call that
    // prints nothing is real, and reporting a working agent as unknown is worse
    // than being slow to notice a dead one.
    static constexpr int kStaleTimeoutMs = 15 * 60 * 1000;

    // Both windows are policy, not physics; a test compresses them to
    // milliseconds instead of spending real minutes of suite time. Values at or
    // below 0 are clamped to 0, which for the silence window means "never
    // demote" and for the idle threshold means "quiet immediately".
    void setFallbackIdleThresholdMs(int ms);
    int fallbackIdleThresholdMs() const { return m_fallbackIdleThresholdMs; }
    void setStaleTimeoutMs(int ms);
    int staleTimeoutMs() const { return m_staleTimeoutMs; }

signals:
    // A terminal's agent state changed. `state` is an int-valued ch::AgentState.
    // Also emitted the FIRST time a (devSessionId, terminalId) pair is observed,
    // including when that first event carries the wire token "unknown": going
    // from "no event has ever named this terminal" to "the producer says its
    // agent state is unknown" is a real change of knowledge even though
    // stateFor() reports AgentState::Unknown for both. Never emitted for a
    // repeat of the state already recorded for that pair.
    //
    // Also carries the states this class DERIVES rather than receives: the
    // SPEC 6.6 activity states for a generic pane, and the demotion of a silent
    // pane to Unknown. Those are indistinguishable from a wire transition here
    // on purpose — a consumer merges per-terminal agent state and must not care
    // which observation produced it.
    void agentStateChanged(const QString& devSessionId,
                           const QString& terminalId, int state);
    // The Dev Session's unseen-completion flag flipped.
    void unseenChanged(const QString& devSessionId, bool unseen);
    // Desktop-notification hook, emitted on a transition into waiting_input or
    // idle_unseen, and on any idle_unseen that newly flags the Dev Session as
    // having unseen work. The actual OS notification is raised by the display
    // layer.
    void notify(const QString& title, const QString& body);

private slots:
    void onReadyRead();

private:
    void processLine(const QByteArray& line);
    void applyEvent(const AgentEvent& ev);

    // Everything known about one terminal pane's agent.
    struct TerminalStatus {
        AgentState state = AgentState::Unknown;
        // Monotonic marks off m_clock, in ms. -1 means "never".
        qint64 lastEventMs = -1;   // last agent event applied
        qint64 lastOutputMs = -1;  // last terminal output observed
        bool attached = false;     // a PTY channel is (or was) bound
        bool generic = false;      // harness == "generic": SPEC 6.6 owns the state
    };

    // The pane's status, or nullptr when it has never been registered or
    // observed. Deliberately non-creating: a bare output observation on a pane
    // nobody registered must not invent a row for it.
    TerminalStatus* findStatus(const QString& devSessionId, const QString& terminalId);

    // Re-derive every pane's time-dependent state (SPEC 6.6 activity, and the
    // silence demotion) and emit what changed.
    void onAgeTick();
    // Whether one pane's state can still change with the passage of time
    // alone: the exact predicate onAgeTick() finds work by, so rearmAgeTimer()
    // can never stop the timer while a pane still has a pending transition.
    // `devSessionUnseen` is the pane's Dev Session's unseen-completion flag,
    // which onAgeTick() consults before letting time overwrite a completion.
    bool agesWithTime(const TerminalStatus& st, bool devSessionUnseen) const;
    // Run the tick timer iff some pane's state can still change with time, at
    // an interval fine enough for the shorter of the two windows.
    void rearmAgeTimer();

    // QPointer so it auto-nulls if a caller-owned transport is destroyed while
    // the monitor outlives it; a raw pointer would dangle and setTransport()'s
    // disconnect() on the old transport would be a use-after-free.
    QPointer<QIODevice> m_transport = nullptr;
    QByteArray m_readBuffer;
    // Set when a frame exceeded the size cap and its head was discarded: the
    // bytes up to the NEXT newline are that frame's tail, not an event, and are
    // dropped without being parsed. Cleared with m_readBuffer on rebind.
    bool m_discardingLine = false;
    // devSessionId -> (terminalId -> status). Evicted only in whole
    // Dev Session subtrees by retainDevSessions(), called after the sidebar is
    // rebuilt from the server: ids are server-minted and never reused, so a Dev
    // Session the server no longer lists is genuinely gone and safe to drop.
    // NEVER evicted on terminal close, which would lose the raw IdleUnseen
    // state the sidebar's unseen badge is derived from.
    QHash<QString, QHash<QString, TerminalStatus>> m_states;
    // devSessionIds with an unseen completion pending markSeen(). Same
    // eviction: retainDevSessions() drops the flag with its Dev Session subtree.
    QSet<QString> m_unseen;
    // Monotonic time source for both windows. Started in the constructor and
    // never restarted: wall-clock readings would let a clock step (NTP, a
    // suspend/resume) expire or freeze a window.
    QElapsedTimer m_clock;
    QTimer m_ageTimer;
    int m_fallbackIdleThresholdMs = kFallbackIdleThresholdMs;
    int m_staleTimeoutMs = kStaleTimeoutMs;
};

} // namespace ch
