#include "AgentStatusMonitor.h"

#include <QIODevice>
#include <QVarLengthArray>

#include <optional>

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

// Whether the SPEC 6.6 activity clock may replace this state with one it
// derived itself. The clock's entire vocabulary is Starting/Running/Idle, so a
// pane sitting in a state the clock cannot express is carrying information the
// clock does not have, and neither the passage of time nor a transport event
// may overwrite it:
//
//   * WaitingInput - the agent is blocked on the user, and NOTHING else clears
//     it, so expiring it deletes the one signal this subsystem exists to raise.
//   * Error - a failure nobody has acted on. Re-deriving the pane as "running"
//     a tick later hides the failure completely.
//   * Stopped - the agent process is gone. "Starting" would be a claim about a
//     process that does not exist.
//   * IdleUnseen - a completion. Overwritable only once the user has seen it,
//     which every caller checks separately against m_unseen; this answers true.
//
// Starting/Running/Idle are the clock's own outputs and Unknown is the absence
// of knowledge it starts from, so re-deriving those is exactly its job.
//
// An explicit observation of OUTPUT still overrides any of these: bytes on the
// pane's channel are new evidence that the pane is alive. Time and a channel
// reattach are not evidence of anything, and are what this restrains.
//
// Exhaustive switch with no default: adding an AgentState enumerator must be a
// compiler warning here, not a silent fall into one of the two answers.
bool activityClockMayOverwrite(AgentState state)
{
    switch (state) {
    case AgentState::Starting:
    case AgentState::Running:
    case AgentState::Idle:
    case AgentState::IdleUnseen:
    case AgentState::Unknown:
        return true;
    case AgentState::WaitingInput:
    case AgentState::Error:
    case AgentState::Stopped:
        return false;
    }
    return false;
}

} // namespace

AgentStatusMonitor::AgentStatusMonitor(QObject* parent) : QObject(parent)
{
    m_clock.start();
    m_ageTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_ageTimer, &QTimer::timeout, this, &AgentStatusMonitor::onAgeTick);
}

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
    // Each consumed line is REMOVED from the front of the buffer, and that is
    // O(1), not a shift. This was rewritten to walk a consume offset with a
    // single compaction per read, on the theory that removing from the front
    // memmoves everything behind it and makes one read carrying n events cost
    // order n times its byte count — and then measured, and reverted.
    // QByteArray on Qt 6 owns a QArrayDataPointer with free space allowed at
    // BOTH ends, so remove(0, n) on a detached buffer advances the begin
    // pointer and moves nothing: probed on 6.10.2, two remove(0, 1000) calls on
    // a 1 MiB array each advanced constData() by exactly 1000 with the capacity
    // unchanged. Timed end to end against the real monitor (g++ -O2, one read
    // carrying n framed events), both versions are linear and the offset
    // version is consistently slower:
    //
    //   n       this version    consume-offset version
    //   20000   160-181 ms      201-238 ms
    //   80000   521-587 ms      983-1143 ms
    //   160000  1659-1951 ms    1900-2286 ms
    //
    // (4x n costs ~3.2x time here, so it was never quadratic.) Do not rewrite
    // this into an offset ring: the cost it would be avoiding does not exist,
    // and the rewrite is a measured pessimisation. A container whose
    // front-erase really is a memmove — std::string, std::vector — is a
    // different question; QByteArray's is not.
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
    //
    // Re-entrancy: processLine() reaches slots that may drive another delivery
    // straight back into this handler. The re-entrant call appends to the END
    // of the same buffer and frames from the FRONT, so the oldest complete line
    // is always consumed first and no line is framed twice.
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
        // The cap is a property of the FRAME, not of how the bytes happened to
        // arrive. An over-cap line that comes in one read would otherwise be
        // parsed and applied, while the same line split across reads trips the
        // unterminated-buffer guard below and is dropped — the same event
        // accepted or rejected depending on socket chunking. Drop it either
        // way. The shipped relay cannot produce such a line (makeStreamSink in
        // remote/src/bridge.ts measures every outbound payload with
        // bridgeLineFits() and drops both the event and the producer past
        // 1 MiB), but this side does not get to assume its peer is the shipped
        // relay: the guard is defence against an untrusted or buggy producer,
        // and it must not depend on how that producer chunks its writes.
        if (newline > kMaxLineBytes) {
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

    QHash<QString, TerminalStatus>& terms = m_states[dev];
    const auto it = terms.find(term);
    const bool hadPrev = (it != terms.end());
    const bool changed = !hadPrev || it.value().state != next;

    if (hadPrev) {
        it.value().state = next;
        it.value().lastEventMs = m_clock.elapsed();
    } else {
        TerminalStatus fresh;
        fresh.state = next;
        fresh.lastEventMs = m_clock.elapsed();
        terms.insert(term, fresh);
    }

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

    // The pane just spoke, so the silence window restarts from here — and a
    // pane that entered Starting/Running now needs the tick timer that will
    // eventually demote it. Done BEFORE the emits: rearmAgeTimer() re-walks
    // m_states, and a re-entrant applyEvent() must find the timer already
    // consistent with the state it can see.
    rearmAgeTimer();

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
    if (!m_unseen.remove(devSessionId))
        return;
    // Clearing the flag can put a pane back under time-driven derivation:
    // onAgeTick() refuses to let time overwrite an UNSEEN completion, so a
    // generic pane parked at IdleUnseen starts ageing again the moment the
    // user views it. Without this the tick timer stays stopped and the pane
    // only moves if some OTHER pane happens to keep it running. Rearmed before
    // the emit so a slot that re-enters finds the timer already consistent.
    rearmAgeTimer();
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
    // Evicting the last pane that could still age must stop the tick timer;
    // nothing else would, since the timer is only re-evaluated on an
    // observation and a dropped Dev Session produces no more of those.
    rearmAgeTimer();
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
    return static_cast<int>(tit.value().state);
}

