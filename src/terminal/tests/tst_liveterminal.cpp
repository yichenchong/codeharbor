#include "KnownHosts.h"
#include "SessionState.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"
#include "TerminalController.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QString>
#include <QtTest/QtTest>

#include <functional>

using ch::KnownHosts;
using ch::SshChannelDevice;
using ch::SshConnectionPool;
using ch::TerminalController;
using ch::TerminalState;

namespace {

// A real TCP connect + SSH handshake + a cold tmux server and login shell.
constexpr int kAttachTimeoutMs = 60000;
// Typing a line into a live pane and seeing the result echo back.
constexpr int kCommandTimeoutMs = 30000;
// A one-shot exec channel (cleanup / out-of-band probes).
constexpr int kExecTimeoutMs = 20000;
// Re-type an unanswered command this often; the very first keystrokes can land
// before the shell inside a freshly created tmux session has readline up.
constexpr int kRetypeIntervalMs = 5000;

QString env(const char* key)
{
    return qEnvironmentVariable(key);
}

// Last capture group of `pattern` over `haystack`. tmux redraws re-emit older
// screen content, so the newest match is the one a probe just produced.
QByteArray lastCapture(const QByteArray& haystack, const QString& pattern)
{
    const QRegularExpression re(pattern);
    QString last;
    auto it = re.globalMatch(QString::fromLatin1(haystack));
    while (it.hasNext())
        last = it.next().captured(1);
    return last.toLatin1();
}

} // namespace

// LIVE gate for workstream T (SPEC 5.1-5.6): a real tmux session, inside a real
// SSH PTY, driven end to end through the production TerminalController transport
// seam. Nothing here is simulated — the pane's bytes come off a socket, the
// window-change is an SSH channel request, and the persistence claim is checked
// by re-attaching a brand new channel to a session the fixture kept alive.
//
// Skipped wholesale unless CH_LIVE_SSH is set, so the default suite stays green
// on a machine with no fixture.
class TstLiveTerminal : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void attachDeliversRemoteMarkerThroughController();
    void resizeIsObservedByTheRemotePty();
    void reattachAfterDropRecoversPaneAndProcess();

private:
    void ensureConnected();
    // Open a PTY channel onto the pane's tmux session and bind it to the
    // controller. `startBeforeTransport` mirrors a reconnect helper that opens
    // the channel at a default size and lets the controller re-assert the pane
    // geometry it already knows.
    void attachPane(int cols, int rows, bool startBeforeTransport = false);
    void dropPane();
    bool runInPane(const QByteArray& command, const std::function<bool()>& done,
                   int timeoutMs);
    bool runInPane(const QByteArray& command, const QByteArray& expect,
                   int timeoutMs);
    QByteArray paneShellPid(const QByteArray& tag);
    QByteArray runExec(const QString& command, int timeoutMs);
    // "<tag>=<client_width>x<client_height>" as reported from inside the pane:
    // the remote's own view of the PTY window, i.e. what the resize must move.
    // Each probe gets a fresh `tag` so a tmux redraw of an earlier answer can
    // never stand in for a new one.
    bool paneReportsSize(const QByteArray& tag, int cols, int rows, int timeoutMs);

    SshConnectionPool m_pool;
    TerminalController m_controller;
    SshChannelDevice* m_device = nullptr;
    // Everything the controller flushed towards the renderer. This is the ONLY
    // place the test reads terminal bytes, so every assertion below is on data
    // that traversed TerminalController's SPEC 5.5 buffering path.
    QByteArray m_out;
    QString m_channelErrors;

    QString m_host;
    quint16 m_port = 0;
    QString m_user;
    QString m_repo;
    QString m_knownHostsPath;

    QString m_sessionName;
    QString m_attachCommand;
    QByteArray m_marker;
    QByteArray m_shellPid;
};

