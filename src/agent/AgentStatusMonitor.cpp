#include "AgentStatusMonitor.h"

#include <QIODevice>

namespace ch {

namespace {

// Hard cap on a single unframed line. A well-behaved producer delimits every
// event with '\n'; a peer that streams without one is malformed and must not
// grow the read buffer without bound. Agent events are small (state + short
// summary), so 1 MiB is generous headroom.
constexpr qsizetype kMaxLineBytes = 1 * 1024 * 1024;

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
    // Both halves of the framing state go together: a half-received line from
    // the dead producer must never be spliced onto the new one's first frame,
    // and a pending oversize discard belongs to a stream that no longer exists.
    m_readBuffer.clear();
    m_discardingLine = false;

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
    //
    // No CR stripping and no blank-line test here: parseAgentEventLine() trims
    // the frame before it looks at it, so a CRLF-framed line loses its '\r'
    // there and a blank (or whitespace-only, or bare "\r") line is rejected as
    // "not an event" by the same code path as any other unparseable line.
    // Doing it twice costs an extra QByteArray allocation per line for no
    // behavioural difference.
    //
    // indexOf() returns qsizetype: narrowing it to int would wrap a >2 GiB
    // buffer to a negative offset and hand left()/remove() nonsense. The
    // oversize guard below makes that unreachable in practice, but the guard
    // runs AFTER this loop, so the loop must be correct on its own.
    qsizetype newline;
    while ((newline = m_readBuffer.indexOf('\n')) != -1) {
        if (m_discardingLine) {
            // Everything up to this newline is the tail of a frame whose head
            // already blew the size cap and was thrown away. It is half of
            // something, never an event of its own, so it is dropped without
            // even being copied out: framing resumes at the NEXT newline.
            m_discardingLine = false;
            m_readBuffer.remove(0, newline + 1);
            continue;
        }
        const QByteArray line = m_readBuffer.left(newline);
        m_readBuffer.remove(0, newline + 1);
        processLine(line);
    }

    // Guard against an unterminated line growing the buffer without bound. The
    // accumulated bytes are dropped and the remainder of that frame is marked
    // for discard, so the arbitrary cut point can never produce a fragment that
    // is mistaken for a complete event.
    if (m_readBuffer.size() > kMaxLineBytes) {
        m_readBuffer.clear();
        m_discardingLine = true;
    }
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
    const bool changed = !hadPrev || it.value() != next;

    if (hadPrev)
        it.value() = next;
    else
        terms.insert(term, next);

    // Reaching idle_unseen marks the Dev Session's completion unseen until the
    // user views it (markSeen). Evaluated for EVERY idle_unseen event, not only
    // for a transition into it: markSeen() clears the per-session unseen flag
    // but deliberately leaves the terminal's raw state at IdleUnseen (see
    // AppController::rebuildRows, which downgrades it for display). A genuinely
    // new completion that arrives with no intervening state — a re-fired
    // agent_end, or an adapter that only ever reports completions — would
    // otherwise be discarded here as a no-op and the badge would never come
    // back for work the user has not seen.
    const bool armedUnseen =
        (next == AgentState::IdleUnseen) && !m_unseen.contains(dev);
    if (armedUnseen)
        m_unseen.insert(dev);

    // No reads of `terms`/`it` past this point: a slot may re-enter applyEvent()
    // and rehash the QHash, invalidating both.
    //
    // Both data signals go out before the notification hook so the display
    // layer has already re-derived the sidebar row by the time the desktop
    // bubble is raised; notify() is a side effect, not a source of truth, and
    // must never be the thing that tells the UI something changed.
    if (changed)
        emit agentStateChanged(dev, term, static_cast<int>(next));
    if (armedUnseen)
        emit unseenChanged(dev, true);

    // Desktop-notification hook: a genuine transition into an attention-worthy
    // state, or a completion that re-arms a badge the user had already cleared.
    // A repeat of the same state with nothing newly pending raises nothing —
    // this transition gate is the first line of defence against a chatty agent
    // becoming a notification storm.
    if (armedUnseen
        || (changed
            && (next == AgentState::WaitingInput
                || next == AgentState::IdleUnseen)))
        emit notify(titleFor(next), bodyFor(ev));
}

void AgentStatusMonitor::markSeen(const QString& devSessionId)
{
    if (m_unseen.remove(devSessionId))
        emit unseenChanged(devSessionId, false);
}

void AgentStatusMonitor::retainDevSessions(const QSet<QString>& liveDevSessionIds)
{
    // Whole-subtree eviction: a Dev Session absent from the freshly rebuilt
    // sidebar list is gone server-side, so its terminals' states and its unseen
    // flag are dropped together. Never called on a mere terminal close, which
    // would destroy the finished-with-unseen-output signal.
    for (auto it = m_states.begin(); it != m_states.end();) {
        if (!liveDevSessionIds.contains(it.key()))
            it = m_states.erase(it);
        else
            ++it;
    }
    for (auto it = m_unseen.begin(); it != m_unseen.end();) {
        if (!liveDevSessionIds.contains(*it))
            it = m_unseen.erase(it);
        else
            ++it;
    }
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
