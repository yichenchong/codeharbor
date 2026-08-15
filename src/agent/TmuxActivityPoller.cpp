#include "TmuxActivityPoller.h"

#include "CodeharbordClient.h"
#include "RpcTypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <optional>

namespace ch {

TmuxActivityPoller::TmuxActivityPoller(QObject* parent) : QObject(parent)
{
    // Coarse: this is a five-second housekeeping poll feeding a sidebar badge,
    // and it must never be a reason for the process to wake precisely.
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &TmuxActivityPoller::pollNow);
}

void TmuxActivityPoller::setRpcClient(CodeharbordClient* client)
{
    if (m_client == client)
        return;
    m_client = client;
    // The answer the previous client still owes us can never arrive for THIS
    // one, and its callback below refuses to touch the guard once the client
    // has moved on. Clearing it here is therefore the only thing that keeps a
    // client swap from latching the poller permanently silent.
    m_inFlight = false;
    rearm();
}

void TmuxActivityPoller::setDevSessionIds(const QSet<QString>& devSessionIds)
{
    if (m_devSessionIds == devSessionIds)
        return;
    m_devSessionIds = devSessionIds;
    rearm();
}

void TmuxActivityPoller::setPollIntervalMs(int ms)
{
    if (ms <= 0 || ms == m_pollIntervalMs)
        return;
    m_pollIntervalMs = ms;
    // Stop first so rearm() sees the transition and applies the new cadence;
    // QTimer keeps its old interval until it is restarted.
    m_timer.stop();
    rearm();
}

void TmuxActivityPoller::rearm()
{
    if (!m_client || m_devSessionIds.isEmpty()) {
        m_timer.stop();
        return;
    }
    if (m_timer.isActive())
        return;
    m_timer.start(m_pollIntervalMs);
    // Ask at once rather than at the end of the first interval: the user has
    // just switched Dev Session, and a sidebar that reports nothing for five
    // seconds about the session they left is the very gap this poller exists to
    // close.
    pollNow();
}

void TmuxActivityPoller::pollNow()
{
    if (!m_client || m_devSessionIds.isEmpty())
        return;
    // See m_inFlight: overlapping identical requests must never queue up behind
    // a server that has stopped answering.
    if (m_inFlight)
        return;

    QJsonArray ids;
    for (const QString& devSessionId : m_devSessionIds)
        ids.append(devSessionId);

    m_inFlight = true;
    QPointer<TmuxActivityPoller> self(this);
    QPointer<CodeharbordClient> askedOf(m_client);
    m_client->call(QString::fromLatin1(rpc::kMethodPaneActivity),
                   QJsonObject{{QStringLiteral("devSessionIds"), ids}},
                   [self, askedOf](QJsonValue result,
                                   std::optional<RpcError> error) {
                       if (!self)
                           return;
                       // An answer from a peer this poller has since been moved
                       // off describes somebody else's host, and clearing the
                       // guard on its behalf would let two requests be in flight
                       // on the CURRENT client at once. setRpcClient() has
                       // already cleared the guard for the new one.
                       if (self->m_client != askedOf)
                           return;
                       self->m_inFlight = false;
                       // A failure is not evidence about any pane: the panes
                       // simply keep whatever the monitor last knew, and the
                       // next tick asks again. Nothing here is ever surfaced to
                       // the user — a sidebar badge is not worth a toast.
                       if (error)
                           return;
                       self->publishActivity(result.toObject());
                   });
}

void TmuxActivityPoller::publishActivity(const QJsonObject& result)
{
    // The server's own wall clock at listing time. Everything below is
    // differenced against THIS and never against the local clock, which is the
    // whole reason the daemon sends it: the two machines' clocks need not agree,
    // and an age computed across them would be wrong by the offset between them
    // — enough, on a host a few minutes out, to report every pane as either
    // permanently busy or permanently idle.
    const QJsonValue nowValue = result.value(QStringLiteral("nowMs"));
    if (!nowValue.isDouble())
        return;  // no clock to measure against: the whole listing says nothing
    const qint64 nowMs = nowValue.toInteger(0);

    const QJsonArray panes = result.value(QStringLiteral("panes")).toArray();
    QPointer<TmuxActivityPoller> self(this);
    for (const QJsonValue& entry : panes) {
        // Each emit reaches ch::AgentStatusMonitor and from there the sidebar
        // and QML, any of which may tear this poller down; the rest of the
        // listing is not worth a use-after-free.
        if (!self)
            return;
        const QJsonObject pane = entry.toObject();
        const QString devSessionId =
            pane.value(QStringLiteral("devSessionId")).toString();
        const QString terminalId =
            pane.value(QStringLiteral("terminalId")).toString();
        if (devSessionId.isEmpty() || terminalId.isEmpty())
            continue;
        // A null (or absent, or non-numeric) activity time is the daemon saying
        // it could not date this pane — tmux renders a format name it does not
        // know as an EMPTY field in an otherwise successful listing, so this is
        // exactly what a tmux whose format names differ from the one we were
        // written against produces. Report nothing. Any age synthesized here
        // would be a silent lie in one direction or the other: 0 dates the pane
        // to 1970 and makes it permanently idle, `nowMs` makes it permanently
        // busy.
        const QJsonValue activity = pane.value(QStringLiteral("lastActivityMs"));
        if (!activity.isDouble())
            continue;
        emit activityObserved(devSessionId, terminalId,
                              nowMs - activity.toInteger(0),
                              pane.value(QStringLiteral("alive")).toBool(false));
    }
}

} // namespace ch
