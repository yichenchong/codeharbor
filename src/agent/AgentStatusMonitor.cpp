#include "AgentStatusMonitor.h"

#include <QIODevice>

namespace ch {

namespace {

// Hard cap on a single unframed line. A well-behaved producer delimits every
// event with '\n'; a peer that streams without one is malformed and must not
// grow the read buffer without bound. Agent events are small (state + short
// summary), so 1 MiB is generous headroom.
constexpr int kMaxLineBytes = 1 * 1024 * 1024;

// Human-readable label for a notification title.
QString titleFor(AgentState state)
{
    switch (state) {
    case AgentState::WaitingInput:
        return QStringLiteral("Agent waiting for input");
    case AgentState::IdleUnseen:
        return QStringLiteral("Agent finished");
    default:
        return QStringLiteral("Agent status");
    }
}

// Notification body: prefer the event's summary, else a compact identifier.
QString bodyFor(const AgentEvent& ev)
{
    if (!ev.summary.isEmpty())
        return ev.summary;
    return QStringLiteral("%1 / %2").arg(ev.devSessionId, ev.terminalId);
}

} // namespace

AgentStatusMonitor::AgentStatusMonitor(QObject* parent) : QObject(parent) {}

void AgentStatusMonitor::setTransport(QIODevice* transport)
{
    if (m_transport == transport)
        return;

    if (m_transport)
        m_transport->disconnect(this);

    m_transport = transport;
    m_readBuffer.clear();

    if (!m_transport)
        return;

    connect(m_transport, &QIODevice::readyRead, this,
            &AgentStatusMonitor::onReadyRead);

    // Drain anything already buffered on the transport before we subscribed.
    if (m_transport->bytesAvailable() > 0)
        onReadyRead();
}

void AgentStatusMonitor::onReadyRead()
{
    if (!m_transport)
        return;

    m_readBuffer.append(m_transport->readAll());

    // Consume every complete line; a trailing partial line stays buffered until
    // the rest arrives on a later readyRead (a line split across reads).
    int newline;
    while ((newline = m_readBuffer.indexOf('\n')) != -1) {
        QByteArray line = m_readBuffer.left(newline);
        m_readBuffer.remove(0, newline + 1);
        if (line.endsWith('\r'))
            line.chop(1); // tolerate CRLF framing
        if (line.trimmed().isEmpty())
            continue;
        processLine(line);
    }

    // Guard against an unterminated line growing the buffer without bound.
    if (m_readBuffer.size() > kMaxLineBytes)
        m_readBuffer.clear();
}

void AgentStatusMonitor::processLine(const QByteArray& line)
{
    // Malformed/invalid lines are silently skipped (SPEC 6.4): a broken
    // producer must not take down the client.
    if (auto ev = parseAgentEventLine(line))
        applyEvent(*ev);
}

void AgentStatusMonitor::applyEvent(const AgentEvent& ev)
{
    const QString& dev = ev.devSessionId;
    const QString& term = ev.terminalId;
    const AgentState next = ev.state;

    QHash<QString, AgentState>& terms = m_states[dev];
    const auto it = terms.find(term);
    const bool hadPrev = (it != terms.end());
    const AgentState prev = hadPrev ? it.value() : AgentState::Unknown;
    const bool changed = !hadPrev || prev != next;

    if (hadPrev)
        it.value() = next;
    else
        terms.insert(term, next);

    if (!changed)
        return;

    emit agentStateChanged(dev, term, static_cast<int>(next));

    // Desktop-notification hook fires only on a genuine transition into these
    // attention-worthy states, not on repeated same-state events.
    if (next == AgentState::WaitingInput || next == AgentState::IdleUnseen)
        emit notify(titleFor(next), bodyFor(ev));

    // Reaching idle_unseen marks the Dev Session's completion unseen until the
    // user views it (markSeen).
    if (next == AgentState::IdleUnseen && !m_unseen.contains(dev)) {
        m_unseen.insert(dev);
        emit unseenChanged(dev, true);
    }
}

void AgentStatusMonitor::markSeen(const QString& devSessionId)
{
    if (m_unseen.remove(devSessionId))
        emit unseenChanged(devSessionId, false);
}

int AgentStatusMonitor::stateFor(const QString& devSessionId,
                                 const QString& terminalId) const
{
    const auto sit = m_states.constFind(devSessionId);
    if (sit == m_states.constEnd())
        return static_cast<int>(AgentState::Unknown);
    const auto tit = sit.value().constFind(terminalId);
    if (tit == sit.value().constEnd())
        return static_cast<int>(AgentState::Unknown);
    return static_cast<int>(tit.value());
}

bool AgentStatusMonitor::hasUnseen(const QString& devSessionId) const
{
    return m_unseen.contains(devSessionId);
}

} // namespace ch
