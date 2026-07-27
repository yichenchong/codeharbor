#include "TerminalController.h"

#include "SshChannelDevice.h"

#include <QIODevice>

#include <utility>

namespace {
// POSIX single-quote a value for safe interpolation into a shell command: wrap
// it in single quotes and rewrite every embedded quote as the '\'' sequence, so
// no quote, space, or metacharacter in a working directory or id can break out
// of the quoting (SPEC 5.2). Without this a workingDir like /a'; rm -rf ~; ' or
// an id carrying a quote would inject shell.
QString shellSingleQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}
} // namespace

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

void TerminalController::setTransport(QIODevice *transport)
{
    if (m_transport == transport)
        return;

    if (m_transport)
        m_transport->disconnect(this);

    m_transport = transport;
    // m_pending/m_hidden survive on purpose: a reconnect swaps the channel
    // underneath the same pane and must not drop buffered scrollback.

    if (!m_transport)
        return;

    connect(m_transport, &QIODevice::readyRead, this,
            &TerminalController::onTransportReadyRead);
    connect(m_transport, &QIODevice::readChannelFinished, this,
            &TerminalController::onTransportFinished);

    // A reconnect opens a fresh PTY at the channel's default size; re-assert the
    // geometry the renderer last reported so the pane does not snap back.
    if (m_columns > 0 && m_rows > 0)
        applyPtySize(m_columns, m_rows);

    // Drain whatever the transport buffered before we subscribed.
    if (m_transport->bytesAvailable() > 0)
        onTransportReadyRead();
}

QIODevice *TerminalController::transport() const
{
    return m_transport.data();
}

void TerminalController::onTransportReadyRead()
{
    // isReadable(), not just non-null: closeChannel() fires readChannelFinished()
    // on an already-closed device, and QIODevice::readAll() on one is a warning
    // plus a guaranteed empty result.
    if (!m_transport || !m_transport->isReadable())
        return;
    ingestOutput(m_transport->readAll());
}

void TerminalController::onTransportFinished()
{
    // The final payload precedes readChannelFinished(); claim it before
    // reporting the drop so the last remote bytes still reach the pane.
    onTransportReadyRead();

    // A channel that ends under a live pane is a dropped connection (SPEC 5.6).
    // Only live states transition: an already Disconnected/Error pane must not
    // be walked backwards, and a pane that never came up keeps its state.
    switch (m_state) {
    case TerminalState::OpeningChannel:
    case TerminalState::AttachingTmux:
    case TerminalState::Ready:
        setState(TerminalState::Disconnected);
        break;
    default:
        break;
    }
}

bool TerminalController::sendInput(const QByteArray &bytes)
{
    if (!m_transport || !m_transport->isOpen() || !m_transport->isWritable())
        return false;
    if (bytes.isEmpty())
        return true; // nothing to send, but the pane is writable
    return m_transport->write(bytes) == bytes.size();
}

bool TerminalController::resize(int cols, int rows)
{
    if (cols <= 0 || rows <= 0)
        return false;
    m_columns = cols;
    m_rows = rows;
    return applyPtySize(cols, rows);
}

int TerminalController::columns() const
{
    return m_columns;
}

int TerminalController::rows() const
{
    return m_rows;
}

bool TerminalController::applyPtySize(int cols, int rows)
{
    // The seam is a plain QIODevice, but a window-change is not a byte stream:
    // it is an out-of-band SSH channel request. Narrow to the one transport
    // that can carry it; every other transport records the size and no more.
    if (auto *pty = qobject_cast<SshChannelDevice *>(m_transport.data()))
        return pty->resizePty(cols, rows);
    return false;
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
    return QStringLiteral("tmux new-session -A -s %1 -c %2")
        .arg(shellSingleQuote(tmuxTarget(devSession, terminal)), shellSingleQuote(workingDir));
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
