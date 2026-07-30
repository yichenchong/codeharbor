#include "CodeharbordClient.h"
#include "KnownHosts.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QString>
#include <QStringList>
#include <QtTest/QtTest>

#include <optional>

using ch::CodeharbordClient;
using ch::KnownHosts;
using ch::RpcError;
using ch::SshChannelDevice;
using ch::SshConnectionPool;

namespace {

// Generous: a real TCP connect + SSH handshake + remote fork, and for the RPC
// case a cold node start that also type-strips the TypeScript entry point.
constexpr int kExecTimeoutMs = 20000;
constexpr int kRpcTimeoutMs = 60000;

QString env(const char* key)
{
    return qEnvironmentVariable(key);
}

// Accumulates one channel's observable surface so assertions can be made on the
// read stream and the out-of-band error stream independently.
struct ChannelSink {
    QByteArray out;
    QString err;
    bool finished = false;

    void attach(SshChannelDevice* device)
    {
        QObject::connect(device, &SshChannelDevice::readyRead, device,
                         [this, device]() { out += device->readAll(); });
        QObject::connect(device, &SshChannelDevice::channelError, device,
                         [this](const QString& text) { err += text; });
        QObject::connect(device, &SshChannelDevice::readChannelFinished, device,
                         [this]() { finished = true; });
    }
};

} // namespace

// LIVE gate for the SSH spine (SPEC 5.3, 10.1). Everything here talks to a real
// sshd over a real socket: no mocks, no loopback QIODevice stand-ins. Skipped
// wholesale unless CH_LIVE_SSH is set, so the default suite stays green on a
// machine with no fixture.
class TstLiveSsh : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void connectsToFixture();
    void connectionLogRecordsLibsshTrace();
    void execChannelDeliversStdout();
    void manyShortLivedChannelsReuseTheirSlots();
    void rpcServerInfoOverSshChannel();
    void stderrStaysOutOfReadStream();
    void encryptedIdentityUsesPassphraseFromCallback();
    void missingAgentAndKeyExplainAuthenticationFailure();
    void multiStepServerAcceptsKeyThenPassword();
    void keyboardInteractiveOnlyServerAuthenticatesWithPassword();
    void windowsNamedPipeAgentFallsBackToIdentityFile();
    void unavailableTrustedAlgorithmStillReachesHostVerification();
    void deviceOutlivingItsSessionStopsCleanly();

private:
    void ensureConnected();

    SshConnectionPool m_pool;
    QString m_host;
    quint16 m_port = 0;
    QString m_user;
    QString m_node;
    QString m_repo;
    QString m_knownHostsPath;
    QString m_identityFile;
};

void TstLiveSsh::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH is not set; live SSH gate skipped");
    if (!SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; live SSH gate skipped");

    m_host = env("CH_LIVE_HOST");
    m_port = static_cast<quint16>(env("CH_LIVE_PORT").toUInt());
    m_user = env("CH_LIVE_USER");
    m_node = env("CH_LIVE_NODE");
    m_repo = env("CH_LIVE_REPO");
    m_knownHostsPath = env("CH_LIVE_KNOWN_HOSTS");
    m_identityFile = env("CH_LIVE_IDENTITY");
    if (m_knownHostsPath.isEmpty()) {
        m_knownHostsPath =
            QDir::temp().filePath(QStringLiteral("ch_live_known_hosts"));
    }

    QVERIFY2(!m_host.isEmpty() && m_port != 0 && !m_user.isEmpty()
                 && !m_node.isEmpty() && !m_repo.isEmpty(),
             "CH_LIVE_HOST/PORT/USER/NODE/REPO must all be set");
}

void TstLiveSsh::cleanupTestCase()
{
    m_pool.disconnectFromHost();
}

void TstLiveSsh::ensureConnected()
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
    const auto conn = QObject::connect(
        &m_pool, &SshConnectionPool::errorOccurred,
        [&failure](const QString& text) { failure += text; });

    // Authentication comes from the ssh-agent named by SSH_AUTH_SOCK.
    const bool ok = m_pool.connectToHost(m_host, m_port, m_user);
    QObject::disconnect(conn);
    QVERIFY2(ok, qPrintable(QStringLiteral("connectToHost(%1:%2) failed: %3")
                                .arg(m_host)
                                .arg(m_port)
                                .arg(failure)));

    QDir().mkpath(QFileInfo(m_knownHostsPath).absolutePath());
    QFile out(m_knownHostsPath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        out.write(m_pool.knownHosts().serialize());
}

