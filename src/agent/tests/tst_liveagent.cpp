#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "KnownHosts.h"
#include "SessionBootstrap.h"
#include "SessionState.h"
#include "SessionsModel.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"
#include "TmuxActivityPoller.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtTest/QtTest>

#include <memory>
#include <optional>

using ch::AgentState;
using ch::AgentStatusMonitor;
using ch::CodeharbordClient;
using ch::KnownHosts;
using ch::RpcError;
using ch::SessionBootstrap;
using ch::SessionRowState;
using ch::SessionsModel;
using ch::SshChannelDevice;
using ch::SshConnectionPool;
using ch::TerminalState;
using ch::TerminalStatus;
using ch::TmuxActivityPoller;

namespace {

// A remote exec is a TCP round-trip plus a fork on the fixture host.
constexpr int kExecTimeoutMs = 30000;
// Anything that starts node pays a cold interpreter start that also
// type-strips the TypeScript entry point.
constexpr int kNodeTimeoutMs = 60000;
// The hook's line only has to cross a Unix socket, the bridge, an SSH channel
// and the monitor's parser once the hook process has already exited.
constexpr int kRelayTimeoutMs = 15000;
// A detached generic pane's state comes from tmux's `#{window_activity}`, which
// is dated to a whole SECOND. Compressing AgentStatusMonitor's ten-second
// remote idle window to one second and then demanding this much real silence is
// what makes the Idle arm decidable: after three seconds of quiet the server's
// age is at least ~2s even in the worst case, where the last output landed just
// before a second ticked over, so it is unambiguously past the compressed
// window. A shorter wait would be measuring the rounding, not the silence.
constexpr int kCompressedRemoteIdleMs = 1000;
constexpr int kQuietWindowMs = 3000;
// Compressed too: the poller's five-second production cadence is policy, and a
// gate that waited it out three times over would spend its budget sleeping.
constexpr int kActivityPollIntervalMs = 500;

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

// One JSON-RPC round trip driven to completion on the caller's event loop, the
// same shape tst_liveshell's RawRpc has. The workspace rows the activity gate
// needs are created through the REAL `workspace.*` surface rather than by
// reaching into the fixture's database, because the daemon answers
// `tmux.paneActivity` from its OWN view of those rows: a pane written behind its
// back would not prove the join works.
struct RawRpc {
    QJsonValue result;
    std::optional<RpcError> error;
    bool done = false;

    bool call(CodeharbordClient& client, const QString& method,
              const QJsonObject& params, int timeoutMs = kExecTimeoutMs)
    {
        client.call(method, params,
                    [this](QJsonValue value, std::optional<RpcError> err) {
                        result = value;
                        error = err;
                        done = true;
                    });
        if (!QTest::qWaitFor([this] { return done; }, timeoutMs))
            return false;
        return !error.has_value();
    }

