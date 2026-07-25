// SessionBootstrap's connection-state machine and reconnect ladder (SPEC 5.6).
//
// The point of this gate is the path no live test can drive on demand: the
// remote session DYING. A live fixture can be wired and torn down, but it
// cannot be made to drop a channel at a chosen instant, ten times, on a
// millisecond budget. So the two side-effecting steps of a wire attempt —
// connectPool() and openChannelDevice() — are overridden here, and everything
// above them (state transitions, teardown ordering, backoff scheduling, the
// attempt cap) is the production code under test.
//
// The fake channel is a real SshChannelDevice subclass, not a QBuffer: the
// device type is part of SessionBootstrap's API (rpcDevice()) and its
// closeChannel() -> readChannelFinished() behaviour is exactly the mechanism
// that makes CodeharbordClient fail its pending calls.

#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "SessionBootstrap.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"

#include <QList>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <optional>

using ch::AgentStatusMonitor;
using ch::CodeharbordClient;
using ch::RpcError;
using ch::SessionBootstrap;
using ch::SshChannelDevice;
using ch::SshConnectionPool;

using State = SessionBootstrap::State;

namespace {

// 1 s of backoff becomes 1 ms, so the full ten-attempt ladder
// (1+2+5+10+30+60*5 = 348 s) runs in 348 ms. The SCHEDULE is asserted in
// unscaled seconds through reconnectScheduled(), so the compression never
// weakens what is checked.
constexpr double kTimeScale = 0.001;

// An SshChannelDevice with no SSH under it: open, writable, and droppable on
// command. writeData() must succeed or CodeharbordClient::call() refuses to
// register a pending callback, and the pending-call teardown is one of the
// things being proven here.
class FakeChannel : public SshChannelDevice {
public:
    explicit FakeChannel(SshConnectionPool::ChannelKind kind, QObject* parent)
        : SshChannelDevice(nullptr, kind, parent)
    {
        QIODevice::open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    }

    // What a dead remote looks like to the device: EOF on the read channel,
    // emitted exactly as SshChannelDevice::pump() emits it.
    void dropRemote() { emit readChannelFinished(); }

    QByteArray written;

protected:
    qint64 writeData(const char* data, qint64 maxSize) override
    {
        written.append(data, maxSize);
        return maxSize;
    }
    qint64 readData(char*, qint64) override { return 0; }
};

class TestBootstrap : public SessionBootstrap {
public:
    using SessionBootstrap::SessionBootstrap;

    bool connectOk = true;
    bool channelsOk = true;
    int connectCalls = 0;
    QList<QPointer<FakeChannel>> channels;

    FakeChannel* rpcChannel() const
    {
        return static_cast<FakeChannel*>(rpcDevice());
    }

protected:
    bool connectPool(const QString&, quint16, const QString&) override
    {
        ++connectCalls;
        return connectOk;
    }

    SshChannelDevice* openChannelDevice(SshConnectionPool::ChannelKind kind,
                                        const QString&, const QString&) override
    {
        if (!channelsOk)
            return nullptr;
        auto* channel = new FakeChannel(kind, this);
        channels.append(channel);
        return channel;
    }
};

// One self-contained set of collaborators per test case.
struct Harness {
    QTemporaryDir dir;
    SshConnectionPool pool;
    CodeharbordClient client;
    AgentStatusMonitor monitor;
    TestBootstrap boot{&pool, &client, &monitor};

    Harness()
    {
        boot.setKnownHostsPath(dir.filePath(QStringLiteral("known_hosts")));
        boot.setReconnectTimeScale(kTimeScale);
    }

    bool wire()
    {
        return boot.connectAndWire(QStringLiteral("example.invalid"), 2222,
                                   QStringLiteral("user"),
                                   QStringLiteral("/usr/bin/node"),
                                   QStringLiteral("/srv/codeharbor"));
    }
};

QList<State> states(const QSignalSpy& spy)
{
    QList<State> out;
    out.reserve(spy.size());
    for (const QList<QVariant>& call : spy)
        out.append(call.at(0).value<State>());
    return out;
}

} // namespace

class TstSessionBootstrap : public QObject {
    Q_OBJECT
private slots:
    void wiresAndReportsState();
    void initialConnectFailureDoesNotRetry();
    void channelLossReconnectsAndRewires();
    void agentChannelLossAlsoReconnects();
    void pendingCallsFailWhenTheSessionDies();
    void poolLossReconnects();
    void backoffLadderMatchesSpec();
    void retryDelaySequenceAndCap();
    void reconnectDisabledNeverSchedules();
    void disablingMidLadderStopsIt();
    void userDisconnectDoesNotReconnect();
    void uncappedLadderKeepsRetryingAtSixty();
};

