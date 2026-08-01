#pragma once

#include "SshConnectionPool.h"

#include <QByteArray>
#include <QIODevice>
#include <QPointer>
#include <QString>
#include <QStringConverter>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace ch {

// A QIODevice over a single SSH channel opened on SshConnectionPool's shared
// session (SPEC 5.3). This is the missing production link between the pool and
// the byte-stream consumers that already speak QIODevice: CodeharbordClient
// (newline-delimited JSON-RPC, SPEC 10.3), AgentStatusMonitor (AgentEvent JSONL,
// SPEC 6.4) and the terminal PTY pump.
//
// Read pump: a per-device QTimer, NOT a QSocketNotifier. Two reasons, both
// structural rather than stylistic:
//   1. The pool multiplexes every channel onto ONE ssh_session, hence one
//      socket fd. Qt forbids two QSocketNotifiers of the same type on the same
//      descriptor ("Multiple socket notifiers for same socket"), so a
//      per-device notifier collides the moment a second channel is opened.
//   2. libssh buffers decrypted channel payload inside the session. After a
//      packet is consumed the fd can be level-idle while channel bytes are
//      still pending, so fd readiness alone under-reports available data.
// The timer is adaptive: it re-arms at 0 ms while the previous pass produced
// bytes (a burst is drained back to back) and at kIdlePollMs (10 ms) when the
// channel is quiet, so it never spins hot and never exceeds the 10 ms cadence.
//
// Ownership: the pool owns the ssh_channel (see SshConnectionPool::openChannel).
// closeChannel() hands it back through SshConnectionPool::releaseChannel()
// instead of freeing it itself: that reclaims the channel's slot on the SSH
// connection right away (a server caps concurrent channels per connection) and
// keeps the pool's channel list accurate. A device also drops its handle when
// the pool announces sessionClosing(), so a device that outlives its session
// never polls freed libssh memory.
//
// Without libssh (CH_HAVE_LIBSSH=0) the class still compiles and constructs;
// every start*/resize call returns false and the device never opens.
class SshChannelDevice : public QIODevice {
    Q_OBJECT
public:
    explicit SshChannelDevice(SshConnectionPool* pool,
                              SshConnectionPool::ChannelKind kind,
                              QObject* parent = nullptr);
    ~SshChannelDevice() override;

    // Open a channel of the configured kind and run `command` on it. On success
    // the device is open ReadWrite|Unbuffered and the read pump is running.
    // Returns false (and emits channelError) if the channel or exec request
    // fails, or if a channel is already started.
    bool startExec(const QString& command);

    // Open a channel with a PTY of `cols` x `rows` and start either `command`
    // (exec) or the login shell (when `command` is empty). `term` is the
    // terminal type sent to the remote session (empty means xterm-256color) and
    // is honoured for every ChannelKind: the single pty-req a channel may carry
    // is issued here, not by the pool.
    bool startPty(const QString& term, int cols, int rows,
                  const QString& command = QString());

    // Push a new window size to the remote PTY. False if no PTY channel is live.
    bool resizePty(int cols, int rows);

    // Close the channel and the device. Idempotent. Never ssh_channel_free()s
    // by hand: the channel is handed back to the pool, which frees it and
    // reclaims its slot on the connection.
    // Emits readChannelFinished() once if it has not already fired.
    void closeChannel();

    // QIODevice
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;
    void close() override;
    // Always false. A channel is what makes this device readable, so the only
    // way in is startExec()/startPty(); QIODevice::open() would otherwise hand
    // back an "open" device with no channel behind it, on which every read
    // returns nothing and every write fails.
    bool open(OpenMode mode) override;

signals:
    // Channel-level diagnostics: stderr of an Exec/Rpc channel plus libssh
    // failures. Kept OUT of the read stream on purpose — folding stderr into
    // readData() would splice non-JSON bytes into the JSON-RPC/JSONL framing.
    void channelError(const QString& message);

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char* data, qint64 maxSize) override;

private:
    // Poll cadence when the channel produced nothing on the previous pass.
    static constexpr int kIdlePollMs = 10;

    bool acquireChannel();
    bool beginStreaming();
    void abortStart(const QString& reason);
    // Report a device-level failure: records it as QIODevice::errorString() —
    // which is what a generic QIODevice consumer inspects — and emits
    // channelError() for the ones that listen for it. Not used for remote
    // stderr: that is the remote program talking, not a failure of the device.
    void failWith(const QString& reason);
    void pump();
    QString lastError() const;

    QPointer<SshConnectionPool> m_pool;
    SshConnectionPool::ChannelKind m_kind = SshConnectionPool::ChannelKind::Exec;
    QTimer* m_pump = nullptr;
    QByteArray m_readBuffer;
    bool m_remoteFinished = false;
    // A window-change request is only meaningful on a channel that really has a
    // PTY, so resizePty() can honour its documented "false if no PTY" contract.
    bool m_hasPty = false;
    bool m_pumping = false;
    // Remote stderr arrives in 16 KiB reads spread over as many pump passes as
    // the writer needs, so a multi-byte UTF-8 character can straddle two of
    // them. A stateful decoder holds the incomplete sequence over until the
    // rest arrives instead of turning it into replacement characters.
    QStringDecoder m_stderrDecoder{QStringDecoder::Utf8};
#if CH_HAVE_LIBSSH
    ssh_channel m_channel = nullptr;
#endif
};

} // namespace ch
