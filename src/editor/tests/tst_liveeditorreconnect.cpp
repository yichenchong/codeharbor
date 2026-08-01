// LIVE gate: an OPEN EDITOR keeps working across a dropped-and-restored SSH
// session (SPEC 5.6 + 8.7).
//
// tst_livereconnect proves the SESSION comes back: new handshake, new
// codeharbord, server.info answers again. That is necessary and not sufficient.
// The subscription an editor established with file.watch does NOT live in the
// wire, it lives in the codeharbord PROCESS — remote/src/files.ts keeps
// FileWatchService's subscriptions in a plain per-process Map. The reconnect
// replaces that process, so every subscription minted by the old one is a dead
// token, and an editor that does not re-establish its watch goes silently blind
// to external changes for the rest of the session: no error, no state change,
// just a buffer that never updates again.
//
// This gate asserts the OBSERVABLE consequence, end to end and un-stubbed:
//
//   1. BEFORE  — a remote edit made on an out-of-band ssh shell reloads the
//                clean buffer. The mechanism works, so a later failure is a
//                reconnect defect and not a broken fixture.
//   2. AWAY    — the SSH session is really killed (same technique as
//                tst_livereconnect: the killer rides an Exec channel of OUR
//                connection and signals that channel's own sshd-session parent,
//                so nothing outside this test's TCP connection is touched) and
//                the file is edited WHILE THE CLIENT IS DOWN. No watch event
//                for that edit can ever exist.
//   3. AFTER   — once the ladder has re-wired, the buffer must show the bytes
//                written during the outage (nothing on the wire announced them,
//                so this is pure client-side reconciliation) AND a fresh remote
//                edit must reload it again (which requires a real subscription
//                on the NEW codeharbord).
//   4. DIRTY   — with unsaved edits in the buffer the same round trip must
//                preserve them: no reload, no revision change, and an external
//                write is FLAGGED (externally_modified) rather than clobbering
//                the user's work.
//
// The out-of-band shell runs on a SECOND SshConnectionPool, i.e. a second TCP
// connection with its own sshd-session, so the kill in step 2 leaves it alone
// and the file can be edited at any point — including while the connection
// under test is down, which is the whole point of step 3.
//
// Skipped wholesale unless CH_LIVE_SSH is set, so the default suite stays green
// on a machine with no fixture.

#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "EditorController.h"
#include "KnownHosts.h"
#include "SessionBootstrap.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"

#include <QByteArray>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QtTest/QtTest>

#include <memory>
#include <optional>

using ch::AgentStatusMonitor;
using ch::CodeharbordClient;
using ch::EditorController;
using ch::KnownHosts;
using ch::RpcError;
using ch::SessionBootstrap;
using ch::SshChannelDevice;
using ch::SshConnectionPool;

using State = SessionBootstrap::State;

namespace {

// An ssh connect plus a remote node cold start (which also type-strips the
// TypeScript entry point) is measured in seconds, not milliseconds.
constexpr int kExecTimeoutMs = 30000;
constexpr int kRpcTimeoutMs = 60000;
// A full re-handshake behind the 1 s first backoff rung, plus a second cold
// node start for the replacement codeharbord.
constexpr int kReconnectTimeoutMs = 120000;
// fs.watch is immediate; the polling fallback in FileWatchService ticks at 1 s.
// The reload it triggers is one more RPC round trip.
constexpr int kWatchTimeoutMs = 30000;

// Kill this exec channel's parent — the per-connection sshd-session — so the
// whole SSH connection drops underneath libssh, exactly as a yanked network
// cable would. The `case` guard makes the signal unreachable unless the target
// really is an sshd process. Verbatim from tst_livereconnect: the drop has to
// be the same drop, or this gate would be testing a different failure.
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

// Quote a string for /bin/sh.
QString sq(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

} // namespace

class TstLiveEditorReconnect : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void watchResubscribesAfterReconnect();
    void dirtyBufferSurvivesReconnect();

private:
    bool startSideShell();
    bool remoteExec(const QString& command, QByteArray* stdoutText = nullptr,
                    QString* stderrText = nullptr);
    // Overwrite the remote file out of band, never through the RPC path under
    // test, and confirm the bytes landed.
    void writeRemote(const QString& content);
    void dropTheConnection();

