#pragma once

#include "AgentEvent.h"
#include "SessionState.h"

#include <QByteArray>
#include <QHash>
#include <QIODevice>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

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

signals:
    // A terminal's agent state changed. `state` is an int-valued ch::AgentState.
    // Also emitted the FIRST time a (devSessionId, terminalId) pair is observed,
    // including when that first event carries the wire token "unknown": going
    // from "no event has ever named this terminal" to "the producer says its
    // agent state is unknown" is a real change of knowledge even though
    // stateFor() reports AgentState::Unknown for both. Never emitted for a
    // repeat of the state already recorded for that pair.
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

    // QPointer so it auto-nulls if a caller-owned transport is destroyed while
    // the monitor outlives it; a raw pointer would dangle and setTransport()'s
    // disconnect() on the old transport would be a use-after-free.
    QPointer<QIODevice> m_transport = nullptr;
    QByteArray m_readBuffer;
    // Set when a frame exceeded the size cap and its head was discarded: the
    // bytes up to the NEXT newline are that frame's tail, not an event, and are
    // dropped without being parsed. Cleared with m_readBuffer on rebind.
    bool m_discardingLine = false;
    // devSessionId -> (terminalId -> current AgentState). Evicted only in whole
    // Dev Session subtrees by retainDevSessions(), called after the sidebar is
    // rebuilt from the server: ids are server-minted and never reused, so a Dev
    // Session the server no longer lists is genuinely gone and safe to drop.
    // NEVER evicted on terminal close, which would lose the raw IdleUnseen
    // state the sidebar's unseen badge is derived from.
    QHash<QString, QHash<QString, AgentState>> m_states;
    // devSessionIds with an unseen completion pending markSeen(). Same
    // eviction: retainDevSessions() drops the flag with its Dev Session subtree.
    QSet<QString> m_unseen;
};

} // namespace ch
