#include "SessionBootstrap.h"

#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "SshChannelDevice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTimer>

#include <cmath>

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
    : QObject(parent), m_pool(pool), m_client(client), m_monitor(monitor),
      m_reconnectTimer(new QTimer(this))
{
    m_knownHostsPath =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
            .filePath(QStringLiteral("known_hosts"));

    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this] {
        if (m_state != State::Reconnecting)
            return;
        ++m_attempt;
        if (!attemptWire())
            scheduleReconnect();
    });

    if (m_pool) {
        // Session-level death. Only Disconnected/Error matter: the transient
        // Connecting/HostKeyCheck/Authenticating hops belong to a connect we
        // are driving ourselves, and handleConnectionLost() ignores anything
        // that is not a loss from State::Wired anyway.
        //
        // A user-driven teardown MUST go through disconnectSession(), not
        // pool->disconnectFromHost(): SshConnectionPool::closeSession() frees
        // every channel it handed out, so a device that is still alive when the
        // pool drops the session holds a dangling ssh_channel (the device
        // destructor dereferences it — a hazard that predates this handler and
        // lives in the frozen src/ssh ownership model). disconnectSession()
        // closes the devices first, which is the correct order.
        connect(m_pool, &SshConnectionPool::stateChanged, this,
                [this](SshConnectionPool::State state) {
                    if (state == SshConnectionPool::State::Disconnected
                        || state == SshConnectionPool::State::Error)
                        handleConnectionLost(
                            QStringLiteral("SSH session went down"));
                });
        connect(m_pool, &SshConnectionPool::errorOccurred, this,
                [this](const QString& message) {
                    handleConnectionLost(QStringLiteral("SSH session error: ")
                                         + message.trimmed());
                });
    }
}

SessionBootstrap::~SessionBootstrap()
{
    // Detach before ~QObject destroys the channel devices, so neither consumer
    // is left holding a pointer to a device that is about to disappear.
    m_tearingDown = true;
    cancelReconnect();
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

int SessionBootstrap::reconnectDelaySeconds(int attempt)
{
    // Same ladder as TerminalController::reconnectDelaySeconds() (SPEC 5.6),
    // mirrored rather than called: ch_app does not link ch_terminal, and adding
    // that edge for one arithmetic helper would couple the app library to the
    // terminal subsystem. tst_sessionbootstrap pins the identical vector as
    // tst_terminalcontroller so the two cannot silently drift.
    static constexpr int schedule[] = {1, 2, 5, 10, 30};
    constexpr int count = static_cast<int>(sizeof(schedule) / sizeof(schedule[0]));
    if (attempt < 0)
        return schedule[0];
    if (attempt < count)
        return schedule[attempt];
    return 60;
}

void SessionBootstrap::setState(State next)
{
    if (m_state == next)
        return;
    m_state = next;
    emit stateChanged(m_state);
}

bool SessionBootstrap::reconnectPending() const
{
    return m_reconnectTimer->isActive();
}

int SessionBootstrap::nextReconnectDelaySeconds() const
{
    return reconnectPending() ? reconnectDelaySeconds(m_attempt) : 0;
}

void SessionBootstrap::setReconnectEnabled(bool enabled)
{
    if (m_reconnectEnabled == enabled)
        return;
    m_reconnectEnabled = enabled;
    if (enabled)
        return;
    // Switching off mid-ladder must actually stop the ladder, otherwise the
    // already-armed retry would fire once more behind the user's back.
    cancelReconnect();
    if (m_state == State::Reconnecting)
        setState(State::Disconnected);
}

void SessionBootstrap::setMaxReconnectAttempts(int attempts)
{
    m_maxAttempts = attempts;
}

void SessionBootstrap::setReconnectTimeScale(double scale)
{
    if (scale > 0.0)
        m_timeScale = scale;
}

void SessionBootstrap::cancelReconnect()
{
    m_reconnectTimer->stop();
}

void SessionBootstrap::scheduleReconnect()
{
    if (!m_reconnectEnabled) {
        setState(State::Disconnected);
        return;
    }
    if (m_maxAttempts > 0 && m_attempt >= m_maxAttempts) {
        emit error(QStringLiteral("giving up on the session after %1 reconnect "
                                  "attempts")
                       .arg(m_attempt));
        setState(State::Failed);
        return;
    }

    const int delaySeconds = reconnectDelaySeconds(m_attempt);
    // qMax(1, ...) keeps a scaled-down test interval a real timer tick rather
    // than a 0 ms spin.
    const int delayMs = qMax<int>(
        1, static_cast<int>(std::llround(delaySeconds * 1000.0 * m_timeScale)));
    setState(State::Reconnecting);
    m_reconnectTimer->start(delayMs);
    emit reconnectScheduled(m_attempt + 1, delaySeconds);
}

void SessionBootstrap::handleConnectionLost(const QString& reason)
{
    // Our own connect/teardown steps make the pool and the devices emit; those
    // are not losses. Neither is anything that arrives when there is no live
    // session to lose.
    if (m_attempting || m_tearingDown || m_state != State::Wired)
        return;

    unwire();
    emit error(reason);

    if (!m_reconnectEnabled) {
        setState(State::Disconnected);
        return;
    }
    m_attempt = 0;
    scheduleReconnect();
}

void SessionBootstrap::disconnectSession()
{
    m_tearingDown = true;
    cancelReconnect();
    unwire();
    if (m_pool)
        m_pool->disconnectFromHost();
    m_tearingDown = false;
    m_attempt = 0;
    setState(State::Disconnected);
}

void SessionBootstrap::fail(const QString& message)
{
    unwire();
    emit error(message);
}

void SessionBootstrap::unwire()
{
    const bool wasTearingDown = m_tearingDown;
    m_tearingDown = true;
    const auto restore = qScopeGuard([this, wasTearingDown] {
        m_tearingDown = wasTearingDown;
    });

    SshChannelDevice* rpc = m_rpcDevice;
    SshChannelDevice* agent = m_agentDevice;
    m_rpcDevice = nullptr;
    m_agentDevice = nullptr;

    // Close BEFORE detaching. closeChannel() emits readChannelFinished(), which
    // is the only thing that drives CodeharbordClient::onTransportClosed() and
    // therefore failAllPending(): every in-flight call gets its synthetic
    // "transport closed with request pending" error instead of hanging forever.
    // Detaching first (the original order) silently orphaned them.
    // Our own connections go first so this teardown is not re-entered as a
    // fresh loss.
    if (rpc) {
        rpc->disconnect(this);
        rpc->closeChannel();
    }
    if (agent) {
        agent->disconnect(this);
        agent->closeChannel();
    }

    if (rpc && m_client && m_client->transport() == rpc)
        m_client->setTransport(nullptr);
    if (agent && m_monitor && m_monitor->transport() == agent)
        m_monitor->setTransport(nullptr);

    // deleteLater, not delete: unwire() runs from inside a device's own
    // readChannelFinished() emission on the loss path, and deleting the sender
    // there is a use-after-free the moment the signal returns.
    if (rpc)
        rpc->deleteLater();
    if (agent)
        agent->deleteLater();
}

bool SessionBootstrap::connectPool(const QString& host, quint16 port,
                                   const QString& user)
{
    return m_pool && m_pool->connectToHost(host, port, user);
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
                // A fault that really killed the channel also reaches
                // readChannelFinished(), which is what triggers a reconnect.
                emit error(role + QStringLiteral(": ") + text.trimmed());
            });
    if (!device->startExec(command)) {
        delete device;
        return nullptr;
    }
    return device;
}