    // The connection under test. Declared before everything that rides it so
    // the members are destroyed in the opposite order.
    SshConnectionPool m_pool;
    CodeharbordClient m_client;
    AgentStatusMonitor m_monitor;
    std::unique_ptr<SessionBootstrap> m_bootstrap;
    std::unique_ptr<EditorController> m_controller;

    // Out-of-band connection: a SECOND TCP session, so the kill above (scoped
    // to m_pool's sshd-session) cannot take it with it.
    SshConnectionPool m_sidePool;
    std::unique_ptr<SshChannelDevice> m_sideShell;
    QByteArray m_sideOut;
    QString m_sideErr;
    int m_sideSeq = 0;

    QString m_remoteDir;
    QString m_filePath;
    QStringList m_bootstrapErrors;

    // Latest contentLoaded payload and how many arrived, recorded straight off
    // the controller's bridge signal.
    QString m_lastContent;
    QString m_lastRevision;
    int m_loads = 0;

    bool m_live = false;
};

void TstLiveEditorReconnect::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH is not set; live editor reconnect gate skipped");
    if (!SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; live editor reconnect gate skipped");

    QVERIFY2(!env("CH_LIVE_HOST").isEmpty() && !env("CH_LIVE_PORT").isEmpty()
                 && !env("CH_LIVE_USER").isEmpty()
                 && !env("CH_LIVE_NODE").isEmpty()
                 && !env("CH_LIVE_REPO").isEmpty(),
             "CH_LIVE_HOST/PORT/USER/NODE/REPO must all be set");

    // ---- the connection under test, wired by the production seam ----------
    m_bootstrap =
        std::make_unique<SessionBootstrap>(&m_pool, &m_client, &m_monitor);
    connect(m_bootstrap.get(), &SessionBootstrap::error, this,
            [this](const QString& message) { m_bootstrapErrors.append(message); });

    QVERIFY2(m_bootstrap->connectAndWireFromEnvironment(),
             qPrintable(QStringLiteral("could not wire a live session: %1")
                            .arg(m_bootstrapErrors.join(QStringLiteral(" | ")))));
    QCOMPARE(m_bootstrap->state(), State::Wired);

    // ---- the out-of-band connection ---------------------------------------
    QVERIFY2(startSideShell(),
             qPrintable(QStringLiteral("could not start the out-of-band shell: %1")
                            .arg(m_sideErr)));

    // ---- remote fixture, created out of band ------------------------------
    // Not through file.writeFile: the fixture must not depend on the RPC surface
    // under test. Only the working directory is created here; the SPEC 11.3
    // recovery snapshot reportContent writes lands under the recovery directory
    // set on the controller below, whose parent file.writeFile creates on the
    // first write.
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    m_remoteDir = QStringLiteral("/tmp/ch-live-editor-reconnect-%1").arg(token);
    m_filePath = m_remoteDir + QStringLiteral("/note.txt");

    QByteArray out;
    QString err;
    QVERIFY2(remoteExec(QStringLiteral("mkdir -p ")
                            + sq(m_remoteDir)
                            + QStringLiteral(" && echo SETUP_OK"),
                        &out, &err),
             qPrintable(QStringLiteral("remote fixture setup timed out: %1").arg(err)));
    QVERIFY2(out.contains("SETUP_OK"),
             qPrintable(QStringLiteral("remote fixture setup failed: out=%1 err=%2")
                            .arg(QString::fromUtf8(out), err)));

    m_live = true;
    qInfo("live editor reconnect fixture ready: file=%s", qPrintable(m_filePath));
}