// A cold wire walks Disconnected -> Connecting -> Wired exactly once and hands
// both transports over.
void TstSessionBootstrap::wiresAndReportsState()
{
    Harness h;
    QCOMPARE(h.boot.state(), State::Disconnected);
    QSignalSpy stateSpy(&h.boot, &SessionBootstrap::stateChanged);
    QSignalSpy wiredSpy(&h.boot, &SessionBootstrap::wired);

    QVERIFY(h.wire());

    QCOMPARE(states(stateSpy), (QList<State>{State::Connecting, State::Wired}));
    QCOMPARE(wiredSpy.size(), 1);
    QCOMPARE(h.boot.state(), State::Wired);
    QCOMPARE(h.boot.reconnectAttempt(), 0);
    QVERIFY(!h.boot.reconnectPending());
    QVERIFY(h.boot.rpcDevice() != nullptr);
    QVERIFY(h.boot.agentDevice() != nullptr);
    QCOMPARE(h.client.transport(),
             static_cast<QIODevice*>(h.boot.rpcDevice()));
    QCOMPARE(h.monitor.transport(),
             static_cast<QIODevice*>(h.boot.agentDevice()));
}

// A connect the user asked for and that never came up is reported to the
// caller, not retried behind its back: there is no session to survive yet.
void TstSessionBootstrap::initialConnectFailureDoesNotRetry()
{
    Harness h;
    h.boot.connectOk = false;
    QSignalSpy stateSpy(&h.boot, &SessionBootstrap::stateChanged);
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QVERIFY(!h.wire());

    QCOMPARE(states(stateSpy), (QList<State>{State::Connecting, State::Failed}));
    QCOMPARE(errorSpy.size(), 1);
    QVERIFY(errorSpy.at(0).at(0).toString().contains(QStringLiteral("failed")));
    QVERIFY(!h.boot.reconnectPending());
    QCOMPARE(h.boot.connectCalls, 1);

    // And it stays down: no timer was armed to change its mind.
    QTest::qWait(50);
    QCOMPARE(h.boot.state(), State::Failed);
    QCOMPARE(h.boot.connectCalls, 1);
}

// The load-bearing case: the RPC channel dies, both devices are dropped, a
// retry is armed on the first rung of the ladder, and the session comes back.
void TstSessionBootstrap::channelLossReconnectsAndRewires()
{
    Harness h;
    QVERIFY(h.wire());
    FakeChannel* dead = h.boot.rpcChannel();
    QPointer<FakeChannel> deadAgent =
        static_cast<FakeChannel*>(h.boot.agentDevice());

    QSignalSpy stateSpy(&h.boot, &SessionBootstrap::stateChanged);
    QSignalSpy wiredSpy(&h.boot, &SessionBootstrap::wired);
    QSignalSpy scheduleSpy(&h.boot, &SessionBootstrap::reconnectScheduled);

    dead->dropRemote();

    // Torn down at once, both sides, and armed for the first retry.
    QCOMPARE(h.boot.state(), State::Reconnecting);
    QVERIFY(h.client.transport() == nullptr);
    QVERIFY(h.monitor.transport() == nullptr);
    QVERIFY(h.boot.reconnectPending());
    QCOMPARE(h.boot.nextReconnectDelaySeconds(), 1);
    QCOMPARE(scheduleSpy.size(), 1);
    QCOMPARE(scheduleSpy.at(0).at(0).toInt(), 1);  // attempt 1
    QCOMPARE(scheduleSpy.at(0).at(1).toInt(), 1);  // after 1 s

    QTRY_COMPARE(h.boot.state(), State::Wired);
    QCOMPARE(states(stateSpy),
             (QList<State>{State::Reconnecting, State::Wired}));
    QCOMPARE(wiredSpy.size(), 1);
    QCOMPARE(h.boot.connectCalls, 2);
    QCOMPARE(h.boot.reconnectAttempt(), 0);
    QVERIFY(!h.boot.reconnectPending());

    // Fresh channels, and the consumers hold the new ones.
    QVERIFY(h.boot.rpcDevice() != static_cast<SshChannelDevice*>(dead));
    QCOMPARE(h.client.transport(),
             static_cast<QIODevice*>(h.boot.rpcDevice()));
    QCOMPARE(h.monitor.transport(),
             static_cast<QIODevice*>(h.boot.agentDevice()));

    // The dropped devices are deleted, not leaked onto the parent.
    QTRY_VERIFY(deadAgent.isNull());
}

