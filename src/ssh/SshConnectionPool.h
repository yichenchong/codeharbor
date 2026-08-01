#pragma once

#include "KnownHosts.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

#if CH_HAVE_LIBSSH
#include <libssh/libssh.h>
#endif

namespace ch {

// Process-wide router for libssh's diagnostic logging, so that every component
// that wants a transcript gets its OWN lines and only its own.
//
// WHAT LIBSSH ACTUALLY DOES — measured against the installed 0.11.3 library,
// because all three of these contradict what the headers suggest:
//   1. There is exactly ONE logging hook (ssh_set_log_callback), ONE user-data
//      pointer (ssh_set_log_userdata) and ONE verbosity level
//      (ssh_set_log_level) — not one per session. Whoever installs last wins,
//      so two objects that each install their own around their own work
//      overwrite each other and collect each other's lines.
//   2. Those three are THREAD-LOCAL, not process-global: libssh declares them
//      LIBSSH_THREAD, which is __thread wherever the compiler supports it. A
//      callback installed on one thread is simply absent on every other, so
//      installing once for the process silences every thread but the installer.
//      (A libssh built without thread-local-storage support degrades to truly
//      global; routing stays correct there because it is keyed by thread
//      anyway, but the saved verbosity could then be another thread's.)
//   3. ssh_set_log_callback(NULL) is REFUSED (returns SSH_ERROR) and leaves the
//      current callback in place. A hook cannot be uninstalled once set, only
//      replaced. This is why every caller that installs its own callback around
//      a piece of work and then "restores" a previously-empty one in fact
//      leaves its callback — and its user-data pointer — behind for good.
//
// The router therefore installs the hook ON EACH THREAD that takes a route, the
// first time one is taken there, and restores that thread's previous verbosity
// and user data when the last route on that thread is dropped. The previous
// callback is put back too when there was one; when there was none, (3) makes
// removal impossible, so the router's own hook stays installed and is INERT: it
// forwards nothing while no route is active on that thread, and the restored
// verbosity means libssh does not even format a line. Routes may be taken and
// dropped in ANY order.
//
// ATTRIBUTION IS BY THREAD, NOT BY SESSION. The hook receives only a priority,
// the emitting libssh function name, the formatted message and that one
// user-data pointer — there is no session parameter. The per-session
// log_function of ssh_callbacks_struct looks like the answer but is vestigial:
// libssh routes every log line through the global hook and never calls it
// (measured, not remembered). A line therefore cannot be traced back to the
// session that produced it, so the router attributes each line to the innermost
// route active ON THE THREAD THAT EMITTED IT. That is exact for
// SshConnectionPool, whose handshake is synchronous and runs start to finish on
// its caller's thread. Its one limitation, stated plainly: a DIFFERENT libssh
// session driven on the same thread while a route is active there — reachable
// only by re-entering libssh from inside one of the pool's own handshake
// callbacks — is logged into that route.
//
// THREADING CONTRACT:
//   * A Route MUST be taken and dropped on the same thread. That thread is the
//     only one whose libssh lines it receives, and the only one whose libssh
//     state it saves and restores. ENFORCED at runtime in release(), not merely
//     asserted: a release from another thread is refused with a warning, and
//     the route is deactivated so no further line can reach a sink whose owner
//     is going away. What cannot be repaired from the wrong thread is the
//     OWNING thread's libssh state, which is thread-local and unreachable from
//     anywhere else; it stays as it is until that thread drops its own last
//     route, which costs verbosity and nothing else.
//   * A Route's sink is invoked synchronously from inside libssh on that same
//     thread and never on any other, so a sink needs no locking of its own —
//     which is what lets a pool mutate its QString transcript and emit a Qt
//     signal from it exactly as it did before this router existed.
//   * All routing state is thread-local, so routes on different threads cannot
//     race. The only cross-thread data are the process-wide route count kept
//     for the test seam below and each route's active flag, both atomic.
class SshLogRouter {
public:
    // Receives one libssh log line: libssh's priority, the emitting libssh
    // function name (may be null) and the formatted message (may be null).
    using Sink = std::function<void(int priority, const char* function,
                                    const char* buffer)>;

    // One claim on libssh's logging state for the creating thread, and the
    // routing target for that thread's log lines.
    class Route {
    public:
        explicit Route(Sink sink);
        ~Route();
        Route(const Route&) = delete;
        Route& operator=(const Route&) = delete;
        Route(Route&&) = delete;
        Route& operator=(Route&&) = delete;

        // Stop receiving lines and drop this route's claim. Idempotent;
        // ~Route() calls it. See the THREADING CONTRACT above for what a
        // release from the wrong thread does and does not repair.
        void release();

    private:
        friend class SshLogRouter;
        // The routing target, held in a heap block SHARED with the owning
        // thread's route stack rather than reached through a Route pointer.
        // That is what makes a wrong-thread release safe: the block outlives
        // this Route, so deactivating it cannot leave the owning thread's stack
        // holding a pointer into a destroyed object.
        struct Entry {
            explicit Entry(Sink s) : sink(std::move(s)) {}
            Sink sink;
            // Cleared by whichever thread releases the route — possibly not the
            // owning one — and read by the owning thread's dispatch(), which is
            // also the only thread that removes the entry from its stack.
            std::atomic<bool> active{true};
        };
        std::shared_ptr<Entry> m_entry;
        // The thread that took this route: the only one whose libssh state it
        // saved, and so the only one that may put that state back.
        Qt::HANDLE m_thread = nullptr;
    };