void TstLiveEditorReconnect::cleanupTestCase()
{
    // The controller first: its destructor releases the server-side watch
    // through a still-live transport (SPEC 8.7).
    m_controller.reset();

    if (m_live && !m_remoteDir.isEmpty()) {
        QByteArray out;
        QString err;
        const bool ok = remoteExec(QStringLiteral("rm -rf ") + sq(m_remoteDir)
                                       + QStringLiteral(" && echo CLEANUP_OK"),
                                   &out, &err);
        if (!ok || !out.contains("CLEANUP_OK")) {
            qWarning("remote cleanup of %s may have failed: out=%s err=%s",
                     qPrintable(m_remoteDir), out.constData(), qPrintable(err));
        }
    }

    if (m_sideShell) {
        // EOF on stdin is what reaps the remote `sh`; an exec channel sends no
        // SIGHUP, so this close IS the shutdown signal.
        m_sideShell->closeChannel();
        m_sideShell.reset();
    }
    m_sidePool.disconnectFromHost();

    // Bootstrap before pool: the pool frees every channel it handed out, so the
    // devices must be gone before the session is.
    m_bootstrap.reset();
    m_pool.disconnectFromHost();
}

bool TstLiveEditorReconnect::startSideShell()
{
    // First-use trust, mirroring SessionBootstrap::attemptWire(): load whatever
    // the fixture already trusts (the live env points CH_LIVE_KNOWN_HOSTS at
    // it), and accept an unknown key for this throwaway side connection.
    KnownHosts hosts;
    QFile store(env("CH_LIVE_KNOWN_HOSTS"));
    if (store.open(QIODevice::ReadOnly | QIODevice::Text))
        hosts = KnownHosts::parse(QString::fromUtf8(store.readAll()));
    m_sidePool.setKnownHosts(hosts);
    m_sidePool.setHostKeyCallback([](const QString&, const QString&,
                                     const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });

    if (!m_sidePool.connectToHost(env("CH_LIVE_HOST"),
                                  static_cast<quint16>(env("CH_LIVE_PORT").toUInt()),
                                  env("CH_LIVE_USER"))) {
        m_sideErr = QStringLiteral("side pool could not connect");
        return false;
    }

    // ONE channel carries every out-of-band command: sshd caps concurrent
    // session channels per connection, and `sh -s` reads its command stream
    // from stdin and exits on EOF, so closing the channel reaps it.
    m_sideShell = std::make_unique<SshChannelDevice>(&m_sidePool);
    connect(m_sideShell.get(), &QIODevice::readyRead, m_sideShell.get(),
            [this] { m_sideOut += m_sideShell->readAll(); });
    connect(m_sideShell.get(), &SshChannelDevice::channelError, m_sideShell.get(),
            [this](const QString& text) { m_sideErr += text; });
    return m_sideShell->startExec(QStringLiteral("/bin/sh -s"));
}

bool TstLiveEditorReconnect::remoteExec(const QString& command,
                                        QByteArray* stdoutText, QString* stderrText)
{
    if (stdoutText)
        stdoutText->clear();
    if (stderrText)
        stderrText->clear();
    if (!m_sideShell)
        return false;

    // Commands share one stdout stream, so each is terminated by a unique
    // sentinel line: everything before it is this command's output.
    const QByteArray sentinel = QByteArrayLiteral("__CH_EXEC_DONE_")
                                + QByteArray::number(++m_sideSeq)
                                + QByteArrayLiteral("__");
    m_sideOut.clear();
    m_sideErr.clear();

    const QString line = command + QStringLiteral("\nprintf '%s\\n' ")
                         + QString::fromUtf8(sentinel) + QStringLiteral("\n");
    if (m_sideShell->write(line.toUtf8()) < 0) {
        if (stderrText)
            *stderrText = m_sideErr + QStringLiteral(" (write to shell channel failed)");
        return false;
    }

    QDeadlineTimer deadline(kExecTimeoutMs);
    while (!m_sideOut.contains(sentinel) && !deadline.hasExpired())
        QTest::qWait(20);

    const qsizetype at = m_sideOut.indexOf(sentinel);
    if (at < 0) {
        if (stderrText)
            *stderrText = m_sideErr + QStringLiteral(" (sentinel never arrived)");
        return false;
    }
    if (stdoutText)
        *stdoutText = m_sideOut.left(at);
    if (stderrText)
        *stderrText = m_sideErr;
    return true;
}