void SessionBootstrap::wireLossDetection(SshChannelDevice* device,
                                         const QString& role)
{
    // EOF on a channel means the remote end of this session is gone: the peer
    // process exited, the session dropped, or libssh faulted (SshChannelDevice
    // emits readChannelFinished() for all three). channelError() alone is NOT a
    // loss — it also carries plain remote stderr, e.g. the bridge banner.
    connect(device, &SshChannelDevice::readChannelFinished, this,
            [this, role] {
                handleConnectionLost(role + QStringLiteral(" channel closed"));
            });
}

bool SessionBootstrap::attemptWire()
{
    if (!m_pool) {
        emit error(QStringLiteral("no SSH connection pool"));
        return false;
    }

    // Everything below provokes pool and device signals of its own; none of
    // them is a loss of a live session.
    m_attempting = true;
    const auto clearAttempting = qScopeGuard([this] { m_attempting = false; });

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

    if (!connectPool(m_host, m_port, m_user)) {
        emit error(QStringLiteral("SSH connection to %1:%2 failed")
                       .arg(m_host)
                       .arg(m_port));
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
                                    rpcCommand(m_nodePath, m_repoRoot),
                                    QStringLiteral("codeharbord"));
    if (!m_rpcDevice) {
        fail(QStringLiteral("could not start codeharbord over SSH"));
        return false;
    }
    wireLossDetection(m_rpcDevice, QStringLiteral("codeharbord"));
    if (m_client)
        m_client->setTransport(m_rpcDevice);

    m_agentDevice = openChannelDevice(SshConnectionPool::ChannelKind::AgentStatus,
                                      bridgeCommand(m_nodePath, m_repoRoot),
                                      QStringLiteral("codeharbor-bridge"));
    if (!m_agentDevice) {
        fail(QStringLiteral("could not start codeharbor-bridge over SSH"));
        return false;
    }
    wireLossDetection(m_agentDevice, QStringLiteral("codeharbor-bridge"));
    if (m_monitor)
        m_monitor->setTransport(m_agentDevice);

    m_attempt = 0;
    setState(State::Wired);
    emit wired();
    return true;
}

bool SessionBootstrap::connectAndWire(const QString& host, quint16 port,
                                      const QString& user,
                                      const QString& nodePath,
                                      const QString& repoRoot)
{
    // Remember the target: every automatic retry replays exactly this call.
    m_host = host;
    m_port = port;
    m_user = user;
    m_nodePath = nodePath;
    m_repoRoot = repoRoot;

    cancelReconnect();
    m_attempt = 0;
    setState(State::Connecting);

    if (attemptWire())
        return true;

    // A user-initiated connect that never came up is reported to its caller
    // (which returns false all the way to the UI) rather than retried behind
    // its back: there is no established session to survive yet. Only a loss
    // from State::Wired arms the ladder.
    setState(State::Failed);
    return false;
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