// The agent-status channel is the other half of the session; losing it means
// the remote side is gone too.
void TstSessionBootstrap::agentChannelLossAlsoReconnects()
{
    Harness h;
    QVERIFY(h.wire());
    auto* agent = static_cast<FakeChannel*>(h.boot.agentDevice());

    agent->dropRemote();

    QCOMPARE(h.boot.state(), State::Reconnecting);
    QTRY_COMPARE(h.boot.state(), State::Wired);
    QCOMPARE(h.boot.connectCalls, 2);
}

// CodeharbordClient fails its pending callbacks from onTransportClosed(), which
// only fires on transport EOF. SessionBootstrap must therefore close the RPC
// device BEFORE detaching it, or every in-flight call hangs forever.
void TstSessionBootstrap::pendingCallsFailWhenTheSessionDies()
{
    Harness h;
    QVERIFY(h.wire());

    std::optional<RpcError> failure;
    bool answered = false;
    h.client.call(QStringLiteral("workspace.getTree"), QJsonValue(),
                  [&](const QJsonValue&, std::optional<RpcError> err) {
                      answered = true;
                      failure = err;
                  });
    // The request really went out on the wire and is pending, not
    // short-circuited by call()'s own "cannot transmit" path.
    QVERIFY(h.boot.rpcChannel()->written.contains("workspace.getTree"));
    QVERIFY(!answered);

    h.boot.rpcChannel()->dropRemote();

    QVERIFY(answered);
    QVERIFY(failure.has_value());
    QCOMPARE(failure->message,
             QStringLiteral("transport closed with request pending"));

    // Same guarantee on a deliberate teardown, where nobody emits EOF for us:
    // unwire() must close the channel while it is still attached.
    QTRY_COMPARE(h.boot.state(), State::Wired);
    std::optional<RpcError> second;
    h.client.call(QStringLiteral("workspace.getTree"), QJsonValue(),
                  [&](const QJsonValue&, std::optional<RpcError> err) {
                      second = err;
                  });
    QVERIFY(!second.has_value());
    h.boot.disconnectSession();
    QVERIFY(second.has_value());
    QCOMPARE(second->message,
             QStringLiteral("transport closed with request pending"));
}

// A session-level fault (the pool itself going down) is a loss even if no
// channel reported EOF first.
void TstSessionBootstrap::poolLossReconnects()
{
    Harness h;
    QVERIFY(h.wire());

    emit h.pool.errorOccurred(QStringLiteral("Socket error: disconnected"));

    QCOMPARE(h.boot.state(), State::Reconnecting);
    QTRY_COMPARE(h.boot.state(), State::Wired);
    QCOMPARE(h.boot.connectCalls, 2);
}

// SPEC 5.6, identical to the vector tst_terminalcontroller pins for
// TerminalController::reconnectDelaySeconds(). These two ladders must not
// drift.
void TstSessionBootstrap::backoffLadderMatchesSpec()
{
    QCOMPARE(SessionBootstrap::reconnectDelaySeconds(-1), 1);
    QCOMPARE(SessionBootstrap::reconnectDelaySeconds(0), 1);
    QCOMPARE(SessionBootstrap::reconnectDelaySeconds(1), 2);
    QCOMPARE(SessionBootstrap::reconnectDelaySeconds(2), 5);
    QCOMPARE(SessionBootstrap::reconnectDelaySeconds(3), 10);
    QCOMPARE(SessionBootstrap::reconnectDelaySeconds(4), 30);
    QCOMPARE(SessionBootstrap::reconnectDelaySeconds(5), 60);
    QCOMPARE(SessionBootstrap::reconnectDelaySeconds(6), 60);
    QCOMPARE(SessionBootstrap::reconnectDelaySeconds(100), 60);
}

