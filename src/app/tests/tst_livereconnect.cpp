// LIVE gates for SessionBootstrap against the real fixture (SPEC 5.6, 10.1).
//
// 1. survivesADroppedConnection: a real SSH session is really dropped, and the
//    app really comes back. The unit gate (tst_sessionbootstrap) drives the
//    state machine through test seams; this one drives nothing. It wires the
//    production bootstrap against the fixture, kills the connection from the
//    remote end, and then requires a fresh handshake, a fresh codeharbord and a
//    working RPC round-trip on the other side of the backoff — none of which the
//    seam-driven test can prove.
//
// 2. provisionsAnEmptyLocationThenWires: a server with NOTHING installed at the
//    configured location is brought up by the client itself. The unit gate can
//    prove the decision tree with canned answers, but only a real server proves
//    that the scripts this client sends actually run under its sh, that the
//    archive unpacks into something launchable, and that codeharbord then
//    answers over the channel. It installs into a throwaway directory and
//    removes it again.
//
// How the drop in (1) is made, and why it is safe next to other live tests: the
// kill runs on an Exec channel of OUR OWN session and targets that channel's
// parent, which is the per-connection `sshd-session: <user>@notty` process. It is
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

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QRandomGenerator>
#include <QScopeGuard>
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
// A one-shot out-of-band exec channel: mkdir, tar, cat, rm.
constexpr int kExecTimeoutMs = 30000;
// Staging a tarball on the server, copying it into place, unpacking it and
// confirming the result — four remote round trips plus a tar of the source tree.
constexpr int kProvisionTimeoutMs = 120000;

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

// Quote one argv element for the remote login shell. Same rule
// SessionBootstrap uses: CH_LIVE_REPO and the throwaway paths below are
// interpolated into remote commands, so they are quoted rather than trusted.
QString sq(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

// One out-of-band remote command on `pool`'s live session, stdout collected and
// trimmed. Out of band on purpose: what provisioning did to the server is
// verified over a plain exec channel, never through the same code path that
// produced it. `ok` reports whether the command ran to end-of-stream.
QString runExec(SshConnectionPool& pool, const QString& command, bool* ok)
{
    SshChannelDevice device(&pool);
    if (!device.startExec(command)) {
        if (ok)
            *ok = false;
        return {};
    }
    bool finished = false;
    QObject::connect(&device, &SshChannelDevice::readChannelFinished, &device,
                     [&finished] { finished = true; });
    QByteArray out;
    QDeadlineTimer deadline(kExecTimeoutMs);
    while (!finished && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        out += device.readAll();
    }
    out += device.readAll();
    device.closeChannel();
    if (ok)
        *ok = finished;
    return QString::fromUtf8(out).trimmed();
}

} // namespace

class TstLiveReconnect : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void theFixtureIsolatesTheWorkspaceDatabase();
    void survivesADroppedConnection();
    void provisionsAnEmptyLocationThenWires();

private:
    bool serverInfoAnswers(QString* detail);
    void dropTheConnection();

    SshConnectionPool m_pool;
    CodeharbordClient m_client;
    AgentStatusMonitor m_monitor;
    std::unique_ptr<SessionBootstrap> m_bootstrap;
    QStringList m_bootstrapErrors;
    // Members rather than locals in the case body: both are appended to from
    // lambdas whose connection lives as long as m_bootstrap, which outlives the
    // slot that made it, so a captured local would dangle.
    QStringList m_provisioningReports;
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
    auto killer = std::make_unique<SshChannelDevice>(&m_pool);
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

