#pragma once

#include "KnownHosts.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

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
//
// The handshake is deliberately SYNCHRONOUS and runs on the calling thread. Its
// worst-case freeze is therefore bounded: connectToHost() sets an explicit
// SSH_OPTIONS_TIMEOUT (kHandshakeTimeoutSeconds, 15s) unless the user's own
// OpenSSH ConnectTimeout was parsed from ~/.ssh/config, so a black-holed
// endpoint stalls the UI thread for at most that timeout rather than libssh's
// much longer version-dependent default. Channel writes are bounded by the same
// session timeout (see SshChannelDevice::writeData).
class SshConnectionPool : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString diagnosticLog READ diagnosticLog NOTIFY diagnosticLogChanged)
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

    // ---- multi-step authentication (SPEC 12.1) -----------------------------
    //
    // An SSH server may require SEVERAL methods before it grants access
    // (OpenSSH's `AuthenticationMethods publickey,password`). libssh reports
    // that with SSH_AUTH_PARTIAL: the method WAS accepted, and the server now
    // expects a further one. Treating that as failure is why a server wanting
    // both a key and a password could not be used at all.
    enum class AuthOutcome {
        Granted,  // access granted; the handshake is authenticated
        Partial,  // accepted, but the server requires a further method
        Refused,  // this method did not succeed
    };
    Q_ENUM(AuthOutcome)

    // The authentication methods the server offers RIGHT NOW. Re-read after
    // every step: the set shrinks (and can grow) after a partial success.
    struct AuthMethods {
        bool publicKey = false;
        bool password = false;
        bool keyboardInteractive = false;
    };

    // One step of this client's authentication ladder.
    enum class AuthRung {
        Agent,          // ssh-agent
        KeyFile,        // configured/default private key, no passphrase
        KeyPassphrase,  // the same keys, with a passphrase asked of the user
        Password,       // the account password asked of the user
        KeyboardInteractive,  // PAM/challenge-response, one password prompt
        Exhausted,      // nothing left to try
    };
    Q_ENUM(AuthRung)

    // Which rungs this handshake has already spent. A rung is climbed at most
    // once, and that is what bounds the multi-step loop: a server that keeps
    // answering SSH_AUTH_PARTIAL without ever granting access runs out of rungs
    // after four steps instead of looping forever.
    struct AuthRungsTried {
        bool agent = false;
        bool keyFile = false;
        bool keyPassphrase = false;
        bool password = false;
        bool keyboardInteractive = false;

        bool contains(AuthRung rung) const
        {
            switch (rung) {
            case AuthRung::Agent: return agent;
            case AuthRung::KeyFile: return keyFile;
            case AuthRung::KeyPassphrase: return keyPassphrase;
            case AuthRung::Password: return password;
            case AuthRung::KeyboardInteractive: return keyboardInteractive;
            case AuthRung::Exhausted: return true;
            }
            return true;
        }

        void add(AuthRung rung)
        {
            switch (rung) {
            case AuthRung::Agent: agent = true; return;
            case AuthRung::KeyFile: keyFile = true; return;
            case AuthRung::KeyPassphrase: keyPassphrase = true; return;
            case AuthRung::Password: password = true; return;
            case AuthRung::KeyboardInteractive:
                keyboardInteractive = true;
                return;
            case AuthRung::Exhausted: return;
            }
        }
    };

    // The next rung to climb: the first in this client's fixed order (agent ->
    // key file -> key file with a passphrase -> password -> keyboard-interactive)
    // that the server still offers and that has not been climbed yet, or
    // Exhausted. `canPrompt` is false when no credential callback is installed,
    // so the rungs that need a secret from the user are unreachable.
    //
    // Pure, and deliberately driven by the CURRENT offer rather than by a fixed
    // sequence: it is re-evaluated after every step, so a partial success that
    // changes the offered set (publickey drops out, password appears) routes
    // the ladder onto the method the server is now asking for — in whichever
    // order the server chose to require them.
    static AuthRung nextAuthRung(const AuthRungsTried& tried,
                                 AuthMethods offered, bool canPrompt);

#if CH_HAVE_LIBSSH
    // Split an ssh_userauth_list() bitmask into the methods this client can
    // climb. A zero mask means the server did not tell us (the "none" request
    // errored), and is treated as "try both" — which is what this client did
    // unconditionally before multi-step support existed. Guarded because the
    // bit values it decodes are libssh's SSH_AUTH_METHOD_* macros; the mask is
    // never redefined here, so the two cannot drift apart.
    static AuthMethods methodsFromMask(int userauthListMask);

    // Classify one ssh_userauth_*() return code. SSH_AUTH_SUCCESS is Granted,
    // SSH_AUTH_PARTIAL is Partial, and every other code (denied, error, again,
    // info) is Refused. Public so the multi-step decision is testable against
    // the real libssh constants without a server.
    static AuthOutcome classifyAuthResult(int libsshResult);