void TstLiveTerminal::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH is not set; live terminal gate skipped");
    if (!SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; live terminal gate skipped");

    m_host = env("CH_LIVE_HOST");
    m_port = static_cast<quint16>(env("CH_LIVE_PORT").toUInt());
    m_user = env("CH_LIVE_USER");
    m_repo = env("CH_LIVE_REPO");
    m_knownHostsPath = env("CH_LIVE_KNOWN_HOSTS");
    if (m_knownHostsPath.isEmpty()) {
        m_knownHostsPath =
            QDir::temp().filePath(QStringLiteral("ch_live_terminal_known_hosts"));
    }

    QVERIFY2(!m_host.isEmpty() && m_port != 0 && !m_user.isEmpty()
                 && !m_repo.isEmpty(),
             "CH_LIVE_HOST/PORT/USER/REPO must all be set");

    // Unique per run so repeat runs never collide and never inherit a stale
    // session: the pid pins concurrent runs apart, the clock tail pins
    // back-to-back runs of the same pid apart.
    const QString terminalRowId =
        QStringLiteral("t%1x%2")
            .arg(QCoreApplication::applicationPid())
            .arg(QDateTime::currentMSecsSinceEpoch() % 1000000);

    // The target is minted the way codeharbord mints it — `ch_<devSessionId>_
    // <terminal pane row id>` (mintTmuxTarget in remote/src/workspace.ts) — but
    // it is minted HERE rather than fetched, because this gate has no workspace
    // database: it drives the controller and a raw PTY channel directly, one
    // layer below ch::TerminalFactory (which is what tst_liveterminalfactory
    // covers). What matters for this test is that a real tmux session comes up
    // under a real name and survives a detach; the production helper that
    // builds the attach command around that name is still the one under test.
    m_sessionName = QStringLiteral("ch_live_") + terminalRowId;
    // The identity handed to the builder is this gate's own: no workspace means
    // no server-minted Dev Session id, so the "dev session" is the name this
    // test invented and the terminal id is the row id it minted above. It only
    // has to be non-empty for the two OMP_* variables to be exported.
    m_attachCommand = TerminalController::tmuxNewSessionCommand(
        m_sessionName, m_repo, QStringLiteral("ch_live"), terminalRowId);
    m_marker = QByteArrayLiteral("CH_LIVE_MARKER_") + terminalRowId.toLatin1();

    // Collect what the renderer would see, and acknowledge it as the renderer
    // does. The acknowledgement is not decoration: the controller stops
    // emitting once kMaxUnacknowledgedBytes of output is outstanding (SPEC
    // 5.4), so a gate that never answered would silently stop seeing remote
    // bytes half a megabyte into a run and time out on a marker that had
    // already been printed. Posted rather than called inline, exactly as the
    // page's own acknowledgement is: releasing retained output re-enters
    // flushReady, and doing that from inside the emission would recurse.
    connect(&m_controller, &TerminalController::flushReady, this,
            [this](const QByteArray& batch) {
                m_out += batch;
                const qint64 bytes = batch.size();
                QMetaObject::invokeMethod(
                    &m_controller, [this, bytes]() { m_controller.acknowledgeOutput(bytes); },
                    Qt::QueuedConnection);
            });

    // This gate drives the SPEC 5.6 states BY HAND and deliberately parks the
    // pane in AttachingTmux while it types into it and waits for remote answers
    // — minutes, in the worst case it budgets for. That is exactly the shape the
    // controller's attach watchdog exists to end (a pane that is attaching and
    // producing nothing), so the bound is switched off here: what it protects is
    // pinned by tst_terminalcontroller, and leaving it armed would let a slow
    // fixture turn a passing run into a pane that reported Error halfway.
    m_controller.setAttachTimeoutMs(0);
}

void TstLiveTerminal::cleanupTestCase()
{
    m_controller.setTransport(nullptr);
    delete m_device;
    m_device = nullptr;

    if (m_sessionName.isEmpty()
        || m_pool.state() != SshConnectionPool::State::Connected) {
        m_pool.disconnectFromHost();
        return;
    }

    // (d) Deterministic teardown: the session this run created must not outlive
    // it. Verified, not assumed — has-session is the remote's own answer.
    const QByteArray verdict = runExec(
        QStringLiteral("tmux kill-session -t '%1' >/dev/null 2>&1; "
                       "tmux has-session -t '%1' >/dev/null 2>&1 "
                       "&& echo CH_SESSION_ALIVE || echo CH_SESSION_GONE")
            .arg(m_sessionName),
        kExecTimeoutMs);
    qInfo().noquote() << "cleanup" << m_sessionName << "->" << verdict.trimmed();
    QCOMPARE(verdict.trimmed(), QByteArray("CH_SESSION_GONE"));

    m_pool.disconnectFromHost();
}

void TstLiveTerminal::ensureConnected()
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

void TstLiveTerminal::attachPane(int cols, int rows, bool startBeforeTransport)
{
    QVERIFY(!m_device);
    m_device = new SshChannelDevice(&m_pool, this);
    connect(m_device, &SshChannelDevice::channelError, this,
            [this](const QString& text) { m_channelErrors += text; });

    m_controller.setState(TerminalState::OpeningChannel);
    if (!startBeforeTransport)
        m_controller.setTransport(m_device);

    const bool started = m_device->startPty(QStringLiteral("xterm-256color"), cols,
                                            rows, m_attachCommand);
    QVERIFY2(started, qPrintable(m_attachCommand + QStringLiteral(" | ")
                                 + m_channelErrors));

    if (startBeforeTransport) {
        // Binding after the channel is live is the reconnect ordering: the
        // controller drains what the device already buffered and re-asserts the
        // geometry the renderer last reported.
        m_controller.setTransport(m_device);
    }
    QCOMPARE(m_controller.transport(), static_cast<QIODevice*>(m_device));
    m_controller.setState(TerminalState::AttachingTmux);
}

