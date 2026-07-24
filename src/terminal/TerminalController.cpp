#include "TerminalController.h"

#include <utility>

namespace ch {

TerminalController::TerminalController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<TerminalState>("ch::TerminalState");
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(kFlushIntervalMs);
    connect(&m_flushTimer, &QTimer::timeout, this, &TerminalController::flush);
}

TerminalState TerminalController::state() const
{
    return m_state;
}

bool TerminalController::viewVisible() const
{
    return m_viewVisible;
}

void TerminalController::setViewVisible(bool visible)
{
    if (m_viewVisible == visible)
        return;
    m_viewVisible = visible;
    if (visible && !m_hidden.isEmpty()) {
        // Replay everything retained while hidden as one batch, then reset the
        // rolling buffer so tmux history covers anything older (SPEC 5.4/5.5).
        const QByteArray replay = std::move(m_hidden);
        m_hidden.clear();
        emit flushReady(replay);
    }
}

void TerminalController::ingestOutput(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return; // empty output: nothing to buffer or flush
    m_pending.append(bytes);
    if (m_pending.size() >= kFlushSizeBytes) {
        flush(); // size threshold reached first
    } else if (!m_flushTimer.isActive()) {
        m_flushTimer.start(); // arm the time threshold from the first buffered byte
    }
}

const QByteArray &TerminalController::hiddenBuffer() const
{
    return m_hidden;
}

void TerminalController::flush()
{
    m_flushTimer.stop();
    if (m_pending.isEmpty())
        return;
    const QByteArray batch = std::move(m_pending);
    m_pending.clear();
    if (m_viewVisible)
        emit flushReady(batch);
    else
        appendHidden(batch);
}

void TerminalController::appendHidden(const QByteArray &batch)
{
    m_hidden.append(batch);
    const qsizetype overflow = m_hidden.size() - kHiddenBufferMaxBytes;
    if (overflow > 0)
        m_hidden.remove(0, overflow); // evict oldest bytes past the cap
}

void TerminalController::setState(TerminalState next)
{
    if (m_state == next)
        return;
    m_state = next;
    emit stateChanged(m_state);
}

QString TerminalController::tmuxTarget(const DevSessionId &devSession,
                                       const TerminalId &terminal)
{
    return QStringLiteral("ch_%1_%2").arg(devSession.value, terminal.value);
}

QString TerminalController::tmuxNewSessionCommand(const DevSessionId &devSession,
                                                  const TerminalId &terminal,
                                                  const QString &workingDir)
{
    return QStringLiteral("tmux new-session -A -s '%1' -c '%2'")
        .arg(tmuxTarget(devSession, terminal), workingDir);
}

int TerminalController::reconnectDelaySeconds(int attempt)
{
    static constexpr int schedule[] = {1, 2, 5, 10, 30};
    constexpr int count = static_cast<int>(sizeof(schedule) / sizeof(schedule[0]));
    if (attempt < 0)
        return schedule[0];
    if (attempt < count)
        return schedule[attempt];
    return 60;
}

} // namespace ch
