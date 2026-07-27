#include "CodeharbordClient.h"
#include "KnownHosts.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QtTest/QtTest>

#include <optional>

using ch::CodeharbordClient;
using ch::KnownHosts;
using ch::RpcError;
using ch::SshChannelDevice;
using ch::SshConnectionPool;

namespace {

// Generous: a real TCP connect + SSH handshake + remote fork, and for the RPC
// case a cold node start that also type-strips the TypeScript entry point.
constexpr int kExecTimeoutMs = 20000;
constexpr int kRpcTimeoutMs = 60000;

QString env(const char* key)
{
    return qEnvironmentVariable(key);
}

// Accumulates one channel's observable surface so assertions can be made on the
// read stream and the out-of-band error stream independently.
struct ChannelSink {
    QByteArray out;
    QString err;
    bool finished = false;

    void attach(SshChannelDevice* device)
    {
        QObject::connect(device, &SshChannelDevice::readyRead, device,
                         [this, device]() { out += device->readAll(); });
        QObject::connect(device, &SshChannelDevice::channelError, device,
                         [this](const QString& text) { err += text; });
        QObject::connect(device, &SshChannelDevice::readChannelFinished, device,
                         [this]() { finished = true; });
    }
};

} // namespace

// LIVE gate for the SSH spine (SPEC 5.3, 10.1). Everything here talks to a real
// sshd over a real socket: no mocks, no loopback QIODevice stand-ins. Skipped
// wholesale unless CH_LIVE_SSH is set, so the default suite stays green on a
// machine with no fixture.
class TstLiveSsh : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void connectsToFixture();
    void execChannelDeliversStdout();
    void rpcServerInfoOverSshChannel();
    void stderrStaysOutOfReadStream();

private:
    void ensureConnected();

    SshConnectionPool m_pool;
    QString m_host;
    quint16 m_port = 0;
    QString m_user;
    QString m_node;
    QString m_repo;
    QString m_knownHostsPath;
};

void TstLiveSsh::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH is not set; live SSH gate skipped");
    if (!SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; live SSH gate skipped");

    m_host = env("CH_LIVE_HOST");
    m_port = static_cast<quint16>(env("CH_LIVE_PORT").toUInt());
    m_user = env("CH_LIVE_USER");
    m_node = env("CH_LIVE_NODE");
    m_repo = env("CH_LIVE_REPO");
    m_knownHostsPath = env("CH_LIVE_KNOWN_HOSTS");
    if (m_knownHostsPath.isEmpty()) {
        m_knownHostsPath =
            QDir::temp().filePath(QStringLiteral("ch_live_known_hosts"));
    }

    QVERIFY2(!m_host.isEmpty() && m_port != 0 && !m_user.isEmpty()
                 && !m_node.isEmpty() && !m_repo.isEmpty(),
             "CH_LIVE_HOST/PORT/USER/NODE/REPO must all be set");
}

void TstLiveSsh::cleanupTestCase()
{
    m_pool.disconnectFromHost();
}

