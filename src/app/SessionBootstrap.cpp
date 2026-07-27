#include "SessionBootstrap.h"

#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "SshChannelDevice.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpSocket>
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

// POSIX sh that leaves the first existing candidate in $__ch_entry, or names
// every path it tried on stderr and exits 127. The selection happens on the
// SERVER, inside the exec we were going to issue anyway: a client-side probe
// would cost an extra round trip on every connect and would still be a guess
// (the probe and the exec are two different moments), and a per-profile
// "layout" field would make every user answer a question about our build
// system. `stem` is the entry name and doubles as the label in the error.
QString selectEntry(const QString& repoRoot, const QString& stem)
{
    const QStringList candidates =
        SessionBootstrap::entryCandidates(repoRoot, stem);
    QStringList quoted;
    quoted.reserve(candidates.size());
    for (const QString& candidate : candidates)
        quoted << shellQuote(candidate);

    return QStringLiteral("__ch_entry=; for __ch_c in ")
           + quoted.join(QLatin1Char(' '))
           + QStringLiteral("; do if [ -f \"$__ch_c\" ]; then "
                            "__ch_entry=\"$__ch_c\"; break; fi; done; "
                            "if [ -z \"$__ch_entry\" ]; then echo ")
           + shellQuote(QStringLiteral("codeharbor: no %1 entry point on this "
                                       "server. Tried: %2")
                            .arg(stem, candidates.join(QStringLiteral(", "))))
           + QStringLiteral(" >&2; exit 127; fi; ");
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
        // m_attempting: probeEndpoint() spins a nested event loop, so this
        // timer really can fire while an attempt is already in flight. Firing
        // then would re-enter attemptWire() and unwire the session the outer
        // attempt is halfway through building.
        if (m_state != State::Reconnecting || m_attempting)
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

QStringList SessionBootstrap::entryCandidates(const QString& repoRoot,
                                              const QString& stem)
{
    // Most-built first. Two layouts have to work, because the client is the
    // only thing that decides which one it can talk to:
    //
    //   <root>/dist/<stem>.js         codeharbor-remote.tar.gz unpacked — the
    //                                 RELEASE artifact, which is `dist`,
    //                                 `package.json` and `sql` side by side
    //                                 (.github/workflows/release.yml). This is
    //                                 what a normal user installs, and until it
    //                                 was listed here nothing could launch it.
    //   <root>/remote/dist/<stem>.js  a dev checkout that has been built.
    //   <root>/remote/src/<stem>.ts   a dev checkout, source, type-stripped by
    //                                 node >= 23.6.
    //
    // Built output is preferred over source: it is what `package.json` bin
    // points at, and it drops the node >= 23.6 requirement.
    return {
        remoteJoin(repoRoot, QStringLiteral("dist/") + stem + QStringLiteral(".js")),
        remoteJoin(repoRoot, QStringLiteral("remote/dist/") + stem + QStringLiteral(".js")),
        remoteJoin(repoRoot, QStringLiteral("remote/src/") + stem + QStringLiteral(".ts")),
    };
}

QString SessionBootstrap::rpcCommand(const QString& nodePath,
                                     const QString& repoRoot)
{
    // The packaged entry point is `codeharbord rpc --stdio` (SPEC 10.1).
    // `exec` replaces the selecting shell with node, so codeharbord keeps the
    // channel's stdin (its JSON-RPC request stream) and still exits on EOF
    // without an extra process sitting in between.
    const QString script = selectEntry(repoRoot, QStringLiteral("codeharbord"))
                           + QStringLiteral("exec ") + shellQuote(nodePath)
                           + QStringLiteral(" \"$__ch_entry\" rpc --stdio");
    return QStringLiteral("sh -c ") + shellQuote(script);
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
    const QString script =
        selectEntry(repoRoot, QStringLiteral("bridge")) + shellQuote(nodePath)
        + QStringLiteral(" \"$__ch_entry\""
                         " & __ch_bridge=$!; cat >/dev/null;"
                         " kill $__ch_bridge 2>/dev/null");
    // Quoted as one argument to `sh -c` so the script only relies on POSIX sh,
    // whatever login shell the remote account happens to use.
    return QStringLiteral("sh -c ") + shellQuote(script);
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
    // already-armed retry would fire once more behind the user's back — and a
    // retry that is already inside its connect pre-flight must unwind rather
    // than finish wiring a session the user just opted out of.
    cancelReconnect();
    abortAttempt();
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

void SessionBootstrap::setConnectTimeoutMs(int ms)
{
    m_connectTimeoutMs = qMax(0, ms);
}

void SessionBootstrap::cancelReconnect()
{
    m_reconnectTimer->stop();
}

void SessionBootstrap::abortAttempt()
{
    if (!m_attempting)
        return;
    m_cancelRequested = true;
    if (m_probeLoop)
        m_probeLoop->quit();
}

void SessionBootstrap::scheduleReconnect()
{
    // The attempt whose failure got us here was cancelled, not lost: the user
    // has already been put into Disconnected and must stay there.
    if (m_cancelRequested)
        return;
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
    // An attempt parked in the connect pre-flight is interrupted first, so the
    // user's "disconnect" is honoured now rather than after the remaining
    // connect budget — and so the unwinding attempt does not go on to wire the
    // very session that is being torn down here.
    abortAttempt();
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

    // Scoped to the session that just ended: the next attempt's loss must carry
    // ITS remote's words, never the previous one's.
    m_channelDiagnostics.clear();
}

bool SessionBootstrap::connectPool(const QString& host, quint16 port,
                                   const QString& user)
{
    return m_pool && m_pool->connectToHost(host, port, user);
}

bool SessionBootstrap::probeEndpoint(const QString& host, quint16 port,
                                     QString* error)
{
    // Everything here is stack-local and event-loop driven; waitForConnected()
    // and friends would block the GUI thread exactly as hard as libssh does,
    // which is the thing being fixed.
    QTcpSocket socket;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    // A connect budget that a coarse timer may fire up to 5% early on is not a
    // budget. This one is asserted against, so it is honoured exactly.
    deadline.setTimerType(Qt::PreciseTimer);

    bool spoke = false;
    bool timedOut = false;

    // The server's identification string (RFC 4253 §4.2) is sent as soon as the
    // TCP connection is up, so the first byte is a sufficient liveness proof —
    // and it is a far more robust one than parsing for "SSH-", which a server
    // may legally precede with arbitrary lines. The bytes are left unread; the
    // socket is a throwaway and libssh opens its own.
    connect(&socket, &QIODevice::readyRead, &loop, [&] {
        spoke = true;
        loop.quit();
    });
    // A peer that hangs up without speaking is dead for our purposes, and so is
    // any resolve/connect failure.
    connect(&socket, &QAbstractSocket::errorOccurred, &loop,
            [&loop](QAbstractSocket::SocketError) { loop.quit(); });
    connect(&socket, &QAbstractSocket::disconnected, &loop, [&loop] {
        loop.quit();
    });
    connect(&deadline, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });

    socket.connectToHost(host, port, QIODevice::ReadOnly);
    deadline.start(m_connectTimeoutMs);
    // Published so abortAttempt() can cut the wait short; cleared on every exit
    // path, including the exceptional one.
    m_probeLoop = &loop;
    const auto clearLoop = qScopeGuard([this] { m_probeLoop = nullptr; });
    // ExcludeUserInputEvents: repaints, timers and sockets keep running so the
    // shell stays alive on screen, but a second click cannot re-enter connect.
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    if (spoke && !m_cancelRequested)
        return true;

    if (error) {
        if (m_cancelRequested)
            *error = QStringLiteral("connection attempt cancelled");
        else if (timedOut)
            *error = QStringLiteral("%1:%2 did not answer within %3 ms")
                         .arg(host)
                         .arg(port)
                         .arg(m_connectTimeoutMs);
        else
            *error = QStringLiteral("cannot reach %1:%2 — %3")
                         .arg(host)
                         .arg(port)
                         .arg(socket.errorString());
    }
    socket.abort();
    return false;
}

SshChannelDevice* SessionBootstrap::openChannelDevice(
    SshConnectionPool::ChannelKind kind, const QString& command,
    const QString& role)
{
    // `role` only labels the long-lived diagnostics stream, which is wired by
    // wireChannelSignals() rather than here.
    Q_UNUSED(role);

    auto* device = new SshChannelDevice(m_pool, kind, this);
    // Scoped to the exec request and nothing else. startExec() reports failure
    // as a bare false and explains itself through channelError() a moment
    // earlier, so the explanation is captured here or lost. The long-lived
    // diagnostics hook is wireChannelSignals(), applied by attemptWire() once
    // the device exists — deliberately NOT here, so it also covers the devices
    // handed back by the openChannelDevice() test seam.
    m_lastDiagnostic.clear();
    const QMetaObject::Connection capture =
        connect(device, &SshChannelDevice::channelError, this,
                [this](const QString& text) {
                    const QString trimmed = text.trimmed();
                    if (!trimmed.isEmpty())
                        m_lastDiagnostic = trimmed;
                });
    const bool started = device->startExec(command);
    disconnect(capture);
    if (!started) {
        delete device;
        return nullptr;
    }
    return device;
}

QString SessionBootstrap::withLastDiagnostic(const QString& message) const
{
    // startExec() reports failure as a bare false and explains itself through
    // channelError() a moment earlier, so the explanation has to be carried
    // over by hand or the user gets "could not start codeharbord over SSH" with
    // no hint that the real answer was "could not open SSH channel".
    if (m_lastDiagnostic.isEmpty())
        return message;
    return message + QStringLiteral(": ") + m_lastDiagnostic;
}

void SessionBootstrap::wireChannelSignals(SshChannelDevice* device,
                                          const QString& role)
{
    // The two things a channel can tell us, and the whole reason they are wired
    // side by side: they are NOT the same news, and conflating them has burned
    // this code twice in opposite directions.
    //
    // channelError() is the remote process's STDERR plus libssh channel faults.
    // An SSH exec channel has exactly one stderr and the process writes
    // whatever it likes to it, so this stream is mostly chatter —
    // codeharbor-bridge announces "listening on /run/user/<uid>/codeharbor.sock"
    // on every launch. Treating it as a loss would tear down healthy sessions;
    // treating it as an error() put "codeharbor-bridge: codeharbor-bridge
    // listening on ..." in front of the user as a failure toast. It is
    // DIAGNOSTICS: republished for logs and the UI's own use, never a verdict.
    connect(device, &SshChannelDevice::channelError, this,
            [this, role](const QString& text) {
                const QString trimmed = text.trimmed();
                if (trimmed.isEmpty())
                    return;
                // Remembered per role so that when this channel dies its own
                // last words go with the loss. Without it a remote process that
                // execs fine and THEN explains itself before exiting — `sh`
                // reporting that repoRoot holds no codeharbord entry point, for
                // one — reached only channelDiagnostic(), which nothing
                // consumes, and the user was told "codeharbord channel closed"
                // with no reason at all.
                m_channelDiagnostics[role] = trimmed;
                emit channelDiagnostic(role, trimmed);
            });

    // readChannelFinished() is EOF, and EOF is the one thing that actually
    // proves the far end is gone: the peer exited, the session dropped, or
    // libssh faulted (SshChannelDevice emits it for all three). This, and only
    // this, is a loss.
    connect(device, &SshChannelDevice::readChannelFinished, this,
            [this, role] {
                QString reason = role + QStringLiteral(" channel closed");
                const QString last = m_channelDiagnostics.value(role);
                if (!last.isEmpty())
                    reason += QStringLiteral(": ") + last;
                handleConnectionLost(reason);
            });
}

bool SessionBootstrap::attemptWire()
{
    if (!m_pool) {
        emit error(QStringLiteral("no SSH connection pool"));
        return false;
    }

    // Everything below provokes pool and device signals of its own; none of
    // them is a loss of a live session. It also guards re-entry: probeEndpoint()
    // runs a nested event loop, so the reconnect timer and connectAndWire() can
    // both come back round while we are still in here.
    m_attempting = true;
    m_cancelRequested = false;
    QElapsedTimer clock;
    clock.start();
    const auto clearAttempting = qScopeGuard([this, &clock] {
        m_lastAttemptMs = clock.elapsed();
        m_attempting = false;
    });

    unwire();

    // Bounded liveness check BEFORE the blocking libssh handshake, so an
    // unreachable or mute endpoint costs connectTimeoutMs() of responsive UI
    if (m_connectTimeoutMs > 0) {
        QString reason;
        if (!probeEndpoint(m_host, m_port, &reason)) {
            // A cancellation is the user's own doing, not a fault to report.
            if (!m_cancelRequested)
                emit error(reason);
            return false;
        }
    }

    // Trust policy. Load whatever we already trust; Verdict::Mismatch never
    // reaches a callback at all — the pool refuses a changed key outright
    // (SPEC 12.1) and that stays untouched.
    //
    // The accept-an-unknown-key-once default is ONLY for headless/unattended use
    // (the CH_LIVE_* env path and tests), where there is nobody to ask. If a
    // caller has already installed its own policy — AppController installs a
    // prompting callback that refuses the key and asks the user — we MUST NOT
    // replace it: doing so silently trusted and persisted unknown host keys with
    // no consent and made the whole host-key prompt dead code.
    KnownHosts hosts;
    QFile store(m_knownHostsPath);
    if (store.open(QIODevice::ReadOnly | QIODevice::Text))
        hosts = KnownHosts::parse(QString::fromUtf8(store.readAll()));
    store.close();
    const int knownBefore = hosts.entries().size();
    m_pool->setKnownHosts(hosts);
    if (!m_pool->hostKeyCallback()) {
        m_pool->setHostKeyCallback([](const QString&, const QString&,
                                      const QByteArray&, KnownHosts::Verdict) {
            return SshConnectionPool::HostKeyDecision::Accept;
        });
    }

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
        fail(withLastDiagnostic(
            QStringLiteral("could not start codeharbord over SSH")));
        return false;
    }
    wireChannelSignals(m_rpcDevice, QStringLiteral("codeharbord"));
    if (m_client)
        m_client->setTransport(m_rpcDevice);

    m_agentDevice = openChannelDevice(SshConnectionPool::ChannelKind::AgentStatus,
                                      bridgeCommand(m_nodePath, m_repoRoot),
                                      QStringLiteral("codeharbor-bridge"));
    if (!m_agentDevice) {
        fail(withLastDiagnostic(
            QStringLiteral("could not start codeharbor-bridge over SSH")));
        return false;
    }
    wireChannelSignals(m_agentDevice, QStringLiteral("codeharbor-bridge"));
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
    // A connect already in flight owns m_host/m_port and is parked in
    // probeEndpoint()'s nested event loop. Overwriting the target underneath it
    // would wire a session to one host and report it as another, so a second
    // request is refused rather than interleaved.
    if (m_attempting) {
        emit error(QStringLiteral("a connection attempt is already in progress"));
        return false;
    }

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

    // A connect the user cancelled mid-flight already ended in Disconnected via
    // disconnectSession(); relabelling it Failed would put an error banner on
    // an action the user took deliberately.
    if (m_cancelRequested)
        return false;

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