// (a) A real authenticated session against the fixture sshd.
void TstLiveSsh::connectsToFixture()
{
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
}

// (b) An Exec channel is a working QIODevice: the remote stdout bytes arrive
// verbatim through readyRead()/readAll().
void TstLiveSsh::execChannelDeliversStdout()
{
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    SshChannelDevice device(&m_pool, SshConnectionPool::ChannelKind::Exec);
    ChannelSink sink;
    sink.attach(&device);

    QVERIFY(device.startExec(QStringLiteral("echo LIVE_EXEC_OK")));
    QVERIFY(device.isOpen());
    QVERIFY(device.isSequential());

    QTRY_VERIFY_WITH_TIMEOUT(sink.finished, kExecTimeoutMs);
    QCOMPARE(sink.out, QByteArray("LIVE_EXEC_OK\n"));
    device.closeChannel();
}

// An SSH server caps how many channels ("sessions") one connection may have
// open at a time — OpenSSH's MaxSessions, 10 by default — so a device that is
// done with its channel MUST hand it back. Running far more short-lived exec
// channels than that cap on ONE session is the regression test: while
// closeChannel()/~SshChannelDevice() only closed the channel and left the pool
// holding it, the slots were never reclaimed and the eleventh exec simply
// failed to open, wedging the session for everything else.
void TstLiveSsh::manyShortLivedChannelsReuseTheirSlots()
{
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    constexpr int kChannelCount = 25;
    for (int i = 0; i < kChannelCount; ++i) {
        SshChannelDevice device(&m_pool, SshConnectionPool::ChannelKind::Exec);
        ChannelSink sink;
        sink.attach(&device);

        QVERIFY2(device.startExec(QStringLiteral("echo SLOT_%1").arg(i)),
                 qPrintable(QStringLiteral("exec channel %1 of %2 would not "
                                           "open: %3")
                                .arg(i)
                                .arg(kChannelCount)
                                .arg(sink.err)));
        QTRY_VERIFY_WITH_TIMEOUT(sink.finished, kExecTimeoutMs);
        QCOMPARE(sink.out, QStringLiteral("SLOT_%1\n").arg(i).toUtf8());
        // The device goes out of scope here: its destructor gives the channel
        // back to the pool, which frees it and releases the server-side slot.
    }

    // The session itself is untouched by all that churn.
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
}

// (c) The end-to-end spine: real codeharbord, over a real SSH channel, driven
// by the production CodeharbordClient. This is the path that never worked
// before SshChannelDevice existed.
void TstLiveSsh::rpcServerInfoOverSshChannel()
{
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    const QString command = QStringLiteral("'%1' '%2/remote/src/codeharbord.ts' rpc --stdio")
                                .arg(m_node, m_repo);

    SshChannelDevice device(&m_pool, SshConnectionPool::ChannelKind::Rpc);
    QString stderrText;
    QObject::connect(&device, &SshChannelDevice::channelError, &device,
                     [&stderrText](const QString& text) { stderrText += text; });
    QVERIFY2(device.startExec(command), qPrintable(command));
    QVERIFY(device.isWritable());

    CodeharbordClient client;
    client.setTransport(&device);

    QJsonValue result;
    std::optional<RpcError> rpcError;
    bool answered = false;
    client.call(QStringLiteral("server.info"), QJsonValue(),
                [&](QJsonValue value, std::optional<RpcError> err) {
                    result = value;
                    rpcError = err;
                    answered = true;
                });

    QTRY_VERIFY_WITH_TIMEOUT(answered, kRpcTimeoutMs);
    QVERIFY2(!rpcError.has_value(),
             qPrintable(rpcError ? rpcError->message + QStringLiteral(" | stderr: ")
                                       + stderrText
                                 : QString()));

    const QJsonObject info = result.toObject();
    QCOMPARE(info.value(QStringLiteral("name")).toString(),
             QStringLiteral("codeharbord"));
    QVERIFY(info.contains(QStringLiteral("schemaVersion")));
    QVERIFY(info.value(QStringLiteral("schemaVersion")).toInt(-1) >= 1);

    client.setTransport(nullptr);
    device.closeChannel();
}

