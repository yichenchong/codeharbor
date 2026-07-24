#pragma once

#include "KnownHosts.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>

#if CH_HAVE_LIBSSH
#include <libssh/libssh.h>
#endif

namespace ch {

// One authenticated SSH connection per configured server, multiplexing many
// independent channels: terminal PTYs, the codeharbord RPC channel, and the
// agent-status channel (SPEC 5.3). Owns host-key verification (SPEC 12.1).
//
// All libssh calls are guarded by CH_HAVE_LIBSSH; when built without libssh the
// object stays constructible and reports State::NotAvailable. This class does
// not drive PTY I/O, reconnect scheduling, or the live handshake loop — those
// belong to TerminalController and the remote client.
class SshConnectionPool : public QObject {
    Q_OBJECT
public:
    enum class State {
        Disconnected,
        Connecting,
        HostKeyCheck,
        Authenticating,
        Connected,
        Error,
        NotAvailable,  // compiled without libssh
    };
    Q_ENUM(State)

    // Distinguishes the independent channels opened on the shared session.
    enum class ChannelKind {
        Pty,
        Exec,
        Rpc,
        AgentStatus,
    };
    Q_ENUM(ChannelKind)

    enum class HostKeyDecision {
        Accept,  // trust the key and persist it into the known-hosts store
        Reject,  // refuse the connection
    };

    // Decision callback for an unknown host key (Verdict::Unknown). A changed
    // key (Verdict::Mismatch) is a hard refusal and never invokes this.
    using HostKeyCallback = std::function<HostKeyDecision(
        const QString& host, const QString& keyType, const QByteArray& keyBlob,
        KnownHosts::Verdict verdict)>;

    // Credential callback used only when ssh-agent and default-key auth fail.
    // Returns a password or key passphrase for the user; empty aborts auth.
    using CredentialCallback =
        std::function<QString(const QString& user, const QString& prompt)>;

    explicit SshConnectionPool(QObject* parent = nullptr);
    ~SshConnectionPool() override;

    static bool libsshAvailable();

    void setKnownHosts(const KnownHosts& hosts);
    // Access the (possibly updated) store after accepted unknown-host prompts so
    // callers can persist newly trusted keys.
    const KnownHosts& knownHosts() const;

    void setHostKeyCallback(HostKeyCallback callback);
    void setCredentialCallback(CredentialCallback callback);

    // Establish the single authenticated session: connect, verify the host key
    // through KnownHosts, then authenticate (agent -> default key -> callback).
    // Returns true on success; otherwise sets State::Error (or NotAvailable).
    bool connectToHost(const QString& host, quint16 port, const QString& user);
    void disconnectFromHost();

    State state() const;

#if CH_HAVE_LIBSSH
    // Open an independent channel on the shared session. Returns nullptr if not
    // connected or the channel could not be opened. Ownership transfers to the
    // caller (free with ssh_channel_free / ssh_channel_close).
    ssh_channel openChannel(ChannelKind kind);
#endif

signals:
    void stateChanged(ch::SshConnectionPool::State state);
    void errorOccurred(const QString& message);
    // Emitted on a Verdict::Mismatch refusal so the UI can surface a warning.
    void hostKeyMismatch(const QString& host);

private:
    void setState(State next);
#if CH_HAVE_LIBSSH
    bool verifyHostKey(const QString& host);
    bool authenticate(const QString& user);
    void closeSession();
#endif

    State m_state = State::Disconnected;
    KnownHosts m_knownHosts;
    HostKeyCallback m_hostKeyCallback;
    CredentialCallback m_credentialCallback;
    QString m_host;
    quint16 m_port = 22;
    QString m_user;
#if CH_HAVE_LIBSSH
    ssh_session m_session = nullptr;
#endif
};

} // namespace ch
