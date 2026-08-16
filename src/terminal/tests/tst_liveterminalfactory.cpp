// LIVE gate for ch::TerminalFactory (SPEC 5.1-5.3): the production attach path
// a terminal pane actually takes — factory.attach() opens a real PTY channel on
// a real SSH session, runs the pane's tmux command in it, and binds it to the
// pane's controller — with the bytes observed where the renderer sees them: on
// ch::TerminalBridge::write(), i.e. after the controller's SPEC 5.5 buffering
// and the bridge's UTF-8 decode.
//
// tst_liveterminal proves the transport seam by hand; this proves the object
// the QML pane calls does the same thing, plus what only it owns: detach()
// releasing the channel while the remote session survives, and kill() actually
// destroying that session (verified by asking the server, not by assuming).
//
// Skipped wholesale unless CH_LIVE_SSH is set, so the default suite stays green
// on a machine with no fixture.

#include "KnownHosts.h"
#include "SessionState.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"
#include "TerminalBridge.h"
#include "TerminalController.h"
#include "TerminalFactory.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QtTest/QtTest>

#include <functional>

using ch::KnownHosts;
using ch::SshChannelDevice;
using ch::SshConnectionPool;
using ch::TerminalBridge;
using ch::TerminalController;
using ch::TerminalFactory;
using ch::TerminalState;

namespace {

// A real TCP connect + SSH handshake + a cold tmux server and login shell.
constexpr int kAttachTimeoutMs = 60000;
// Typing a line into a live pane and seeing the result come back.
constexpr int kCommandTimeoutMs = 30000;
// A one-shot exec channel (out-of-band probes).
constexpr int kExecTimeoutMs = 20000;
// Re-type an unanswered command this often; the first keystrokes can land
// before the shell inside a freshly created tmux session has readline up.
constexpr int kRetypeIntervalMs = 5000;
// How long a surviving marker process gets to prove itself by writing its next
// line. It writes one a second, so anything under a couple of seconds turns a
// healthy pane into a false report of destroyed work; this is deliberately many
// times that, because a genuine loss is still caught, only later.
constexpr int kMarkerGrowthTimeoutMs = 15000;

QString env(const char* key)
{
    return qEnvironmentVariable(key);
}

} // namespace

class TstLiveTerminalFactory : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void factoryAttachDeliversRemoteMarkerThroughTheBridge();
    void detachReleasesTheChannelButKeepsTheRemoteSession();
    void anUncleanNetworkDropKeepsTheSessionItsWorkAndSaysNothing();
    void killWithoutAConnectionKeepsTheTargetAndTheSession();
    void killDestroysTheRemoteSession();

private:
    void ensureConnected();
    bool typeUntil(const QByteArray& command, const std::function<bool()>& done,
                   int timeoutMs);
    QByteArray runExec(const QString& command, int timeoutMs);
    bool remoteSessionExists();
    // tmux's own session_created for the pane's target, in whole seconds, or an
    // empty string when there is no such session. This is the exact number
    // TerminalFactory's recreated-session probe compares against, so a test can
    // hold the verdict without standing up a codeharbord to answer
    // tmux.listSessions.
    QString remoteSessionCreatedAt();
    // Sever the SSH transport the way a dropped network does: no
    // disconnectFromHost(), no tmux detach, no orderly channel close. Every
    // descendant of the FIXTURE sshd is killed, which drops the connection under
    // libssh while the listener stays up so the client can come back — the
    // shape of the incident this case exists for.
    bool severTheNetwork();

    SshConnectionPool m_pool;
    TerminalFactory* m_factory = nullptr;
    QObject* m_pane = nullptr;  // stands in for the QML pane that owns the pair
    TerminalController* m_controller = nullptr;
    TerminalBridge* m_bridge = nullptr;
    // Everything the RENDERER would have received. Every assertion below reads
    // this, so the data traversed controller buffering + bridge decoding.
    QString m_rendered;
    QString m_factoryErrors;

    QString m_host;
    quint16 m_port = 0;
    QString m_user;
    QString m_repo;
    QString m_knownHostsPath;

    QString m_devSessionId;
    QString m_terminalId;
    QString m_target;
    QString m_marker;
};