// The connection log is the only surface that can explain a handshake that dies
// inside libssh, so a real handshake must leave both CodeHarbor's stage markers
// and libssh's own trace in it - and must name the runtime version, which is
// what distinguishes a defective libssh from a server-side refusal.
void TstLiveSsh::connectionLogRecordsLibsshTrace()
{
    ensureConnected();

    const QString log = m_pool.diagnosticLog();
    QVERIFY2(log.contains(QStringLiteral("libssh runtime: ")), qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("Beginning SSH handshake.")),
             qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("SSH authentication succeeded.")),
             qPrintable(log));
    // A libssh-emitted line, i.e. proof the log callback is really installed.
    QVERIFY2(log.contains(QStringLiteral("libssh[")), qPrintable(log));
}

// (d) stderr must never be spliced into the read stream: a single stray
// non-JSON line would desynchronise the JSON-RPC/JSONL framing on that channel.
void TstLiveSsh::stderrStaysOutOfReadStream()
{
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    // Exec (not Pty): a PTY would merge stderr into stdout at the kernel level
    // and the separation could not be observed at all.
    SshChannelDevice device(&m_pool, SshConnectionPool::ChannelKind::Exec);
    ChannelSink sink;
    sink.attach(&device);

    QVERIFY(device.startExec(
        QStringLiteral("sh -c 'echo OUT; echo ERR 1>&2'")));

    QTRY_VERIFY_WITH_TIMEOUT(sink.finished, kExecTimeoutMs);

    QCOMPARE(sink.out, QByteArray("OUT\n"));
    QVERIFY(!sink.out.contains("ERR"));
    QCOMPARE(sink.err.trimmed(), QStringLiteral("ERR"));
    device.closeChannel();
}

