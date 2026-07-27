#pragma once

// AgentStatusMonitor and CodeharbordClient are INCLUDED, not forward-declared:
// both are held in QPointer members below, and QPointer needs a complete type
// to prove convertibility to QObject* (Qt 6.6 rejects the incomplete form that
// 6.10 accepts).
#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "SshConnectionPool.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QEventLoop;
class QTimer;
QT_END_NAMESPACE

namespace ch {

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

    // Upper bound, in milliseconds, on how long connectAndWire() may stall
    // before the endpoint has proven it is a live SSH server (see
    // probeEndpoint()). 0 disables the pre-flight probe entirely and hands the
    // stall back to libssh's own default. Default kDefaultConnectTimeoutMs.
    void setConnectTimeoutMs(int ms);
    int connectTimeoutMs() const { return m_connectTimeoutMs; }

    // Wall time of the last completed wire attempt (probe + handshake + both
    // channel execs), in milliseconds. -1 before the first attempt. Exposed so
    // the latency gate measures the real thing instead of re-timing a
    // reimplementation of it.
    qint64 lastAttemptMs() const { return m_lastAttemptMs; }

    // `nodePath` is the remote node binary (it need not be on the login PATH)
    // and `repoRoot` the remote CodeHarbor installation: either an unpacked
    // codeharbor-remote.tar.gz or a git checkout — see entryCandidates().
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
    // retry AND aborts a retry that is already inside its connect pre-flight,
    // so a user who asked to stay disconnected is not dragged back.
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

    // User-initiated teardown: cancels any pending retry, aborts an in-flight
    // connect attempt (probeEndpoint()'s nested event loop returns at once),
    // unwires both consumers, drops the SSH session and ends in
    // State::Disconnected WITHOUT scheduling a reconnect.
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

    // Pre-flight budget for one connect attempt. Five seconds is long enough
    // for a transcontinental TCP handshake plus banner on a bad day and short
    // enough that a mistyped host does not read as a hung application. It also
    // sits below libssh 0.11's undocumented 10 s internal default, so it is the
    // bound that actually decides.
    static constexpr int kDefaultConnectTimeoutMs = 5000;

    // Remote command lines, exposed so tests and diagnostics assert the exact
    // strings that are executed rather than reconstructing them.
    static QString rpcCommand(const QString& nodePath, const QString& repoRoot);
    static QString bridgeCommand(const QString& nodePath,
                                 const QString& repoRoot);

    // Every path, most-preferred first, that a remote entry point named `stem`
    // ("codeharbord" or "bridge") may live at under `repoRoot`. The commands
    // above pick the first that exists ON THE SERVER and, when none does, name
    // exactly this list on stderr, so "it does not launch" is answerable
    // without an SSH session of your own.
    static QStringList entryCandidates(const QString& repoRoot,
                                       const QString& stem);

signals:
    void wired();
    // A FAILURE the user needs to see. AppController surfaces this verbatim in
    // a toast, so nothing routine may ever reach it — see channelDiagnostic().
    void error(const QString& message);
    void stateChanged(ch::SessionBootstrap::State state);
    // A retry is now armed: `attempt` is 1-based (the first retry after a loss
    // is 1) and `delaySeconds` is how long it waits. Lets the shell say
    // "reconnecting in 30 s" without duplicating the ladder.
    void reconnectScheduled(int attempt, int delaySeconds);
    // Informational output from a remote process on one of the session's
    // channels: its stderr, plus libssh channel faults. `role` is
    // "codeharbord" or "codeharbor-bridge".
    //
    // NOT a failure, and deliberately not error(). An SSH exec channel has one
    // stderr and every remote process writes whatever it likes to it, so this
    // stream carries ordinary startup chatter — codeharbor-bridge announces
    // "listening on /run/user/<uid>/codeharbor.sock" on every single launch.
    // That used to be forwarded to error(), which AppController shows verbatim,
    // so a perfectly healthy session greeted the user with error toasts
    // reporting that it had started correctly.
    //
    // A channel that really died reaches the user through the paths that can
    // actually tell: startExec() failing during wiring (fail()), and EOF on a
    // live channel (handleConnectionLost()). stderr on its own proves nothing —
    // which is exactly why readChannelFinished(), not this, drives reconnect.
    void channelDiagnostic(const QString& role, const QString& text);

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

