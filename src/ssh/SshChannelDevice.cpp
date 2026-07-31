#include "SshChannelDevice.h"

#include <QTimer>

#include <cstring>

namespace ch {

namespace {

// Drain granularity per libssh read. 16 KiB comfortably exceeds one SSH channel
// data packet, so a burst is normally emptied in a couple of iterations.
constexpr int kChunkBytes = 16 * 1024;

} // namespace

SshChannelDevice::SshChannelDevice(SshConnectionPool* pool,
                                   SshConnectionPool::ChannelKind kind,
                                   QObject* parent)
    : QIODevice(parent), m_pool(pool), m_kind(kind), m_pump(new QTimer(this))
{
    // Single-shot and re-armed by pump() so the interval can adapt between
    // "burst in flight" (0 ms) and "quiet" (kIdlePollMs) without the restart
    // semantics of mutating a repeating timer's interval.
    m_pump->setSingleShot(true);
    m_pump->setTimerType(Qt::PreciseTimer);
    connect(m_pump, &QTimer::timeout, this, &SshChannelDevice::pump);

    // The pool frees every channel when its session goes down, so a device that
    // outlives that moment MUST let go first: the read pump would otherwise
    // call into a freed ssh_channel. Nothing else guarantees the owner tore the
    // device down in time — a terminal pane deliberately survives an SSH drop
    // and reattaches (SPEC 5.6).
    if (m_pool) {
        connect(m_pool, &SshConnectionPool::sessionClosing, this,
                &SshChannelDevice::closeChannel);
    }
}

SshChannelDevice::~SshChannelDevice()
{
    m_pump->stop();
    // Tear down without emitting: readChannelFinished()/aboutToClose() during
    // destruction would hand subscribers a half-destroyed object.
    m_remoteFinished = true;
#if CH_HAVE_LIBSSH
    if (m_channel) {
        // Hand it back rather than merely closing it: the channel's slot on the
        // SSH connection is only reclaimed when the pool frees the channel, and
        // a session that opens one channel per remote command runs out of slots
        // long before it is disconnected.
        if (m_pool)
            m_pool->releaseChannel(m_channel);
        m_channel = nullptr;
    }
#endif
}

qint64 SshChannelDevice::bytesAvailable() const
{
    return QIODevice::bytesAvailable() + m_readBuffer.size();
}

void SshChannelDevice::close()
{
    closeChannel();
}

bool SshChannelDevice::open(OpenMode mode)
{
    Q_UNUSED(mode);
    // Refused on purpose; see the header. beginStreaming() opens the device
    // through QIODevice::open() once a channel really exists.
    setErrorString(
        QStringLiteral("an SSH channel device is opened by startExec() or "
                       "startPty(), not by open()"));
    return false;
}

qint64 SshChannelDevice::readData(char* data, qint64 maxSize)
{
    if (maxSize <= 0)
        return 0;
    const qint64 count = qMin<qint64>(maxSize, m_readBuffer.size());
    if (count <= 0) {
        // 0 means "nothing buffered right now" for a sequential device; the
        // pump will emit readyRead() again when more arrives. Never -1: that
        // would be reported as a device error to the reader.
        return 0;
    }
    std::memcpy(data, m_readBuffer.constData(), static_cast<size_t>(count));
    m_readBuffer.remove(0, count);
    return count;
}

QString SshChannelDevice::lastError() const
{
#if CH_HAVE_LIBSSH
    if (m_channel) {
        if (ssh_session session = ssh_channel_get_session(m_channel)) {
            const char* text = ssh_get_error(session);
            if (text && *text)
                return QString::fromUtf8(text);
        }
    }
#endif
    return QStringLiteral("unknown libssh error");
}

void SshChannelDevice::failWith(const QString& reason)
{
    setErrorString(reason);
    emit channelError(reason);
}

void SshChannelDevice::abortStart(const QString& reason)
{
#if CH_HAVE_LIBSSH
    if (m_channel) {
        // A start that failed halfway must not keep the channel: every failed
        // attempt would otherwise burn one of the connection's channel slots
        // for good, and a reconnect loop retries this path.
        if (m_pool)
            m_pool->releaseChannel(m_channel);
        m_channel = nullptr;
    }
    m_hasPty = false;
#endif
    failWith(reason);
}

#if !CH_HAVE_LIBSSH

bool SshChannelDevice::startExec(const QString& command)
{
    Q_UNUSED(command);
    failWith(QStringLiteral("built without libssh"));
    return false;
}

bool SshChannelDevice::startPty(const QString& term, int cols, int rows,
                                const QString& command)
{
    Q_UNUSED(term);
    Q_UNUSED(cols);
    Q_UNUSED(rows);
    Q_UNUSED(command);
    failWith(QStringLiteral("built without libssh"));
    return false;
}

bool SshChannelDevice::resizePty(int cols, int rows)
{
    Q_UNUSED(cols);
    Q_UNUSED(rows);
    return false;
}

void SshChannelDevice::closeChannel()
{
    m_pump->stop();
    m_hasPty = false;
    m_readBuffer.clear();
    if (isOpen())
        QIODevice::close();
    // Same contract as the libssh build: the end of the read channel is
    // reported exactly once. A consumer that keys its teardown on this signal
    // (CodeharbordClient fails every pending call from it) must not behave
    // differently just because the client was built without libssh.
    if (!m_remoteFinished) {
        m_remoteFinished = true;
        emit readChannelFinished();
    }
}

qint64 SshChannelDevice::writeData(const char* data, qint64 maxSize)
{
    Q_UNUSED(data);
    Q_UNUSED(maxSize);
    return -1;
}

bool SshChannelDevice::acquireChannel()
{
    return false;
}

bool SshChannelDevice::beginStreaming()
{
    return false;
}

void SshChannelDevice::pump() {}

#else // CH_HAVE_LIBSSH

bool SshChannelDevice::acquireChannel()
{
    if (m_channel) {
        failWith(QStringLiteral("channel already started"));
        return false;
    }
    if (!m_pool) {
        failWith(QStringLiteral("no SSH connection pool"));
        return false;
    }
    m_channel = m_pool->openChannel(m_kind);
    if (!m_channel) {
        failWith(QStringLiteral("could not open SSH channel"));
        return false;
    }
    return true;
}

bool SshChannelDevice::beginStreaming()
{
    m_readBuffer.clear();
    m_remoteFinished = false;
    // A previous channel may have ended mid-character; the new channel's stderr
    // must not inherit that half-decoded state.
    m_stderrDecoder.resetState();
    // Unbuffered: QIODevice must not interpose a read buffer in front of our
    // own, and CodeharbordClient requires isOpen() && isWritable() before it
    // will emit a request.
    if (!QIODevice::open(QIODevice::ReadWrite | QIODevice::Unbuffered)) {
        abortStart(QStringLiteral("QIODevice::open() failed"));
        return false;
    }
    m_pump->start(0);
    return true;
}

bool SshChannelDevice::startExec(const QString& command)
{
    if (!acquireChannel())
        return false;

    const QByteArray commandUtf8 = command.toUtf8();
    if (ssh_channel_request_exec(m_channel, commandUtf8.constData()) != SSH_OK) {
        abortStart(lastError());
        return false;
    }
    return beginStreaming();
}

bool SshChannelDevice::startPty(const QString& term, int cols, int rows,
                                const QString& command)
{
    if (!acquireChannel())
        return false;

    // A terminal is at least one cell in each direction. A renderer can report
    // 0 columns for one frame while its layout settles, and a 0-wide remote tty
    // makes line-editing shells and full-screen programs draw nonsense until
    // the next resize arrives.
    cols = qMax(1, cols);
    rows = qMax(1, rows);

    if (m_kind == SshConnectionPool::ChannelKind::Pty) {
        // openChannel(Pty) already issued ssh_channel_request_pty(); a second
        // pty-req on the same channel is a protocol violation, so only resize.
        if (ssh_channel_change_pty_size(m_channel, cols, rows) != SSH_OK) {
            abortStart(lastError());
            return false;
        }
    } else {
        const QByteArray termUtf8 =
            term.isEmpty() ? QByteArrayLiteral("xterm-256color") : term.toUtf8();
        if (ssh_channel_request_pty_size(m_channel, termUtf8.constData(), cols,
                                         rows)
            != SSH_OK) {
            abortStart(lastError());
            return false;
        }
    }
    m_hasPty = true;

    if (command.isEmpty()) {
        if (ssh_channel_request_shell(m_channel) != SSH_OK) {
            abortStart(lastError());
            return false;
        }
    } else {
        const QByteArray commandUtf8 = command.toUtf8();
        if (ssh_channel_request_exec(m_channel, commandUtf8.constData())
            != SSH_OK) {
            abortStart(lastError());
            return false;
        }
    }
    return beginStreaming();
}

bool SshChannelDevice::resizePty(int cols, int rows)
{
    if (!m_channel || !m_hasPty)
        return false;
    // Same floor as startPty(): never push a zero-sized window to the remote.
    return ssh_channel_change_pty_size(m_channel, qMax(1, cols), qMax(1, rows))
           == SSH_OK;
}

void SshChannelDevice::closeChannel()
{
    m_pump->stop();

    if (m_channel) {
        // Hand the channel back: the pool owns it (SshConnectionPool.h) and
        // freeing it here would double-free. Closing alone is not enough —
        // an already-EOF channel is not even closable, and its slot on the SSH
        // connection would stay consumed until the whole session went down.
        if (m_pool)
            m_pool->releaseChannel(m_channel);
        m_channel = nullptr;
    }
    m_hasPty = false;

    // Drop anything still buffered: bytesAvailable() must not advertise
    // readable bytes on a device that has been closed.
    m_readBuffer.clear();
    if (isOpen())
        QIODevice::close();

    if (!m_remoteFinished) {
        m_remoteFinished = true;
        emit readChannelFinished();
    }
}

qint64 SshChannelDevice::writeData(const char* data, qint64 maxSize)
{
    if (!m_channel) {
        setErrorString(QStringLiteral("no SSH channel"));
        return -1;
    }
    if (maxSize <= 0)
        return 0;

    // The session is in blocking mode, so ssh_channel_write() normally consumes
    // the whole chunk; loop anyway so a short write is resumed rather than
    // silently truncating a JSON-RPC frame. Each ssh_channel_write() is bounded
    // by the session's SSH_OPTIONS_TIMEOUT (set at connect: kHandshakeTimeoutSeconds,
    // or the user's OpenSSH ConnectTimeout), so a wedged remote reader stalls
    // this call for at most that timeout rather than indefinitely (SH15).
    qint64 written = 0;
    while (written < maxSize) {
        const qint64 chunk = qMin<qint64>(maxSize - written, kChunkBytes);
        const int n = ssh_channel_write(m_channel, data + written,
                                        static_cast<uint32_t>(chunk));
        if (n == SSH_ERROR) {
            // -1 without an errorString() is what QIODevice consumers see as
            // "Unknown error"; failWith() records the libssh message so a
            // generic reader/writer can report the real cause.
            failWith(lastError());
            return -1;
        }
        if (n <= 0)
            break;  // no progress possible right now; report the short write
        written += n;
    }
    return written;
}

void SshChannelDevice::pump()
{
    if (!m_channel)
        return;
    // A slot reached from readyRead() may spin a nested event loop (QTRY_*,
    // QSignalSpy::wait); re-entering libssh from inside our own drain would
    // reorder the stream. The timer is single-shot, so a swallowed tick MUST be
    // re-armed or the read pump would be dead for good — silently, with the
    // channel still open.
    if (m_pumping) {
        m_pump->start(kIdlePollMs);
        return;
    }
    m_pumping = true;

    char chunk[kChunkBytes];
    QByteArray stderrBytes;
    QString failure;
    bool eof = false;
    // Append straight into the read buffer: staging payload in a second
    // QByteArray would copy every byte that crosses the channel.
    const qsizetype bufferedBefore = m_readBuffer.size();

    // Drain stdout and stderr separately. Everything below stays inside libssh
    // until the loops finish: no signal is emitted mid-drain, so a handler can
    // never re-enter ssh_channel_read_nonblocking() on this channel.
    for (int stream = 0; stream < 2 && failure.isEmpty(); ++stream) {
        const int isStderr = stream;
        for (;;) {
            const int n = ssh_channel_read_nonblocking(
                m_channel, chunk, static_cast<uint32_t>(sizeof(chunk)),
                isStderr);
            if (n == SSH_ERROR) {
                failure = lastError();
                break;
            }
            if (n == SSH_EOF) {
                eof = true;
                break;
            }
            if (n <= 0)
                break;  // nothing more buffered on this stream right now
            if (isStderr)
                stderrBytes.append(chunk, n);
            else
                m_readBuffer.append(chunk, n);
        }
    }

    if (!eof && failure.isEmpty() && ssh_channel_is_eof(m_channel) != 0)
        eof = true;

    const bool gotPayload = m_readBuffer.size() > bufferedBefore;
    m_pumping = false;

    // Re-arm before emitting so a handler that calls closeChannel() wins: its
    // m_pump->stop() then cancels the timer we just started.
    if (m_channel && !eof && failure.isEmpty())
        m_pump->start(gotPayload || !stderrBytes.isEmpty() ? 0 : kIdlePollMs);

    // Decode with the device's own stateful decoder, not QString::fromUtf8():
    // stderr is a byte stream cut at arbitrary 16 KiB boundaries, and a
    // multi-byte character split across two reads (or two pump passes) would
    // otherwise become replacement characters at both ends of the seam.
    if (!stderrBytes.isEmpty()) {
        const QString text = m_stderrDecoder.decode(stderrBytes);
        // A pass that carried nothing but the first half of one character has
        // nothing to report yet.
        if (!text.isEmpty())
            emit channelError(text);
    }
    if (!failure.isEmpty())
        failWith(failure);
    if (gotPayload)
        emit readyRead();

    // Remote EOF: report the end of the read channel exactly once, after the
    // final bytes have been surfaced. The device stays open so buffered data is
    // still readable; the owner decides when to closeChannel().
    if ((eof || !failure.isEmpty()) && !m_remoteFinished) {
        m_remoteFinished = true;
        emit readChannelFinished();
    }
}

#endif // CH_HAVE_LIBSSH

} // namespace ch