// Full key-unlock path against a real sshd. The fixture's unencrypted key is
// copied and encrypted at runtime, so no private material or passphrase enters
// the repository. CH_LIVE_IDENTITY identifies the fixture key; live runs that
// do not provide it still exercise the normal agent path and skip this case.
void TstLiveSsh::encryptedIdentityUsesPassphraseFromCallback()
{
    if (m_identityFile.isEmpty())
        QSKIP("CH_LIVE_IDENTITY is not set; encrypted-key live gate skipped");
    const QString keygen = QStandardPaths::findExecutable(
        QStringLiteral("ssh-keygen"));
    if (keygen.isEmpty())
        QSKIP("ssh-keygen is unavailable; encrypted-key live gate skipped");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString encryptedKey = dir.filePath(QStringLiteral("id"));
    QVERIFY(QFile::copy(m_identityFile, encryptedKey));
    QVERIFY(QFile::setPermissions(
        encryptedKey, QFile::ReadOwner | QFile::WriteOwner));
    const QString publicKey = m_identityFile + QStringLiteral(".pub");
    if (QFileInfo(publicKey).isFile())
        QVERIFY(QFile::copy(publicKey, encryptedKey + QStringLiteral(".pub")));

    static const QString kPassphrase = QStringLiteral("codeharbor-live-passphrase");
    QProcess encrypt;
    encrypt.start(keygen, {QStringLiteral("-p"), QStringLiteral("-P"),
                           QString(), QStringLiteral("-N"), kPassphrase,
                           QStringLiteral("-f"), encryptedKey});
    QVERIFY2(encrypt.waitForFinished(kExecTimeoutMs),
             qPrintable(encrypt.errorString()));
    QCOMPARE(encrypt.exitCode(), 0);

    // Do not let the loaded fixture agent prove the wrong rung. libssh reads
    // SSH_AUTH_SOCK when ssh_userauth_agent() runs; restore it immediately
    // after this assertion so other live tests keep their ordinary setup.
    const QByteArray oldAgent = qgetenv("SSH_AUTH_SOCK");
    qunsetenv("SSH_AUTH_SOCK");
    const auto restoreAgent = qScopeGuard([oldAgent] {
        if (oldAgent.isEmpty())
            qunsetenv("SSH_AUTH_SOCK");
        else
            qputenv("SSH_AUTH_SOCK", oldAgent);
    });

    SshConnectionPool pool;
    KnownHosts hosts;
    pool.setKnownHosts(hosts);
    pool.setHostKeyCallback([](const QString&, const QString&,
                               const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });

    int passphraseRequests = 0;
    bool passwordRequested = false;
    pool.setCredentialCallback(
        [&passphraseRequests, &passwordRequested](const QString&,
                                                   SshConnectionPool::CredentialKind kind) {
            if (kind == SshConnectionPool::CredentialKind::KeyPassphrase) {
                ++passphraseRequests;
                return SshConnectionPool::CredentialReply{kPassphrase, false};
            }
            passwordRequested = true;
            return SshConnectionPool::CredentialReply{};
        });
    QString failure;
    QObject::connect(&pool, &SshConnectionPool::errorOccurred, &pool,
                     [&failure](const QString& message) { failure = message; });

    const bool connected =
        pool.connectToHost(m_host, m_port, m_user, encryptedKey);
    QVERIFY2(connected,
             qPrintable(QStringLiteral("encrypted key auth failed; passphrase requests=%1, "
                                       "password requested=%2, error=%3")
                            .arg(passphraseRequests)
                            .arg(passwordRequested)
                            .arg(failure)));
    QCOMPARE(passphraseRequests, 1);
    QVERIFY(!passwordRequested);
    // The diagnostic transcript is user-visible and copy-pasteable, so the
    // passphrase this attempt supplied must not appear anywhere in it.
    QVERIFY2(!pool.diagnosticLog().contains(kPassphrase),
             qPrintable(pool.diagnosticLog()));
    pool.disconnectFromHost();

    // The same encrypted key through a real ~/.ssh/config IdentityFile entry.
    // This exercises ssh_options_parse_config(), not only the profile's
    // SSH_OPTIONS_IDENTITY path above.
    const QByteArray oldHome = qgetenv("HOME");
    qputenv("HOME", dir.path().toUtf8());
    const auto restoreHome = qScopeGuard([oldHome] {
        if (oldHome.isEmpty())
            qunsetenv("HOME");
        else
            qputenv("HOME", oldHome);
    });
    const QString sshDir = dir.filePath(QStringLiteral(".ssh"));
    QVERIFY(QDir().mkpath(sshDir));
    QFile config(sshDir + QStringLiteral("/config"));
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Text));
    config.write("Host 127.0.0.1\n  IdentityFile ");
    config.write(QFile::encodeName(encryptedKey));
    config.write("\n");
    config.close();

    SshConnectionPool configuredPool;
    configuredPool.setKnownHosts(KnownHosts{});
    configuredPool.setHostKeyCallback([](const QString&, const QString&,
                                         const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });
    int configPassphraseRequests = 0;
    configuredPool.setCredentialCallback(
        [&configPassphraseRequests](const QString&,
                                    SshConnectionPool::CredentialKind kind) {
            if (kind == SshConnectionPool::CredentialKind::KeyPassphrase) {
                ++configPassphraseRequests;
                return SshConnectionPool::CredentialReply{kPassphrase, false};
            }
            return SshConnectionPool::CredentialReply{};
        });
    QVERIFY2(configuredPool.connectToHost(m_host, m_port, m_user),
             "OpenSSH config IdentityFile did not authenticate through the passphrase callback");
    QCOMPARE(configPassphraseRequests, 1);
    configuredPool.disconnectFromHost();
    QVERIFY(QFile::remove(config.fileName()));

    // Profile paths are allowed to use the shell spelling people type in the
    // connect sheet. SSH_OPTIONS_IDENTITY does not expand `~` itself, so this
    // verifies CodeHarbor resolves it before handing the value to libssh.
    SshConnectionPool tildePool;
    tildePool.setKnownHosts(KnownHosts{});
    tildePool.setHostKeyCallback([](const QString&, const QString&,
                                    const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });
    int tildePassphraseRequests = 0;
    tildePool.setCredentialCallback(
        [&tildePassphraseRequests](const QString&,
                                   SshConnectionPool::CredentialKind kind) {
            if (kind == SshConnectionPool::CredentialKind::KeyPassphrase) {
                ++tildePassphraseRequests;
                return SshConnectionPool::CredentialReply{kPassphrase, false};
            }
            return SshConnectionPool::CredentialReply{};
        });
    QVERIFY2(tildePool.connectToHost(m_host, m_port, m_user,
                                     QStringLiteral("~/id")),
             "Profile Private key file beginning with ~/ did not authenticate");
    QCOMPARE(tildePassphraseRequests, 1);
    tildePool.disconnectFromHost();
}