// Every rung actually used by the scheduler, in order, and the cap that ends
// it: ten consecutive failures -> Failed, with the delay pinned at 60 s rather
// than growing without bound.
void TstSessionBootstrap::retryDelaySequenceAndCap()
{
    Harness h;
    QVERIFY(h.wire());
    QCOMPARE(h.boot.maxReconnectAttempts(),
             SessionBootstrap::kDefaultMaxReconnectAttempts);

    QList<int> attempts;
    QList<int> delays;
    connect(&h.boot, &SessionBootstrap::reconnectScheduled, this,
            [&](int attempt, int delaySeconds) {
                attempts.append(attempt);
                delays.append(delaySeconds);
            });
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    h.boot.connectOk = false;  // nothing will come back up
    h.boot.rpcChannel()->dropRemote();

    QTRY_COMPARE(h.boot.state(), State::Failed);

    // Printed so a test run shows the ladder it actually walked, not just that
    // it matched.
    qInfo() << "observed backoff seconds:" << delays << "attempts:" << attempts;
    QCOMPARE(delays, (QList<int>{1, 2, 5, 10, 30, 60, 60, 60, 60, 60}));
    QCOMPARE(attempts, (QList<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
    QCOMPARE(h.boot.reconnectAttempt(), 10);
    QCOMPARE(h.boot.connectCalls, 11);  // the original wire plus ten retries
    QVERIFY(!h.boot.reconnectPending());
    QVERIFY(errorSpy.last().at(0).toString().contains(
        QStringLiteral("giving up")));

    // Terminal: nothing fires after the cap.
    QTest::qWait(100);
    QCOMPARE(h.boot.state(), State::Failed);
    QCOMPARE(h.boot.connectCalls, 11);
}

// Reconnect turned off before the loss: the session is reported down and left
// down.
void TstSessionBootstrap::reconnectDisabledNeverSchedules()
{
    Harness h;
    h.boot.setReconnectEnabled(false);
    QVERIFY(!h.boot.reconnectEnabled());
    QVERIFY(h.wire());

    h.boot.rpcChannel()->dropRemote();

    QCOMPARE(h.boot.state(), State::Disconnected);
    QVERIFY(!h.boot.reconnectPending());
    QVERIFY(h.client.transport() == nullptr);
    QTest::qWait(50);
    QCOMPARE(h.boot.connectCalls, 1);
}

// Turned off with a retry already armed: the armed retry must be cancelled, not
// allowed one last shot.
void TstSessionBootstrap::disablingMidLadderStopsIt()
{
    Harness h;
    h.boot.connectOk = true;
    QVERIFY(h.wire());
    h.boot.connectOk = false;
    h.boot.rpcChannel()->dropRemote();
    QCOMPARE(h.boot.state(), State::Reconnecting);
    QVERIFY(h.boot.reconnectPending());

    h.boot.setReconnectEnabled(false);

    QVERIFY(!h.boot.reconnectPending());
    QCOMPARE(h.boot.state(), State::Disconnected);
    QTest::qWait(50);
    QCOMPARE(h.boot.connectCalls, 1);
    QCOMPARE(h.boot.state(), State::Disconnected);
}

// The user closed the session. It must stay closed.
void TstSessionBootstrap::userDisconnectDoesNotReconnect()
{
    Harness h;
    QVERIFY(h.wire());
    QSignalSpy stateSpy(&h.boot, &SessionBootstrap::stateChanged);

    h.boot.disconnectSession();

    QCOMPARE(states(stateSpy), (QList<State>{State::Disconnected}));
    QVERIFY(!h.boot.reconnectPending());
    QVERIFY(h.client.transport() == nullptr);
    QVERIFY(h.monitor.transport() == nullptr);
    QVERIFY(h.boot.rpcDevice() == nullptr);
    QVERIFY(h.boot.agentDevice() == nullptr);
    QTest::qWait(50);
    QCOMPARE(h.boot.state(), State::Disconnected);
    QCOMPARE(h.boot.connectCalls, 1);
}

// Opting out of the cap keeps the ladder alive at its last rung instead of
// escalating or giving up.
void TstSessionBootstrap::uncappedLadderKeepsRetryingAtSixty()
{
    Harness h;
    h.boot.setMaxReconnectAttempts(0);
    QVERIFY(h.wire());

    QList<int> delays;
    connect(&h.boot, &SessionBootstrap::reconnectScheduled, this,
            [&](int, int delaySeconds) { delays.append(delaySeconds); });

    h.boot.connectOk = false;
    h.boot.rpcChannel()->dropRemote();

    QTRY_VERIFY(delays.size() >= 14);
    QCOMPARE(h.boot.state(), State::Reconnecting);
    QCOMPARE(delays.mid(5, 9), QList<int>(9, 60));
    h.boot.setReconnectEnabled(false);  // stop the ladder before teardown
}

// Guiless: nothing here needs a display, and QTEST_MAIN would pull in
// QGuiApplication (ch_app links Qt6::Gui) and abort headless.
QTEST_GUILESS_MAIN(TstSessionBootstrap)
#include "tst_sessionbootstrap.moc"