    // Bounded liveness pre-flight, run immediately before connectPool().
    //
    // connectPool() is a BLOCKING libssh handshake on the caller's thread, and
    // the caller is the GUI thread: every millisecond it spends is a frozen
    // window. Worse, its stall is not ours to bound — name resolution inside
    // libssh is a plain getaddrinfo() that SSH_OPTIONS_TIMEOUT does not cover,
    // and the socket/banner phases fall back to a libssh default (measured:
    // 10.0 s for both a black-holed IP and a TCP peer that never sends a
    // banner).
    //
    // So the endpoint is asked to prove itself first, over a throwaway
    // QTcpSocket driven by the event loop rather than by blocking waits: DNS,
    // TCP connect and the server's first byte must all land inside
    // connectTimeoutMs(). Failing that we never call libssh at all, and the
    // attempt ends in a clean State::Failed. Because the probe spins a nested
    // QEventLoop with ExcludeUserInputEvents, the shell keeps repainting while
    // it runs and cannot be re-entered by a second click.
    //
    // Cost: one extra TCP connection per attempt, which an OpenSSH server logs
    // as "Did not receive identification string". That is the price of not
    // freezing, and it is opt-out via setConnectTimeoutMs(0).
    //
    // Returns true when the endpoint spoke; otherwise fills `error`.
    virtual bool probeEndpoint(const QString& host, quint16 port,
                               QString* error);

private:
    bool attemptWire();
    // Ask an attempt that is parked in probeEndpoint()'s nested event loop to
    // give up now, instead of waiting out connectTimeoutMs(). No-op when no
    // attempt is in flight.
    void abortAttempt();
    // Classify and route this channel's two signals: stderr/libssh faults to
    // channelDiagnostic(), EOF to handleConnectionLost(). Applied by
    // attemptWire() rather than openChannelDevice() so it covers the devices
    // the openChannelDevice() test seam substitutes.
    void wireChannelSignals(SshChannelDevice* device, const QString& role);
    void handleConnectionLost(const QString& reason);
    void scheduleReconnect();
    void cancelReconnect();
    void unwire();
    void fail(const QString& message);
    // `message` with the most recent channelDiagnostic() of the exec attempt
    // appended, so a setup failure carries the remote side's own explanation.
    QString withLastDiagnostic(const QString& message) const;
    void setState(State next);

    SshConnectionPool* m_pool = nullptr;
    QPointer<CodeharbordClient> m_client;
    QPointer<AgentStatusMonitor> m_monitor;
    SshChannelDevice* m_rpcDevice = nullptr;
    SshChannelDevice* m_agentDevice = nullptr;
    QString m_knownHostsPath;
    // Most recent channelDiagnostic() text of the exec attempt in progress.
    // Cleared per attempt; only read by withLastDiagnostic().
    QString m_lastDiagnostic;
    // Last stderr line each live channel produced, keyed by role. A channel
    // that dies takes its own last words with it otherwise: the exec-scoped
    // m_lastDiagnostic above is disconnected the moment startExec() returns, so
    // a remote process that starts fine and THEN complains before exiting —
    // `sh` reporting that no codeharbord entry point exists under repoRoot, for
    // one — used to reach only channelDiagnostic(), which nothing consumes. The
    // user got "codeharbord channel closed" and no reason. Per role, so the
    // bridge's routine startup banner is never appended to codeharbord's death.
    QHash<QString, QString> m_channelDiagnostics;

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
    int m_connectTimeoutMs = kDefaultConnectTimeoutMs;
    qint64 m_lastAttemptMs = -1;
    bool m_reconnectEnabled = true;
    // Set while we are inside our own connect/teardown, so the pool and device
    // signals those steps provoke are not mistaken for a fresh loss.
    bool m_attempting = false;
    bool m_tearingDown = false;
    // Non-owning: valid only for the duration of probeEndpoint()'s nested event
    // loop, which is what abortAttempt() interrupts.
    QEventLoop* m_probeLoop = nullptr;
    // The in-flight attempt was cancelled from inside its own nested loop; it
    // must unwind without wiring anything, failing, or arming a retry.
    bool m_cancelRequested = false;
};

} // namespace ch
