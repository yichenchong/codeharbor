#include "AgentStatusMonitor.h"
#include "KnownHosts.h"
#include "SessionState.h"
#include "SessionsModel.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtTest/QtTest>

#include <memory>

using ch::AgentState;
using ch::AgentStatusMonitor;
using ch::KnownHosts;
using ch::SessionRowState;
using ch::SessionsModel;
using ch::SshChannelDevice;
using ch::SshConnectionPool;
using ch::TerminalState;
using ch::TerminalStatus;

namespace {

// A remote exec is a TCP round-trip plus a fork on the fixture host.
constexpr int kExecTimeoutMs = 30000;
// Anything that starts node pays a cold interpreter start that also
// type-strips the TypeScript entry point.
constexpr int kNodeTimeoutMs = 60000;
// The hook's line only has to cross a Unix socket, the bridge, an SSH channel
// and the monitor's parser once the hook process has already exited.
constexpr int kRelayTimeoutMs = 15000;

QString env(const char* key)
{
    return qEnvironmentVariable(key);
}

// Quote one argv element for the remote login shell: an SSH exec request
// carries a single command string that the server hands to the user's shell.
// Same rule as SessionBootstrap::shellQuote.
QString q(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

int asInt(AgentState s) { return static_cast<int>(s); }
int asInt(SessionRowState s) { return static_cast<int>(s); }

QString stateNames(const QVector<int>& states)
{
    QStringList names;
    names.reserve(states.size());
    for (int s : states)
        names << ch::toString(static_cast<AgentState>(s));
    return names.join(QStringLiteral(" -> "));
}

} // namespace

// LIVE gate for agent awareness (SPEC 6.2-6.5, workstream A). The whole chain
// is real and remote:
//
//   fake harness (tests/live/fake-omp-agent.sh)
//     -> REAL hook  remote/src/hooks/oh-my-pi-hook.ts   (one node process each)
//     -> Unix socket $XDG_RUNTIME_DIR/codeharbor.sock
//     -> REAL relay remote/src/bridge.ts                (adapter mapping)
//     -> AgentEvent JSONL on an SSH AgentStatus channel (SshChannelDevice)
//     -> ch::AgentStatusMonitor                         (client)
//     -> ch::SessionsModel sidebar row state
//
// Only the harness itself is stood in for: Oh My Pi is not installed on the
// fixture host, so a shell script fires its lifecycle hooks. Everything the
// hook and the bridge do is production code.
//
// Skipped wholesale unless CH_LIVE_SSH is set, so the default suite stays green
// on a machine with no fixture.
class TstLiveAgent : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void bridgeChannelWiresMonitor();     // (a)
    void realHookDrivesOrderedStates();   // (b)
    void sidebarRowIsFinishedUnseen();    // (d), while unseen
    void markSeenClearsBadgeAndRow();     // (c) + (d), after markSeen
    void remoteBridgeIsReaped();          // (e)

private:
    void connectPool();
    // Run one remote command on its own Exec channel and collect its streams.
    // Returns false if the channel never reached remote EOF within `timeoutMs`.
    bool runRemote(const QString& command, QByteArray* out, QString* err,
                   int timeoutMs);
    // Fire one native Oh My Pi lifecycle hook on the remote side. Returns false
    // on the first failed check so a caller can QVERIFY it and abort — a void
    // helper's QVERIFY only returns from the helper, letting the caller wait for
    // a state change that can never arrive and report a misleading timeout.
    bool fireHook(const QString& nativeEvent,
                  const QString& tool = QString(),
                  const QString& summary = QString());
    // Live per-terminal status as the sidebar would carry it.
    QVector<TerminalStatus> liveTerminals() const;
    // Aggregate row state read back through the real model's RowStateRole.
    SessionRowState modelRowState(const QVector<TerminalStatus>& terminals);

    SshConnectionPool m_pool;
    AgentStatusMonitor m_monitor;
    SessionsModel m_sessions;
    SshChannelDevice* m_bridge = nullptr;