    QString diagnostic(const QString& method) const
    {
        if (!done)
            return method + QStringLiteral(": no response within timeout");
        if (error)
            return method + QStringLiteral(": rpc error %1 %2")
                                .arg(error->code)
                                .arg(error->message);
        return QString();
    }
};

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
    void errorAndShutdownReachTheRowState(); // (b) remainder of SPEC 6.5
    void remoteBridgeIsReaped();          // (e)
    // (f) SPEC 6.6 for the Dev Session nobody is looking at. Ordered LAST: the
    // slots above share one fixture, one bridge channel and one (m_dev, m_term)
    // deliberately, and this one opens a second channel and a second pane.
    void detachedGenericPaneReportsThroughTmuxActivity();

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
                  const QString& summary = QString(),
                  bool error = false);
    // Live per-terminal status as the sidebar would carry it.
    QVector<TerminalStatus> liveTerminals() const;
    // Aggregate row state read back through the real model's RowStateRole, for
    // the bridge-driven pane.
    SessionRowState modelRowState(const QVector<TerminalStatus>& terminals);
    // The same, for any Dev Session: the activity gate below builds its row from
    // the real pane row the daemon minted rather than from m_dev.
    SessionRowState modelRowStateFor(const QString& devSessionId,
                                     const QVector<TerminalStatus>& terminals);

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

    // The activity gate's own RPC channel: `codeharbord rpc --stdio`, which the
    // slots above have no need for. A second channel on the SAME pool, exactly
    // as production runs the RPC and the AgentStatus channels side by side.
    SshChannelDevice* m_rpc = nullptr;
    CodeharbordClient m_client;
    QString m_rpcStderr;
    // Rows this gate created on the fixture, remembered by server id so
    // cleanupTestCase deletes exactly them and never sweeps by name.
    QString m_activityServerId;
    QString m_activityGroupId;
    QString m_activityDev;
    QString m_activityTerm;
    // The pane's `terminal_panes.tmux_target`, and therefore the name of the
    // real tmux session this gate starts on the fixture host.
    QString m_activityTarget;
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
    // Same rule as the scratch directory below: a failed cleanup is not a test
    // failure, but it must not be silent. A surviving tmux session keeps the
    // pane's target bound on the host, and a surviving group keeps its rows —
    // including the UNIQUE tmux_target — in the fixture database, so the next
    // run of this gate would be asked to mint a target that is already taken.
    if (m_pool.state() == SshConnectionPool::State::Connected
        && !m_activityTarget.isEmpty()) {
        QByteArray out;
        QString err;
        if (!runRemote(QStringLiteral("tmux kill-session -t ")
                           + q(QStringLiteral("=") + m_activityTarget),
                       &out, &err, kExecTimeoutMs)) {
            qWarning("failed to kill remote tmux session %s",
                     qPrintable(m_activityTarget));
        }
    }
    if (m_client.transport() != nullptr && !m_activityGroupId.isEmpty()) {
        RawRpc removed;
        if (!removed.call(m_client, QStringLiteral("workspace.deleteGroup"),
                          {{QStringLiteral("id"), m_activityGroupId}})) {
            qWarning("failed to delete live workspace group %s: %s",
                     qPrintable(m_activityGroupId),
                     qPrintable(removed.diagnostic(
                         QStringLiteral("workspace.deleteGroup"))));
        }
    }
    if (m_rpc) {
        // The order SessionBootstrap::unwire() uses: CLOSE the channel first,
        // THEN detach the client, so an in-flight call fails through the
        // transport-error path instead of having its transport pulled away.
        m_rpc->closeChannel();
        m_client.setTransport(nullptr);
        delete m_rpc;
        m_rpc = nullptr;
    }
    if (m_pool.state() == SshConnectionPool::State::Connected
        && !m_runtimeDir.isEmpty()) {
        // A failed cleanup is not a test failure — the gate has already made
        // its assertions — but it MUST NOT be invisible: silently swallowing it
        // leaves a private bridge socket and its scratch directory on the
        // fixture host, and the next run inherits the litter.
        QByteArray out;
        QString err;
        if (!runRemote(QStringLiteral("rm -rf ") + q(m_runtimeDir), &out, &err,
                       kExecTimeoutMs)
            || !err.trimmed().isEmpty()) {
            qWarning("failed to remove remote scratch dir %s: %s",
                     qPrintable(m_runtimeDir), qPrintable(err.trimmed()));
        }
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
    SshChannelDevice device(&m_pool);
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
                            const QString& summary, bool error)
{
    // The exact contract a harness must honour, as documented by the hook it
    // installs (remote/src/hooks/oh-my-pi-hook.ts): the native event is the
    // hook's first positional argument, the session coordinates and the optional
    // summary come from the environment, and XDG_RUNTIME_DIR selects the bridge
    // socket (SPEC 6.3 "Remote Agent Bridge").
    QStringList argv;
    argv << QStringLiteral("env")
         << q(QStringLiteral("XDG_RUNTIME_DIR=") + m_runtimeDir)
         << q(QStringLiteral("CH_HOOK_NODE=") + m_node)
         << q(QStringLiteral("CH_HOOK_REPO=") + m_repo)
         << q(QStringLiteral("OMP_DEV_SESSION_ID=") + m_dev)
         << q(QStringLiteral("OMP_TERMINAL_ID=") + m_term);
    if (!summary.isEmpty())
        argv << q(QStringLiteral("OMP_SUMMARY=") + summary);
    // SPEC 6.5's "agent or hook error" arm: the harness marks the firing and
    // the adapter maps it to `error` ahead of the native event name — except
    // for a shutdown, which outranks the flag (SPEC 6.5 precedence).
    if (error)
        argv << q(QStringLiteral("OMP_ERROR=1"));
    // Executed directly rather than as `sh <script>`, so its `#!/usr/bin/env
    // bash` shebang is honoured. The script enables `set -o pipefail`, which
    // is a Bash option: handing it to a POSIX /bin/sh (dash on Debian, ash on
    // Alpine/BusyBox) makes the shell abort with "Illegal option -o pipefail"
    // before a single hook is fired, and the gate then reports a relay timeout
    // for a hook that never ran. `env` execs it, so the file's 0755 mode and
    // its interpreter line are what select the shell.
    argv << q(m_repo + QStringLiteral("/tests/live/fake-omp-agent.sh"))
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
    return modelRowStateFor(m_dev, terminals);
}

