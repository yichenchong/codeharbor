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
    // the caller. Rebinding rewires readyRead and resets the partial-line
    // buffer; accumulated per-session state is preserved.
    void setTransport(QIODevice* transport);
    QIODevice* transport() const { return m_transport; }

    // Clear the unseen-completion flag for a Dev Session (the user viewed it).
    // Emits unseenChanged(devSessionId, false) iff the flag was set.
    Q_INVOKABLE void markSeen(const QString& devSessionId);

    // Current agent state for a (devSessionId, terminalId) pair as an
    // int-valued ch::AgentState; AgentState::Unknown if never observed.
    Q_INVOKABLE int stateFor(const QString& devSessionId,
                             const QString& terminalId) const;

    // Whether the Dev Session has an unseen completion (a terminal reached
    // idle_unseen and markSeen has not been called since).
    Q_INVOKABLE bool hasUnseen(const QString& devSessionId) const;

signals:
    // A terminal's agent state changed. `state` is an int-valued ch::AgentState.
    void agentStateChanged(const QString& devSessionId,
                           const QString& terminalId, int state);
    // The Dev Session's unseen-completion flag flipped.
    void unseenChanged(const QString& devSessionId, bool unseen);
    // Desktop-notification hook, emitted on a transition into waiting_input or
    // idle_unseen. The actual OS notification is raised by the display layer.
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
    // devSessionId -> (terminalId -> current AgentState).
    QHash<QString, QHash<QString, AgentState>> m_states;
    // devSessionIds with an unseen completion pending markSeen().
    QSet<QString> m_unseen;
};

} // namespace ch