void TstLiveSsh::ensureConnected()
{
    if (m_pool.state() == SshConnectionPool::State::Connected)
        return;

    // First-use trust, exactly like SessionBootstrap: load the store, accept an
    // unknown key once, write it back. A Mismatch never reaches the callback.
    KnownHosts hosts;
    QFile store(m_knownHostsPath);
    if (store.open(QIODevice::ReadOnly | QIODevice::Text))
        hosts = KnownHosts::parse(QString::fromUtf8(store.readAll()));
    store.close();
    m_pool.setKnownHosts(hosts);
    m_pool.setHostKeyCallback([](const QString&, const QString&,
                                 const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });

    QString failure;
    const auto conn = QObject::connect(
        &m_pool, &SshConnectionPool::errorOccurred,
        [&failure](const QString& text) { failure += text; });

    // Authentication comes from the ssh-agent named by SSH_AUTH_SOCK.
    const bool ok = m_pool.connectToHost(m_host, m_port, m_user);
    QObject::disconnect(conn);
    QVERIFY2(ok, qPrintable(QStringLiteral("connectToHost(%1:%2) failed: %3")
                                .arg(m_host)
                                .arg(m_port)
                                .arg(failure)));

    QDir().mkpath(QFileInfo(m_knownHostsPath).absolutePath());
    QFile out(m_knownHostsPath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        out.write(m_pool.knownHosts().serialize());
}

// (a) A real authenticated session against the fixture sshd.
void TstLiveSsh::connectsToFixture()
{
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
}

// (b) An Exec channel is a working QIODevice: the remote stdout bytes arrive
// verbatim through readyRead()/readAll().
void TstLiveSsh::execChannelDeliversStdout()
{
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    SshChannelDevice device(&m_pool, SshConnectionPool::ChannelKind::Exec);
    ChannelSink sink;
    sink.attach(&device);

    QVERIFY(device.startExec(QStringLiteral("echo LIVE_EXEC_OK")));
    QVERIFY(device.isOpen());
    QVERIFY(device.isSequential());

    QTRY_VERIFY_WITH_TIMEOUT(sink.finished, kExecTimeoutMs);
    QCOMPARE(sink.out, QByteArray("LIVE_EXEC_OK\n"));
    device.closeChannel();
}

// (c) The end-to-end spine: real codeharbord, over a real SSH channel, driven
// by the production CodeharbordClient. This is the path that never worked
// before SshChannelDevice existed.
void TstLiveSsh::rpcServerInfoOverSshChannel()
{
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    const QString command = QStringLiteral("'%1' '%2/remote/src/codeharbord.ts' rpc --stdio")
                                .arg(m_node, m_repo);

    SshChannelDevice device(&m_pool, SshConnectionPool::ChannelKind::Rpc);
    QString stderrText;
    QObject::connect(&device, &SshChannelDevice::channelError, &device,
                     [&stderrText](const QString& text) { stderrText += text; });
    QVERIFY2(device.startExec(command), qPrintable(command));
    QVERIFY(device.isWritable());

    CodeharbordClient client;
    client.setTransport(&device);

    QJsonValue result;
    std::optional<RpcError> rpcError;
    bool answered = false;
    client.call(QStringLiteral("server.info"), QJsonValue(),
                [&](QJsonValue value, std::optional<RpcError> err) {
                    result = value;
                    rpcError = err;
                    answered = true;
                });

    QTRY_VERIFY_WITH_TIMEOUT(answered, kRpcTimeoutMs);
    QVERIFY2(!rpcError.has_value(),
             qPrintable(rpcError ? rpcError->message + QStringLiteral(" | stderr: ")
                                       + stderrText
                                 : QString()));

    const QJsonObject info = result.toObject();
    QCOMPARE(info.value(QStringLiteral("name")).toString(),
             QStringLiteral("codeharbord"));
    QVERIFY(info.contains(QStringLiteral("schemaVersion")));
    QVERIFY(info.value(QStringLiteral("schemaVersion")).toInt(-1) >= 1);

    client.setTransport(nullptr);
    device.closeChannel();
}

// (d) stderr must never be spliced into the read stream: a single stray
// non-JSON line would desynchronise the JSON-RPC/JSONL framing on that channel.
void TstLiveSsh::stderrStaysOutOfReadStream()
{
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    // Exec (not Pty): a PTY would merge stderr into stdout at the kernel level
    // and the separation could not be observed at all.
    SshChannelDevice device(&m_pool, SshConnectionPool::ChannelKind::Exec);
    ChannelSink sink;
    sink.attach(&device);

    QVERIFY(device.startExec(
        QStringLiteral("sh -c 'echo OUT; echo ERR 1>&2'")));

    QTRY_VERIFY_WITH_TIMEOUT(sink.finished, kExecTimeoutMs);

    QCOMPARE(sink.out, QByteArray("OUT\n"));
    QVERIFY(!sink.out.contains("ERR"));
    QCOMPARE(sink.err.trimmed(), QStringLiteral("ERR"));
    device.closeChannel();
}

QTEST_GUILESS_MAIN(TstLiveSsh)
#include "tst_livessh.moc"