void TstLiveTerminalFactory::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH is not set; live terminal factory gate skipped");
    if (!SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; live terminal factory gate skipped");

    m_host = env("CH_LIVE_HOST");
    m_port = static_cast<quint16>(env("CH_LIVE_PORT").toUInt());
    m_user = env("CH_LIVE_USER");
    m_repo = env("CH_LIVE_REPO");
    m_knownHostsPath = env("CH_LIVE_KNOWN_HOSTS");
    if (m_knownHostsPath.isEmpty()) {
        m_knownHostsPath =
            QDir::temp().filePath(QStringLiteral("ch_live_terminal_factory_known_hosts"));
    }

    QVERIFY2(!m_host.isEmpty() && m_port != 0 && !m_user.isEmpty() && !m_repo.isEmpty(),
             "CH_LIVE_HOST/PORT/USER/REPO must all be set");

    // Unique per run: the pid pins concurrent runs apart, the clock tail pins
    // back-to-back runs of the same pid apart.
    //
    // The target is minted here in codeharbord's own shape (`ch_<devSessionId>_
    // <terminal pane row id>`, mintTmuxTarget in remote/src/workspace.ts) and
    // handed straight to attach(). In production the same string is READ from
    // the pane's `terminal_panes` row, which needs a workspace database this
    // gate has no business standing up: what it exercises is the attach path
    // below the resolution — a real PTY channel, a real tmux session, detach
    // and kill against the real server.
    m_devSessionId = QStringLiteral("livefactory");
    m_terminalId = QStringLiteral("t%1x%2")
                       .arg(QCoreApplication::applicationPid())
                       .arg(QDateTime::currentMSecsSinceEpoch() % 1000000);
    m_target = QStringLiteral("ch_%1_%2").arg(m_devSessionId, m_terminalId);
    m_marker = QStringLiteral("CH_FACTORY_MARKER_") + m_terminalId;

    // Exactly the QML pane's shape: one factory, a pane object owning the
    // controller and its WebChannel bridge.
    m_factory = new TerminalFactory(&m_pool, this);
    m_pane = new QObject(this);
    m_controller = m_factory->create(m_pane);
    m_bridge = m_factory->createBridge(m_controller, m_pane);
    QVERIFY(m_controller && m_bridge);

    // Collect what the renderer would see, and hand the byte weight back as the
    // page does. Without the acknowledgement the controller stops emitting once
    // kMaxUnacknowledgedBytes is outstanding (SPEC 5.4) and this gate would
    // stop seeing remote output half a megabyte into a run. Posted rather than
    // called inline, exactly as the page's own acknowledgement is: releasing
    // retained output re-enters write(), and answering from inside the emission
    // would recurse.
    connect(m_bridge, &TerminalBridge::write, this,
            [this](const QString& text, int bytes) {
                m_rendered += text;
                QMetaObject::invokeMethod(
                    m_bridge, [this, bytes]() { m_bridge->notifyOutputConsumed(bytes); },
                    Qt::QueuedConnection);
            });
    connect(m_factory, &TerminalFactory::error, this,
            [this](TerminalController*, const QString& text) { m_factoryErrors += text; });

    // The page has mounted, as far as the bridge is concerned: without this the
    // controller buffers instead of emitting write() (SPEC 5.4).
    m_bridge->ready();
}

void TstLiveTerminalFactory::cleanupTestCase()
{
    if (m_factory)
        m_factory->detach(m_controller);
    if (m_target.isEmpty() || m_pool.state() != SshConnectionPool::State::Connected) {
        m_pool.disconnectFromHost();
        return;
    }

    // Deterministic teardown: nothing this run created may outlive it, and the
    // remote is the one that says so.
    const QByteArray verdict = runExec(
        QStringLiteral("tmux kill-session -t '%1' >/dev/null 2>&1; "
                       "tmux has-session -t '%1' >/dev/null 2>&1 "
                       "&& echo CH_SESSION_ALIVE || echo CH_SESSION_GONE")
            .arg(m_target),
        kExecTimeoutMs);
    qInfo().noquote() << "cleanup" << m_target << "->" << verdict.trimmed();
    QCOMPARE(verdict.trimmed(), QByteArray("CH_SESSION_GONE"));

    m_pool.disconnectFromHost();
}