void TstLiveTerminal::dropPane()
{
    QVERIFY(m_device);
    // Closing the channel is what a dropped connection looks like from the
    // client side: the pty master goes away, sshd hangs up the tmux CLIENT, and
    // the tmux SERVER (with our shell in it) keeps running detached.
    m_device->closeChannel();
    m_controller.setTransport(nullptr);
    delete m_device;
    m_device = nullptr;
}

bool TstLiveTerminal::runInPane(const QByteArray& command,
                                const std::function<bool()>& done, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    qint64 nextSend = 0;
    while (!done() && clock.elapsed() < timeoutMs) {
        if (clock.elapsed() >= nextSend) {
            if (!m_controller.sendInput(command + '\n'))
                return false; // no writable transport: nothing will ever arrive
            nextSend = clock.elapsed() + kRetypeIntervalMs;
        }
        QTest::qWait(100);
    }
    return done();
}

bool TstLiveTerminal::runInPane(const QByteArray& command,
                                const QByteArray& expect, int timeoutMs)
{
    return runInPane(
        command, [this, expect]() { return m_out.contains(expect); }, timeoutMs);
}

QByteArray TstLiveTerminal::paneShellPid(const QByteArray& tag)
{
    // printf, not echo: the typed line carries "<tag>=%s" while the OUTPUT
    // carries "<tag>=<digits>", so a match can only come from execution, never
    // from the terminal echoing our own keystrokes back.
    const QByteArray command = "printf '" + tag + "=%s\\n' $$";
    const QString pattern = QString::fromLatin1(tag) + QStringLiteral("=(\\d+)");
    QByteArray pid;
    runInPane(
        command,
        [this, pattern, &pid]() {
            pid = lastCapture(m_out, pattern);
            return !pid.isEmpty();
        },
        kCommandTimeoutMs);
    return pid;
}

bool TstLiveTerminal::paneReportsSize(const QByteArray& tag, int cols, int rows,
                                      int timeoutMs)
{
    // client_width/client_height is the remote tmux client's view of the PTY
    // window itself (pane_height would be short by the status line), so this is
    // the remote observing the SSH window-change, not us restating it.
    const QByteArray expect =
        tag + '=' + QByteArray::number(cols) + 'x' + QByteArray::number(rows);
    // The typed line carries "<tag>=#{client_width}..." while the answer carries
    // "<tag>=<digits>x<digits>", so the echo of our keystrokes cannot match.
    const QByteArray command = "tmux display-message -p '" + tag
        + "=#{client_width}x#{client_height}'";
    return runInPane(command, expect, timeoutMs);
}

QByteArray TstLiveTerminal::runExec(const QString& command, int timeoutMs)
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

// (a) ATTACH: a real PTY channel running tmux, with the pane's bytes reaching
// the renderer through TerminalController's transport seam and buffering path.
void TstLiveTerminal::attachDeliversRemoteMarkerThroughController()
{
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    attachPane(80, 24);
    // QVERIFY, not QCOMPARE: ch::toString(TerminalState) returns QString, which
    // QTest's generic formatter cannot consume (same reason as
    // tst_terminalcontroller).
    QVERIFY(m_controller.state() == TerminalState::AttachingTmux);

    // The pane must produce something on its own (tmux status line + shell
    // prompt) before keystrokes mean anything.
    QTRY_VERIFY_WITH_TIMEOUT(!m_out.isEmpty(), kAttachTimeoutMs);

    // The typed line is "printf '%s_%s\n' CH_LIVE MARKER_<id>"; the marker
    // "CH_LIVE_MARKER_<id>" only exists once printf has run remotely, so the
    // terminal echo of our own keystrokes cannot satisfy this.
    const QByteArray command =
        QByteArrayLiteral("printf '%s_%s\\n' CH_LIVE MARKER_")
        + m_marker.mid(QByteArrayLiteral("CH_LIVE_MARKER_").size());
    QVERIFY2(runInPane(command, m_marker, kCommandTimeoutMs),
             qPrintable(QStringLiteral("marker %1 never arrived; errors=%2; tail=%3")
                            .arg(QString::fromLatin1(m_marker), m_channelErrors,
                                 QString::fromLatin1(m_out.right(400)))));
    qInfo().noquote() << "attach: recovered marker" << m_marker;

    // The bytes travelled the controller's own path, not a direct device read.
    QVERIFY(m_out.contains(m_marker));
    QVERIFY(m_channelErrors.isEmpty());

    // Pin the pane's shell process so (c) can prove it is the SAME one.
    m_shellPid = paneShellPid(QByteArrayLiteral("CHPID"));
    QVERIFY2(!m_shellPid.isEmpty(), "pane shell pid was never reported");
    qInfo().noquote() << "attach: pane shell pid" << m_shellPid;

    m_controller.setState(TerminalState::Ready);
}