bool AgentStatusMonitor::hasUnseen(const QString& devSessionId) const
{
    return m_unseen.contains(devSessionId);
}

AgentStatusMonitor::TerminalStatus*
AgentStatusMonitor::findStatus(const QString& devSessionId, const QString& terminalId)
{
    const auto sit = m_states.find(devSessionId);
    if (sit == m_states.end())
        return nullptr;
    const auto tit = sit->find(terminalId);
    return tit == sit->end() ? nullptr : &tit.value();
}

void AgentStatusMonitor::setTerminalHarness(const QString& devSessionId,
                                            const QString& terminalId,
                                            const QString& harness)
{
    if (harness != QLatin1String("generic")) {
        // Not generic: never create a row for an adapter-driven pane that has
        // not spoken on the wire. If this pane was previously generic, clear
        // the output-derived state as the source of truth changes; otherwise
        // a pane that was running in generic mode would remain falsely busy
        // until an adapter event happened to arrive.
        if (TerminalStatus* st = findStatus(devSessionId, terminalId)) {
            const bool wasGeneric = st->generic;
            const bool preserveCompletion =
                st->state == AgentState::IdleUnseen
                && m_unseen.contains(devSessionId);
            const bool clearStale =
                wasGeneric && !preserveCompletion
                && st->state != AgentState::Unknown;
            st->generic = false;
            if (clearStale)
                st->state = AgentState::Unknown;
            rearmAgeTimer();
            if (clearStale)
                emit agentStateChanged(devSessionId, terminalId,
                                       static_cast<int>(AgentState::Unknown));
        }
        return;
    }
    // Generic: a new registration starts at Unknown. Re-registering an
    // already-generic pane must preserve its derived state because this method
    // is called again whenever the workspace tree is rebuilt. If the harness
    // changes while a channel is attached, begin a fresh silent observation;
    // output from the old harness must not be mistaken for generic activity.
    TerminalStatus& st = m_states[devSessionId][terminalId];
    if (!st.generic) {
        const AgentState previous = st.state;
        st.generic = true;
        st.lastOutputMs = -1;
        const bool preserveCompletion =
            previous == AgentState::IdleUnseen
            && m_unseen.contains(devSessionId);
        const AgentState next =
            preserveCompletion
                ? previous
                : st.attached ? AgentState::Starting : AgentState::Unknown;
        const bool changed = next != previous;
        st.state = next;
        if (st.attached)
            st.lastEventMs = m_clock.elapsed();
        rearmAgeTimer();
        if (changed)
            emit agentStateChanged(devSessionId, terminalId,
                                   static_cast<int>(next));
        return;
    }
    rearmAgeTimer();
}