void TstLiveEditorReconnect::writeRemote(const QString& content)
{
    QByteArray out;
    QString err;
    // printf, not echo: no shell-dependent escape processing, and the exact
    // bytes (including the trailing newline) are what lands on disk.
    QVERIFY2(remoteExec(QStringLiteral("printf '%s' ") + sq(content)
                            + QStringLiteral(" > ") + sq(m_filePath)
                            + QStringLiteral(" && cat ") + sq(m_filePath),
                        &out, &err),
             qPrintable(QStringLiteral("remote write timed out: %1").arg(err)));
    QCOMPARE(QString::fromUtf8(out), content);
}

void TstLiveEditorReconnect::dropTheConnection()
{
    // The killer rides the same SSH connection it is about to kill; the signal
    // is delivered before sshd can tear the channel down.
    auto killer = std::make_unique<SshChannelDevice>(&m_pool);
    // channelError carries remote stderr AND libssh faults, and the killer is
    // riding the connection it kills: "Socket error: disconnected" here is the
    // expected outcome. Only the guard's own refusal means the drop missed.
    QString killerStderr;
    connect(killer.get(), &SshChannelDevice::channelError, killer.get(),
            [&killerStderr](const QString& text) { killerStderr += text; });
    QVERIFY(killer->startExec(QString::fromLatin1(kDropConnectionCommand)));

    // Give the remote kill time to land and libssh time to notice, but stay
    // inside the 1 s first backoff rung: the retry calls connectToHost(), which
    // frees every channel on the old session — this killer's included.
    QElapsedTimer since;
    since.start();
    while (m_bootstrap->state() == State::Wired && since.elapsed() < 500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    killer->closeChannel();
    killer.reset();

    QVERIFY2(!killerStderr.contains(QStringLiteral("refusing to kill")),
             qPrintable(QStringLiteral("kill helper refused: %1").arg(killerStderr)));
}

void TstLiveEditorReconnect::watchResubscribesAfterReconnect()
{
    if (!m_live)
        QSKIP("live gate not armed");

    const QString v1 = QStringLiteral("v1: the bytes the editor opened\n");
    const QString v2 = QStringLiteral("v2: changed while the session was up\n");
    const QString v3 = QStringLiteral("v3: changed while the session was DOWN\n");
    const QString v4 = QStringLiteral("v4: changed after the session came back\n");

    writeRemote(v1);

    // ---- open the file through the production controller ------------------
    m_controller = std::make_unique<EditorController>(&m_client, QStringLiteral("viewer-1"));
    // The recovery base a connected AppController would push in (SPEC 11.3); set
    // directly here so reportContent's debounced snapshot write is exercised
    // over the real channel. Its exact path is not asserted by this gate.
    m_controller->setRecoveryDir(m_remoteDir + QStringLiteral("/recovery"));
    connect(m_controller.get(), &EditorController::contentLoaded, this,
            [this](const QString& content, const QString& revision) {
                m_lastContent = content;
                m_lastRevision = revision;
                ++m_loads;
            });
    // Stand in for the WebChannel page finishing its handshake; without it
    // contentLoaded is HELD (EditorController::ready()) and nothing is emitted.
    m_controller->ready();
    m_controller->open(m_filePath);

    QTRY_COMPARE_WITH_TIMEOUT(m_controller->fileState(), QStringLiteral("clean"),
                              kRpcTimeoutMs);
    QCOMPARE(m_loads, 1);
    QCOMPARE(m_lastContent, v1);

    // ---- (1) BEFORE: the watch works on this session -----------------------
    writeRemote(v2);
    QTRY_COMPARE_WITH_TIMEOUT(m_lastContent, v2, kWatchTimeoutMs);
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // ---- (2) AWAY: kill the session, edit the file while it is down --------
    QSignalSpy wiredSpy(m_bootstrap.get(), &SessionBootstrap::wired);
    // QPointer, not a raw pointer: SessionBootstrap::unwire() retires the old
    // device with deleteLater() and attemptWire() allocates the replacement, so
    // the freed address can legitimately be handed straight back. Comparing raw
    // pointers would then claim the session never re-wired. A QPointer that has
    // gone null proves the old device really was destroyed.
    QPointer<SshChannelDevice> rpcBefore = m_bootstrap->rpcDevice();
    QVERIFY(!rpcBefore.isNull());

    QElapsedTimer outage;
    outage.start();
    dropTheConnection();
    QTRY_VERIFY_WITH_TIMEOUT(m_bootstrap->state() != State::Wired, 30000);
    QVERIFY(m_client.transport() == nullptr);

    writeRemote(v3);

    QTRY_COMPARE_WITH_TIMEOUT(m_bootstrap->state(), State::Wired,
                              kReconnectTimeoutMs);
    // A real re-wire, not a session that never actually went down: a brand new
    // channel device on a brand new SSH session, in front of a brand new
    // codeharbord whose watch registry has never heard of us.
    QCOMPARE(wiredSpy.size(), 1);
    QTRY_VERIFY(rpcBefore.isNull());
    QVERIFY(m_bootstrap->rpcDevice() != nullptr);
    QCOMPARE(m_client.transport(),
             static_cast<QIODevice*>(m_bootstrap->rpcDevice()));
    qInfo("session dropped and re-wired in %lld ms", outage.elapsed());

    // ---- (3) AFTER ---------------------------------------------------------
    // The edit made during the outage produced no notification anywhere: the
    // old codeharbord was already dead and the new one starts with an empty
    // watch registry baselined at whatever is on disk when it subscribes. A
    // buffer that still shows v2 here is a user staring at stale bytes whose
    // next save will be refused for a revision they never saw change.
    QTRY_COMPARE_WITH_TIMEOUT(m_lastContent, v3, kWatchTimeoutMs);
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));

    // ... and the subscription is genuinely live again on the NEW process:
    // a fresh remote edit still reloads the clean buffer.
    writeRemote(v4);
    QTRY_COMPARE_WITH_TIMEOUT(m_lastContent, v4, kWatchTimeoutMs);
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("clean"));
}