// Windows' built-in ssh-agent uses \\.\pipe\openssh-ssh-agent rather than an
// AF_UNIX socket. Native libssh cannot talk to that pipe, but it must still
// authenticate an explicitly configured key without surfacing the agent error.
void TstLiveSsh::windowsNamedPipeAgentFallsBackToIdentityFile()
{
#ifdef Q_OS_WIN
    constexpr bool onWindows = true;
#else
    constexpr bool onWindows = false;
#endif
    if (!onWindows)
        QSKIP("Windows named-pipe regression case");
    if (m_identityFile.isEmpty())
        QSKIP("CH_LIVE_IDENTITY is not set; named-pipe regression case skipped");

    const QByteArray oldAgent = qgetenv("SSH_AUTH_SOCK");
    qputenv("SSH_AUTH_SOCK", R"(\\.\pipe\openssh-ssh-agent)");
    const auto restoreAgent = qScopeGuard([oldAgent] {
        if (oldAgent.isEmpty())
            qunsetenv("SSH_AUTH_SOCK");
        else
            qputenv("SSH_AUTH_SOCK", oldAgent);
    });

    SshConnectionPool pool;
    pool.setKnownHosts(KnownHosts{});
    pool.setHostKeyCallback([](const QString&, const QString&,
                               const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });
    QString failure;
    QObject::connect(&pool, &SshConnectionPool::errorOccurred, &pool,
                     [&failure](const QString& message) { failure = message; });

    QVERIFY2(pool.connectToHost(m_host, m_port, m_user, m_identityFile),
             qPrintable(failure));
    pool.disconnectFromHost();
}

// A desktop-launched client may not inherit SSH_AUTH_SOCK. The failure must
// name that fact and an invalid profile key path, rather than collapsing both
// into libssh's unhelpful "Access denied".
void TstLiveSsh::missingAgentAndKeyExplainAuthenticationFailure()
{
    const QByteArray oldAgent = qgetenv("SSH_AUTH_SOCK");
    qunsetenv("SSH_AUTH_SOCK");
    const auto restoreAgent = qScopeGuard([oldAgent] {
        if (oldAgent.isEmpty())
            qunsetenv("SSH_AUTH_SOCK");
        else
            qputenv("SSH_AUTH_SOCK", oldAgent);
    });

    QTemporaryDir missingKeyDir;
    QVERIFY(missingKeyDir.isValid());
    const QString missingKey = missingKeyDir.filePath(QStringLiteral("id"));

    SshConnectionPool pool;
    pool.setKnownHosts(KnownHosts{});
    pool.setHostKeyCallback([](const QString&, const QString&,
                               const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });
    QString failure;
    QObject::connect(&pool, &SshConnectionPool::errorOccurred, &pool,
                     [&failure](const QString& message) { failure = message; });

    QVERIFY(!pool.connectToHost(m_host, m_port, m_user, missingKey));
    QVERIFY2(failure.contains(
                 QStringLiteral("SSH_AUTH_SOCK is not available to this CodeHarbor process")),
             qPrintable(failure));
    QVERIFY2(failure.contains(
                 QStringLiteral("Private key file does not exist: %1").arg(missingKey)),
             qPrintable(failure));
}