// The live gates run a REAL codeharbord as this user, and its workspace database
// defaults to the ONE the installed server uses. A fixture that does not override
// that path therefore migrates the developer's own workspace to whatever schema
// version the tree under test carries, and the installed server then refuses to
// start at all ("workspace schema is newer than the build supports") until it is
// upgraded too. That happened; this is the guard so it cannot happen quietly.
//
// Checked by asking the SERVER what it sees, over the same SSH session the daemon
// is launched on, because that is the environment that actually decides - not the
// environment this test process happens to have.
void TstLiveReconnect::theFixtureIsolatesTheWorkspaceDatabase()
{
    if (!m_live)
        QSKIP("live gate not armed");

    const QString host = env("CH_LIVE_HOST");
    const quint16 port = static_cast<quint16>(env("CH_LIVE_PORT").toUInt());
    const QString user = env("CH_LIVE_USER");
    const QString identity = env("CH_LIVE_IDENTITY");

    // Its OWN pool, with its own host-key answer. The shared m_pool is the one
    // the later cases drive through SessionBootstrap, which installs the
    // host-key callback itself; borrowing it here would either be refused (the
    // pool declines an unknown key when no callback is set) or would leave it
    // connected and pre-answered for a case that means to start cold.
    SshConnectionPool probe;
    probe.setHostKeyCallback([](const QString&, const QString&,
                                const QByteArray&, ch::KnownHosts::Verdict) {
        // A throwaway fixture on loopback, and this probe reads one environment
        // variable: accepting is the whole point, and nothing here is persisted.
        return SshConnectionPool::HostKeyDecision::Accept;
    });
    QVERIFY2(probe.connectToHost(host, port, user, identity),
             qPrintable(probe.diagnosticLog()));

    bool ok = false;
    const QString reported =
        runExec(probe,
                QStringLiteral("printf 'DB=%s\\n' \"${CODEHARBOR_DB-}\""), &ok);
    QVERIFY2(ok, "could not ask the fixture what CODEHARBOR_DB it sets");

    const QString value =
        reported.section(QStringLiteral("DB="), 1).trimmed();
    QVERIFY2(!value.isEmpty(),
             "the live fixture does not set CODEHARBOR_DB, so codeharbord would "
             "open the workspace database of the server installed on this "
             "machine and migrate it. Add a SetEnv line to the fixture's "
             "sshd_config (see docs/DEVELOPMENT.md, Live gates).");
    QVERIFY2(!value.contains(QStringLiteral("/.local/share/codeharbor/")),
             qPrintable(QStringLiteral(
                            "the live fixture points CODEHARBOR_DB at the real "
                            "workspace database (%1); it must be a throwaway file")
                            .arg(value)));
    probe.disconnectFromHost();
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

    // Held as a QPointer, not a raw pointer. The old device is destroyed during
    // the teardown below, and the heap is free to hand its exact address back
    // for the replacement -- which really does happen, and made the old raw
    // "new != old" comparison fail intermittently for a correct reconnect. A
    // QPointer self-clears when its object is destroyed, so "the original was
    // torn down" becomes an observation about the object rather than a
    // comparison of a dangling address.
    QPointer<SshChannelDevice> firstRpc = m_bootstrap->rpcDevice();
    QVERIFY(!firstRpc.isNull());

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
    // The original RPC device was destroyed, not merely detached. This is the
    // half of "the transport was rebuilt" that address reuse cannot fake.
    QVERIFY(firstRpc.isNull());

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
    // ... and its replacement exists. Together with the null check above, that
    // is strictly stronger than comparing the two addresses.
    QCOMPARE(m_client.transport(),
             static_cast<QIODevice*>(m_bootstrap->rpcDevice()));
    QCOMPARE(m_monitor.transport(),
             static_cast<QIODevice*>(m_bootstrap->agentDevice()));
    QCOMPARE(m_bootstrap->reconnectAttempt(), 0);

    // The only claim that matters: RPC works again over the new transport.
    QVERIFY2(serverInfoAnswers(&detail), qPrintable(detail));
}

