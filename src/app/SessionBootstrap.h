#pragma once

#include "SshConnectionPool.h"

#include <QObject>
#include <QPointer>
#include <QString>

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
class SessionBootstrap : public QObject {
    Q_OBJECT
public:
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

    // Remote command lines, exposed so tests and diagnostics assert the exact
    // strings that are executed rather than reconstructing them.
    static QString rpcCommand(const QString& nodePath, const QString& repoRoot);
    static QString bridgeCommand(const QString& nodePath,
                                 const QString& repoRoot);

signals:
    void wired();
    void error(const QString& message);

private:
    SshChannelDevice* openChannelDevice(SshConnectionPool::ChannelKind kind,
                                        const QString& command,
                                        const QString& role);
    void unwire();
    void fail(const QString& message);

    SshConnectionPool* m_pool = nullptr;
    QPointer<CodeharbordClient> m_client;
    QPointer<AgentStatusMonitor> m_monitor;
    SshChannelDevice* m_rpcDevice = nullptr;
    SshChannelDevice* m_agentDevice = nullptr;
    QString m_knownHostsPath;
};

} // namespace ch
