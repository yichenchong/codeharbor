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
    //
    // Provisioning is APPENDED rather than slotted in beside Connecting, where
    // it belongs chronologically, on purpose: the enumerator values are what
    // QSignalSpy records and what every existing switch over this enum was
    // compiled against, so inserting in the middle renumbers three states that
    // other code already reads. A consumer that has not yet learned about
    // Provisioning simply keeps showing whatever it showed for Connecting,
    // which is the truthful fallback rather than a wrong label.
    enum class State {
        Disconnected,
        Connecting,
        Wired,
        Reconnecting,
        Failed,
        // First connect to a server with no usable remote service: the client
        // is installing codeharbor-remote into the configured location before
        // it can exec anything there. See ensureRemoteService().
        Provisioning,
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

    // Opt in to accepting an UNKNOWN host key without asking anybody (SPEC
    // 12.1). OFF by default, and it must stay off in every attended build: with
    // it off, a wire attempt that finds no host-key decision callback on the
    // pool FAILS instead of connecting, so there is no path on which an unknown
    // key can be trusted and persisted with no user consent. AppController
    // installs a prompting callback before every connect, so the shipped app
    // never needs this.
    //
    // It exists for the two genuinely unattended entry points, where there is
    // nobody to ask and refusing would simply mean "cannot connect at all":
    // connectAndWireFromEnvironment() (the CH_LIVE_* harness path) turns it on
    // for itself, and a live test that drives connectAndWire() directly turns it
    // on explicitly.
    void setTrustUnknownHostKeys(bool enabled);
    bool trustUnknownHostKeys() const { return m_trustUnknownHostKeys; }

    // Wall time of the last completed wire attempt (probe + handshake + both
    // channel execs), in milliseconds. -1 before the first attempt. Exposed so
    // the latency gate measures the real thing instead of re-timing a
    // reimplementation of it.
    qint64 lastAttemptMs() const { return m_lastAttemptMs; }

    // `identityFile` is an optional local private-key path. Before connecting,
    // the pool also parses the user's ~/.ssh/config, so IdentityFile entries
    // work without duplicating them here. `nodePath` is the remote node binary
    // (it need not be on the login PATH) and `repoRoot` the remote CodeHarbor
    // installation: either an unpacked codeharbor-remote.tar.gz or a git
    // checkout — see entryCandidates().
    //
    // `repoRoot` no longer has to EXIST: when it holds no usable service, the
    // client installs one there before wiring anything (see
    // ensureRemoteService()). Node.js on the server is still a prerequisite
    // nothing here can install.
    bool connectAndWire(const QString& host, quint16 port, const QString& user,
                        const QString& nodePath, const QString& repoRoot,
                        const QString& identityFile = QString());

    // CH_LIVE_HOST, CH_LIVE_PORT, CH_LIVE_USER, CH_LIVE_NODE, CH_LIVE_REPO,
    // optional CH_LIVE_IDENTITY, and optional CH_LIVE_KNOWN_HOSTS overrides.
    // Returns false WITHOUT emitting error() when CH_LIVE_SSH is unset; that is
    // the normal desktop path.
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
    // retry AND aborts a retry that is already inside its connect pre-flight or
    // remote provisioning step, so a user who asked to stay disconnected is
    // not dragged back.
    //
    // A connect the USER asked for is not the ladder's to abandon: switching
    // this off while connectAndWire() is in flight leaves that attempt alone.
    // Cancelling it here would strand the object, because connectAndWire()
    // treats a cancelled attempt as "the canceller already chose the end state"
    // and disconnectSession() is the only caller that does — so nothing would
    // ever move the state off Connecting.
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
    // connect attempt (including probeEndpoint() or remote provisioning's
    // nested event loop), unwires both consumers, drops the SSH session and
    // ends in State::Disconnected WITHOUT scheduling a reconnect.
    void disconnectSession();

    // Retry delay in seconds for the Nth (0-based) automatic reconnect attempt:
    // 1, 2, 5, 10, 30, then 60 thereafter (SPEC 5.6). The sole retry ladder —
    // reconnect is a session-level concern, not per terminal pane —
    // and tst_sessionbootstrap pins this exact vector against the spec.
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

    // ---- remote provisioning (first connect to a bare server) --------------
    //
    // Until this existed, `repoRoot` had to be populated BY HAND before the
    // client could talk to a server at all: the user checked this repository
    // out (or unpacked codeharbor-remote.tar.gz) on the far side, then typed
    // the path into the connection profile. Get either wrong and the connect
    // died with "codeharbord channel closed", which names nothing the user can
    // act on. ensureRemoteService() closes that gap: on every connect the
    // server is asked what it already has, and only when there is nothing
    // usable is codeharbor-remote installed into the configured location.
    //
    // WHERE THE CODE COMES FROM, and why. The SERVER downloads the release
    // artifact; the client does not upload it. Two things rule the upload out
    // rather than merely making it less attractive:
    //   * The client has no copy to send. `remote/` is not embedded in the
    //     binary (no qt_add_resources covers it - see src/qml/CMakeLists.txt,
    //     which embeds only the two web bundles), and the shipped AppImage /
    //     zip / dmg contain Qt runtime and one executable.
    //   * An SSH exec channel here cannot be half-closed. SshChannelDevice
    //     exposes write() and closeChannel() and nothing in between, so there
    //     is no way to send a tarball and then give `tar -xzf -` the EOF it
    //     waits for; the remote extractor would hang forever.
    // Downloading is also what README.md already documents as the manual
    // install ("curl -fsSL .../codeharbor-remote.tar.gz | tar -xz -C ..."), so
    // this automates the published procedure instead of inventing a second one.
    //
    // The cost of that choice is a server with outbound network access and
    // either curl or wget. When that does not hold, provisioning fails with a
    // message naming the URL and the tools it looked for, and the escape hatch
    // is remoteArtifactUrl(): point it at a tarball already staged on the
    // server (a plain path or a file:// URL, copied with `cp` and needing no
    // network at all).

    // The codeharbor-remote tarball provisioning installs. Defaults to
    // defaultRemoteArtifactUrl(); setting it empty restores that default.
    void setRemoteArtifactUrl(const QString& url);
    QString remoteArtifactUrl() const;

    // Arm a ONE-SHOT forced reinstall for the next connect attempt. This is the
    // "update the server from the client" action: ensureRemoteService() then
    // installs remoteArtifactUrl() even when the location already holds a
    // working service and even when it carries no release marker.
    //
    // Why a flag consumed by the next connect rather than a method of its own:
    // an install runs over an authenticated SSH session, needs the same
    // host-key and credential chain, the same cancellation handling and the
    // same progress reporting as provisioning already has, and must be followed
    // by a wire against the service it just installed. Reconnecting IS all of
    // that, so the upgrade is one extra bit on the path that already does it.
    //
    // Marker-less installs are included on purpose, and only here. The connect
    // path never overwrites a directory a person populated by hand, because
    // nobody asked it to; this is the user asking, for one attempt. The one
    // location still refused is a git checkout — see ensureRemoteService().
    void requestRemoteUpgrade();
    // Drop an armed request that will never be spent (the connect chain it was
    // made for ended without reaching the install).
    void cancelRemoteUpgrade();
    bool remoteUpgradeRequested() const { return m_forceUpgrade; }

    // The release asset matching THIS client, or an empty string when the
    // client does not know its own version (QCoreApplication::applicationVersion
    // is unset - true of a test host, never of the shipped binary, which
    // main.cpp sets from CODEHARBOR_VERSION).
    //
    // This is how "versions must match" is enforced without a second version
    // constant to keep in sync: the URL carries the client's own version, so a
    // client can only ever install its own release, and the marker file below
    // records the exact URL that was installed. Upgrade the client and the URL
    // changes, the marker no longer matches, and the next connect REPLACES the
    // remote copy instead of driving a service it was never tested against.
    // CH_REMOTE_ARTIFACT_URL overrides it for operators who mirror releases
    // internally or stage the tarball on the server themselves.
    static QString defaultRemoteArtifactUrl();

    // File provisioning writes inside `repoRoot` recording the artifact URL it
    // installed. Its presence means "this install is ours"; its absence means a
    // human manages this directory and provisioning MUST NOT overwrite it.
    static QString releaseMarkerPath(const QString& repoRoot);

    // What the server answered when asked about its prerequisites. `reported`
    // is false when no report came back at all, which is treated as "cannot
    // tell" rather than as a fault (see ensureRemoteService()).
    struct RemoteInspection {
        bool reported = false;
        // `node --version` output, e.g. "v24.16.0"; empty when node is absent.
        QString nodeVersion;
        bool nodePresent = false;
        // First entryCandidates() path that exists, or empty when none does.
        QString entry;
        // releaseMarkerPath() contents, or empty when the file is absent.
        QString marker;
        // "curl", "wget" or "none".
        QString fetcher;
        bool tar = false;
    };

    // POSIX sh printing one CH_<KEY>=<value> line per RemoteInspection field.
    // Exposed, like rpcCommand() above, so tests assert the exact script that
    // runs on someone else's machine rather than a reconstruction of it.
    static QString remoteInspectScript(const QString& nodePath,
                                       const QString& repoRoot);

    // POSIX sh that fetches `artifactUrl`, unpacks it into `repoRoot`, proves a
    // codeharbord entry point exists afterwards and only then writes the
    // release marker. `fetcher` is the RemoteInspection::fetcher value. Every
    // scratch file lives under `repoRoot`, so provisioning never writes outside
    // the directory the user chose.
    static QString remoteProvisionScript(const QString& repoRoot,
                                         const QString& artifactUrl,
                                         const QString& fetcher);

    static RemoteInspection parseInspection(const QString& output);

    // Does `version` ("v24.16.0", "23.6.1", ...) satisfy remote/package.json's
    // declared engine floor? Pure, so the comparison is testable without a
    // server.
    static bool nodeVersionIsSupported(const QString& version);

    // The bare path when `url` names a tarball already present on the server (a
    // plain absolute path, or file://): it is copied with `cp`, needing neither
    // network access nor a download tool. Empty for a network URL. This is the
    // escape hatch for an air-gapped or curl-less server.
    static QString stagedArtifactPath(const QString& url);

    // remote/package.json: "engines": { "node": ">=23.6" }. The remote service
    // is run straight from TypeScript by Node's native type stripping, which
    // 23.6 is the first release to provide.
    static constexpr int kMinimumRemoteNodeMajor = 23;
    static constexpr int kMinimumRemoteNodeMinor = 6;

    // Budget for the prerequisite report: one round trip on a session that is
    // already authenticated, so this only has to cover a slow link.
    static constexpr int kInspectTimeoutMs = 15000;
    // Budget for the install: several megabytes over whatever link the server
    // has, plus tar. Generous on purpose — a first connect that gives up on a
    // slow line leaves a half-populated directory nobody asked for.
    static constexpr int kProvisionTimeoutMs = 180000;

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
    // Progress of a provisioning install, one line per step ("fetching ...",
    // "unpacking ...", "installed ..."). NOT a failure: this is what stops a
    // first connect that spends a minute downloading several megabytes from
    // being indistinguishable, on screen, from a hung application. A
    // provisioning FAILURE goes to error() like every other failure.
    void provisioning(const QString& message);
    // A requested upgrade (requestRemoteUpgrade()) did NOT happen, on an
    // attempt that is otherwise going fine and will connect.
    //
    // Separate from error() because error() is HELD while a connect attempt is
    // in flight — an expected refusal (an unknown host key) must not paint a
    // toast — and dropped when that attempt then succeeds. This message is the
    // opposite case: the connect succeeds and the thing the user actually asked
    // for is the part that failed, so it must survive exactly the path that
    // discards a held connect error. AppController routes it straight to a
    // toast.
    void upgradeFailed(const QString& message);

protected:
    // Test seams. The two side-effecting steps of one wire attempt, isolated so
    // tst_sessionbootstrap can drive the whole state machine (connect ok/fail,
    // channel loss, the retry ladder) with no SSH server in reach. Production
    // behaviour lives in these base implementations; nothing else overrides.
    virtual bool connectPool(const QString& host, quint16 port,
                             const QString& user,
                             const QString& identityFile);
    virtual SshChannelDevice* openChannelDevice(const QString& command,
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
    // connectTimeoutMs(). Failing that we never call libssh. A user-initiated
    // attempt ends in State::Failed; a reconnect rung returns to its retry
    // ladder unless cancellation or a fatal prerequisite ends it. Because the
    // probe spins a nested QEventLoop with ExcludeUserInputEvents, the shell
    // keeps repainting while it runs and cannot be re-entered by a second click.
    //
    // Cost: one extra TCP connection per attempt, which an OpenSSH server logs
    // as "Did not receive identification string". That is the price of not
    // freezing, and it is opt-out via setConnectTimeoutMs(0).
    //
    // Returns true when the endpoint spoke; otherwise fills `error`.
    virtual bool probeEndpoint(const QString& host, quint16 port,
                               QString* error);

    // Run `script` under POSIX sh on a throwaway Exec channel of the session
    // that is already up, collecting its stdout until the remote closes the
    // channel or `timeoutMs` elapses. Returns false when the channel could not
    // be opened, the remote exited without EOF inside the budget, or the
    // attempt was cancelled; `error` then carries the reason and the remote's
    // stderr. stdout lines beginning "codeharbor: " are republished through
    // provisioning() AS THEY ARRIVE, which is the only reason this waits on an
    // event loop instead of blocking.
    //
    // A test seam for the same reason connectPool() is one: the provisioning
    // decision tree has to be drivable with canned server answers, and there is
    // no other way to fake "this server has node 22 and nothing installed".
    // Deliberately NOT routed through openChannelDevice(): that seam's fakes
    // never reach EOF, so reusing it would park every existing test in this
    // nested wait for the full budget.
    virtual bool runRemoteScript(const QString& script, int timeoutMs,
                                 QString* output, QString* error);

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
    // Same teardown as fail(), but marks a server/configuration prerequisite
    // failure as non-retryable. The reconnect ladder stops after this attempt.
    void failFatal(const QString& message);
    // `message` with the most recent channelDiagnostic() of the exec attempt
    // appended, so a setup failure carries the remote side's own explanation.
    QString withLastDiagnostic(const QString& message) const;
    // A synchronous pool-handshake failure reaches errorOccurred() before
    // connectPool() returns false. Preserve it so the generic endpoint failure
    // includes the authentication diagnosis shown by the connection sheet.
    QString withLastPoolError(const QString& message) const;
    void setState(State next);
    // Make sure the configured location holds a remote service this client can
    // launch, installing one when it does not. Called by attemptWire() after
    // the handshake and before the first exec.
    //
    // Returns false ONLY for a failure the user must act on (no node, no
    // fetcher, a download that did not land), having already called fail().
    // Everything else returns true and lets the ordinary exec path proceed —
    // including the case where the inspection itself could not run, because
    // refusing to connect over a failed DIAGNOSTIC would turn servers that work
    // today into unreachable ones.
    bool ensureRemoteService();

    SshConnectionPool* m_pool = nullptr;
    QPointer<CodeharbordClient> m_client;
    QPointer<AgentStatusMonitor> m_monitor;
    SshChannelDevice* m_rpcDevice = nullptr;
    SshChannelDevice* m_agentDevice = nullptr;
    QString m_knownHostsPath;
    // Most recent channelDiagnostic() text of the exec attempt in progress.
    // Cleared per attempt; only read by withLastDiagnostic().
    QString m_lastDiagnostic;
    // Most recent pool error from the currently executing handshake. Cleared
    // immediately before connectPool() and never reused for reconnects.
    QString m_lastPoolError;
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
    QString m_identityFile;
    // Explicit remoteArtifactUrl() override; empty means "use
    // defaultRemoteArtifactUrl()".
    QString m_artifactUrl;
    // One-shot: set by requestRemoteUpgrade(), consumed (and always cleared) by
    // the next ensureRemoteService(). Cleared unconditionally so a failed or
    // cancelled upgrade cannot keep reinstalling on every later reconnect.
    bool m_forceUpgrade = false;
    int m_attempt = 0;
    int m_maxAttempts = kDefaultMaxReconnectAttempts;
    double m_timeScale = 1.0;
    int m_connectTimeoutMs = kDefaultConnectTimeoutMs;
    qint64 m_lastAttemptMs = -1;
    bool m_reconnectEnabled = true;
    // connectAndWireFromEnvironment() keeps this opt-in only for the
    // unattended session it started. A later attended connect on the same
    // object must not inherit trust-on-first-use.
    bool m_trustUnknownHostKeys = false;
    bool m_environmentTrustActive = false;
    bool m_environmentTrustPrevious = false;
    bool m_environmentConnectInProgress = false;
    // Set while we are inside our own connect/teardown, so the pool and device
    // signals those steps provoke are not mistaken for a fresh loss.
    bool m_attempting = false;
    // Whether the attempt m_attempting refers to is a rung of the reconnect
    // ladder rather than a connectAndWire() the user asked for. Only a rung may
    // be abandoned by setReconnectEnabled(false); see there.
    bool m_attemptIsRetry = false;
    bool m_tearingDown = false;
    // Non-owning: valid only for the duration of the nested event loop an
    // attempt is currently parked in — probeEndpoint()'s connect pre-flight or
    // runRemoteScript()'s wait on a provisioning step. Either way it is what
    // abortAttempt() interrupts, so "disconnect" is honoured during a
    // multi-minute download as well as during a five-second probe.
    QEventLoop* m_nestedLoop = nullptr;
    // The in-flight attempt was cancelled from inside its own nested loop; it
    // must unwind without wiring anything, failing, or arming a retry.
    bool m_cancelRequested = false;
    // Set by failFatal() for an attempt whose cause cannot be repaired by
    // reconnecting (missing/incompatible Node, invalid install target, or a bad
    // artifact). The timer path turns that attempt directly into Failed instead
    // of spending the rest of the ladder re-asking the same question.
    bool m_attemptFatal = false;
    // Covers the stateChanged() delivery in connectAndWire() before
    // attemptWire() can raise m_attempting.
    bool m_connectRequested = false;
};

} // namespace ch
