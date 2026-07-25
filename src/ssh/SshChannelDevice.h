#pragma once

#include "SshConnectionPool.h"

#include <QByteArray>
#include <QIODevice>
#include <QString>

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
// Ownership: the pool retains ownership of the ssh_channel (see
// SshConnectionPool::openChannel). closeChannel() closes but NEVER frees it.
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
    // (exec) or the login shell (when `command` is empty). When the device was
    // constructed with ChannelKind::Pty the pool has already negotiated the PTY,
    // so only the window size is applied and `term` is ignored; construct with
    // another kind to choose the TERM value here.
    bool startPty(const QString& term, int cols, int rows,
                  const QString& command = QString());

    // Push a new window size to the remote PTY. False if no PTY channel is live.
    bool resizePty(int cols, int rows);

    // Close the channel and the device. Idempotent. Does NOT ssh_channel_free():
    // the pool frees every channel it handed out when the session goes down.
    // Emits readChannelFinished() once if it has not already fired.
    void closeChannel();

    // QIODevice
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;
    void close() override;

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
    void pump();
    QString lastError() const;

    SshConnectionPool* m_pool = nullptr;
    SshConnectionPool::ChannelKind m_kind = SshConnectionPool::ChannelKind::Exec;
    QTimer* m_pump = nullptr;
    QByteArray m_readBuffer;
    bool m_remoteFinished = false;
    bool m_pumping = false;
#if CH_HAVE_LIBSSH
    ssh_channel m_channel = nullptr;
#endif
};

} // namespace ch