void AgentStatusMonitor::noteTerminalAttached(const QString& devSessionId,
                                              const QString& terminalId)
{
    TerminalStatus* st = findStatus(devSessionId, terminalId);
    if (!st)
        return;
    st->attached = true;
    // A new channel: the previous one's output age says nothing about this one.
    st->lastOutputMs = -1;
    st->lastEventMs = m_clock.elapsed();
    // A reattach is a TRANSPORT event, not evidence about the agent. SPEC 5.6
    // rewires every open pane onto a fresh channel on reconnect, with no user
    // action and nothing new learned about what is running in the pane, so it
    // may only move the pane inside the activity clock's own vocabulary. A pane
    // parked at WaitingInput/Error/Stopped, or holding a completion the user
    // has not seen, keeps that state: rewriting it as Starting would leave a
    // blocked, failed or finished agent reading "starting" for as long as it
    // stays quiet, since a silent generic pane never leaves Starting. Real
    // output (noteTerminalOutput) still overrides all of them.
    const bool derivable =
        activityClockMayOverwrite(st->state)
        && !(st->state == AgentState::IdleUnseen
             && m_unseen.contains(devSessionId));
    const bool changed =
        st->generic && derivable && st->state != AgentState::Starting;
    if (changed)
        st->state = AgentState::Starting;
    rearmAgeTimer();
    if (changed)
        emit agentStateChanged(devSessionId, terminalId,
                               static_cast<int>(AgentState::Starting));
}

void AgentStatusMonitor::noteTerminalOutput(const QString& devSessionId,
                                            const QString& terminalId)
{
    TerminalStatus* st = findStatus(devSessionId, terminalId);
    if (!st)
        return;
    st->lastOutputMs = m_clock.elapsed();
    const bool changed =
        st->generic && st->attached && st->state != AgentState::Running;
    if (changed)
        st->state = AgentState::Running;
    // No pointer use past here: the emit below can re-enter and rehash m_states.
    rearmAgeTimer();
    if (changed)
        emit agentStateChanged(devSessionId, terminalId,
                               static_cast<int>(AgentState::Running));
}

void AgentStatusMonitor::setFallbackIdleThresholdMs(int ms)
{
    m_fallbackIdleThresholdMs = qMax(0, ms);
    rearmAgeTimer();
}

void AgentStatusMonitor::setStaleTimeoutMs(int ms)
{
    m_staleTimeoutMs = qMax(0, ms);
    rearmAgeTimer();
}

// Whether a pane's state can still change with the passage of time alone.
// This must be EXACTLY the set of panes onAgeTick() would find work in, or the
// two disagree: rearmAgeTimer() stops the timer while a pane still has a
// pending transition, and that pane then only moves if some OTHER pane happens
// to keep the timer alive. It is therefore written as the same arms onAgeTick()
// evaluates, in the same order, reading the same fields.
bool AgentStatusMonitor::agesWithTime(const TerminalStatus& st,
                                      bool devSessionUnseen) const
{
    if (st.generic && st.attached) {
        // A state the activity clock cannot express is never overwritten by
        // time; see the same arm, and the reasoning, in onAgeTick().
        if (!activityClockMayOverwrite(st.state))
            return false;
        // A completion the user has not seen is never overwritten by time.
        if (st.state == AgentState::IdleUnseen && devSessionUnseen)
            return false;
        // SPEC 6.6's arms. With no output yet the pane settles at Starting and
        // stays there; once output has been seen it is Running until the quiet
        // window elapses and Idle after that. So the pane has time-driven work
        // left exactly while it has not reached the state its own arm ends in.
        return st.lastOutputMs < 0 ? st.state != AgentState::Starting
                                   : st.state != AgentState::Idle;
    }
    // Silence demotion: only a pane that claims a live agent ages, and only
    // once it has produced at least one sign of life to measure silence from.
    return m_staleTimeoutMs > 0
        && (st.state == AgentState::Starting || st.state == AgentState::Running)
        && qMax(st.lastEventMs, st.lastOutputMs) >= 0;
}

