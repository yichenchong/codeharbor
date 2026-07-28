#pragma once

#include "KnownHosts.h"

#include <QByteArray>
#include <QList>
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

    // An authentication method whose secret cannot safely be interchanged with
    // the other one: a private-key passphrase must never be sent to a server as
    // its account password.
    enum class CredentialKind {
        KeyPassphrase,
        Password,
    };
    Q_ENUM(CredentialKind)

    // A callback may deliberately park the handshake while the GUI asks for a
    // secret. `promptRequested` distinguishes that from "this callback has no
    // credential for this method", so authenticate() cannot overwrite a
    // passphrase prompt with a password prompt.
    struct CredentialReply {
        QString secret;
        bool promptRequested = false;
    };

    // Called after non-interactive agent/key authentication fails. The callback
    // returns a secret only for `kind`, or parks the attempt for user input.
    // Empty secrets are never cached by the pool.
    using CredentialCallback =
        std::function<CredentialReply(const QString& user, CredentialKind kind)>;

    explicit SshConnectionPool(QObject* parent = nullptr);
    ~SshConnectionPool() override;

    static bool libsshAvailable();

    void setKnownHosts(const KnownHosts& hosts);
    // Access the (possibly updated) store after accepted unknown-host prompts so
    // callers can persist newly trusted keys.
    const KnownHosts& knownHosts() const;

    void setHostKeyCallback(HostKeyCallback callback);
    // The installed decision callback. Read-only, and the only way to exercise
    // an unknown-key decision without a live handshake: verifyHostKey() is the
    // sole caller and it only runs inside connectToHost(). Used by
    // tst_appcontroller to prove the retry after a host-key prompt accepts ONLY
    // the exact key the user was shown.
    const HostKeyCallback& hostKeyCallback() const { return m_hostKeyCallback; }
    void setCredentialCallback(CredentialCallback callback);
    // The installed callback, read-only, and the exact counterpart of
    // hostKeyCallback() above: authenticate() is its only caller. Tests use
    // this seam to prove the controller requests the right credential type and
    // consumes a supplied secret exactly once.
    const CredentialCallback& credentialCallback() const
    {
        return m_credentialCallback;
    }

    // through KnownHosts, then authenticate (agent -> configured/default key
    // -> requested key passphrase or password). `identityFile` is a local
    // private key; empty leaves OpenSSH config and libssh defaults in charge.
    bool connectToHost(const QString& host, quint16 port, const QString& user,
                       const QString& identityFile = QString());
    void disconnectFromHost();

    // The known_hosts lookup token for an endpoint: the bare host on the default
    // port, OpenSSH's "[host]:port" form otherwise. Static and public because
    // both the algorithm pin (before the handshake) and the verdict (during it)
    // must key off the SAME token.
    static QString lookupHostFor(const QString& host, quint16 port);

    // SSH_OPTIONS_HOSTKEYS value pinning the handshake to the algorithms we
    // already trust for a host, or empty when nothing is trusted yet (first use
    // must still work, so an untrusted host keeps libssh's default ordering).
    //
    // SPEC 12.1's hard refusal is only hard if the SERVER cannot choose which
    // key it is challenged on. libssh reorders host-key algorithms from
    // ~/.ssh/known_hosts, which is not our store, so without this pin a MITM
    // presenting an ssh-rsa key for a host trusted as ssh-ed25519 downgraded a
    // refusal into a first-use prompt.
    //
    // "ssh-rsa" expands to the SHA-2 signature algorithms first: OpenSSH 8.8+
    // disables the SHA-1 "ssh-rsa" algorithm by default, and libssh reports all
    // three under the single key type "ssh-rsa", so pinning the literal string
    // alone would lock the user out of their own RSA host.
    static QByteArray hostKeyAlgorithms(const QStringList& trustedKeyTypes);

    State state() const;

#if CH_HAVE_LIBSSH
    // Open an independent channel on the shared session. Returns nullptr if not
    // connected or the channel could not be opened. The pool RETAINS ownership:
    // channels MUST NOT outlive the session — disconnectFromHost()/closeSession()
    // closes and frees every opened channel before freeing the session. Callers
    // must not ssh_channel_free() a returned channel themselves.
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
    QString m_identityFile;
#if CH_HAVE_LIBSSH
    ssh_session m_session = nullptr;
    QList<ssh_channel> m_channels;
#endif
};

} // namespace ch