    std::unique_ptr<QSignalSpy> m_unseenSpy;
    std::unique_ptr<QSignalSpy> m_notifySpy;
    // Ordered agent states observed for our (devSessionId, terminalId).
    QVector<int> m_observed;

    QString m_host;
    quint16 m_port = 0;
    QString m_user;
    QString m_node;
    QString m_repo;
    QString m_knownHostsPath;

    // Remote scratch dir selecting a private bridge socket, so this gate never
    // shares a socket with the desktop app or another live test.
    QString m_runtimeDir;
    QString m_socketPath;
    QString m_dev;
    QString m_term;
    QString m_summary;

    QString m_bridgeStderr;
    qint64 m_bridgePid = -1;
    qint64 m_wrapperPid = -1;
};

void TstLiveAgent::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH is not set; live agent gate skipped");
    if (!SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; live agent gate skipped");

    m_host = env("CH_LIVE_HOST");
    m_port = static_cast<quint16>(env("CH_LIVE_PORT").toUInt());
    m_user = env("CH_LIVE_USER");
    m_node = env("CH_LIVE_NODE");
    m_repo = env("CH_LIVE_REPO");
    m_knownHostsPath = env("CH_LIVE_KNOWN_HOSTS");
    if (m_knownHostsPath.isEmpty()) {
        m_knownHostsPath =
            QDir::temp().filePath(QStringLiteral("ch_live_agent_known_hosts"));
    }
    QVERIFY2(!m_host.isEmpty() && m_port != 0 && !m_user.isEmpty()
                 && !m_node.isEmpty() && !m_repo.isEmpty(),
             "CH_LIVE_HOST/PORT/USER/NODE/REPO must all be set");

    const QString tag = QString::number(QCoreApplication::applicationPid());
    m_runtimeDir = QStringLiteral("/tmp/ch-live-agent-") + tag;
    m_socketPath = m_runtimeDir + QStringLiteral("/codeharbor.sock");
    m_dev = QStringLiteral("dev-live-") + tag;
    m_term = QStringLiteral("term-live-") + tag;
    m_summary = QStringLiteral("live gate: agent turn finished");

    // Installed before the bridge is wired so no transition can be missed.
    connect(&m_monitor, &AgentStatusMonitor::agentStateChanged, this,
            [this](const QString& dev, const QString& term, int state) {
                if (dev == m_dev && term == m_term)
                    m_observed.append(state);
            });
    m_unseenSpy = std::make_unique<QSignalSpy>(
        &m_monitor, &AgentStatusMonitor::unseenChanged);
    m_notifySpy =
        std::make_unique<QSignalSpy>(&m_monitor, &AgentStatusMonitor::notify);
}

void TstLiveAgent::cleanupTestCase()
{
    m_monitor.setTransport(nullptr);
    if (m_bridge) {
        m_bridge->closeChannel();
        delete m_bridge;
        m_bridge = nullptr;
    }
    if (m_pool.state() == SshConnectionPool::State::Connected
        && !m_runtimeDir.isEmpty()) {
        QByteArray out;
        QString err;
        runRemote(QStringLiteral("rm -rf ") + q(m_runtimeDir), &out, &err,
                  kExecTimeoutMs);
    }
    m_pool.disconnectFromHost();
}

