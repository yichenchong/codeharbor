#include "SshChannelDevice.h"

#include <QTimer>

#include <cstring>

namespace ch {

namespace {

// Drain granularity per libssh read. 16 KiB comfortably exceeds one SSH channel
// data packet, so a burst is normally emptied in a couple of iterations.
constexpr int kChunkBytes = 16 * 1024;
// Yield after a bounded amount even when a remote producer never goes quiet.
// Without this cap, a `yes`-style command can keep the nonblocking read loop
// inside one timer callback forever and starve the Qt event loop.
constexpr qsizetype kMaxPumpBytes = 256 * 1024;
constexpr qsizetype kReadCompactionThreshold = 64 * 1024;
// Let the SSH channel's own window apply backpressure instead of retaining an
// unbounded remote flood in this process when a consumer stops reading.
constexpr qsizetype kMaxReadBufferBytes = 8 * 1024 * 1024;


} // namespace

SshChannelDevice::SshChannelDevice(SshConnectionPool* pool, QObject* parent)
    : QIODevice(parent), m_pool(pool), m_pump(new QTimer(this))
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
    return QIODevice::bytesAvailable()
           + (m_readBuffer.size() - m_readOffset);
}

bool SshChannelDevice::canReadLine() const
{
    // QIODevice's own buffer stays empty because this device is Unbuffered;
    // line framing lives in m_readBuffer instead.
    return m_readBuffer.indexOf('\n', m_readOffset) >= 0
           || QIODevice::canReadLine();
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
    const qsizetype available = m_readBuffer.size() - m_readOffset;
    const qint64 count = qMin<qint64>(maxSize, available);
    if (count <= 0) {
        // QIODevice's contract distinguishes the two empty answers, and a
        // generic consumer relies on it: 0 means "nothing buffered right now,
        // ask again after readyRead()", -1 means "this stream is finished". A
        // finished channel that keeps answering 0 reads as a permanently idle
        // one, so QIODevice::atEnd()/readAll()/waitForReadyRead() loops written
        // against the plain interface wait for bytes that can never come.
        return m_remoteFinished ? -1 : 0;
    }
    std::memcpy(data, m_readBuffer.constData() + m_readOffset,
                static_cast<size_t>(count));
    m_readOffset += count;
    if (m_readOffset == m_readBuffer.size()) {
        m_readBuffer.clear();
        m_readOffset = 0;
    } else if (m_readOffset >= kReadCompactionThreshold
               && m_readOffset * 2 >= m_readBuffer.size()) {
        m_readBuffer.remove(0, m_readOffset);
        m_readOffset = 0;
    }
    return count;
}

void SshChannelDevice::finishReadChannel()
{
    if (m_remoteFinished)
        return;
    m_remoteFinished = true;
    emit readChannelFinished();
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
        ++m_channelGeneration;
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
    m_readOffset = 0;
    if (isOpen())
        QIODevice::close();
    // Same contract as the libssh build: the end of the read channel is
    // reported exactly once. A consumer that keys its teardown on this signal
    // (CodeharbordClient fails every pending call from it) must not behave
    // differently just because the client was built without libssh.
    finishReadChannel();
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
    m_channel = m_pool->openChannel();
    if (!m_channel) {
        failWith(QStringLiteral("could not open SSH channel"));
        return false;
    }
    ++m_channelGeneration;
    return true;
}