// A server configured with `AuthenticationMethods publickey,password` accepts
// the key, answers SSH_AUTH_PARTIAL, and then insists on a password too. That
// answer used to be read as failure, so such a server could not be used at all.
//
// This needs a capability the fixture in docs/DEVELOPMENT.md cannot provide: its
// sshd runs unprivileged with `PasswordAuthentication no`, and an unprivileged
// sshd cannot verify an account password in the first place. So the case is
// gated on the operator pointing CH_LIVE_PASSWORD at a server that really does
// require both, and it QSKIPs with that explanation otherwise rather than
// pretending to have tested something.
void TstLiveSsh::multiStepServerAcceptsKeyThenPassword()
{
    const QString password = env("CH_LIVE_PASSWORD");
    if (password.isEmpty()) {
        QSKIP("CH_LIVE_PASSWORD is not set; the multi-step gate needs an sshd "
              "with `AuthenticationMethods publickey,password` and a real "
              "account password, which the unprivileged fixture sshd cannot "
              "provide");
    }

    SshConnectionPool pool;
    KnownHosts hosts;
    QFile store(m_knownHostsPath);
    if (store.open(QIODevice::ReadOnly | QIODevice::Text))
        hosts = KnownHosts::parse(QString::fromUtf8(store.readAll()));
    store.close();
    pool.setKnownHosts(hosts);
    pool.setHostKeyCallback([](const QString&, const QString&,
                               const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });

    // Stand where AppController stands on the LAST attempt of a credential
    // chain, with the secret already gathered: the pool has to satisfy the key
    // method and the password method inside ONE handshake.
    QList<SshConnectionPool::CredentialKind> asked;
    pool.setCredentialCallback(
        [&asked, &password](const QString&,
                            SshConnectionPool::CredentialKind kind) {
            asked.append(kind);
            if (kind == SshConnectionPool::CredentialKind::Password)
                return SshConnectionPool::CredentialReply{password, false};
            return SshConnectionPool::CredentialReply{};
        });

    QString failure;
    const auto conn =
        QObject::connect(&pool, &SshConnectionPool::errorOccurred,
                         [&failure](const QString& text) { failure += text; });
    const bool ok = pool.connectToHost(m_host, m_port, m_user, m_identityFile);
    QObject::disconnect(conn);
    QVERIFY2(ok, qPrintable(failure));
    QCOMPARE(pool.state(), SshConnectionPool::State::Connected);

    // Proof that this really was the multi-step path and not an ordinary
    // single-method connect that would have succeeded without any of it.
    QVERIFY2(asked.contains(SshConnectionPool::CredentialKind::Password),
             "the password rung was never reached: this server does not "
             "require a second method, so nothing was exercised");
    QVERIFY2(pool.diagnosticLog().contains(
                 QStringLiteral("requires a further authentication method")),
             qPrintable(pool.diagnosticLog()));
    // The log is shown to the user from the connection sheet.
    QVERIFY(!pool.diagnosticLog().contains(password));
    pool.disconnectFromHost();
}

// SH14: a keyboard-interactive-only server (PAM: PasswordAuthentication no,
// KbdInteractiveAuthentication yes) must authenticate with the user's password
// delivered through the ordinary single-secret credential prompt. Gated on the
// operator pointing CH_LIVE_KBDINT_PASSWORD at such a server, since the
// unprivileged fixture sshd cannot verify an account password.
void TstLiveSsh::keyboardInteractiveOnlyServerAuthenticatesWithPassword()
{
    const QString password = env("CH_LIVE_KBDINT_PASSWORD");
    if (password.isEmpty()) {
        QSKIP("CH_LIVE_KBDINT_PASSWORD is not set; the keyboard-interactive "
              "gate needs an sshd with `PasswordAuthentication no` and "
              "`KbdInteractiveAuthentication yes` plus a real account password, "
              "which the unprivileged fixture sshd cannot provide");
    }

    SshConnectionPool pool;
    KnownHosts hosts;
    QFile store(m_knownHostsPath);
    if (store.open(QIODevice::ReadOnly | QIODevice::Text))
        hosts = KnownHosts::parse(QString::fromUtf8(store.readAll()));
    store.close();
    pool.setKnownHosts(hosts);
    pool.setHostKeyCallback([](const QString&, const QString&,
                               const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });

    // The prompt seam AppController drives: keyboard-interactive maps each of
    // the server's prompts onto this ONE password request.
    QList<SshConnectionPool::CredentialKind> asked;
    pool.setCredentialCallback(
        [&asked, &password](const QString&,
                            SshConnectionPool::CredentialKind kind) {
            asked.append(kind);
            if (kind == SshConnectionPool::CredentialKind::Password)
                return SshConnectionPool::CredentialReply{password, false};
            return SshConnectionPool::CredentialReply{};
        });

    QString failure;
    const auto conn =
        QObject::connect(&pool, &SshConnectionPool::errorOccurred,
                         [&failure](const QString& text) { failure += text; });
    const bool ok = pool.connectToHost(m_host, m_port, m_user);
    QObject::disconnect(conn);
    QVERIFY2(ok, qPrintable(failure));
    QCOMPARE(pool.state(), SshConnectionPool::State::Connected);

    // Proof it really went through keyboard-interactive with the password
    // request rather than some other method.
    QVERIFY2(asked.contains(SshConnectionPool::CredentialKind::Password),
             "the password credential was never requested");
    QVERIFY2(pool.diagnosticLog().contains(
                 QStringLiteral("Keyboard-interactive prompt:")),
             qPrintable(pool.diagnosticLog()));
    // The transcript is user-visible; the password must never appear in it.
    QVERIFY(!pool.diagnosticLog().contains(password));
    pool.disconnectFromHost();
}

