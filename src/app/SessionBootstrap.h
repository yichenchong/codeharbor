#pragma once

#include "SshConnectionPool.h"

#include <QObject>
#include <QPointer>
#include <QString>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace ch {

class AgentStatusMonitor;
class CodeharbordClient;
class SshChannelDevice;
class SshConnectionPool;

// Brings a remote workspace session up and hands its byte streams to the
// consumers that already speak QIODevice (SPEC 5.3, 6.4, 10.1).
//
// Until this existed, CodeharbordClient::setTransport() and
// AgentStatusMonitor::setTransport() had no production caller: the pool could
// authenticate but nothing ever attached a channel to the RPC client, so the
// shipped app could not reach a server at all. connectAndWire() is that seam.
//
// Sequence: connect the pool -> open the Rpc channel and exec
// `codeharbord rpc --stdio` -> client->setTransport() -> open the AgentStatus
// channel and exec the codeharbor-bridge relay -> monitor->setTransport().
// Any failure emits error() and returns false, leaving nothing half-wired.
//
// Reconnect (SPEC 5.6): once wired, an EOF on either channel or a pool-level
// fault means the remote session is gone. Both devices are torn down and a
// retry is scheduled on the project's standard backoff ladder — 1, 2, 5, 10,
// 30 then 60 seconds — until the attempt cap is reached. See
// reconnectDelaySeconds().
class SessionBootstrap : public QObject {
    Q_OBJECT
public:
    // Lifecycle of the wired session. Disconnected is also the state reached
    // after disconnectSession() or a loss with reconnect disabled; Failed means
    // "not wired and not retrying" (a user-initiated connect that failed, or
    // the reconnect ladder exhausted).
    enum class State {
        Disconnected,
        Connecting,
        Wired,
        Reconnecting,
        Failed,
    };
    Q_ENUM(State)

    Q_PROPERTY(State state READ state NOTIFY stateChanged)

    explicit SessionBootstrap(SshConnectionPool* pool, CodeharbordClient* client,
                              AgentStatusMonitor* monitor,
                              QObject* parent = nullptr);
    ~SessionBootstrap() override;

    // known_hosts store used for the first-use trust decision. Loaded before
    // connecting and rewritten after a newly trusted key is accepted. Defaults
    // to <AppConfigLocation>/known_hosts.
    void setKnownHostsPath(const QString& path);
    QString knownHostsPath() const { return m_knownHostsPath; }

    // `nodePath` is the remote node binary (it need not be on the login PATH)
    // and `repoRoot` the remote CodeHarbor checkout holding remote/src.
    bool connectAndWire(const QString& host, quint16 port, const QString& user,
                        const QString& nodePath, const QString& repoRoot);

    // Env-driven variant used by the live gate and by main.cpp so a normal
    // desktop launch stays server-less. Reads CH_LIVE_SSH (must be set),
    // CH_LIVE_HOST, CH_LIVE_PORT, CH_LIVE_USER, CH_LIVE_NODE, CH_LIVE_REPO and
    // the optional CH_LIVE_KNOWN_HOSTS override. Returns false WITHOUT emitting
    // error() when CH_LIVE_SSH is unset; that is the normal desktop path.
    bool connectAndWireFromEnvironment();

    SshChannelDevice* rpcDevice() const { return m_rpcDevice; }
    SshChannelDevice* agentDevice() const { return m_agentDevice; }

    State state() const { return m_state; }

    // Reconnect attempts made since the last successful wire; 0 while Wired.
    int reconnectAttempt() const { return m_attempt; }
    // Seconds the currently scheduled retry waits, 0 when none is pending.
    int nextReconnectDelaySeconds() const;
    bool reconnectPending() const;

    // Automatic reconnect is on by default. Turning it off cancels a pending
    // retry, so a user who asked to stay disconnected is not dragged back.
    void setReconnectEnabled(bool enabled);
    bool reconnectEnabled() const { return m_reconnectEnabled; }