// THE PROVISIONING GATE. A first connect to a location with nothing in it has
// to bring up a working remote service by itself; the whole point of the feature
// is that the user no longer checks this repository out on the server by hand.
//
// The artifact is STAGED ON THE SERVER (tarred out of the checkout the fixture
// already has) and installed through a file:// URL rather than downloaded from
// the releases page. That is deliberate and it does not weaken the gate: the
// fetch step is the only line that differs (`cp` instead of `curl`), while
// everything this case exists to prove — that the inspection script runs under
// the server's real sh, that the archive unpacks into something
// entryCandidates() finds, that the marker makes a second connect a no-op, and
// that codeharbord then answers JSON-RPC over the channel — is exercised for
// real. It also keeps the gate off the network and independent of which release
// happens to be published, and it is exactly the air-gapped install path a user
// with no outbound access would take.
void TstLiveReconnect::provisionsAnEmptyLocationThenWires()
{
    if (!m_live)
        QSKIP("live gate not armed");

    // The reconnect case above leaves a wired session on the shared pool; this
    // case drives its own connects, so start from nothing.
    m_bootstrap.reset();
    m_pool.disconnectFromHost();
    m_bootstrapErrors.clear();

    const QString host = env("CH_LIVE_HOST");
    const quint16 port = static_cast<quint16>(env("CH_LIVE_PORT").toUInt());
    const QString user = env("CH_LIVE_USER");
    const QString node = env("CH_LIVE_NODE");
    const QString repo = env("CH_LIVE_REPO");
    const QString identity = env("CH_LIVE_IDENTITY");

    // A throwaway base on the server, stamped with this process so two runs (or
    // a run beside another live target) cannot collide. EVERYTHING this case
    // creates lives under it and is removed at the end.
    const QString base =
        QStringLiteral("/tmp/ch-provision-%1-%2")
            .arg(QCoreApplication::applicationPid())
            .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
    const QString target = base + QStringLiteral("/root");
    const QString tarball = base + QStringLiteral("/codeharbor-remote.tar.gz");
    const QString artifactUrl = QStringLiteral("file://") + tarball;

    m_bootstrap =
        std::make_unique<SessionBootstrap>(&m_pool, &m_client, &m_monitor);
    // Nobody to raise a host-key prompt in this gate, and the fixture key is
    // usually unknown to a fresh known_hosts store.
    m_bootstrap->setTrustUnknownHostKeys(true);
    // connectAndWireFromEnvironment() honours this override; connectAndWire()
    // cannot see it, so the fixture's own store is respected here by hand rather
    // than writing the fixture key into the developer's ~/.config.
    const QString knownHosts = env("CH_LIVE_KNOWN_HOSTS");
    if (!knownHosts.isEmpty())
        m_bootstrap->setKnownHostsPath(knownHosts);
    // Reconnect would fight the deliberate disconnects below.
    m_bootstrap->setReconnectEnabled(false);
    connect(m_bootstrap.get(), &SessionBootstrap::error, this,
            [this](const QString& message) { m_bootstrapErrors.append(message); });
    m_provisioningReports.clear();
    connect(m_bootstrap.get(), &SessionBootstrap::provisioning, this,
            [this](const QString& message) {
                m_provisioningReports.append(message);
            });

    const auto why = [this] { return m_bootstrapErrors.join(QStringLiteral(" | ")); };

    // ---- phase 1: the fixture's own checkout, which must be left alone ------
    QVERIFY2(m_bootstrap->connectAndWire(host, port, user, node, repo, identity),
             qPrintable(why()));
    QCOMPARE(m_bootstrap->state(), State::Wired);
    // A directory a person manages has no release marker, so nothing was
    // installed over it and the user was told nothing about provisioning.
    QVERIFY2(m_provisioningReports.isEmpty(),
             qPrintable(QStringLiteral("the fixture checkout was provisioned "
                                       "over: %1")
                            .arg(m_provisioningReports.join(
                                QStringLiteral(" | ")))));

    // Stage the artifact. `remote/src`, `remote/sql` and `remote/package.json`
    // are everything the service needs: it has no runtime dependencies, and
    // workspace.ts reads ../sql/schema.sql relative to itself.
    bool execOk = false;
    const QString staged = runExec(
        m_pool,
        QStringLiteral("mkdir -p %1 && tar -czf %2 -C %3 remote/src remote/sql "
                       "remote/package.json && echo STAGED")
            .arg(sq(base), sq(tarball), sq(repo)),
        &execOk);
    QVERIFY2(execOk && staged.contains(QStringLiteral("STAGED")),
             qPrintable(QStringLiteral("could not stage %1 from %2: %3")
                            .arg(tarball, repo, staged)));

    // Whatever happens below, the server does not keep our litter. The session
    // is deliberately dropped and remade between phases, so the sweep opens the
    // channel on whatever session is live at the end; if there is none, it says
    // where the directory is instead of leaving it unmentioned.
    const auto sweep = qScopeGuard([this, base] {
        if (m_pool.state() != SshConnectionPool::State::Connected) {
            qWarning().noquote()
                << "provisioning gate: no live session to clean up with; remove"
                << base << "on the fixture by hand";
            return;
        }
        bool ok = false;
        const QString left =
            runExec(m_pool,
                    QStringLiteral("rm -rf %1; [ -e %1 ] && echo LEFTOVER")
                        .arg(sq(base)),
                    &ok);
        if (!ok || left.contains(QStringLiteral("LEFTOVER")))
            qWarning().noquote() << "provisioning gate: could not remove" << base;
    });

    m_bootstrap->disconnectSession();

    // ---- phase 2: an EMPTY location, which must be provisioned -------------
    m_bootstrap->setRemoteArtifactUrl(artifactUrl);
    QElapsedTimer install;
    install.start();
    QVERIFY2(m_bootstrap->connectAndWire(host, port, user, node, target, identity),
             qPrintable(QStringLiteral("provisioning connect failed: %1")
                            .arg(why())));
    QCOMPARE(m_bootstrap->state(), State::Wired);
    QVERIFY2(install.elapsed() < kProvisionTimeoutMs,
             qPrintable(QStringLiteral("provisioning took %1 ms")
                            .arg(install.elapsed())));

    // The user was told what was happening, from BOTH layers: the client's own
    // "Installing ..." announcement and the remote script's per-step lines,
    // republished as they landed. A first connect that silently spends a minute
    // installing is indistinguishable from a hung application.
    const QString reported = m_provisioningReports.join(QStringLiteral(" | "));
    QVERIFY2(reported.contains(QStringLiteral("Installing")), qPrintable(reported));
    QVERIFY2(reported.contains(QStringLiteral("fetching")), qPrintable(reported));
    QVERIFY2(reported.contains(QStringLiteral("unpacking")), qPrintable(reported));
    QVERIFY2(reported.contains(QStringLiteral("Installed")), qPrintable(reported));

    // The claim that matters: the service the client installed actually answers.
    QString detail;
    QVERIFY2(serverInfoAnswers(&detail), qPrintable(detail));

    // Verified OUT OF BAND, over a plain exec channel rather than the RPC path
    // that produced it: the entry point is really on disk, the release marker
    // records the artifact, and the scratch directory was cleaned up.
    execOk = false;
    const QString landed = runExec(
        m_pool,
        QStringLiteral("[ -f %1 ] && echo ENTRY_OK; [ -d %2 ] && echo "
                       "SCRATCH_LEFT; cat %3")
            .arg(sq(target + QStringLiteral("/remote/src/codeharbord.ts")),
                 sq(target + QStringLiteral("/.codeharbor-provision")),
                 sq(SessionBootstrap::releaseMarkerPath(target))),
        &execOk);
    QVERIFY(execOk);
    QVERIFY2(landed.contains(QStringLiteral("ENTRY_OK")), qPrintable(landed));
    QVERIFY2(!landed.contains(QStringLiteral("SCRATCH_LEFT")), qPrintable(landed));
    QVERIFY2(landed.contains(artifactUrl), qPrintable(landed));

    // ---- phase 3: connecting again must not reinstall ----------------------
    m_bootstrap->disconnectSession();
    m_provisioningReports.clear();
    QVERIFY2(m_bootstrap->connectAndWire(host, port, user, node, target, identity),
             qPrintable(why()));
    QCOMPARE(m_bootstrap->state(), State::Wired);
    QVERIFY2(m_provisioningReports.isEmpty(),
             qPrintable(QStringLiteral("a second connect reinstalled: %1")
                            .arg(m_provisioningReports.join(
                                QStringLiteral(" | ")))));
    QVERIFY2(serverInfoAnswers(&detail), qPrintable(detail));

    // ---- phase 4: a requested upgrade, against a checkout-shaped install ---
    //
    // The staged tarball unpacks `remote/src/...`, which is the SOURCE layout,
    // so what sits at `target` now is indistinguishable from a git checkout:
    // its entry point is not the release layout's `dist/codeharbord.js`. A
    // user-requested upgrade must refuse it by name and write nothing, because
    // unpacking a release beside a checkout leaves the checkout in place but no
    // longer the thing that runs.
    //
    // Proven against a real server rather than only in the unit gate because
    // the entry point being compared is the one the server's own sh reported.
    // The marker is what says "this install is ours". Removing it makes this
    // exactly the hand-unpacked install a user creates by following README.md.
    // Done while phase 3's session is still up, because runExec() needs a live
    // pool.
    execOk = false;
    const QString unmarked =
        runExec(m_pool,
                QStringLiteral("rm -f %1 && echo MARKER_GONE")
                    .arg(sq(SessionBootstrap::releaseMarkerPath(target))),
                &execOk);
    QVERIFY(execOk);
    QVERIFY2(unmarked.contains(QStringLiteral("MARKER_GONE")),
             qPrintable(unmarked));

    m_bootstrap->disconnectSession();
    m_bootstrapErrors.clear();
    m_provisioningReports.clear();

    m_bootstrap->requestRemoteUpgrade();
    QVERIFY2(!m_bootstrap->connectAndWire(host, port, user, node, target,
                                          identity),
             "a requested upgrade overwrote a source-layout installation");
    QCOMPARE(m_bootstrapErrors.size(), 1);
    const QString refusal = m_bootstrapErrors.at(0);
    QVERIFY2(refusal.contains(QStringLiteral("source checkout")),
             qPrintable(refusal));
    QVERIFY2(refusal.contains(QStringLiteral("Nothing was changed")),
             qPrintable(refusal));
    QVERIFY2(m_provisioningReports.isEmpty(),
             qPrintable(m_provisioningReports.join(QStringLiteral(" | "))));
    // Spent, so the next connect is an ordinary one.
    QVERIFY(!m_bootstrap->remoteUpgradeRequested());

    // Nothing was written: no release layout appeared and no marker came back.
    m_pool.disconnectFromHost();
    m_bootstrapErrors.clear();
    QVERIFY2(m_bootstrap->connectAndWire(host, port, user, node, target,
                                         identity),
             qPrintable(why()));
    execOk = false;
    const QString after =
        runExec(m_pool,
                QStringLiteral("[ -e %1 ] && echo RELEASE_LAYOUT; [ -e %2 ] && "
                               "echo MARKER_BACK; echo SWEPT")
                    .arg(sq(target + QStringLiteral("/dist")),
                         sq(SessionBootstrap::releaseMarkerPath(target))),
                &execOk);
    QVERIFY(execOk);
    QVERIFY2(!after.contains(QStringLiteral("RELEASE_LAYOUT")), qPrintable(after));
    QVERIFY2(!after.contains(QStringLiteral("MARKER_BACK")), qPrintable(after));
    QVERIFY2(serverInfoAnswers(&detail), qPrintable(detail));

    QVERIFY2(m_bootstrapErrors.isEmpty(), qPrintable(why()));
}

// Guiless: no display anywhere in this gate, and ch_app links Qt6::Gui.
QTEST_GUILESS_MAIN(TstLiveReconnect)
#include "tst_livereconnect.moc"
