#include "SessionBootstrap.h"

#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "SshChannelDevice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace ch {

namespace {

// Quote one argv element for the remote login shell. SSH exec requests carry a
// single command string that the server hands to the user's shell, so paths
// with spaces (or anything else shell-special) must be protected.
QString shellQuote(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString remoteJoin(const QString& root, const QString& relative)
{
    QString base = root;
    while (base.endsWith(QLatin1Char('/')) && base.size() > 1)
        base.chop(1);
    return base + QLatin1Char('/') + relative;
}

} // namespace

SessionBootstrap::SessionBootstrap(SshConnectionPool* pool,
                                   CodeharbordClient* client,
                                   AgentStatusMonitor* monitor, QObject* parent)
    : QObject(parent), m_pool(pool), m_client(client), m_monitor(monitor)
{
    m_knownHostsPath =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
            .filePath(QStringLiteral("known_hosts"));
}

SessionBootstrap::~SessionBootstrap()
{
    // Detach before ~QObject destroys the channel devices, so neither consumer
    // is left holding a pointer to a device that is about to disappear.
    unwire();
}

void SessionBootstrap::setKnownHostsPath(const QString& path)
{
    m_knownHostsPath = path;
}

QString SessionBootstrap::rpcCommand(const QString& nodePath,
                                     const QString& repoRoot)
{
    // The packaged entry point is `codeharbord rpc --stdio` (SPEC 10.1); over a
    // dev checkout we run the TypeScript source directly, which node >= 23.6
    // strips natively, so no build step is required on the remote side.
    return shellQuote(nodePath) + QLatin1Char(' ')
           + shellQuote(remoteJoin(repoRoot,
                                   QStringLiteral("remote/src/codeharbord.ts")))
           + QStringLiteral(" rpc --stdio");
}

QString SessionBootstrap::bridgeCommand(const QString& nodePath,
                                        const QString& repoRoot)
{
    // codeharbor-bridge (remote/package.json bin -> dist/bridge.js, source
    // remote/src/bridge.ts). Its main guard starts the socket relay and writes
    // AgentEvent JSONL on stdout; its "listening on ..." banner goes to stderr,
    // which SshChannelDevice keeps out of the monitor's byte stream.
    //
    // The bridge is wrapped in a stdin watchdog because it must not outlive the
    // session. An SSH exec channel has no controlling terminal, so closing it
    // sends no SIGHUP: it only closes the pipes. codeharbord exits on stdin EOF
    // by itself, but the bridge holds a listening Unix socket and would idle
    // forever, leaking one orphan per app launch. `cat` reaching EOF is the
    // channel-closed signal; it then kills the relay. Only the bridge gets this
    // — codeharbord needs its stdin for the JSON-RPC request stream.
    const QString relay =
        shellQuote(nodePath) + QLatin1Char(' ')
        + shellQuote(remoteJoin(repoRoot, QStringLiteral("remote/src/bridge.ts")))
        + QStringLiteral(
            " & __ch_bridge=$!; cat >/dev/null; kill $__ch_bridge 2>/dev/null");
    // Quoted as one argument to `sh -c` so the script only relies on POSIX sh,
    // whatever login shell the remote account happens to use.
    return QStringLiteral("sh -c ") + shellQuote(relay);
}

void SessionBootstrap::fail(const QString& message)
{
    unwire();
    emit error(message);
}

void SessionBootstrap::unwire()
{
    if (m_client && m_client->transport()
        && (m_client->transport() == m_rpcDevice))
        m_client->setTransport(nullptr);
    if (m_monitor && m_monitor->transport()
        && (m_monitor->transport() == m_agentDevice))
        m_monitor->setTransport(nullptr);

    delete m_rpcDevice;
    m_rpcDevice = nullptr;
    delete m_agentDevice;
    m_agentDevice = nullptr;
}

SshChannelDevice* SessionBootstrap::openChannelDevice(
    SshConnectionPool::ChannelKind kind, const QString& command,
    const QString& role)
{
    auto* device = new SshChannelDevice(m_pool, kind, this);
    connect(device, &SshChannelDevice::channelError, this,
            [this, role](const QString& text) {
                // Remote diagnostics (stderr, libssh faults) are surfaced but
                // never fatal on their own: the bridge banner arrives this way.
                emit error(role + QStringLiteral(": ") + text.trimmed());
            });
    if (!device->startExec(command)) {
        delete device;
        return nullptr;
    }
    return device;
}

bool SessionBootstrap::connectAndWire(const QString& host, quint16 port,
                                      const QString& user,
                                      const QString& nodePath,
                                      const QString& repoRoot)
{
    if (!m_pool) {
        emit error(QStringLiteral("no SSH connection pool"));
        return false;
    }

    unwire();

    // First-use trust: load whatever we already trust, accept an UNKNOWN key
    // once and persist it. Verdict::Mismatch never reaches this callback — the
    // pool refuses a changed key outright (SPEC 12.1) and that stays untouched.
    KnownHosts hosts;
    QFile store(m_knownHostsPath);
    if (store.open(QIODevice::ReadOnly | QIODevice::Text))
        hosts = KnownHosts::parse(QString::fromUtf8(store.readAll()));
    store.close();
    const int knownBefore = hosts.entries().size();
    m_pool->setKnownHosts(hosts);
    m_pool->setHostKeyCallback([](const QString&, const QString&,
                                  const QByteArray&, KnownHosts::Verdict) {
        return SshConnectionPool::HostKeyDecision::Accept;
    });

    if (!m_pool->connectToHost(host, port, user)) {
        emit error(QStringLiteral("SSH connection to %1:%2 failed")
                       .arg(host)
                       .arg(port));
        return false;
    }

    if (m_pool->knownHosts().entries().size() != knownBefore) {
        const QFileInfo info(m_knownHostsPath);
        QDir().mkpath(info.absolutePath());
        QFile out(m_knownHostsPath);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            out.write(m_pool->knownHosts().serialize());
    }

    m_rpcDevice = openChannelDevice(SshConnectionPool::ChannelKind::Rpc,
                                    rpcCommand(nodePath, repoRoot),
                                    QStringLiteral("codeharbord"));
    if (!m_rpcDevice) {
        fail(QStringLiteral("could not start codeharbord over SSH"));
        return false;
    }
    if (m_client)
        m_client->setTransport(m_rpcDevice);

    m_agentDevice = openChannelDevice(SshConnectionPool::ChannelKind::AgentStatus,
                                      bridgeCommand(nodePath, repoRoot),
                                      QStringLiteral("codeharbor-bridge"));
    if (!m_agentDevice) {
        fail(QStringLiteral("could not start codeharbor-bridge over SSH"));
        return false;
    }
    if (m_monitor)
        m_monitor->setTransport(m_agentDevice);

    emit wired();
    return true;
}

bool SessionBootstrap::connectAndWireFromEnvironment()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        return false;  // normal desktop launch: no server, nothing to wire

    const QString host = qEnvironmentVariable("CH_LIVE_HOST");
    const QString user = qEnvironmentVariable("CH_LIVE_USER");
    const QString nodePath = qEnvironmentVariable("CH_LIVE_NODE");
    const QString repoRoot = qEnvironmentVariable("CH_LIVE_REPO");
    bool portOk = false;
    const uint port = qEnvironmentVariable("CH_LIVE_PORT").toUInt(&portOk);

    if (host.isEmpty() || user.isEmpty() || nodePath.isEmpty()
        || repoRoot.isEmpty() || !portOk || port == 0 || port > 65535) {
        emit error(QStringLiteral(
            "CH_LIVE_SSH is set but CH_LIVE_HOST/PORT/USER/NODE/REPO are "
            "incomplete"));
        return false;
    }

    const QString knownHosts = qEnvironmentVariable("CH_LIVE_KNOWN_HOSTS");
    if (!knownHosts.isEmpty())
        setKnownHostsPath(knownHosts);

    return connectAndWire(host, static_cast<quint16>(port), user, nodePath,
                          repoRoot);
}

} // namespace ch