void TstLiveTerminalFactory::ensureConnected()
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
    m_pool.setHostKeyCallback([](const QString&, const QString&, const QByteArray&,
                                 KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });

    QString failure;
    const auto conn = connect(&m_pool, &SshConnectionPool::errorOccurred,
                              [&failure](const QString& text) { failure += text; });
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

bool TstLiveTerminalFactory::typeUntil(const QByteArray& command,
                                       const std::function<bool()>& done, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    qint64 nextSend = 0;
    while (!done() && clock.elapsed() < timeoutMs) {
        if (clock.elapsed() >= nextSend) {
            // Through the BRIDGE, i.e. the same slot the xterm.js page calls.
            m_bridge->sendInput(QString::fromLatin1(command) + QLatin1Char('\n'));
            nextSend = clock.elapsed() + kRetypeIntervalMs;
        }
        QTest::qWait(100);
    }
    return done();
}

QByteArray TstLiveTerminalFactory::runExec(const QString& command, int timeoutMs)
{
    SshChannelDevice device(&m_pool);
    QByteArray out;
    bool finished = false;
    connect(&device, &SshChannelDevice::readyRead, &device,
            [&out, &device]() { out += device.readAll(); });
    connect(&device, &SshChannelDevice::readChannelFinished, &device,
            [&finished]() { finished = true; });

    if (!device.startExec(command))
        return QByteArray();

    QElapsedTimer clock;
    clock.start();
    while (!finished && clock.elapsed() < timeoutMs)
        QTest::qWait(50);

    out += device.readAll();
    device.closeChannel();
    return out;
}

bool TstLiveTerminalFactory::remoteSessionExists()
{
    const QByteArray verdict =
        runExec(QStringLiteral("tmux has-session -t '%1' >/dev/null 2>&1 "
                               "&& echo CH_SESSION_ALIVE || echo CH_SESSION_GONE")
                    .arg(m_target),
                kExecTimeoutMs);
    return verdict.trimmed() == QByteArrayLiteral("CH_SESSION_ALIVE");
}

QString TstLiveTerminalFactory::remoteSessionCreatedAt()
{
    // The `=` exact-match prefix, as everywhere a target is composed in this
    // product: without it tmux falls back to prefix and fnmatch matching and
    // would answer about somebody else's session.
    const QByteArray answer =
        runExec(QStringLiteral("tmux display-message -p -t '=%1:' '#{session_created}' "
                               "2>/dev/null")
                    .arg(m_target),
                kExecTimeoutMs);
    return QString::fromLatin1(answer.trimmed());
}

bool TstLiveTerminalFactory::severTheNetwork()
{
    // Every DESCENDANT of the fixture sshd, and deliberately not the listener
    // itself: the connection dies where the client can see nothing but a
    // transport that stopped, while the port stays open so the reconnect below
    // has something to come back to. No disconnectFromHost(), no tmux detach,
    // no orderly channel close — the client is told nothing, which is the whole
    // point of the case.
    //
    // `ssh[d]` rather than `sshd` so this script's OWN command line cannot match
    // the pattern it passes to pgrep, which would send the kill at the shell
    // running it.
    const QString fixtureDir = QFileInfo(env("CH_LIVE_IDENTITY")).absolutePath();
    if (fixtureDir.isEmpty() || !QFile::exists(fixtureDir + QStringLiteral("/sshd_config")))
        return false;
    const QString script =
        QStringLiteral("set -e\n"
                       "listeners=$(pgrep -f \"ssh[d] -f %1/sshd_config\" || true)\n"
                       "[ -n \"$listeners\" ] || exit 1\n"
                       "for l in $listeners; do\n"
                       "  for c in $(pgrep -P \"$l\" || true); do\n"
                       "    for g in $(pgrep -P \"$c\" || true); do kill -9 \"$g\" || true; done\n"
                       "    kill -9 \"$c\" || true\n"
                       "  done\n"
                       "done\n")
            .arg(fixtureDir);
    QProcess sh;
    sh.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script});
    if (!sh.waitForFinished(10000))
        return false;
    return sh.exitStatus() == QProcess::NormalExit && sh.exitCode() == 0;
}