    // Test seams. activeRouteCount() is process-wide; ownsThreadLoggingState()
    // answers for the CALLING thread, which is the granularity libssh's state
    // actually has. Together they show the state is neither restored while a
    // route still needs it nor left raised after the last one goes.
    static int activeRouteCount();
    static bool ownsThreadLoggingState();

private:
    // libssh's logging hook. Deliberately spelled in plain C types so this
    // declaration needs no libssh header: it matches ssh_logging_callback.
    static void dispatch(int priority, const char* function, const char* buffer,
                         void* userdata);
    static void acquire(Route* route);
    static void release(Route* route);

    // The routes active on the CALLING thread, innermost last. A stack rather
    // than a single pointer so a route taken from inside another route's scope
    // restores its parent when it goes; a list rather than a linked chain so a
    // route released OUT of order removes exactly itself and nothing else.
    //
    // Entries, not Route pointers: an entry outlives the Route that created it,
    // so a Route destroyed on the WRONG thread cannot leave this list holding a
    // pointer into freed memory. Only the owning thread appends to or removes
    // from its own list; another thread can at most clear an entry's atomic
    // `active` flag.
    static thread_local QList<std::shared_ptr<Route::Entry>> s_threadRoutes;

    // Drop entries that no longer route anywhere, including any deactivated by
    // a wrong-thread release. Only ever called on the owning thread, which is
    // the only one allowed to touch the list itself.
    static void pruneInactiveThreadRoutes();
};

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
// SSH_OPTIONS_TIMEOUT (kHandshakeTimeoutSeconds, 15s) BEFORE parsing
// ~/.ssh/config, so a ConnectTimeout the user really did configure still wins
// while everyone else keeps the bound. A black-holed endpoint therefore stalls
// the UI thread for at most that timeout rather than libssh's much longer
// version-dependent default. Channel writes are bounded by the same session
// timeout (see SshChannelDevice::writeData).
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
    // climb. A zero mask (the server did not tell us) and SSH_AUTH_ERROR (the
    // query itself failed) both mean "not told", and are treated as "try
    // everything this client can supply" — which is what this client did
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

    // Hard ceiling on the retained transcript. A handshake against a
    // misbehaving server can emit libssh lines without bound, and the
    // transcript is held in memory for the whole session.
    static constexpr qsizetype kTranscriptCharacterLimit = 64 * 1024;

    // Append one already-trimmed line to `transcript`, enforcing the cap above
    // and the single, non-repeating "earlier diagnostics discarded" marker.
    // Static and pure precisely so the cap is testable without a handshake:
    // reaching it through a real connection would need a server that emits
    // 64 KiB of log lines.
    static void appendTranscriptLine(QString& transcript, const QString& line);


    // Apply this client's known-hosts policy to a host key already read off the
    // session, and report whether the connection may proceed. Split out of
    // verifyHostKey(), which does nothing else but extract the key from libssh,
    // so every verdict — including the user declining an unknown key — is
    // exercisable without a live server. Emits errorOccurred() for every
    // refusal, hostKeyMismatch() for a changed key, and adds an accepted unknown
    // key to the store.
    bool applyHostKeyPolicy(const QString& host, quint16 port,
                            const QString& keyType, const QByteArray& keyBlob);

#if CH_HAVE_LIBSSH
    // Open an independent channel on the shared session. Returns nullptr if not
    // connected, if the pool is being destroyed, or if the channel could not be
    // opened. No PTY is negotiated here: a channel accepts exactly one pty-req
    // and the terminal type and geometry are the caller's to choose
    // (SshChannelDevice::startPty). The pool RETAINS ownership: channels MUST
    // NOT outlive the session — disconnectFromHost()/closeSession() closes and
    // frees every opened channel before freeing the session. Callers must not
    // ssh_channel_free() a returned channel themselves; they hand it back with
    // releaseChannel() instead.
    ssh_channel openChannel();

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
    //
    // This is the ONE signal ~SshConnectionPool() emits, and the only one it is
    // safe to: it runs from the body of the most-derived destructor, so every
    // member and the QObject subobject are still alive, and the destructor sets
    // m_destroying first so a handler cannot start a second teardown or a new
    // handshake on a dying pool. stateChanged()/diagnosticLogChanged() are NOT
    // emitted during destruction — those reach QML property bindings, which
    // would re-read a pool that is going away for no benefit at all.
    void sessionClosing();

private:
    void clearDiagnostics();
    void appendDiagnostic(const QString& message);
    void setState(State next);
#if CH_HAVE_LIBSSH
    bool verifyHostKey(const QString& host);
    bool authenticate(const QString& user);
    QString authenticationFailure() const;
    void closeSession();
#endif

    State m_state = State::Disconnected;
    // Set by the destructor before it tears the session down, so a slot reached
    // from sessionClosing() cannot connect, disconnect or open a channel on a
    // pool that is being destroyed.
    bool m_destroying = false;
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
    // Held only for the duration of one synchronous connectToHost(), so the
    // raised libssh log level is never left behind. Destroying the pool
    // destroys the route too, which is what makes a pool that dies while
    // registered deregister cleanly.
    std::unique_ptr<SshLogRouter::Route> m_logRoute;
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