void TstLiveAgent::connectPool()
{
    if (m_pool.state() == SshConnectionPool::State::Connected)
        return;

    // First-use trust, exactly like SessionBootstrap: load the store, accept an
    // unknown key once, write it back.
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
    const auto conn = connect(&m_pool, &SshConnectionPool::errorOccurred, this,
                              [&failure](const QString& t) { failure += t; });
    const bool ok = m_pool.connectToHost(m_host, m_port, m_user);
    disconnect(conn);
    QVERIFY2(ok, qPrintable(QStringLiteral("connectToHost(%1:%2) failed: %3")
                                .arg(m_host)
                                .arg(m_port)
                                .arg(failure)));

    QDir().mkpath(QFileInfo(m_knownHostsPath).absolutePath());
    QFile out(m_knownHostsPath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        out.write(m_pool.knownHosts().serialize());
}

bool TstLiveAgent::runRemote(const QString& command, QByteArray* out,
                             QString* err, int timeoutMs)
{
    SshChannelDevice device(&m_pool, SshConnectionPool::ChannelKind::Exec);
    bool finished = false;
    connect(&device, &SshChannelDevice::readyRead, &device,
            [&]() { *out += device.readAll(); });
    connect(&device, &SshChannelDevice::channelError, &device,
            [&](const QString& text) { *err += text; });
    connect(&device, &SshChannelDevice::readChannelFinished, &device,
            [&]() { finished = true; });

    if (!device.startExec(command))
        return false;

    QElapsedTimer clock;
    clock.start();
    while (!finished && clock.elapsed() < timeoutMs)
        QTest::qWait(10);

    device.closeChannel();
    return finished;
}

bool TstLiveAgent::fireHook(const QString& nativeEvent, const QString& tool,
                            const QString& summary)
{
    // The exact contract a harness must honour (SPEC 6.2): the native event is
    // argv[1] of the installed hook, the session coordinates and the optional
    // summary come from the environment, and XDG_RUNTIME_DIR selects the bridge
    // socket (SPEC 6.3).
    QStringList argv;
    argv << QStringLiteral("env")
         << q(QStringLiteral("XDG_RUNTIME_DIR=") + m_runtimeDir)
         << q(QStringLiteral("CH_HOOK_NODE=") + m_node)
         << q(QStringLiteral("CH_HOOK_REPO=") + m_repo)
         << q(QStringLiteral("OMP_DEV_SESSION_ID=") + m_dev)
         << q(QStringLiteral("OMP_TERMINAL_ID=") + m_term);
    if (!summary.isEmpty())
        argv << q(QStringLiteral("OMP_SUMMARY=") + summary);
    argv << QStringLiteral("sh")
         << q(m_repo + QStringLiteral("/tests/live/fake-omp-agent.sh"))
         << q(nativeEvent);
    if (!tool.isEmpty())
        argv << q(tool);

    // Each check reports through QTest::qVerify (as QVERIFY2 does) but returns
    // false instead of a bare return, so the caller's QVERIFY(fireHook(...))
    // aborts the test on the first failure rather than falling through into a
    // wait for a state change that can never arrive.
    QByteArray out;
    QString err;
    const QString command = argv.join(QLatin1Char(' '));
    if (!QTest::qVerify(runRemote(command, &out, &err, kNodeTimeoutMs),
                        "runRemote(command, &out, &err, kNodeTimeoutMs)",
                        qPrintable(QStringLiteral("hook %1 never finished: %2")
                                       .arg(nativeEvent, command)),
                        __FILE__, __LINE__))
        return false;
    // SPEC 6.4: the hook swallows every failure and exits 0 so a broken
    // producer cannot take down the agent. Its only symptom is a stderr
    // warning, so a silent run is the proof that it really reached the bridge
    // socket — without this assertion an unreachable bridge would look green.
    if (!QTest::qVerify(err.trimmed().isEmpty(), "err.trimmed().isEmpty()",
                        qPrintable(QStringLiteral("hook %1 warned: %2")
                                       .arg(nativeEvent, err.trimmed())),
                        __FILE__, __LINE__))
        return false;
    // The hook is a pure producer: its payload goes to the socket, never stdout.
    if (!QTest::qVerify(out.trimmed().isEmpty(), "out.trimmed().isEmpty()",
                        out.constData(), __FILE__, __LINE__))
        return false;
    return true;
}

QVector<TerminalStatus> TstLiveAgent::liveTerminals() const
{
    // Mirrors AppController::rebuildRows() (src/app/AppController.cpp): the row
    // carries the monitor's raw per-terminal agent state, except that
    // IdleUnseen is downgraded to Idle once the Dev Session's completion has
    // been marked seen — markSeen() clears the per-session unseen flag, not the
    // terminal's raw state, so without the downgrade the badge would never
    // clear.
    TerminalStatus status;
    status.id = ch::TerminalId{m_term};
    status.connection = TerminalState::Ready;
    auto agent = static_cast<AgentState>(m_monitor.stateFor(m_dev, m_term));
    if (agent == AgentState::IdleUnseen && !m_monitor.hasUnseen(m_dev))
        agent = AgentState::Idle;
    status.agent = agent;
    return {status};
}

SessionRowState TstLiveAgent::modelRowState(
    const QVector<TerminalStatus>& terminals)
{
    ch::SessionRow session;
    session.session.id = ch::DevSessionId{m_dev};
    session.session.name = QStringLiteral("live agent gate");
    session.terminals = terminals;

    ch::GroupRow group;
    group.group.id = ch::GroupId{QStringLiteral("grp-live")};
    group.group.name = QStringLiteral("live");
    group.sessions.push_back(session);

    m_sessions.setGroups({group});
    const QModelIndex groupIndex = m_sessions.index(0, 0, QModelIndex());
    const QModelIndex sessionIndex = m_sessions.index(0, 0, groupIndex);
    Q_ASSERT(sessionIndex.isValid());
    return static_cast<SessionRowState>(
        m_sessions.data(sessionIndex, SessionsModel::RowStateRole).toInt());
}

// (a) A real AgentStatus channel running the real bridge, wired to a real
// monitor. The bridge is wrapped in the same stdin watchdog that
// SessionBootstrap::bridgeCommand() uses: an SSH exec channel has no
// controlling terminal, so closing it sends no SIGHUP and the relay — which
// holds a listening Unix socket — would otherwise idle forever as an orphan.
// `cat` reaching EOF is the channel-closed signal; it then kills the relay.
void TstLiveAgent::bridgeChannelWiresMonitor()
{
    connectPool();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    const QString relay =
        QStringLiteral("XDG_RUNTIME_DIR=") + q(m_runtimeDir) + QLatin1Char(' ')
        + q(m_node) + QLatin1Char(' ')
        + q(m_repo + QStringLiteral("/remote/src/bridge.ts"))
        + QStringLiteral(" & __ch_bridge=$!;")
        // Both pids on stderr (out of the JSONL read stream) so the reaping
        // check below can name the exact processes instead of pattern matching.
        + QStringLiteral(" echo \"ch-live wrapper=$$ bridge=$__ch_bridge\" 1>&2;")
        + QStringLiteral(" cat >/dev/null; kill $__ch_bridge 2>/dev/null");

    m_bridge = new SshChannelDevice(&m_pool,
                                    SshConnectionPool::ChannelKind::AgentStatus,
                                    this);
    connect(m_bridge, &SshChannelDevice::channelError, this,
            [this](const QString& text) { m_bridgeStderr += text; });
    QVERIFY(m_bridge->startExec(QStringLiteral("sh -c ") + q(relay)));

    m_monitor.setTransport(m_bridge);
    QCOMPARE(m_monitor.transport(), static_cast<QIODevice*>(m_bridge));

    // The relay announces its socket on stderr once it is listening; until then
    // a hook firing would find nothing to connect to.
    const QString banner =
        QStringLiteral("codeharbor-bridge listening on ") + m_socketPath;
    QTRY_VERIFY_WITH_TIMEOUT(m_bridgeStderr.contains(banner), kNodeTimeoutMs);

    static const QRegularExpression pids(
        QStringLiteral("ch-live wrapper=(\\d+) bridge=(\\d+)"));
    const QRegularExpressionMatch m = pids.match(m_bridgeStderr);
    QVERIFY2(m.hasMatch(), qPrintable(m_bridgeStderr));
    m_wrapperPid = m.captured(1).toLongLong();
    m_bridgePid = m.captured(2).toLongLong();
    QVERIFY(m_wrapperPid > 0 && m_bridgePid > 0);
    qInfo("bridge listening on %s (wrapper pid %lld, relay pid %lld)",
          qPrintable(m_socketPath), m_wrapperPid, m_bridgePid);

    // Nothing has fired yet: the pair is unknown to the monitor.
    QCOMPARE(m_monitor.stateFor(m_dev, m_term), asInt(AgentState::Unknown));
    QVERIFY(!m_monitor.hasUnseen(m_dev));
}

// (b) A realistic Oh My Pi turn, fired one native hook at a time through the
// REAL hook script, asserted after each firing so the ORDER is observed and not
// merely the final state: session_start -> agent_start -> tool_call(ask) ->
// tool_result(ask) -> agent_end, which the bridge's adapter maps to
// starting -> running -> waiting_input -> running -> idle_unseen (SPEC 6.5).
void TstLiveAgent::realHookDrivesOrderedStates()
{
    QVERIFY2(m_bridge != nullptr, "bridge channel was not wired");

    QVERIFY(fireHook(QStringLiteral("session_start")));
    QTRY_COMPARE_WITH_TIMEOUT(m_monitor.stateFor(m_dev, m_term),
                              asInt(AgentState::Starting), kRelayTimeoutMs);

    QVERIFY(fireHook(QStringLiteral("agent_start")));
    QTRY_COMPARE_WITH_TIMEOUT(m_monitor.stateFor(m_dev, m_term),
                              asInt(AgentState::Running), kRelayTimeoutMs);

    // `ask` is the tool that blocks on the user (SPEC 6.5).
    QVERIFY(fireHook(QStringLiteral("tool_call"), QStringLiteral("ask")));
    QTRY_COMPARE_WITH_TIMEOUT(m_monitor.stateFor(m_dev, m_term),
                              asInt(AgentState::WaitingInput), kRelayTimeoutMs);
    QVERIFY(!m_monitor.hasUnseen(m_dev));

    QVERIFY(fireHook(QStringLiteral("tool_result"), QStringLiteral("ask")));
    QTRY_COMPARE_WITH_TIMEOUT(m_monitor.stateFor(m_dev, m_term),
                              asInt(AgentState::Running), kRelayTimeoutMs);

    QVERIFY(fireHook(QStringLiteral("agent_end"), QString(), m_summary));
    QTRY_COMPARE_WITH_TIMEOUT(m_monitor.stateFor(m_dev, m_term),
                              asInt(AgentState::IdleUnseen), kRelayTimeoutMs);

    const QVector<int> expected{
        asInt(AgentState::Starting), asInt(AgentState::Running),
        asInt(AgentState::WaitingInput), asInt(AgentState::Running),
        asInt(AgentState::IdleUnseen)};
    QVERIFY2(m_observed == expected,
             qPrintable(QStringLiteral("observed [%1], expected [%2]")
                            .arg(stateNames(m_observed), stateNames(expected))));
    qInfo("observed live transitions: %s", qPrintable(stateNames(m_observed)));

    // The notification hook fires on the attention-worthy transitions only, and
    // OMP_SUMMARY survives hook -> bridge -> SSH -> monitor intact.
    QCOMPARE(m_notifySpy->count(), 2);
    QCOMPARE(m_notifySpy->at(0).at(0).toString(),
             QStringLiteral("Agent waiting for input"));
    QCOMPARE(m_notifySpy->at(1).at(0).toString(),
             QStringLiteral("Agent finished"));
    QCOMPARE(m_notifySpy->at(1).at(1).toString(), m_summary);
}

// (d) The row-level effect while the completion is unseen: the sidebar row for
// the Dev Session reads FinishedUnseen, both from the aggregate helper and
// through the model role QML actually binds to.
void TstLiveAgent::sidebarRowIsFinishedUnseen()
{
    QCOMPARE(m_monitor.stateFor(m_dev, m_term), asInt(AgentState::IdleUnseen));
    QVERIFY(m_monitor.hasUnseen(m_dev));

    const QVector<TerminalStatus> terminals = liveTerminals();
    QCOMPARE(asInt(terminals.at(0).agent), asInt(AgentState::IdleUnseen));
    QCOMPARE(asInt(SessionsModel::aggregateSessionState(terminals)),
             asInt(SessionRowState::FinishedUnseen));
    QCOMPARE(asInt(modelRowState(terminals)),
             asInt(SessionRowState::FinishedUnseen));
}

// (c) The unseen badge and its clearing, plus (d) the row falling back to Idle.
void TstLiveAgent::markSeenClearsBadgeAndRow()
{
    QVERIFY(m_monitor.hasUnseen(m_dev));
    // Exactly one flip to true, raised by the live idle_unseen event.
    QCOMPARE(m_unseenSpy->count(), 1);
    QCOMPARE(m_unseenSpy->at(0).at(0).toString(), m_dev);
    QCOMPARE(m_unseenSpy->at(0).at(1).toBool(), true);

    m_monitor.markSeen(m_dev);
    QVERIFY(!m_monitor.hasUnseen(m_dev));
    QCOMPARE(m_unseenSpy->count(), 2);
    QCOMPARE(m_unseenSpy->at(1).at(0).toString(), m_dev);
    QCOMPARE(m_unseenSpy->at(1).at(1).toBool(), false);

    // Idempotent: a second markSeen on an already-seen session is a no-op.
    m_monitor.markSeen(m_dev);
    QCOMPARE(m_unseenSpy->count(), 2);
    QVERIFY(!m_monitor.hasUnseen(m_dev));

    // The terminal's raw agent state is untouched...
    QCOMPARE(m_monitor.stateFor(m_dev, m_term), asInt(AgentState::IdleUnseen));
    // ...but the sidebar row drops back to Idle.
    const QVector<TerminalStatus> terminals = liveTerminals();
    QCOMPARE(asInt(terminals.at(0).agent), asInt(AgentState::Idle));
    QCOMPARE(asInt(SessionsModel::aggregateSessionState(terminals)),
             asInt(SessionRowState::Idle));
    QCOMPARE(asInt(modelRowState(terminals)), asInt(SessionRowState::Idle));
}

// (e) Closing the channel must reap the relay: no orphan node process may
// survive the session. Both the wrapper shell and the relay it spawned are
// named by pid, so this cannot be fooled by an unrelated node process.
void TstLiveAgent::remoteBridgeIsReaped()
{
    QVERIFY(m_bridgePid > 0 && m_wrapperPid > 0);

    QByteArray alive;
    QString err;
    const QString probe =
        QStringLiteral("sh -c ")
        + q(QStringLiteral(
                "for p in %1 %2; do kill -0 $p 2>/dev/null && echo ALIVE=$p; "
                "done; echo PROBE_DONE")
                .arg(m_wrapperPid)
                .arg(m_bridgePid));

    // Sanity: while the channel is open, both processes are running. Without
    // this the reaping assertion below would also pass against a bridge that
    // never started.
    QVERIFY(runRemote(probe, &alive, &err, kExecTimeoutMs));
    QVERIFY2(alive.contains(QByteArray("ALIVE=")
                            + QByteArray::number(m_bridgePid)),
             alive.constData());
    QVERIFY2(alive.contains(QByteArray("ALIVE=")
                            + QByteArray::number(m_wrapperPid)),
             alive.constData());

    m_monitor.setTransport(nullptr);
    m_bridge->closeChannel();

    QByteArray after;
    QString discard;
    for (int attempt = 0; attempt < 40; ++attempt) {
        QTest::qWait(250);
        after.clear();
        discard.clear();
        if (!runRemote(probe, &after, &discard, kExecTimeoutMs))
            continue;
        if (!after.contains("ALIVE="))
            break;
    }
    QVERIFY2(after.contains("PROBE_DONE"), after.constData());
    QVERIFY2(!after.contains("ALIVE="), after.constData());
}

QTEST_GUILESS_MAIN(TstLiveAgent)
#include "tst_liveagent.moc"