#endif

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

    // Connect, verify the host key through KnownHosts, then authenticate. The
    // ladder is agent -> configured/default key -> that key with a passphrase
    // asked of the user -> the account password -> keyboard-interactive (PAM),
    // restricted at every step to the methods the server still offers, so a
    // server that requires SEVERAL methods (`AuthenticationMethods
    // publickey,password`) is satisfied one step at a time. `identityFile` is a
    // local private key; empty leaves OpenSSH config and libssh defaults in
    // charge. The synchronous handshake is bounded by kHandshakeTimeoutSeconds
    // (see the class comment) unless the user configured their own ConnectTimeout.
    bool connectToHost(const QString& host, quint16 port, const QString& user,
                       const QString& identityFile = QString());
    void disconnectFromHost();

    // The known-hosts lookup token for an endpoint: the bare host on the
    // default port, OpenSSH's "[host]:port" form otherwise. Host-key
    // verification must use this same canonical endpoint.
    static QString lookupHostFor(const QString& host, quint16 port);


    // True only for the Windows OpenSSH named-pipe spelling. This is a pure
    // classifier so platform-specific SSH setup remains testable everywhere.
    static bool isWindowsNamedPipeAgentSocket(const QString& socket);

    // True for the libssh releases whose hybrid ML-KEM key exchange cannot pack
    // its own client KEX init (0.12.0 only; fixed upstream in 0.12.1). Takes the
    // ssh_version() string, e.g. "0.12.0/openssl/zlib". Pure, so the mitigation
    // is testable against a build linked with any libssh.
    static bool hasBrokenHybridKex(const QString& runtimeVersion);

    State state() const;
    // In-memory libssh and connection-stage diagnostics for the most recent
    // handshake. It is never persisted and excludes supplied credentials.
    QString diagnosticLog() const { return m_diagnosticLog; }


#if CH_HAVE_LIBSSH
    // Open an independent channel on the shared session. Returns nullptr if not
    // connected or the channel could not be opened. The pool RETAINS ownership:
    // channels MUST NOT outlive the session — disconnectFromHost()/closeSession()
    // closes and frees every opened channel before freeing the session. Callers
    // must not ssh_channel_free() a returned channel themselves; they hand it
    // back with releaseChannel() instead.
    ssh_channel openChannel(ChannelKind kind);

    // Give a channel back: closes it if still open, frees it, and drops it from
    // the pool's list. Unknown or null handles are ignored, so a double release
    // is harmless. This is the ONLY way a channel slot is reclaimed before the
    // whole session goes down, and it matters: an SSH server caps the number of
    // concurrent sessions per connection (OpenSSH's MaxSessions, 10 by default),
    // so a client that opens a channel per remote command and never releases
    // wedges the connection after ten of them.
    void releaseChannel(ssh_channel channel);
#endif

signals:
    void stateChanged(ch::SshConnectionPool::State state);
    void diagnosticLogChanged();
    void errorOccurred(const QString& message);
    // Emitted on a Verdict::Mismatch refusal so the UI can surface a warning.
    void hostKeyMismatch(const QString& host);
    // Emitted immediately before the session's channels are freed, so anything
    // holding a channel handle (SshChannelDevice) can drop it first. Without it
    // a device that outlives its session keeps polling freed libssh memory.
    void sessionClosing();

private:
    void clearDiagnostics();
    static void libsshLog(int priority, const char* function,
                          const char* buffer, void* userdata);
    void appendDiagnostic(const QString& message);
    void setState(State next);
#if CH_HAVE_LIBSSH
    bool verifyHostKey(const QString& host);
    bool authenticate(const QString& user);
    QString authenticationFailure() const;
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
    QString m_diagnosticLog;
#if CH_HAVE_LIBSSH
    ssh_session m_session = nullptr;
    QList<ssh_channel> m_channels;
    // Names of the methods the server accepted with SSH_AUTH_PARTIAL during the
    // most recent handshake, so authenticationFailure() can say "your key was
    // accepted, the server also wants X" instead of a flat "authentication
    // failed". Method NAMES only: no secret is ever recorded here.
    QStringList m_partialMethods;
    // Whether the server of the most recent handshake offered public-key
    // authentication at all. Without it authenticationFailure() would lecture the
    // user about ssh-agent and identity files on a password-only server, where no
    // key was ever sent.
    bool m_publicKeyOffered = false;
#endif
};

} // namespace ch