// (a) The production attach: no hand-rolled channel anywhere. The factory opens
// the PTY, runs the pane's tmux target, and the marker comes back out of the
// bridge — proving the whole pane object graph carries live remote bytes.
void TstLiveTerminalFactory::factoryAttachDeliversRemoteMarkerThroughTheBridge()
{
    ensureConnected();
    QVERIFY(m_factory->connected());

    QVERIFY2(m_factory->attach(m_controller, m_target, m_repo, 100, 30),
             qPrintable(m_factoryErrors));

    // The factory recorded what it attached, which is what kill() destroys.
    QCOMPARE(m_factory->targetFor(m_controller), m_target);
    QVERIFY(m_controller->transport() != nullptr);
    // The size it opened with is the pane's, and the controller knows it so a
    // reconnect can re-assert it (SPEC 5.6).
    QCOMPARE(m_controller->columns(), 100);
    QCOMPARE(m_controller->rows(), 30);

    // tmux drawing itself is the pane coming up: the factory promotes the pane
    // to Ready on the first bytes, and the renderer sees them as text.
    QTRY_VERIFY_WITH_TIMEOUT(!m_rendered.isEmpty(), kAttachTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(m_controller->state() == TerminalState::Ready, kAttachTimeoutMs);
    QCOMPARE(m_bridge->connectionState(), ch::toString(TerminalState::Ready));

    // The typed line is "printf '%s_%s\n' CH_FACTORY MARKER_<id>", so the marker
    // string only exists once printf has RUN remotely: the terminal echoing our
    // own keystrokes cannot satisfy this.
    const QByteArray command =
        QByteArrayLiteral("printf '%s_%s\\n' CH_FACTORY MARKER_")
        + m_terminalId.toLatin1();
    QVERIFY2(typeUntil(
                 command, [this]() { return m_rendered.contains(m_marker); },
                 kCommandTimeoutMs),
             qPrintable(QStringLiteral("marker %1 never arrived; errors=%2; tail=%3")
                            .arg(m_marker, m_factoryErrors, m_rendered.right(400))));
    qInfo().noquote() << "attach: recovered marker" << m_marker << "through the bridge";

    // The remote really does own a session under the pane's tmux target.
    QVERIFY2(remoteSessionExists(), qPrintable(m_target));
    QVERIFY2(m_factoryErrors.isEmpty(), qPrintable(m_factoryErrors));
}

// (b) detach() is the pane going away, not the work: the channel must be
// released and the pane reported dropped, while the remote session keeps
// running (that is the whole point of tmux, SPEC 5.2).
void TstLiveTerminalFactory::detachReleasesTheChannelButKeepsTheRemoteSession()
{
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    QVERIFY(m_controller->transport() != nullptr);

    m_factory->detach(m_controller);

    QVERIFY(m_controller->transport() == nullptr);
    QVERIFY(m_controller->state() == TerminalState::Disconnected);
    // Nothing can be typed into a detached pane.
    QVERIFY(!m_controller->sendInput(QByteArrayLiteral("noop\n")));
    // The target survives the detach: it is what kill() still needs.
    QCOMPARE(m_factory->targetFor(m_controller), m_target);

    QVERIFY2(remoteSessionExists(),
             "detach() must not take the remote tmux session down with it");
    qInfo().noquote() << "detach: channel released," << m_target << "still alive remotely";

    // Re-attaching through the factory returns to the SAME session, and the
    // scrollback tmux kept proves it is the same pane, not a fresh one.
    m_rendered.clear();
    QVERIFY2(m_factory->attach(m_controller, m_target, m_repo, 100, 30),
             qPrintable(m_factoryErrors));
    QTRY_VERIFY_WITH_TIMEOUT(!m_rendered.isEmpty(), kAttachTimeoutMs);
    QVERIFY2(typeUntil(
                 QByteArrayLiteral("tmux capture-pane -p -S -200"),
                 [this]() { return m_rendered.contains(m_marker); }, kCommandTimeoutMs),
             qPrintable(QStringLiteral("marker %1 did not survive the detach; tail=%2")
                            .arg(m_marker, m_rendered.right(400))));
    qInfo().noquote() << "detach: re-attached and recovered" << m_marker;
}

// (b2) THE INCIDENT. A user's network dropped, and the pane came back announcing
// that the remote session was gone and their work had stopped. Two hypotheses:
// something destroys tmux sessions when the transport dies, or the detection
// lies. This case settles the first one against a real server, with a real
// process running inside the session and a real transport death — no
// disconnectFromHost(), no tmux detach, no orderly channel close, exactly what a
// dropped network gives the client.
//
// It also holds the SECOND one at the level this gate can reach: the pane's
// verdict is a comparison of tmux's session_created values (ch::TerminalFactory
// probeForRecreatedSession), so an unchanged creation time across the drop is
// precisely the evidence that says "the same session, nothing lost" and no
// notice is possible.
void TstLiveTerminalFactory::anUncleanNetworkDropKeepsTheSessionItsWorkAndSaysNothing()
{
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    QVERIFY(m_controller->transport() != nullptr);

    // A process INSIDE the tmux session that leaves proof it kept running: one
    // line a second into a file this test reads over its own exec channel.
    // Deliberately no `nohup` and no `setsid` — it must die with the session when
    // the kill case below destroys it rather than outliving the run, and the log
    // path carries the run's unique id so the cleanup can name every copy.
    const QString log = QStringLiteral("/tmp/ch_drop_%1.log").arg(m_terminalId);
    const QByteArray heartbeat =
        QStringLiteral("sh -c 'while :; do date +%%s >> %1; sleep 1; done' &")
            .arg(log)
            .toLatin1();
    const auto heartbeatLines = [this, &log]() {
        return runExec(QStringLiteral("wc -l < %1 2>/dev/null || echo 0").arg(log),
                       kExecTimeoutMs)
            .trimmed()
            .toInt();
    };
    QVERIFY2(typeUntil(heartbeat, [&heartbeatLines]() { return heartbeatLines() >= 2; },
                       kCommandTimeoutMs),
             qPrintable(QStringLiteral("the marker process never wrote to %1").arg(log)));

    const QString createdBefore = remoteSessionCreatedAt();
    QVERIFY2(!createdBefore.isEmpty(), qPrintable(m_target));
    const int linesBefore = heartbeatLines();
    qInfo().noquote() << "before the drop:" << m_target << "created" << createdBefore
                      << "marker lines" << linesBefore;

    QSignalSpy recreated(m_factory, &TerminalFactory::sessionRecreated);

    // ---- the network goes away -------------------------------------------
    QVERIFY2(severTheNetwork(),
             "could not find the fixture sshd to sever; this case needs the local "
             "fixture from tests/live/generate-fixture.sh");

    // The pane must NOTICE. A channel that stops delivering while the pane still
    // reports Ready is its own bug: the user would type into a dead terminal.
    QTRY_VERIFY_WITH_TIMEOUT(!TerminalController::isLiveState(m_controller->state()),
                             kAttachTimeoutMs);
    qInfo().noquote() << "after the drop: pane state" << ch::toString(m_controller->state())
                      << "pool state" << static_cast<int>(m_pool.state());

    // The pool may still believe it holds a session: libssh learns of a dead
    // socket when it next uses it. ch::SessionBootstrap::handleConnectionLost
    // drops the session before arming its retry for exactly this reason, and
    // that is what is imitated here — the reconnect must start from a pool that
    // is not pretending.
    if (m_pool.state() == SshConnectionPool::State::Connected)
        m_pool.disconnectFromHost();

    // ---- and comes back ---------------------------------------------------
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    m_rendered.clear();
    QVERIFY2(m_factory->attach(m_controller, m_target, m_repo, 100, 30),
             qPrintable(m_factoryErrors));
    QTRY_VERIFY_WITH_TIMEOUT(!m_rendered.isEmpty(), kAttachTimeoutMs);

    // 1. The session SURVIVED, and this attach found it rather than creating a
    // replacement. tmux never changes a session's creation time, so an unchanged
    // one is the same session — the check the pane's own notice is built on.
    const QString createdAfter = remoteSessionCreatedAt();
    QCOMPARE(createdAfter, createdBefore);

    // 2. The work inside it kept running the whole time, which is the part no
    // timestamp can tell us.
    //
    // WAITED FOR, never sampled once. The marker writes one line a SECOND, and a
    // drop plus a reconnect plus an attach can complete well inside that second,
    // so a surviving process is legitimately still on its old count the instant
    // the attach returns. Reading it once therefore fails at random and, worse,
    // fails by claiming the user's work died — the most alarming thing this
    // suite can say, and it must never be said on a race. Verified by hand
    // against plain ssh + tmux: across the same unclean drop the marker went
    // from 3 lines to 8 while its pane stayed alive, so survival is the truth
    // this waits for. The bound is generous for the same reason: a real loss
    // still fails, just a few seconds later.
    QTRY_VERIFY_WITH_TIMEOUT(heartbeatLines() > linesBefore, kMarkerGrowthTimeoutMs);
    // Read once more AFTER the wait, so the diagnostic below reports the count
    // the assertion actually accepted rather than the stale pre-drop one.
    const int linesAfter = heartbeatLines();

    // 3. And it is the user's own shell they are back in: tmux still holds the
    // scrollback from before the drop.
    QVERIFY2(typeUntil(
                 QByteArrayLiteral("tmux capture-pane -p -S -200"),
                 [this]() { return m_rendered.contains(m_marker); }, kCommandTimeoutMs),
             qPrintable(QStringLiteral("marker %1 did not survive the drop; tail=%2")
                            .arg(m_marker, m_rendered.right(400))));

    // 4. Nothing was claimed about lost work, because nothing was lost.
    QCOMPARE(recreated.count(), 0);
    QVERIFY2(m_factoryErrors.isEmpty() || !m_factoryErrors.contains(QStringLiteral("gone")),
             qPrintable(m_factoryErrors));
    qInfo().noquote() << "after the reconnect:" << m_target << "created" << createdAfter
                      << "marker lines" << linesAfter << "(survived)";

    // The heartbeat has made its point. Every copy of it is named by the log
    // path, so one pattern ends them all whether typeUntil retyped or not.
    runExec(QStringLiteral("pkill -f 'ch_drop_%1' >/dev/null 2>&1; rm -f %2")
                .arg(m_terminalId, log),
            kExecTimeoutMs);
    m_factoryErrors.clear();
}

// (c1) A kill that cannot run must not pretend it did. The pane's tmux target
// is the ONLY handle anything still has on the remote session, so forgetting it
// on a kill that never reached the server strands the user's processes: the
// session keeps running, and neither Retry nor any later kill can name it any
// more. Proved against the real server: drop the connection, ask for a kill,
// reconnect, and check that both the target and the session are still there.
void TstLiveTerminalFactory::killWithoutAConnectionKeepsTheTargetAndTheSession()
{
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    QCOMPARE(m_factory->targetFor(m_controller), m_target);

    m_pool.disconnectFromHost();
    QVERIFY(!m_factory->connected());

    m_factoryErrors.clear();
    m_factory->kill(m_controller);

    // The refusal is reported rather than swallowed...
    QVERIFY2(m_factoryErrors.contains(m_target),
             qPrintable(QStringLiteral("kill() said nothing useful: '%1'").arg(m_factoryErrors)));
    // ...and the pane can still name what it failed to destroy.
    QCOMPARE(m_factory->targetFor(m_controller), m_target);
    m_factoryErrors.clear();

    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    QVERIFY2(remoteSessionExists(),
             "a kill that never ran must leave the remote session alone");
    qInfo().noquote() << "kill refusal: target" << m_target << "and session both survived";
}

// (c) kill() destroys the remote session for real; the server is asked, not
// trusted.
void TstLiveTerminalFactory::killDestroysTheRemoteSession()
{
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    QVERIFY(remoteSessionExists());

    m_factory->kill(m_controller);

    QVERIFY(m_controller->transport() == nullptr);
    // Nothing left to kill twice.
    QVERIFY(m_factory->targetFor(m_controller).isEmpty());

    // The kill travels its own exec channel, so give the remote a moment.
    QElapsedTimer clock;
    clock.start();
    bool gone = false;
    while (!gone && clock.elapsed() < kExecTimeoutMs) {
        gone = !remoteSessionExists();
        if (!gone)
            QTest::qWait(250);
    }
    QVERIFY2(gone, qPrintable(QStringLiteral("tmux session %1 outlived kill(); errors=%2")
                                  .arg(m_target, m_factoryErrors)));
    qInfo().noquote() << "kill: remote session" << m_target << "is gone";

    // Already destroyed: cleanupTestCase() must not need it, and asking again
    // must not resurrect an entry.
    m_target.clear();
}

QTEST_GUILESS_MAIN(TstLiveTerminalFactory)
#include "tst_liveterminalfactory.moc"