void TstLiveEditorReconnect::dirtyBufferSurvivesReconnect()
{
    if (!m_live)
        QSKIP("live gate not armed");
    QVERIFY2(m_controller != nullptr, "watchResubscribesAfterReconnect must run first");

    const QString v5 = QStringLiteral("v5: external write against a dirty buffer\n");

    // Unsaved edits, exactly as the page reports them (debounced reportContent).
    m_controller->reportContent(QStringLiteral("the user's unsaved work\n"));
    QTRY_COMPARE(m_controller->fileState(), QStringLiteral("modified"));

    const QString revisionBefore = m_controller->revision();
    const int loadsBefore = m_loads;

    QSignalSpy wiredSpy(m_bootstrap.get(), &SessionBootstrap::wired);
    // QPointer for the same reason as above: the retired device's address can be
    // recycled by its own replacement.
    QPointer<SshChannelDevice> rpcBefore = m_bootstrap->rpcDevice();
    dropTheConnection();
    QTRY_VERIFY_WITH_TIMEOUT(m_bootstrap->state() != State::Wired, 30000);
    QTRY_COMPARE_WITH_TIMEOUT(m_bootstrap->state(), State::Wired,
                              kReconnectTimeoutMs);
    QCOMPARE(wiredSpy.size(), 1);
    QTRY_VERIFY(rpcBefore.isNull());
    QVERIFY(m_bootstrap->rpcDevice() != nullptr);

    // Re-establishing the watch must not touch the buffer: no reload was
    // delivered, the guarded revision is untouched, the state is still dirty.
    QCOMPARE(m_loads, loadsBefore);
    QCOMPARE(m_controller->revision(), revisionBefore);
    QCOMPARE(m_controller->fileState(), QStringLiteral("modified"));

    // An external write with unsaved edits present is FLAGGED, never applied
    // (SPEC 8.7) — and it can only be noticed at all through a live
    // subscription on the replacement codeharbord.
    writeRemote(v5);
    QTRY_COMPARE_WITH_TIMEOUT(m_controller->fileState(),
                              QStringLiteral("externally_modified"), kWatchTimeoutMs);
    QCOMPARE(m_loads, loadsBefore);
}

// Guiless: no display anywhere in this gate, and ch_app links Qt6::Gui.
QTEST_GUILESS_MAIN(TstLiveEditorReconnect)
#include "tst_liveeditorreconnect.moc"