    // Cap on consecutive reconnect attempts before giving up with State::Failed
    // (default kDefaultMaxReconnectAttempts). <= 0 retries forever at 60 s.
    void setMaxReconnectAttempts(int attempts);
    int maxReconnectAttempts() const { return m_maxAttempts; }

    // Multiplies every scheduled backoff delay. Production keeps 1.0; the unit
    // test shrinks it so the whole ladder runs in milliseconds instead of
    // minutes. Values <= 0 are ignored.
    void setReconnectTimeScale(double scale);
    double reconnectTimeScale() const { return m_timeScale; }

    // User-initiated teardown: cancels any pending retry, unwires both
    // consumers, drops the SSH session and ends in State::Disconnected WITHOUT
    // scheduling a reconnect.
    void disconnectSession();

    // Retry delay in seconds for the Nth (0-based) automatic reconnect attempt:
    // 1, 2, 5, 10, 30, then 60 thereafter (SPEC 5.6). Mirrors
    // TerminalController::reconnectDelaySeconds() value for value —
    // tst_sessionbootstrap pins the same vector the terminal test pins — rather
    // than making ch_app link ch_terminal for six numbers.
    static int reconnectDelaySeconds(int attempt);

    // Consecutive failed reconnects tolerated before State::Failed. Ten spans
    // 1+2+5+10+30+60*5 = 348 s of retrying, long enough to ride out a laptop
    // suspend or a Wi-Fi handover, short enough that a server that is really
    // gone stops being polled forever behind the user's back.
    static constexpr int kDefaultMaxReconnectAttempts = 10;

    // Remote command lines, exposed so tests and diagnostics assert the exact
    // strings that are executed rather than reconstructing them.
    static QString rpcCommand(const QString& nodePath, const QString& repoRoot);
    static QString bridgeCommand(const QString& nodePath,
                                 const QString& repoRoot);

signals:
    void wired();
    void error(const QString& message);
    void stateChanged(ch::SessionBootstrap::State state);
    // A retry is now armed: `attempt` is 1-based (the first retry after a loss
    // is 1) and `delaySeconds` is how long it waits. Lets the shell say
    // "reconnecting in 30 s" without duplicating the ladder.
    void reconnectScheduled(int attempt, int delaySeconds);

protected:
    // Test seams. The two side-effecting steps of one wire attempt, isolated so
    // tst_sessionbootstrap can drive the whole state machine (connect ok/fail,
    // channel loss, the retry ladder) with no SSH server in reach. Production
    // behaviour lives in these base implementations; nothing else overrides.
    virtual bool connectPool(const QString& host, quint16 port,
                             const QString& user);
    virtual SshChannelDevice* openChannelDevice(
        SshConnectionPool::ChannelKind kind, const QString& command,
        const QString& role);

private:
    bool attemptWire();
    void wireLossDetection(SshChannelDevice* device, const QString& role);
    void handleConnectionLost(const QString& reason);
    void scheduleReconnect();
    void cancelReconnect();
    void unwire();
    void fail(const QString& message);
    void setState(State next);

    SshConnectionPool* m_pool = nullptr;
    QPointer<CodeharbordClient> m_client;
    QPointer<AgentStatusMonitor> m_monitor;
    SshChannelDevice* m_rpcDevice = nullptr;
    SshChannelDevice* m_agentDevice = nullptr;
    QString m_knownHostsPath;

    State m_state = State::Disconnected;
    QTimer* m_reconnectTimer = nullptr;
    // Last target handed to connectAndWire(), replayed by every retry.
    QString m_host;
    quint16 m_port = 0;
    QString m_user;
    QString m_nodePath;
    QString m_repoRoot;
    int m_attempt = 0;
    int m_maxAttempts = kDefaultMaxReconnectAttempts;
    double m_timeScale = 1.0;
    bool m_reconnectEnabled = true;
    // Set while we are inside our own connect/teardown, so the pool and device
    // signals those steps provoke are not mistaken for a fresh loss.
    bool m_attempting = false;
    bool m_tearingDown = false;
};

} // namespace ch