bool SshChannelDevice::beginStreaming()
{
    m_readBuffer.clear();
    m_readOffset = 0;
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

    // The channel's one pty-req, issued here: the pool never negotiates a PTY
    // of its own, so the terminal type asked for is the one the remote session
    // actually gets. It used to be sent by the pool as
    // ssh_channel_request_pty(), which hard-codes TERM=xterm — a pane asking for
    // xterm-256color then silently ran a 16-colour terminal, and only the window
    // size could still be applied.
    const QByteArray termUtf8 =
        term.isEmpty() ? QByteArrayLiteral("xterm-256color") : term.toUtf8();
    if (ssh_channel_request_pty_size(m_channel, termUtf8.constData(), cols, rows)
        != SSH_OK) {
        abortStart(lastError());
        return false;
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
    if (!m_channel || !m_hasPty || m_remoteFinished)
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
        ++m_channelGeneration;
    }
    m_hasPty = false;

    // Drop anything still buffered: bytesAvailable() must not advertise
    // readable bytes on a device that has been closed.
    m_readBuffer.clear();
    m_readOffset = 0;
    if (isOpen())
        QIODevice::close();

    finishReadChannel();
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
            // generic reader/writer can report the real cause. Preserve a
            // prefix that was already accepted: returning -1 after a partial
            // write invites a caller to retry the entire frame and duplicate
            // those bytes on the remote side.
            failWith(lastError());
            return written > 0 ? written : -1;
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
    const ssh_channel channel = m_channel;
    const quint64 generation = m_channelGeneration;
    // A slot reached from readyRead() may spin a nested event loop (QTRY_*,
    // QSignalSpy::wait); re-entering libssh from inside our own drain would
    // reorder the stream. The timer is single-shot, so a swallowed tick MUST
    // be re-armed or the read pump would be dead for good — silently, with the
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
    const qsizetype bufferedBefore =
        m_readBuffer.size() - m_readOffset;

    // Drain stdout and stderr separately. Everything below stays inside libssh
    // until the loops finish: no signal is emitted mid-drain, so a handler can
    // never re-enter ssh_channel_read_nonblocking() on this channel.
    for (int stream = 0; stream < 2 && failure.isEmpty(); ++stream) {
        const int isStderr = stream;
        qsizetype streamBytes = 0;
        while (streamBytes < kMaxPumpBytes) {
            qsizetype remaining = kMaxPumpBytes - streamBytes;
            if (!isStderr) {
                remaining = qMin<qsizetype>(
                    remaining,
                    kMaxReadBufferBytes
                        - (m_readBuffer.size() - m_readOffset));
            }
            if (remaining <= 0)
                break;
            const uint32_t request = static_cast<uint32_t>(
                qMin<qsizetype>(sizeof(chunk), remaining));
            const int n =
                ssh_channel_read_nonblocking(channel, chunk, request, isStderr);
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
            streamBytes += n;
            if (isStderr)
                stderrBytes.append(chunk, n);
            else
                m_readBuffer.append(chunk, n);
        }
    }

    if (!eof && failure.isEmpty() && ssh_channel_is_eof(channel) != 0)
        eof = true;

    const bool gotPayload =
        m_readBuffer.size() - m_readOffset > bufferedBefore;
    m_pumping = false;

    // Re-arm before emitting so a handler that calls closeChannel() wins: its
    // m_pump->stop() then cancels the timer we just started. The generation
    // check also prevents an old pass from arming a newly started channel.
    if (m_channel == channel && m_channelGeneration == generation && !eof
        && failure.isEmpty()) {
        m_pump->start(gotPayload || !stderrBytes.isEmpty() ? 0 : kIdlePollMs);
    }

    // Every emit below can reach a handler that destroys this device: the
    // documented teardown path runs from inside readChannelFinished(), and a
    // stray `delete` there (rather than deleteLater()) would leave the rest of
    // this function writing to freed memory. Watch for it instead of trusting
    // every present and future consumer to get that right.
    const QPointer<SshChannelDevice> self(this);

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
    // A channel-error handler may close this channel, destroy the device, or
    // immediately start a replacement channel. Do not publish old bytes or
    // finish the replacement in any of those cases.
    if (self && m_channel == channel && m_channelGeneration == generation
        && !failure.isEmpty()) {
        failWith(failure);
    }
    if (self && m_channel == channel && m_channelGeneration == generation
        && gotPayload) {
        emit readyRead();
    }

    // Remote EOF: report the end of the read channel exactly once, after the
    // final bytes have been surfaced. The device stays open so buffered data
    // is still readable; the owner decides when to closeChannel().
    if (self && m_channel == channel && m_channelGeneration == generation
        && (eof || !failure.isEmpty())) {
        finishReadChannel();
    }
}

#endif // CH_HAVE_LIBSSH

} // namespace ch
