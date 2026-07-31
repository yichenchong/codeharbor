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
    void killWithoutAConnectionKeepsTheTargetAndTheSession();
    void killDestroysTheRemoteSession();

private:
    void ensureConnected();
    bool typeUntil(const QByteArray& command, const std::function<bool()>& done,
                   int timeoutMs);
    QByteArray runExec(const QString& command, int timeoutMs);
    bool remoteSessionExists();

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
    m_devSessionId = QStringLiteral("livefactory");
    m_terminalId = QStringLiteral("t%1x%2")
                       .arg(QCoreApplication::applicationPid())
                       .arg(QDateTime::currentMSecsSinceEpoch() % 1000000);
    m_target = TerminalController::tmuxTarget(ch::DevSessionId{m_devSessionId},
                                              ch::TerminalId{m_terminalId});
    m_marker = QStringLiteral("CH_FACTORY_MARKER_") + m_terminalId;

    // Exactly the QML pane's shape: one factory, a pane object owning the
    // controller and its WebChannel bridge.
    m_factory = new TerminalFactory(&m_pool, this);
    m_pane = new QObject(this);
    m_controller = m_factory->create(m_pane);
    m_bridge = m_factory->createBridge(m_controller, m_pane);
    QVERIFY(m_controller && m_bridge);

    connect(m_bridge, &TerminalBridge::write, this,
            [this](const QString& text) { m_rendered += text; });
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
    SshChannelDevice device(&m_pool, SshConnectionPool::ChannelKind::Exec);
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

// (a) The production attach: no hand-rolled channel anywhere. The factory opens
// the PTY, runs the pane's tmux target, and the marker comes back out of the
// bridge — proving the whole pane object graph carries live remote bytes.
void TstLiveTerminalFactory::factoryAttachDeliversRemoteMarkerThroughTheBridge()
{
    ensureConnected();
    QVERIFY(m_factory->connected());

    QVERIFY2(m_factory->attach(m_controller, m_devSessionId, m_terminalId, m_repo, 100, 30),
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
    QVERIFY2(m_factory->attach(m_controller, m_devSessionId, m_terminalId, m_repo, 100, 30),
             qPrintable(m_factoryErrors));
    QTRY_VERIFY_WITH_TIMEOUT(!m_rendered.isEmpty(), kAttachTimeoutMs);
    QVERIFY2(typeUntil(
                 QByteArrayLiteral("tmux capture-pane -p -S -200"),
                 [this]() { return m_rendered.contains(m_marker); }, kCommandTimeoutMs),
             qPrintable(QStringLiteral("marker %1 did not survive the detach; tail=%2")
                            .arg(m_marker, m_rendered.right(400))));
    qInfo().noquote() << "detach: re-attached and recovered" << m_marker;
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