void AgentStatusMonitor::onAgeTick()
{
    const qint64 now = m_clock.elapsed();

    // Two passes. Every state is mutated in place while the iterators are
    // valid, then the signals go out: a slot is free to re-enter and rehash
    // m_states, which would invalidate an iterator held across an emit.
    struct Pending {
        QString dev;
        QString term;
        AgentState state;
    };
    QVarLengthArray<Pending, 8> pending;

    for (auto sit = m_states.begin(); sit != m_states.end(); ++sit) {
        for (auto tit = sit.value().begin(); tit != sit.value().end(); ++tit) {
            TerminalStatus& st = tit.value();
            std::optional<AgentState> next;
            if (st.generic && st.attached) {
                // A state the activity clock cannot express outranks it
                // outright: WaitingInput (nothing but a new observation ever
                // clears it, so letting the quiet window overwrite it would
                // silently delete the one signal that says the agent is
                // blocked on the user), Error (a failure the clock would hide
                // by re-deriving the pane as running) and Stopped (a process
                // that no longer exists cannot be "starting"). This pane is
                // registered as the adapterless "generic" harness yet has
                // received a lifecycle event anyway — the pane's configured
                // harness and the harness that actually reported are
                // independent — which is the same way an unseen completion
                // lands here.
                if (!activityClockMayOverwrite(st.state))
                    continue;
                // A completion remains an explicit user-facing state until
                // the user has seen it. The generic activity clock must not
                // resurrect that pane as Starting/Running merely because
                // another pane caused this shared timer to tick. An explicit
                // observation of OUTPUT may still replace it; time, and a
                // channel reattach, never may.
                if (st.state == AgentState::IdleUnseen
                    && m_unseen.contains(sit.key())) {
                    continue;
                }
                // SPEC 6.6, mirroring the three arms the fallback detector
                // defines: no output yet, output within the quiet window, or
                // quiet for longer than it.
                if (st.lastOutputMs < 0)
                    next = AgentState::Starting;
                else
                    next = (now - st.lastOutputMs < m_fallbackIdleThresholdMs)
                        ? AgentState::Running
                        : AgentState::Idle;
            } else if (m_staleTimeoutMs > 0
                       && (st.state == AgentState::Starting
                           || st.state == AgentState::Running)) {
                // Silence demotion. Either channel counts as a sign of life:
                // an agent event, or bytes on the pane's PTY.
                const qint64 lastSign = qMax(st.lastEventMs, st.lastOutputMs);
                if (lastSign >= 0 && now - lastSign >= m_staleTimeoutMs)
                    next = AgentState::Unknown;
            }
            if (!next || *next == st.state)
                continue;
            st.state = *next;
            pending.push_back({sit.key(), tit.key(), *next});
        }
    }

    for (const Pending& p : pending) {
        // A signal handler may evict or replace this pane while an earlier
        // pending transition is being emitted. Do not report a transition for
        // a pane that no longer exists, or for a state superseded re-entrantly.
        TerminalStatus* st = findStatus(p.dev, p.term);
        if (!st || st->state != p.state)
            continue;
        emit agentStateChanged(p.dev, p.term, static_cast<int>(p.state));
    }

    rearmAgeTimer();
}

void AgentStatusMonitor::rearmAgeTimer()
{
    bool needed = false;
    for (auto sit = m_states.constBegin(); sit != m_states.constEnd() && !needed;
         ++sit) {
        const bool unseen = m_unseen.contains(sit.key());
        for (auto tit = sit.value().constBegin(); tit != sit.value().constEnd();
             ++tit) {
            if (agesWithTime(tit.value(), unseen)) {
                needed = true;
                break;
            }
        }
    }
    if (!needed) {
        m_ageTimer.stop();
        return;
    }
    // Fine enough for the shorter of the two windows, so a threshold expires
    // within a quarter of itself, and never so fine that an idle application
    // wakes up for nothing. With the shipped values the tick is 500 ms.
    qint64 unit = m_fallbackIdleThresholdMs;
    if (m_staleTimeoutMs > 0)
        unit = qMin(unit, static_cast<qint64>(m_staleTimeoutMs));
    const int interval =
        static_cast<int>(qBound<qint64>(10, unit / 4, qint64{500}));
    if (m_ageTimer.interval() != interval)
        m_ageTimer.setInterval(interval);
    if (!m_ageTimer.isActive())
        m_ageTimer.start();
}

} // namespace ch
