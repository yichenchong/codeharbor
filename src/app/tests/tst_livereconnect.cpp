// LIVE gate for SessionBootstrap's reconnect ladder (SPEC 5.6): a real SSH
// session is really dropped, and the app really comes back.
//
// The unit gate (tst_sessionbootstrap) drives the state machine through test
// seams. This one drives nothing: it wires the production bootstrap against the
// fixture, kills the connection from the remote end, and then requires a fresh
// handshake, a fresh codeharbord, and a working RPC round-trip on the other
// side of the backoff — none of which the seam-driven test can prove.
//
// How the drop is made, and why it is safe next to other live tests: the kill
// runs on an Exec channel of OUR OWN session and targets that channel's parent,
// which is the per-connection `sshd-session: <user>@notty` process. It is
// therefore scoped to this test's TCP connection — never the shared fixture
// listener, never another test's session — and the target's command line is
// checked to contain "sshd" before any signal is sent. Killing remote
// codeharbord processes by name was rejected for exactly this reason: it would
// shoot down concurrent live tests.

#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "SessionBootstrap.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QtTest/QtTest>

#include <memory>
#include <optional>

using ch::AgentStatusMonitor;
using ch::CodeharbordClient;
using ch::RpcError;
using ch::SessionBootstrap;
using ch::SshChannelDevice;
using ch::SshConnectionPool;

using State = SessionBootstrap::State;

namespace {

// A cold node start that also type-strips the TypeScript entry point, twice
// (once per wire), plus the 1 s first backoff rung and a full re-handshake.
constexpr int kRpcTimeoutMs = 60000;
constexpr int kReconnectTimeoutMs = 90000;

// Kill this exec channel's parent — the per-connection sshd-session — so the
// whole SSH connection drops underneath libssh, exactly as a yanked network
// cable would. The `case` guard makes the signal unreachable unless the target
// really is an sshd process.
constexpr const char* kDropConnectionCommand =
    "pp=$(ps -o ppid= -p $$ | tr -d ' '); "
    "case \"$(ps -o args= -p $pp)\" in "
    "*sshd*) kill -9 \"$pp\";; "
    "*) echo \"refusing to kill non-sshd parent: $(ps -o args= -p $pp)\" >&2;; "
    "esac";

QString env(const char* key)
{
    return qEnvironmentVariable(key);
}

} // namespace

class TstLiveReconnect : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void survivesADroppedConnection();

private:
    bool serverInfoAnswers(QString* detail);
    void dropTheConnection();

    SshConnectionPool m_pool;
    CodeharbordClient m_client;
    AgentStatusMonitor m_monitor;
    std::unique_ptr<SessionBootstrap> m_bootstrap;
    QStringList m_bootstrapErrors;
    bool m_live = false;
};

void TstLiveReconnect::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH is not set; live reconnect gate skipped");
    if (!SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; live reconnect gate skipped");

    QVERIFY2(!env("CH_LIVE_HOST").isEmpty() && !env("CH_LIVE_PORT").isEmpty()
                 && !env("CH_LIVE_USER").isEmpty()
                 && !env("CH_LIVE_NODE").isEmpty()
                 && !env("CH_LIVE_REPO").isEmpty(),
             "CH_LIVE_HOST/PORT/USER/NODE/REPO must all be set");
    m_live = true;
}

void TstLiveReconnect::cleanupTestCase()
{
    if (!m_live)
        return;
    // Bootstrap first: the pool frees every channel it handed out, so the
    // devices must be gone before the session is.
    m_bootstrap.reset();
    m_pool.disconnectFromHost();
}