// A stored key type may be disabled in the libssh build used by the desktop
// client. It must not be passed back as SSH_OPTIONS_HOSTKEYS: connecting then
// dies during KEXINIT, before verification can reject the mismatched key.
void TstLiveSsh::unavailableTrustedAlgorithmStillReachesHostVerification()
{
    SshConnectionPool pool;
    KnownHosts hosts;
    hosts.add(SshConnectionPool::lookupHostFor(m_host, m_port),
              QStringLiteral("ssh-dss"), QByteArrayLiteral("obsolete-dsa-key"));
    pool.setKnownHosts(hosts);

    QSignalSpy mismatch(&pool, &SshConnectionPool::hostKeyMismatch);
    QString failure;
    QObject::connect(&pool, &SshConnectionPool::errorOccurred, &pool,
                     [&failure](const QString& message) { failure = message; });

    QVERIFY(!pool.connectToHost(m_host, m_port, m_user));
    QCOMPARE(mismatch.size(), 1);
    QVERIFY2(failure.contains(QStringLiteral("Host key changed")),
             qPrintable(failure));
    QVERIFY2(!failure.contains(QStringLiteral("client init buffer"),
                               Qt::CaseInsensitive),
             qPrintable(failure));
}

// The dangerous ordering for a device that outlives its session: a channel with
// a live remote process still on it when the session goes down. The pool frees
// every channel it handed out during disconnectFromHost(), so unless the device
// is told first, its poll timer keeps calling libssh on a freed ssh_channel.
// sessionClosing() is that notification; this proves the device drops the
// handle, reports end of stream exactly once, and stays inert afterwards.
// Declared last on purpose: it deliberately takes the shared session down.
void TstLiveSsh::deviceOutlivingItsSessionStopsCleanly()
{
    ensureConnected();
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);

    SshChannelDevice device(&m_pool, SshConnectionPool::ChannelKind::Exec);
    ChannelSink sink;
    sink.attach(&device);

    // `cat` copies the channel's own stdin, which nothing ever closes here, so
    // the remote end sends no EOF of its own: the channel is still live when the
    // session is torn down below.
    QVERIFY2(device.startExec(QStringLiteral("cat")), qPrintable(sink.err));
    QVERIFY(device.isOpen());
    QVERIFY(!sink.finished);

    m_pool.disconnectFromHost();

    // Synchronous: sessionClosing() reaches closeChannel() on a direct
    // connection, before the pool frees anything.
    QVERIFY(sink.finished);
    QVERIFY(!device.isOpen());
    QCOMPARE(device.bytesAvailable(), qint64(0));
    QVERIFY(!device.resizePty(80, 24));

    // Longer than the device's idle poll interval: a pump that survived the
    // teardown would fault inside libssh right here instead of doing nothing.
    QTest::qWait(100);
    QVERIFY(!device.isOpen());
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Disconnected);
}

QTEST_GUILESS_MAIN(TstLiveSsh)
#include "tst_livessh.moc"
