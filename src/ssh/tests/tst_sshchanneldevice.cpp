#include "SshChannelDevice.h"
#include "SshConnectionPool.h"

#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QtTest/QtTest>

using ch::SshChannelDevice;
using ch::SshConnectionPool;

// The parts of SshChannelDevice that need no server: its QIODevice contract on
// a device with no channel behind it, and the way it reports a refused start.
// Everything that needs real remote bytes lives in the `live` gate
// (src/ssh/tests/tst_livessh.cpp); these cases are the ones a machine with no
// sshd can still hold to account, and they are exactly the paths a caller hits
// when the SSH connection is down — which is most of what goes wrong in the
// field.
class TstSshChannelDevice : public QObject {
    Q_OBJECT
private slots:
    void openIsRefusedBecauseAChannelIsWhatOpensThisDevice();
    void startingWithoutAConnectionPoolExplainsItself();
    void startingOnADisconnectedPoolExplainsItself();
    void closeChannelIsIdempotentAndReportsTheEndExactlyOnce();
    void resizeIsRefusedWhenThereIsNoPty();
};

// QIODevice::open() is the obvious thing for a caller to reach for, and on this
// device it is always wrong: the channel is what makes it readable. Answering
// true would hand back a device that is open, never delivers a byte and fails
// every write — a hang with no error anywhere.
void TstSshChannelDevice::openIsRefusedBecauseAChannelIsWhatOpensThisDevice()
{
    SshChannelDevice device(nullptr, SshConnectionPool::ChannelKind::Exec);

    QVERIFY(!device.open(QIODevice::ReadWrite));
    QVERIFY(!device.isOpen());
    QVERIFY(!device.errorString().isEmpty());
    QVERIFY(device.isSequential());
    QCOMPARE(device.bytesAvailable(), qint64(0));
}

void TstSshChannelDevice::startingWithoutAConnectionPoolExplainsItself()
{
    SshChannelDevice device(nullptr, SshConnectionPool::ChannelKind::Exec);
    QStringList errors;
    connect(&device, &SshChannelDevice::channelError, &device,
            [&errors](const QString& text) { errors << text; });

    QVERIFY(!device.startExec(QStringLiteral("echo hi")));
    QVERIFY(!device.isOpen());
    QCOMPARE(errors.size(), 1);
    // The same text reaches BOTH surfaces: the channelError() signal that
    // SessionBootstrap and TerminalFactory listen on, and
    // QIODevice::errorString(), which is all a generic byte-stream consumer can
    // see. Before, errorString() stayed "Unknown error" on every failure.
    QCOMPARE(device.errorString(), errors.at(0));
#if CH_HAVE_LIBSSH
    QCOMPARE(errors.at(0), QStringLiteral("no SSH connection pool"));
#endif
}

void TstSshChannelDevice::startingOnADisconnectedPoolExplainsItself()
{
    // A pool that never connected hands out no channels, which is the state
    // every start request lands in after an SSH drop.
    SshConnectionPool pool;
    SshChannelDevice device(&pool, SshConnectionPool::ChannelKind::Pty);
    QStringList errors;
    connect(&device, &SshChannelDevice::channelError, &device,
            [&errors](const QString& text) { errors << text; });

    QVERIFY(!device.startPty(QStringLiteral("xterm-256color"), 80, 24));
    QVERIFY(!device.isOpen());
    QCOMPARE(errors.size(), 1);
    QCOMPARE(device.errorString(), errors.at(0));
    // No PTY was ever negotiated, so a window-change request has nothing to
    // apply to and must say so rather than pretending to have resized.
    QVERIFY(!device.resizePty(100, 30));
}

// closeChannel() is called from several places for the same device — the owner
// tearing a pane down, the pool announcing that the session is going away, and
// the destructor. Every one of them must be safe, and the end of the read
// channel must be reported once: CodeharbordClient turns that signal into
// "every pending request failed", and a second one would fail an already
// failed set of requests all over again.
void TstSshChannelDevice::closeChannelIsIdempotentAndReportsTheEndExactlyOnce()
{
    SshChannelDevice device(nullptr, SshConnectionPool::ChannelKind::Rpc);
    QSignalSpy finished(&device, &SshChannelDevice::readChannelFinished);

    device.closeChannel();
    QCOMPARE(finished.size(), 1);
    QVERIFY(!device.isOpen());
    QCOMPARE(device.bytesAvailable(), qint64(0));

    device.closeChannel();
    device.close();  // the QIODevice spelling routes to the same place
    QCOMPARE(finished.size(), 1);
}

void TstSshChannelDevice::resizeIsRefusedWhenThereIsNoPty()
{
    SshChannelDevice exec(nullptr, SshConnectionPool::ChannelKind::Exec);
    QVERIFY(!exec.resizePty(80, 24));
    // Degenerate geometry is refused for the same reason and not by accident:
    // there is no channel, so there is nothing to resize either way.
    QVERIFY(!exec.resizePty(0, 0));
}

QTEST_GUILESS_MAIN(TstSshChannelDevice)
#include "tst_sshchanneldevice.moc"