SessionRowState TstLiveAgent::modelRowStateFor(
    const QString& devSessionId, const QVector<TerminalStatus>& terminals)
{
    ch::SessionRow session;
    session.session.id = ch::DevSessionId{devSessionId};
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

    m_bridge = new SshChannelDevice(&m_pool, this);
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

// The two SPEC 6.5 mappings realHookDrivesOrderedStates() does not reach, and
// with them the top of the sidebar's precedence ladder. Both go through the
// same real chain: fake harness -> real hook -> real bridge adapter -> SSH ->
// monitor -> SessionsModel row state.
//
//   agent or hook error -> error     highest-priority row state there is
//   session_shutdown    -> stopped   the terminal's last word, and the one
//                                    mapping the error flag may NOT mask
//
// Error is also the state most easily got wrong in the direction the user
// notices: a row that latches red, or an error that pops a desktop bubble the
// spec never asked for. Both are asserted against here.
void TstLiveAgent::errorAndShutdownReachTheRowState()
{
    QVERIFY2(m_bridge != nullptr, "bridge channel was not wired");
    const int notifiesBefore = m_notifySpy->count();
    const int unseenBefore = m_unseenSpy->count();

    // OMP_ERROR=1 marks the firing; adapters/pi-family.ts (shared by the
    // oh-my-pi and pi adapters) checks it BEFORE the native event name, so even
    // an agent_end maps to error.
    QVERIFY(fireHook(QStringLiteral("agent_end"), QString(),
                     QStringLiteral("live gate: agent blew up"), true));
    QTRY_COMPARE_WITH_TIMEOUT(m_monitor.stateFor(m_dev, m_term),
                              asInt(AgentState::Error), kRelayTimeoutMs);
    QCOMPARE(asInt(modelRowState(liveTerminals())),
             asInt(SessionRowState::Error));
    // An error is not a completion: no unseen badge...
    QVERIFY(!m_monitor.hasUnseen(m_dev));
    QCOMPARE(m_unseenSpy->count(), unseenBefore);
    // ...and not attention-worthy in the notification sense either: the hook
    // fires for waiting_input and idle_unseen only, the state names SPEC 6.4
    // defines (notifications themselves are a Phase 4 deliverable in SPEC 16).
    QCOMPARE(m_notifySpy->count(), notifiesBefore);

    QVERIFY(fireHook(QStringLiteral("session_shutdown")));
    QTRY_COMPARE_WITH_TIMEOUT(m_monitor.stateFor(m_dev, m_term),
                              asInt(AgentState::Stopped), kRelayTimeoutMs);
    // Error is a state, not a latch: the shutdown replaced it with no explicit
    // reset, and the row leaves Error for Idle (the pane is still connected, so
    // it is Idle rather than Disconnected).
    QCOMPARE(asInt(modelRowState(liveTerminals())), asInt(SessionRowState::Idle));
    QCOMPARE(m_notifySpy->count(), notifiesBefore);
    QCOMPARE(m_unseenSpy->count(), unseenBefore);

    // SPEC 6.5 precedence, the arm that matters most in practice: a producer
    // may leave OMP_ERROR=1 set for a later firing, INCLUDING the shutdown.
    // This helper starts each hook in a fresh remote process, so the final
    // call passes error=true explicitly to model that stale flag. If the flag
    // won there, the row would stay red for a session that is gone, because
    // nothing after a shutdown can ever arrive to replace the state. Drive the
    // terminal back into Error first so the shutdown is a real transition and
    // not a repeat of the state it is already in.
    QVERIFY(fireHook(QStringLiteral("agent_end"), QString(),
                     QStringLiteral("live gate: agent blew up again"), true));
    QTRY_COMPARE_WITH_TIMEOUT(m_monitor.stateFor(m_dev, m_term),
                              asInt(AgentState::Error), kRelayTimeoutMs);
    QVERIFY(fireHook(QStringLiteral("session_shutdown"), QString(), QString(), true));
    QTRY_COMPARE_WITH_TIMEOUT(m_monitor.stateFor(m_dev, m_term),
                              asInt(AgentState::Stopped), kRelayTimeoutMs);
    QCOMPARE(asInt(modelRowState(liveTerminals())), asInt(SessionRowState::Idle));
    QCOMPARE(m_notifySpy->count(), notifiesBefore);
    QCOMPARE(m_unseenSpy->count(), unseenBefore);
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

// (f) SPEC 6.6 for the Dev Session the user is NOT looking at, over a chain that
// is real from end to end:
//
//   a REAL tmux session on the fixture host, with NO client attached
//     -> `tmux.paneActivity` in REAL codeharbord over a REAL SSH RPC channel
//     -> ch::TmuxActivityPoller                          (client)
//     -> ch::AgentStatusMonitor::noteRemoteActivity
//     -> ch::SessionsModel sidebar row state
//
// The bug it defends against is the one a user actually hit. Switching Dev
// Session destroys the pane and detaches its PTY, and for an adapterless
// `generic` pane those bytes were the ONLY status source there was: no adapter
// exists for it, so nothing the slots above exercise produces a single event
// for one. The pane therefore settled to Idle about two seconds after the last
// byte this client happened to catch and stayed there for the lifetime of the
// application — the sidebar claiming "done" about a session that might be
// building the whole time — and the fifteen-minute silence demotion could not
// correct it either, because its arm requires NOT (generic && attached) and the
// `attached` flag was never cleared.
//
// So the two halves asserted here are the two halves of the fix. First that a
// detach is honest: Unknown, not a false Idle. Then that the server's own
// observation of a pane nothing is attached to reaches the sidebar as Running,
// and that the pane going quiet reaches it as Idle — from the age the DAEMON
// measured on its own clock, never from this process's.
//
// Nothing here is stood in for. The bytes are produced inside the remote tmux
// session by `send-keys`, so they demonstrably never reach this client: the
// pane has no channel here at all, which is the entire point.
void TstLiveAgent::detachedGenericPaneReportsThroughTmuxActivity()
{
    connectPool();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    // The production RPC channel, chosen by the production entry-point ladder:
    // SessionBootstrap::rpcCommand() is the exact string the shipped client
    // execs, so this gate cannot pass against a layout the app could not launch.
    m_rpc = new SshChannelDevice(&m_pool, this);
    connect(m_rpc, &SshChannelDevice::channelError, this,
            [this](const QString& text) { m_rpcStderr += text; });
    QVERIFY(m_rpc->startExec(SessionBootstrap::rpcCommand(m_node, m_repo)));
    m_client.setTransport(m_rpc);

    // Prove the channel really reaches codeharbord before anything below waits
    // on an answer from it, and warm the cold node start while we are here.
    RawRpc info;
    QVERIFY2(info.call(m_client, QStringLiteral("server.info"), {},
                       kNodeTimeoutMs),
             qPrintable(info.diagnostic(QStringLiteral("server.info"))
                        + m_rpcStderr));
    const QJsonObject serverInfo = info.result.toObject();
    QCOMPARE(serverInfo.value(QStringLiteral("name")).toString(),
             QStringLiteral("codeharbord"));
    // `tmux.paneActivity` was introduced at schema 8, and a daemon below that
    // floor would answer the poll with a method-not-found the poller silently
    // swallows (a failed poll is not evidence about any pane). Without this the
    // gate would then fail with a timeout that blames the monitor.
    QVERIFY2(serverInfo.value(QStringLiteral("schemaVersion")).toInt() >= 8,
             qPrintable(QStringLiteral("remote schemaVersion is %1, need >= 8")
                            .arg(serverInfo.value(QStringLiteral("schemaVersion"))
                                     .toInt())));

    // Real rows, through the real `workspace.*` surface: the daemon answers
    // `tmux.paneActivity` by joining ITS OWN terminal_panes rows against the
    // tmux server's listing, so a pane written behind its back would prove
    // nothing about that join. The tmux target is supplied rather than minted so
    // this test knows the exact session name to start on the host; it is
    // `[A-Za-z0-9_-]` as TMUX_TARGET_SAFE demands.
    const QString tag = QString::number(QCoreApplication::applicationPid());
    m_activityServerId = QStringLiteral("live-agent-activity-") + tag;
    m_activityTarget = QStringLiteral("ch_live_activity_") + tag;

    RawRpc group;
    QVERIFY2(group.call(m_client, QStringLiteral("workspace.createGroup"),
                        {{QStringLiteral("serverId"), m_activityServerId},
                         {QStringLiteral("name"),
                          QStringLiteral("live activity ") + tag}}),
             qPrintable(group.diagnostic(QStringLiteral("workspace.createGroup"))));
    m_activityGroupId = group.result.toObject().value(QStringLiteral("id")).toString();
    QVERIFY(!m_activityGroupId.isEmpty());

    RawRpc session;
    QVERIFY2(session.call(m_client, QStringLiteral("workspace.createSession"),
                          {{QStringLiteral("serverId"), m_activityServerId},
                           {QStringLiteral("groupId"), m_activityGroupId},
                           {QStringLiteral("name"),
                            QStringLiteral("unwatched ") + tag},
                           {QStringLiteral("repositoryRoot"), m_repo}}),
             qPrintable(session.diagnostic(QStringLiteral("workspace.createSession"))));
    m_activityDev = session.result.toObject().value(QStringLiteral("id")).toString();
    QVERIFY(!m_activityDev.isEmpty());

    RawRpc pane;
    QVERIFY2(pane.call(m_client, QStringLiteral("workspace.createTerminalPane"),
                       {{QStringLiteral("serverId"), m_activityServerId},
                        {QStringLiteral("devSessionId"), m_activityDev},
                        {QStringLiteral("name"), QStringLiteral("terminal-1")},
                        {QStringLiteral("tmuxTarget"), m_activityTarget},
                        // The adapterless harness, as the pane's row carries it.
                        {QStringLiteral("harness"), QStringLiteral("generic")}}),
             qPrintable(pane.diagnostic(
                 QStringLiteral("workspace.createTerminalPane"))));
    const QJsonObject paneRow = pane.result.toObject();
    m_activityTerm = paneRow.value(QStringLiteral("id")).toString();
    QVERIFY(!m_activityTerm.isEmpty());
    // The row must carry the target we asked for verbatim: the daemon REJECTS a
    // target it will not honour rather than quietly substituting another, and a
    // substituted one would leave this gate starting a tmux session the join
    // below can never match.
    QCOMPARE(paneRow.value(QStringLiteral("tmuxTarget")).toString(),
             m_activityTarget);

    // A real tmux session under that exact name, DETACHED: `new-session -d`
    // leaves `#{session_attached}` at 0, which is the property the whole
    // subsystem rests on — tmux keeps dating output for a session no client is
    // reading.
    QByteArray out;
    QString err;
    // Two target spellings, both anchored with tmux's `=` exact-match prefix so
    // a FOREIGN session whose name merely contains ours can never be addressed:
    // `=name` for the commands that take a SESSION (has-session/kill-session,
    // the same form remote/src/tmux.ts uses), and `=name:` for the ones that
    // take a PANE. The trailing colon is required there — `-t '=name'` is not a
    // valid pane target and tmux answers "can't find pane", which is exactly how
    // this gate first failed.
    const QString sessionTarget = q(QStringLiteral("=") + m_activityTarget);
    const QString paneTarget =
        q(QStringLiteral("=") + m_activityTarget + QLatin1Char(':'));
    QVERIFY(runRemote(QStringLiteral("tmux new-session -d -s ")
                          + q(m_activityTarget)
                          + QStringLiteral("; tmux has-session -t ")
                          + sessionTarget
                          + QStringLiteral(" && echo HAVE_SESSION"),
                      &out, &err, kExecTimeoutMs));
    QVERIFY2(out.contains("HAVE_SESSION"),
             qPrintable(QStringLiteral("tmux session %1 not started: %2 %3")
                            .arg(m_activityTarget,
                                 QString::fromUtf8(out), err.trimmed())));

    // Output produced INSIDE that session, so the bytes exist only on the host.
    // capture-pane reads them back from the remote scrollback, which is the
    // proof that they were printed there and never crossed the wire to us: this
    // client holds no channel for this pane at all.
    const QString marker = QStringLiteral("live-activity-") + tag;
    QVERIFY(runRemote(QStringLiteral("tmux send-keys -t ") + paneTarget
                          + QLatin1Char(' ')
                          + q(QStringLiteral("printf '%s\\n' ") + marker)
                          + QStringLiteral(" Enter"),
                      &out, &err, kExecTimeoutMs));
    QByteArray screen;
    for (int attempt = 0; attempt < 20; ++attempt) {
        QTest::qWait(100);
        screen.clear();
        err.clear();
        if (!runRemote(QStringLiteral("tmux capture-pane -p -t ") + paneTarget,
                       &screen, &err, kExecTimeoutMs))
            continue;
        if (screen.contains(marker.toUtf8()))
            break;
    }
    QVERIFY2(screen.contains(marker.toUtf8()),
             qPrintable(QStringLiteral("no remote output in %1: %2")
                            .arg(m_activityTarget, QString::fromUtf8(screen))));

    // The pane as the client knows it, in the order a pane supplies its three
    // inputs: the harness from its row, then the PTY channel, then bytes on it.
    m_monitor.setTerminalHarness(m_activityDev, m_activityTerm,
                                 QStringLiteral("generic"));
    QCOMPARE(m_monitor.stateFor(m_activityDev, m_activityTerm),
             asInt(AgentState::Unknown));
    m_monitor.noteTerminalAttached(m_activityDev, m_activityTerm);
    QCOMPARE(m_monitor.stateFor(m_activityDev, m_activityTerm),
             asInt(AgentState::Starting));
    // Back to back with the attach on purpose: the local fallback clock parks a
    // silent generic pane at Idle two seconds after the last byte, so a wait
    // here would be asserting against that clock instead of this one.
    m_monitor.noteTerminalOutput(m_activityDev, m_activityTerm);
    QCOMPARE(m_monitor.stateFor(m_activityDev, m_activityTerm),
             asInt(AgentState::Running));

    // THE REGRESSION. The user switches Dev Session: the layout destroys the
    // pane and TerminalFactory::detach() releases its channel. The pane must go
    // to Unknown — the honest word for "this client can no longer see it" — and
    // NOT to the Idle the fallback clock used to strand it at and never leave.
    m_monitor.noteTerminalDetached(m_activityDev, m_activityTerm);
    QCOMPARE(m_monitor.stateFor(m_activityDev, m_activityTerm),
             asInt(AgentState::Unknown));

    // Now the only remaining witness: the daemon. Wired exactly as
    // AppController does it — activityObserved straight into noteRemoteActivity,
    // whose trailing `alive` Qt drops because the monitor derives nothing from
    // it. The local collector is connected FIRST so the ages the server reported
    // can be named in a failure message.
    TmuxActivityPoller poller;
    QVector<qint64> ages;
    bool aliveSeen = false;
    connect(&poller, &TmuxActivityPoller::activityObserved, &poller,
            [&](const QString& dev, const QString& term, qint64 ageMs,
                bool alive) {
                if (dev != m_activityDev || term != m_activityTerm)
                    return;
                ages.append(ageMs);
                aliveSeen = aliveSeen || alive;
            });
    connect(&poller, &TmuxActivityPoller::activityObserved, &m_monitor,
            &AgentStatusMonitor::noteRemoteActivity);
    poller.setPollIntervalMs(kActivityPollIntervalMs);
    poller.setRpcClient(&m_client);
    // Arming issues the first poll immediately, exactly as a Dev Session switch
    // does in production.
    poller.setDevSessionIds({m_activityDev});

    QTRY_VERIFY_WITH_TIMEOUT(!ages.isEmpty(), kRelayTimeoutMs);
    QVERIFY2(aliveSeen, "the daemon reported the pane's tmux session as dead");
    QTRY_COMPARE_WITH_TIMEOUT(m_monitor.stateFor(m_activityDev, m_activityTerm),
                              asInt(AgentState::Running), kRelayTimeoutMs);
    qInfo("server-observed ages for the detached pane: first %lld ms",
          ages.constFirst());

    // The row the user actually reads, through the aggregate helper AND the
    // model role QML binds to. The pane's connection state is Unloaded, not
    // Ready: this client has no channel for it, which is precisely what
    // AppController::rebuildRows() leaves in the row for a pane with no live
    // terminal state. Running outranks that in the precedence ladder, so the
    // sidebar reports work in progress for a Dev Session nobody is watching.
    TerminalStatus unwatched;
    unwatched.id = ch::TerminalId{m_activityTerm};
    unwatched.connection = TerminalState::Unloaded;
    unwatched.agent = static_cast<AgentState>(
        m_monitor.stateFor(m_activityDev, m_activityTerm));
    QVector<TerminalStatus> terminals{unwatched};
    QCOMPARE(asInt(SessionsModel::aggregateSessionState(terminals)),
             asInt(SessionRowState::Running));
    QCOMPARE(asInt(modelRowStateFor(m_activityDev, terminals)),
             asInt(SessionRowState::Running));

    // The other half: silence must be reported as silence, and from the age the
    // SERVER measured. Compressing the window rather than waiting out the real
    // ten seconds is what keeps this affordable; see kCompressedRemoteIdleMs for
    // why it cannot be compressed further against a field dated to the second.
    m_monitor.setRemoteIdleThresholdMs(kCompressedRemoteIdleMs);
    const int agesBefore = static_cast<int>(ages.size());
    QTest::qWait(kQuietWindowMs);
    QTRY_COMPARE_WITH_TIMEOUT(m_monitor.stateFor(m_activityDev, m_activityTerm),
                              asInt(AgentState::Idle), kRelayTimeoutMs);
    // Proof of WHERE that Idle came from: a reading taken after the wait, whose
    // age the daemon measured against its own `nowMs`, past the compressed
    // window. Nothing in this process's clock takes part in the comparison.
    QVERIFY2(ages.size() > agesBefore, "no poll landed during the quiet window");
    QVERIFY2(ages.constLast() >= kCompressedRemoteIdleMs,
             qPrintable(QStringLiteral("last server-observed age %1 ms is inside "
                                       "the compressed %2 ms window")
                            .arg(ages.constLast())
                            .arg(kCompressedRemoteIdleMs)));
    qInfo("quiet pane: %lld ms since the server last saw output",
          ages.constLast());

    // ...and the sidebar follows it down. Idle, with no connection information
    // at all, is the row a Dev Session nobody is watching should read.
    terminals[0].agent = static_cast<AgentState>(
        m_monitor.stateFor(m_activityDev, m_activityTerm));
    QCOMPARE(asInt(SessionsModel::aggregateSessionState(terminals)),
             asInt(SessionRowState::Idle));
    QCOMPARE(asInt(modelRowStateFor(m_activityDev, terminals)),
             asInt(SessionRowState::Idle));

    // The poller is a stack object whose signals reach a member monitor; drop
    // its peer before it goes out of scope so no answer can land in a lambda
    // whose captures are already gone.
    poller.setDevSessionIds({});
    poller.setRpcClient(nullptr);
}

QTEST_GUILESS_MAIN(TstLiveAgent)
#include "tst_liveagent.moc"