// (b) RESIZE: the window-change must be observed by the REMOTE side. Proved by
// reading the size from inside the pane before and after, so a stuck PTY cannot
// pass by coincidence.
void TstLiveTerminal::resizeIsObservedByTheRemotePty()
{
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    QVERIFY(m_device);

    QVERIFY2(paneReportsSize(QByteArrayLiteral("CHSIZEA"), 80, 24, kCommandTimeoutMs),
             qPrintable(QStringLiteral("pane never reported the initial 80x24; tail=%1")
                            .arg(QString::fromLatin1(m_out.right(400)))));
    qInfo().noquote() << "resize: remote reported CHSIZEA=80x24 before the resize";

    // Renderer geometry change -> controller -> SshChannelDevice::resizePty ->
    // ssh window-change -> TIOCSWINSZ -> SIGWINCH to the remote tmux client.
    QVERIFY(m_controller.resize(100, 30));
    QCOMPARE(m_controller.columns(), 100);
    QCOMPARE(m_controller.rows(), 30);

    QVERIFY2(paneReportsSize(QByteArrayLiteral("CHSIZEB"), 100, 30, kCommandTimeoutMs),
             qPrintable(QStringLiteral("pane never reported 100x30; tail=%1")
                            .arg(QString::fromLatin1(m_out.right(400)))));
    qInfo().noquote() << "resize: remote reported CHSIZEB=100x30 after the resize";
}

// (c) RECONNECT: drop the channel, open a NEW one onto the SAME tmux session,
// and prove the pane survived — its text is still there and its shell is still
// the same process.
void TstLiveTerminal::reattachAfterDropRecoversPaneAndProcess()
{
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    QVERIFY(m_device);
    QVERIFY(!m_marker.isEmpty());
    QVERIFY(!m_shellPid.isEmpty());

    dropPane();
    // The seam reports the drop into the SPEC 5.6 state machine.
    QVERIFY(m_controller.state() == TerminalState::Disconnected);
    QVERIFY(!m_controller.transport());
    QVERIFY(!m_controller.sendInput(QByteArrayLiteral("noop\n")));

    // Let the last pre-drop bytes flush, then start from an empty stream: from
    // here on, anything matching can only have come from the reconnected pane.
    QTest::qWait(500);
    m_out.clear();

    // Deliberately open the fresh channel at the channel default 80x24 and bind
    // the controller afterwards: the pane geometry must be restored by the
    // controller, which is the only thing that still knows it.
    attachPane(80, 24, /*startBeforeTransport=*/true);

    QTRY_VERIFY_WITH_TIMEOUT(!m_out.isEmpty(), kAttachTimeoutMs);

    // Scrollback survived the disconnect. capture-pane prints the retained pane
    // content; the marker is nowhere in what we typed, so a hit is tmux having
    // kept it across the dropped channel.
    QVERIFY2(runInPane(QByteArrayLiteral("tmux capture-pane -p -S -200"), m_marker,
                       kCommandTimeoutMs),
             qPrintable(QStringLiteral("marker %1 did not survive the reconnect; tail=%2")
                            .arg(QString::fromLatin1(m_marker),
                                 QString::fromLatin1(m_out.right(400)))));
    qInfo().noquote() << "reconnect: recovered marker" << m_marker
                      << "from the re-attached pane";

    // The process itself survived: same shell pid, under a brand new channel.
    // A different tag than (a) so a redraw of the old screen cannot answer it.
    const QByteArray pid = paneShellPid(QByteArrayLiteral("CHPID2"));
    QVERIFY2(!pid.isEmpty(), "re-attached pane never reported a shell pid");
    qInfo().noquote() << "reconnect: pane shell pid" << pid << "(was" << m_shellPid
                      << ")";
    QCOMPARE(pid, m_shellPid);

    // The controller re-asserted the geometry onto the new PTY.
    // A tag never used before, so tmux redrawing the pre-drop screen (which
    // already carries CHSIZEB=100x30) cannot answer for the new channel.
    QVERIFY2(paneReportsSize(QByteArrayLiteral("CHSIZEC"), 100, 30, kCommandTimeoutMs),
             qPrintable(QStringLiteral("re-attached pane never reported 100x30; tail=%1")
                            .arg(QString::fromLatin1(m_out.right(400)))));
    qInfo().noquote() << "reconnect: remote reported CHSIZEC=100x30 on the new channel";

    m_controller.setState(TerminalState::Ready);
}

QTEST_GUILESS_MAIN(TstLiveTerminal)
#include "tst_liveterminal.moc"