// One real RPC round-trip over whatever transport is currently wired.
bool TstLiveReconnect::serverInfoAnswers(QString* detail)
{
    bool done = false;
    QJsonValue result;
    std::optional<RpcError> failure;
    m_client.call(QStringLiteral("server.info"), QJsonValue(),
                  [&](QJsonValue value, std::optional<RpcError> error) {
                      result = value;
                      failure = error;
                      done = true;
                  });

    QDeadlineTimer deadline(kRpcTimeoutMs);
    while (!done && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    if (!done) {
        *detail = QStringLiteral("server.info never answered");
        return false;
    }
    if (failure) {
        *detail = failure->message;
        return false;
    }
    *detail = result.toObject().value(QStringLiteral("name")).toString();
    return *detail == QStringLiteral("codeharbord");
}

void TstLiveReconnect::dropTheConnection()
{
    // The killer rides on the same SSH connection it is about to kill; the
    // signal is delivered before sshd can tear the channel down.
    auto killer = std::make_unique<SshChannelDevice>(
        &m_pool, SshConnectionPool::ChannelKind::Exec);
    // channelError carries remote stderr AND libssh faults, and the killer is
    // riding the connection it kills: "Socket error: disconnected" on this
    // channel is the expected outcome, not a complaint. Only the guard's own
    // refusal message means the drop did not happen.
    QString killerStderr;
    connect(killer.get(), &SshChannelDevice::channelError, killer.get(),
            [&killerStderr](const QString& text) { killerStderr += text; });
    QVERIFY(killer->startExec(QString::fromLatin1(kDropConnectionCommand)));

    // Give the remote kill time to land and libssh time to notice, but stay
    // well inside the 1 s first backoff rung: the retry calls
    // SshConnectionPool::connectToHost(), which frees every channel on the old
    // session — including this killer's — so the device must be gone by then.
    QElapsedTimer since;
    since.start();
    while (m_bootstrap->state() == State::Wired && since.elapsed() < 500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    killer->closeChannel();
    killer.reset();

    QVERIFY2(!killerStderr.contains(QStringLiteral("refusing to kill")),
             qPrintable(QStringLiteral("kill helper refused: %1")
                            .arg(killerStderr)));
}

void TstLiveReconnect::survivesADroppedConnection()
{
    if (!m_live)
        QSKIP("live gate not armed");

    m_bootstrap =
        std::make_unique<SessionBootstrap>(&m_pool, &m_client, &m_monitor);
    connect(m_bootstrap.get(), &SessionBootstrap::error, this,
            [this](const QString& message) { m_bootstrapErrors.append(message); });

    QVERIFY2(m_bootstrap->connectAndWireFromEnvironment(),
             qPrintable(QStringLiteral("could not wire a live session: %1")
                            .arg(m_bootstrapErrors.join(QStringLiteral(" | ")))));
    QCOMPARE(m_bootstrap->state(), State::Wired);
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    QString detail;
    QVERIFY2(serverInfoAnswers(&detail), qPrintable(detail));

    SshChannelDevice* firstRpc = m_bootstrap->rpcDevice();
    QVERIFY(firstRpc != nullptr);

    QSignalSpy scheduleSpy(m_bootstrap.get(),
                           &SessionBootstrap::reconnectScheduled);
    QSignalSpy wiredSpy(m_bootstrap.get(), &SessionBootstrap::wired);

    // A call in flight across the drop must be answered exactly once, never
    // left hanging. Whether it is answered by the server (the response can win
    // the race with the kill) or failed by the transport teardown is timing;
    // "answered, and nothing left pending" is the guarantee. The synthetic
    // "transport closed with request pending" failure itself is pinned
    // deterministically in tst_sessionbootstrap.
    int orphanAnswers = 0;
    m_client.call(QStringLiteral("server.info"), QJsonValue(),
                  [&](QJsonValue, std::optional<RpcError>) { ++orphanAnswers; });

    dropTheConnection();

    // libssh reports the dead socket on the next poll; the bootstrap tears both
    // devices down and arms the first rung.
    QTRY_VERIFY_WITH_TIMEOUT(m_bootstrap->state() != State::Wired, 20000);
    QCOMPARE(m_bootstrap->state(), State::Reconnecting);
    QCOMPARE(scheduleSpy.size(), 1);
    QCOMPARE(scheduleSpy.at(0).at(0).toInt(), 1);
    QCOMPARE(scheduleSpy.at(0).at(1).toInt(), 1);  // 1 s, first rung
    QVERIFY(m_client.transport() == nullptr);
    QVERIFY(m_monitor.transport() == nullptr);

    // The in-flight call was answered, not left hanging.
    QCOMPARE(orphanAnswers, 1);
    QCOMPARE(m_client.pendingCount(), 0);

    // ... and the session comes back on its own: new handshake, new channels,
    // new remote codeharbord.
    QTRY_COMPARE_WITH_TIMEOUT(m_bootstrap->state(), State::Wired,
                              kReconnectTimeoutMs);
    QCOMPARE(wiredSpy.size(), 1);
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    QVERIFY(m_bootstrap->rpcDevice() != nullptr);
    QVERIFY(m_bootstrap->rpcDevice() != firstRpc);
    QCOMPARE(m_client.transport(),
             static_cast<QIODevice*>(m_bootstrap->rpcDevice()));
    QCOMPARE(m_monitor.transport(),
             static_cast<QIODevice*>(m_bootstrap->agentDevice()));
    QCOMPARE(m_bootstrap->reconnectAttempt(), 0);

    // The only claim that matters: RPC works again over the new transport.
    QVERIFY2(serverInfoAnswers(&detail), qPrintable(detail));
}

// Guiless: no display anywhere in this gate, and ch_app links Qt6::Gui.
QTEST_GUILESS_MAIN(TstLiveReconnect)
#include "tst_livereconnect.moc"
