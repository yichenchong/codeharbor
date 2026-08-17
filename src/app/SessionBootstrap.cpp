#include "SessionBootstrap.h"

#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "SshChannelDevice.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QLockFile>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>

#include <cmath>
#include <utility>

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

// POSIX sh that leaves the first existing candidate for `stem` under `root` in
// $__ch_entry, or the empty string when none of them exists. No verdict of its
// own: the launch commands turn "none" into a fatal exec failure, while the
// provisioning script turns it into a rejected archive.
//
// The selection happens on the SERVER, inside the exec we were going to issue
// anyway: a client-side probe would cost an extra round trip on every connect
// and would still be a guess (the probe and the exec are two different
// moments), and a per-profile "layout" field would make every user answer a
// question about our build system.
QString findEntry(const QString& root, const QString& stem)
{
    const QStringList candidates =
        SessionBootstrap::entryCandidates(root, stem);
    QStringList quoted;
    quoted.reserve(candidates.size());
    for (const QString& candidate : candidates)
        quoted << shellQuote(candidate);

    // One complete command list, with NO trailing separator: callers append
    // their own, and a stray `;` ahead of another one is `;;` — a syntax error
    // outside a `case`, which would break every script this appears in.
    return QStringLiteral("__ch_entry=; for __ch_c in ")
           + quoted.join(QLatin1Char(' '))
           + QStringLiteral("; do if [ -f \"$__ch_c\" ]; then "
                            "__ch_entry=\"$__ch_c\"; break; fi; done");
}

// findEntry() plus the launch commands' verdict: name every path that was tried
// on stderr and refuse to exec, so "it does not launch" is answerable without
// an SSH session of your own. `stem` doubles as the label in the error.
QString selectEntry(const QString& repoRoot, const QString& stem)
{
    return findEntry(repoRoot, stem)
           + QStringLiteral("; if [ -z \"$__ch_entry\" ]; then echo ")
           + shellQuote(QStringLiteral("codeharbor: no %1 entry point on this "
                                       "server. Tried: %2")
                            .arg(stem,
                                 SessionBootstrap::entryCandidates(repoRoot, stem)
                                     .join(QStringLiteral(", "))))
           + QStringLiteral(" >&2; exit 127; fi; ");
}

// Marker that turns one line of a provisioning script's stdout into user-facing
// progress. Everything a remote script prints without it (the inspection
// report's CH_<KEY>=<value> lines, a fetch tool's own chatter) stays internal.
QString progressPrefix()
{
    return QStringLiteral("codeharbor: ");
}

// The one line remoteProvisionScript() prints only after the archive is
// unpacked AND a codeharbord entry point has been proven to exist, so its
// presence is the script's success verdict.
QString installedMarker()
{
    return QStringLiteral("codeharbor: installed");
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
        // m_attempting: probeEndpoint() and runRemoteScript() spin nested event
        // loops, so this timer really can fire while an attempt is already in
        // flight. Running then would re-enter attemptWire() and unwire the
        // session the outer attempt is halfway through building. Simply
        // dropping the tick would end the ladder for good — single-shot timer,
        // nothing left to re-arm it, State::Reconnecting for ever — so the rung
        // is deferred instead.
        if (m_attempting) {
            // Re-arm THIS rung rather than announcing a new one.
            // scheduleReconnect() would emit reconnectScheduled() a second time
            // for a rung the user has already been told about — and on a session
            // the in-flight attempt may yet bring up, so the shell would flash
            // "reconnecting in N s" over a link that is coming back. Whichever
            // way that attempt ends it owns the outcome: on failure it calls
            // scheduleReconnect() itself and overwrites this tick, and on
            // success the tick lands in State::Wired and is dropped above.
            m_reconnectTimer->start();
            return;
        }
        ++m_attempt;
        // This attempt belongs to the ladder, so setReconnectEnabled(false) may
        // abandon it mid-flight; a connectAndWire() may not. See there.
        m_attemptIsRetry = true;
        if (!attemptWire()) {
            // A prerequisite on the SERVER that the user has to change — no
            // Node.js on the far side, a Node too old to run the service, a
            // repository root nothing may be installed into, an artifact that
            // is not a codeharbor-remote release. Waiting does not make any of
            // those true, so the ladder stops here instead of spending nine
            // more rungs re-asking the same question and painting nine more
            // identical toasts. failFatal() has already reported it.
            if (m_attemptFatal) {
                setState(State::Failed);
                return;
            }
            scheduleReconnect();
        }
    });

    if (m_pool) {
        // Session-level death. Only Disconnected/Error matter: the transient
        // Connecting/HostKeyCheck/Authenticating hops belong to a connect we
        // are driving ourselves, and handleConnectionLost() ignores anything
        // that is not a loss from State::Wired anyway.
        //
        // A user-driven teardown SHOULD still go through disconnectSession()
        // rather than pool->disconnectFromHost(), but NOT because of a dangling
        // ssh_channel: SshConnectionPool::closeSession() emits sessionClosing()
        // before it frees anything, every SshChannelDevice has that signal wired
        // to closeChannel() on a direct (same-thread) connection, and
        // closeChannel() hands the channel back and nulls its own handle — so by
        // the time the pool's sweep runs, no device holds a channel and no
        // destructor can dereference one. src/ssh/tests/tst_livessh.cpp
        // (deviceOutlivingItsSessionStopsCleanly) pins exactly that.
        //
        // The reason to prefer disconnectSession() is ordering of the teardown
        // we own: it closes and detaches the devices deliberately, so each
        // channel's readChannelFinished() drives CodeharbordClient's
        // failAllPending() while the wiring is still intact, and no late
        // channelError() arrives at a half-dismantled graph.
        connect(m_pool, &SshConnectionPool::stateChanged, this,
                [this](SshConnectionPool::State state) {
                    if (state == SshConnectionPool::State::Disconnected
                        || state == SshConnectionPool::State::Error)
                        handleConnectionLost(
                            QStringLiteral("SSH session went down"));
                });
        connect(m_pool, &SshConnectionPool::errorOccurred, this,
                [this](const QString& message) {
                    m_lastPoolError = message.trimmed();
                    handleConnectionLost(QStringLiteral("SSH session error: ")
                                         + m_lastPoolError);
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

void SessionBootstrap::setRemoteArtifactUrl(const QString& url)
{
    m_artifactUrl = url;
}

QString SessionBootstrap::remoteArtifactUrl() const
{
    return m_artifactUrl.isEmpty() ? defaultRemoteArtifactUrl() : m_artifactUrl;
}

void SessionBootstrap::requestRemoteUpgrade()
{
    m_forceUpgrade = true;
}

void SessionBootstrap::cancelRemoteUpgrade()
{
    m_forceUpgrade = false;
}

QString SessionBootstrap::defaultRemoteArtifactUrl()
{
    // An operator override wins outright. It is what makes an air-gapped or
    // mirrored deployment work: point it at an internal mirror, or at a plain
    // path where the tarball has already been staged ON the server, and no
    // outbound access is needed at all (see stagedArtifactPath()).
    const QString configured = qEnvironmentVariable("CH_REMOTE_ARTIFACT_URL");
    if (!configured.isEmpty())
        return configured;

    // main.cpp sets this from CODEHARBOR_VERSION, i.e. the CMake project
    // version, i.e. the version the release tag carries. Deliberately read at
    // run time instead of compiled in here: ch_app has no CODEHARBOR_VERSION
    // define, and inventing a second version constant in this file is exactly
    // the drift this feature must not introduce.
    const QString version = QCoreApplication::applicationVersion();
    if (version.isEmpty())
        return {};
    return QStringLiteral("https://github.com/yichenchong/codeharbor/releases/"
                          "download/v%1/codeharbor-remote.tar.gz")
        .arg(version);
}

QString SessionBootstrap::releaseMarkerPath(const QString& repoRoot)
{
    return remoteJoin(repoRoot, QStringLiteral(".codeharbor-release"));
}

QString SessionBootstrap::stagedArtifactPath(const QString& url)
{
    if (url.startsWith(QLatin1String("file://")))
        return url.mid(QLatin1String("file://").size());
    // A bare absolute path is the natural thing to type for "the tarball is
    // already sitting on the server", and no network URL can start with '/'.
    if (url.startsWith(QLatin1Char('/')))
        return url;
    return {};
}

bool SessionBootstrap::nodeVersionIsSupported(const QString& version)
{
    QString text = version.trimmed();
    if (text.startsWith(QLatin1Char('v')))
        text.remove(0, 1);
    // QString::split() always yields at least one element, so parts.at(0) below
    // is safe: an empty version string arrives as a single empty element, whose
    // toInt() then fails.
    const QStringList parts = text.split(QLatin1Char('.'));
    bool ok = false;
    const int major = parts.at(0).toInt(&ok);
    if (!ok)
        return false;
    if (major != kMinimumRemoteNodeMajor)
        return major > kMinimumRemoteNodeMajor;
    // Same major: the minor decides. A version string of just "23" is 23.0,
    // which is below the floor.
    int minor = 0;
    if (parts.size() > 1)
        minor = parts.at(1).toInt();
    return minor >= kMinimumRemoteNodeMinor;
}

QString SessionBootstrap::remoteInspectScript(const QString& nodePath,
                                              const QString& repoRoot)
{
    const QStringList candidates =
        entryCandidates(repoRoot, QStringLiteral("codeharbord"));
    QStringList quoted;
    quoted.reserve(candidates.size());
    for (const QString& candidate : candidates)
        quoted << shellQuote(candidate);
    const QString marker = shellQuote(releaseMarkerPath(repoRoot));

    // One CH_<KEY>=<value> line per RemoteInspection field, always in this
    // order, CH_NODE first — parseInspection() treats that first line as proof
    // that a report arrived at all.
    //
    // `command -v` rather than `[ -x ... ]`: it answers identically for the
    // absolute path a profile normally stores and for a bare name found on the
    // login PATH, and a profile may legitimately carry either.
    return QStringLiteral("__ch_node=; if command -v ") + shellQuote(nodePath)
           + QStringLiteral(" >/dev/null 2>&1; then __ch_node=$(")
           + shellQuote(nodePath)
           + QStringLiteral(" --version 2>/dev/null); fi; "
                            "echo \"CH_NODE=$__ch_node\"; "
                            "__ch_entry=; for __ch_c in ")
           + quoted.join(QLatin1Char(' '))
           + QStringLiteral("; do if [ -f \"$__ch_c\" ]; then "
                            "__ch_entry=\"$__ch_c\"; break; fi; done; "
                            "echo \"CH_ENTRY=$__ch_entry\"; "
                            "__ch_marker=; if [ -f ")
           + marker + QStringLiteral(" ]; then __ch_marker=$(head -n 1 ")
           + marker
           + QStringLiteral(" 2>/dev/null); fi; "
                            "echo \"CH_MARKER=$__ch_marker\"; "
                            "__ch_fetch=none; "
                            "if command -v curl >/dev/null 2>&1; then "
                            "__ch_fetch=curl; "
                            "elif command -v wget >/dev/null 2>&1; then "
                            "__ch_fetch=wget; fi; "
                            "echo \"CH_FETCH=$__ch_fetch\"; "
                            "__ch_tar=no; "
                            "if command -v tar >/dev/null 2>&1; then "
                            "__ch_tar=yes; fi; "
                            "echo \"CH_TAR=$__ch_tar\"");
}

QString SessionBootstrap::remoteProvisionScript(const QString& repoRoot,
                                                const QString& artifactUrl,
                                                const QString& fetcher)
{
    const QString root = shellQuote(repoRoot);
    // Scratch space INSIDE the directory the user chose. Nothing this script
    // touches — not the download, not the staged tree, not the displaced old
    // install — lives anywhere else, so a failed install cannot leave litter on
    // a machine that is not ours.
    const QString scratchDir =
        remoteJoin(repoRoot, QStringLiteral(".codeharbor-provision"));
    const QString scratch = shellQuote(scratchDir);
    const QString tarball = shellQuote(
        remoteJoin(scratchDir, QStringLiteral("codeharbor-remote.tar.gz")));
    // The archive is unpacked HERE, never over the live install: nothing under
    // repoRoot moves until a codeharbord entry point has been proven to exist
    // inside this directory. An in-place `tar -xzf -C <root>` was the original
    // design and is what made a failed update destroy a working service —
    // a truncated download, a full disk or a killed channel left the tree half
    // old and half new, with no way back.
    const QString stageDir = remoteJoin(scratchDir, QStringLiteral("stage"));
    const QString stage = shellQuote(stageDir);
    // Whatever the swap displaces, kept until the swap has finished so
    // ch_restore() can put it back.
    const QString backup = shellQuote(remoteJoin(scratchDir, QStringLiteral("backup")));
    // Every top-level name the swap will take over, one per line, written in
    // full BEFORE the swap touches anything. A file rather than a shell variable
    // because an archive member may contain spaces and POSIX sh has no arrays.
    const QString plan = shellQuote(remoteJoin(scratchDir, QStringLiteral("plan")));
    // Created only once `plan` is complete, and the ONLY thing that authorises an
    // undo to act on it. `set -e` aborts the run if any line of the plan cannot
    // be written, so this file existing means every line in that file is whole —
    // which is what stops an undo from ever reasoning about a half-written name.
    const QString ready = shellQuote(remoteJoin(scratchDir, QStringLiteral("ready")));
    const QString marker = shellQuote(releaseMarkerPath(repoRoot));
    // The marker is not in the archive, so it is not part of the swap and needs
    // its own record: losing it would make the next connect read an install of
    // ours as one a human manages and never update it again, while inventing one
    // would make it overwrite a directory nobody asked us to touch. Which of
    // these two files exists says which case an undo is putting back.
    const QString markerPrev =
        shellQuote(remoteJoin(scratchDir, QStringLiteral("release.prev")));
    const QString markerAbsent =
        shellQuote(remoteJoin(scratchDir, QStringLiteral("release.none")));

    const QString staged = stagedArtifactPath(artifactUrl);
    QString fetch;
    if (!staged.isEmpty()) {
        fetch = QStringLiteral("cp ") + shellQuote(staged) + QLatin1Char(' ')
                + tarball;
    } else if (fetcher == QLatin1String("wget")) {
        fetch = QStringLiteral("wget -q -O ") + tarball + QLatin1Char(' ')
                + shellQuote(artifactUrl);
    } else {
        // -f so an HTML error page is a failure rather than a "tarball" that
        // tar then rejects with something unreadable.
        fetch = QStringLiteral("curl -fsSL -o ") + tarball + QLatin1Char(' ')
                + shellQuote(artifactUrl);
    }

    // `set -e`: every step is load-bearing, and with the rollback below in
    // place the first failure is also where the old installation is put back.
    // Each "codeharbor: " line is republished to the user by runRemoteScript()
    // as it lands, which is what keeps a multi-megabyte download from looking
    // like a hung application.
    QStringList steps;
    steps << QStringLiteral("set -e");
    steps << QStringLiteral("echo ")
                 + shellQuote(progressPrefix()
                              + QStringLiteral("preparing %1").arg(repoRoot));
    steps << QStringLiteral("mkdir -p ") + root;

    // ---- the undo ---------------------------------------------------------
    //
    // Everything it needs is ON DISK, not in shell variables, so it is equally
    // usable by the trap below and by the leftover check that follows it: a run
    // killed outright (SIGKILL, a machine that went down mid-swap) never reaches
    // its own trap, and the next attempt is then the only thing left that can
    // put the installation back together.
    //
    //   plan          every top-level name the swap will take over
    //   ready         present only once `plan` is COMPLETE, and the sole
    //                 authorisation to act on it
    //   release.prev  the marker as it was, when there was one
    //   release.none  present when there was NO marker to keep
    //
    // `ready` is what makes the plan trustworthy rather than merely present. The
    // plan is written before anything is touched and `set -e` stops the run if
    // any line of it fails, so a plan that was interrupted mid-write has no
    // sentinel beside it and is ignored entirely — which is correct, because a
    // run that could not finish writing its plan had not yet moved anything. An
    // undo must never reason about a name it cannot prove it wrote in full: a
    // truncated one could name something of the user's that this install never
    // touched.
    //
    // The two marker files are exclusive and one of them always exists once the
    // swap can run, which is what stops an undo from either deleting a marker it
    // did not write or resurrecting one that was never there.
    //
    // Per planned member, three states have to be told apart, and `backup` alone
    // cannot do it — a member with no old counterpart leaves that directory
    // empty whether it was swapped in or not. The STAGE is the discriminator,
    // because the swap empties it one rename at a time:
    //
    //   backup/<b> exists      the old one was displaced: put it back, dropping
    //                          whatever is at root/<b> now (swapped in or not)
    //   else stage/<b> exists  the swap never reached this member, so root/<b> is
    //                          untouched — LEAVE IT ALONE
    //   else                   it was moved in and had no old counterpart, so
    //                          root/<b> is new and belongs to the failed install
    //
    // Every step is guarded rather than trusted, and a failure anywhere makes the
    // whole undo report failure WITHOUT removing the scratch directory: the
    // backup is then the only copy of the user's files left and deleting it is
    // the one thing that could turn a recoverable state into a lost one.
    steps << QStringLiteral("ch_undo() { __ch_undone=1; if [ -f ") + ready
                 + QStringLiteral(" ]; then while IFS= read -r __ch_b; do "
                                  "if [ -n \"$__ch_b\" ]; then if [ -e ")
                 + backup + QStringLiteral("/\"$__ch_b\" ]; then rm -rf ") + root
                 + QStringLiteral("/\"$__ch_b\" || __ch_undone=; mv ") + backup
                 + QStringLiteral("/\"$__ch_b\" ") + root
                 + QStringLiteral("/\"$__ch_b\" || __ch_undone=; elif [ -e ")
                 + stage + QStringLiteral("/\"$__ch_b\" ]; then :; else rm -rf ")
                 + root + QStringLiteral("/\"$__ch_b\" || __ch_undone=; fi; fi; "
                                        "done < ")
                 + plan + QStringLiteral(" || __ch_undone=; fi; if [ -f ")
                 + markerPrev + QStringLiteral(" ]; then cp ") + markerPrev
                 + QLatin1Char(' ') + marker
                 + QStringLiteral(" || __ch_undone=; elif [ -f ") + markerAbsent
                 + QStringLiteral(" ]; then rm -f ") + marker
                 + QStringLiteral(" || __ch_undone=; fi; if [ -z \"$__ch_undone\" ]; "
                                  "then return 1; fi; rm -f ")
                 + ready
                 // The sentinel goes FIRST, and `rm -f` is one unlink, so the
                 // authorisation to act on this plan is revoked before the plan
                 // and the backup start disappearing. Deleting the scratch first
                 // is a recursive walk that a kill can stop halfway, and the
                 // sentinel surviving beside a pruned backup is the one state
                 // that would make the NEXT undo destructive: every planned
                 // member would then look like one this install had brought in
                 // with no predecessor, which is the case that deletes.
                 + QStringLiteral("; rm -rf ") + scratch + QStringLiteral("; }");

    // A swap interrupted by something no shell can trap left its plan and its
    // sentinel behind. Finish that undo before starting a fresh install: the
    // alternative is `rm -rf` over the only copy of the displaced files, which
    // would turn a recoverable half-swapped tree into a permanently lost one.
    //
    // Best-effort under `set +e` — an undo that stops at its first failed rename
    // leaves MORE of the tree wrong, not less — and then judged: the scratch
    // directory still being there is ch_undo() reporting that it could not
    // finish, and there is nothing safe to do after that but stop and say where
    // the files are. This runs before the trap is armed, so stopping here rolls
    // nothing back; nothing has been touched yet either.
    steps << QStringLiteral("if [ -f ") + ready + QStringLiteral(" ]; then echo ")
                 + shellQuote(QStringLiteral(
                       "codeharbor: an earlier install was interrupted; putting "
                       "that installation back before starting"))
                 + QStringLiteral(" >&2; set +e; ch_undo; set -e; if [ -e ")
                 + scratch + QStringLiteral(" ]; then echo ")
                 + shellQuote(QStringLiteral(
                       "codeharbor: could not put the interrupted install back; "
                       "the displaced files are still under "
                       ".codeharbor-provision/backup and nothing further was "
                       "changed"))
                 + QStringLiteral(" >&2; exit 1; fi; fi");

    steps << QStringLiteral("rm -rf ") + scratch;
    steps << QStringLiteral("mkdir -p ") + stage + QLatin1Char(' ') + backup;
    // Recorded BEFORE the trap is armed, so every path that can reach the undo
    // finds the marker's previous state already on disk.
    steps << QStringLiteral("if [ -f ") + marker + QStringLiteral(" ]; then cp ")
                 + marker + QLatin1Char(' ') + markerPrev
                 + QStringLiteral("; else : > ") + markerAbsent
                 + QStringLiteral("; fi");

    // ---- the rollback -----------------------------------------------------
    //
    // Runs on ANY exit before the install is committed, including the signals a
    // dropped SSH channel produces (PIPE when the client stops reading, TERM
    // when the session is torn down): a swap left half done is the one state
    // that would cost the user a service that worked a moment ago.
    //
    // `set +e` first, because a best-effort restore must not abandon the
    // remaining members when one `rm` fails. The __ch_state guard makes it
    // idempotent: a trapped signal exits, which fires the EXIT trap a second
    // time, and a second pass over `moved` would take the very files the first
    // pass put back for a failed install's own leftovers.
    //
    // A rollback that cannot finish says so on stderr, which runRemoteScript()
    // carries back into the error the user is shown: the client re-reads the
    // directory afterwards either way, but "what is left is not what was there"
    // is not something to leave unsaid.
    steps << QStringLiteral("__ch_state=work");
    steps << QStringLiteral("ch_restore() { __ch_status=$?; set +e; ")
                 + QStringLiteral("if [ \"$__ch_state\" != work ]; then exit "
                                  "$__ch_status; fi; __ch_state=undo; echo ")
                 + shellQuote(QStringLiteral(
                       "codeharbor: the install failed; putting the previous "
                       "installation back"))
                 + QStringLiteral(" >&2; if ch_undo; then :; else echo ")
                 + shellQuote(QStringLiteral(
                       "codeharbor: the previous installation could not be fully "
                       "restored; what it had is under "
                       ".codeharbor-provision/backup"))
                 + QStringLiteral(" >&2; fi; exit $__ch_status; }");
    steps << QStringLiteral("trap ch_restore EXIT HUP INT TERM PIPE");

    steps << QStringLiteral("echo ")
                 + shellQuote(progressPrefix()
                              + QStringLiteral("fetching %1").arg(artifactUrl));
    steps << fetch;
    steps << QStringLiteral("echo ")
                 + shellQuote(progressPrefix()
                              + QStringLiteral("unpacking codeharbor-remote"));
    steps << QStringLiteral("tar -xzf ") + tarball + QStringLiteral(" -C ") + stage;

    // The archive is judged where it landed. Every path under repoRoot is still
    // the one the user has been running when this check decides.
    steps << findEntry(stageDir, QStringLiteral("codeharbord"));
    steps << QStringLiteral("if [ -z \"$__ch_entry\" ]; then echo ")
                 + shellQuote(QStringLiteral(
                       "codeharbor: the archive unpacked but holds no "
                       "codeharbord entry point; it is not a codeharbor-remote "
                       "release"))
                 + QStringLiteral(" >&2; exit 1; fi");

    // ---- the swap ---------------------------------------------------------
    //
    // Every top-level member of the staged tree replaces its namesake under
    // repoRoot, the displaced one going to `backup` first so it can come back.
    // Renames within one directory tree, so the window in which repoRoot is
    // neither wholly old nor wholly new is a handful of syscalls wide — and
    // every state inside that window is one ch_undo() can read back.
    //
    // Planned in FULL, and only then declared ready, before a single member is
    // touched. Journalling each member as the swap claimed it was the obvious
    // alternative and is weaker in both directions: an append that failed after
    // its member had been displaced would hide that member from the undo, and an
    // append that failed PARTWAY would leave the undo reading a fragment of a
    // name — which, being neither in `backup` nor in `stage`, is exactly the
    // shape it would delete from the user's directory. Neither state can arise
    // when the whole plan precedes the whole swap: `set -e` stops the run before
    // the sentinel exists, and an undo without the sentinel does nothing.
    //
    // A running codeharbord is unaffected: node holds its entry file open, and
    // renaming a directory out from under an open file descriptor does not
    // disturb it.
    steps << QStringLiteral("echo ")
                 + shellQuote(progressPrefix()
                              + QStringLiteral("replacing the previous "
                                               "installation"));
    steps << QStringLiteral(": > ") + plan;
    steps << QStringLiteral("for __ch_m in ") + stage + QStringLiteral("/* ")
                 + stage + QStringLiteral("/.[!.]* ") + stage
                 + QStringLiteral("/..?*; do if [ -e \"$__ch_m\" ]; then printf ")
                 + shellQuote(QStringLiteral("%s\\n"))
                 + QStringLiteral(" \"${__ch_m##*/}\" >> ") + plan
                 + QStringLiteral("; fi; done");
    steps << QStringLiteral(": > ") + ready;
    steps << QStringLiteral("while IFS= read -r __ch_b; do if [ -n \"$__ch_b\" ]; "
                            "then if [ -e ")
                 + root + QStringLiteral("/\"$__ch_b\" ]; then mv ") + root
                 + QStringLiteral("/\"$__ch_b\" ") + backup
                 + QStringLiteral("/\"$__ch_b\"; fi; mv ") + stage
                 + QStringLiteral("/\"$__ch_b\" ") + root
                 + QStringLiteral("/\"$__ch_b\"; fi; done < ") + plan;

    // What the client will actually launch, read back from repoRoot rather than
    // inferred from the stage: this is the sentence the user is shown, and it
    // has to name a path that exists now.
    steps << findEntry(repoRoot, QStringLiteral("codeharbord"));
    steps << QStringLiteral("if [ -z \"$__ch_entry\" ]; then echo ")
                 + shellQuote(QStringLiteral(
                       "codeharbor: nothing launchable under the installation "
                       "directory after unpacking the archive"))
                 + QStringLiteral(" >&2; exit 1; fi");

    // The marker, then the commit point, then the cleanup. Written only once
    // there is something launchable to attribute it to, and rolled back with
    // everything else when there is not, so a marker never names a release the
    // directory does not hold.
    steps << QStringLiteral("printf ") + shellQuote(QStringLiteral("%s\\n"))
                 + QLatin1Char(' ') + shellQuote(artifactUrl)
                 + QStringLiteral(" > ") + marker;
    steps << QStringLiteral("__ch_state=ok");
    // Same ordering rule as ch_undo's tail, and load-bearing for the same
    // reason: the install is committed, so nothing may undo it any more. One
    // unlink revokes that, and only then does the recursive delete start — a
    // kill inside `rm -rf` must not be able to leave the sentinel standing over
    // a half-deleted backup, because the next install's undo would read that as
    // "these members are mine and have no predecessor" and delete the very
    // installation this run just put there.
    steps << QStringLiteral("rm -f ") + ready;
    steps << QStringLiteral("rm -rf ") + scratch;
    // $__ch_entry is deliberately OUTSIDE the quoted literal: it is the entry
    // point the install produced, and echo joins its two arguments with a
    // space. This line is the script's success verdict (installedMarker()).
    steps << QStringLiteral("echo ") + shellQuote(installedMarker())
                 + QStringLiteral(" \"$__ch_entry\"");

    return steps.join(QStringLiteral("; "));
}

SessionBootstrap::RemoteInspection
SessionBootstrap::parseInspection(const QString& output)
{
    RemoteInspection info;
    const QStringList lines =
        output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (!line.startsWith(QLatin1String("CH_")))
            continue;
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals < 0)
            continue;
        const QString key = line.left(equals);
        const QString value = line.mid(equals + 1).trimmed();
        if (key == QLatin1String("CH_NODE")) {
            // The first line the script prints, so this is also what proves a
            // report came back rather than, say, a login banner.
            info.reported = true;
            info.nodeVersion = value;
            info.nodePresent = !value.isEmpty();
        } else if (key == QLatin1String("CH_ENTRY")) {
            info.entry = value;
        } else if (key == QLatin1String("CH_MARKER")) {
            info.marker = value;
        } else if (key == QLatin1String("CH_FETCH")) {
            info.fetcher = value;
        } else if (key == QLatin1String("CH_TAR")) {
            info.tar = value == QLatin1String("yes");
        }
    }
    return info;
}

int SessionBootstrap::reconnectDelaySeconds(int attempt)
{
    // The connection retry ladder (SPEC 5.6): this is the sole implementation —
    // reconnect is driven at the session level, not per terminal pane.
    // tst_sessionbootstrap pins this exact vector against the spec.
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
    // ONLY a rung of the ladder. Aborting a connectAndWire() the user asked for
    // strands the object: that attempt unwinds with m_cancelRequested set,
    // connectAndWire() then declines to relabel it Failed (a cancel means the
    // canceller owns the end state, which is true of disconnectSession() and of
    // nothing else), and the state sits on Connecting for ever with no retry
    // armed and no way back. A user turning auto-reconnect off has not asked
    // to abandon the connect they are in the middle of making.
    if (m_attempting && m_attemptIsRetry) {
        abortAttempt();
        // The rung is gone and nothing will re-arm it, so this attempt's end
        // state is ours to set: it unwinds through scheduleReconnect(), which
        // returns at once for a cancelled attempt.
        const bool wasTearingDown = m_tearingDown;
        m_tearingDown = true;
        if (m_pool)
            m_pool->disconnectFromHost();
        m_tearingDown = wasTearingDown;
        // The ladder is over, so its counter is history. reconnectAttempt() is
        // public and is what the shell shows as "attempt N of M"; leaving the
        // last rung's number behind State::Disconnected reports a ladder that
        // is not running. disconnectSession(), the other way out of the ladder,
        // has always cleared it.
        m_attempt = 0;
        setState(State::Disconnected);
    } else if (m_state == State::Reconnecting) {
        m_attempt = 0;
        setState(State::Disconnected);
    }
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

void SessionBootstrap::setTrustUnknownHostKeys(bool enabled)
{
    m_trustUnknownHostKeys = enabled;
    // An explicit setter call is an attended caller taking ownership of the
    // policy. Do not restore an older environment-only value over it later.
    m_environmentTrustActive = false;
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
    if (m_nestedLoop)
        m_nestedLoop->quit();
}

void SessionBootstrap::scheduleReconnect()
{
    // The attempt whose failure got us here was cancelled, not lost: the user
    // has already been put into Disconnected and must stay there.
    if (m_cancelRequested)
        return;
    if (!m_reconnectEnabled) {
        // The ladder is over, so nothing of it may still be ticking:
        // reconnectPending() is public, and a rung left armed behind
        // State::Disconnected makes it report a retry that will be dropped the
        // moment it fires.
        cancelReconnect();
        setState(State::Disconnected);
        return;
    }
    if (m_maxAttempts > 0 && m_attempt >= m_maxAttempts) {
        cancelReconnect();
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

    // A rung can be armed even here: a tick that fired while the previous
    // attempt was still in flight re-arms itself, and that attempt then wired
    // the session. This loss supersedes it, and scheduleReconnect() below arms
    // the rung that actually belongs to it.
    cancelReconnect();

    unwire();
    // A channel can die while the SSH session itself still looks connected.
    // Drop that session before arming a retry; otherwise disabling reconnect
    // leaves a pool that reports Connected while this object reports down.
    const bool wasTearingDown = m_tearingDown;
    m_tearingDown = true;
    if (m_pool)
        m_pool->disconnectFromHost();
    m_tearingDown = wasTearingDown;
    if (!m_reconnectEnabled) {
        setState(State::Disconnected);
    } else {
        m_attempt = 0;
        // A loss of a LIVE session is, by definition, not a cancelled attempt.
        // The flag is scoped to one attempt but only cleared where an attempt
        // STARTS, so a cancel that outlived its attempt would otherwise make
        // scheduleReconnect() return without arming a retry.
        m_cancelRequested = false;
        scheduleReconnect();
    }
    // Publish the stable disconnected/reconnecting state before the diagnostic
    // so observers can safely inspect the state from their error handlers.
    emit error(reason);
}

void SessionBootstrap::disconnectSession()
{
    // An attempt parked in the connect pre-flight is interrupted first, so the
    // user's "disconnect" is honoured now rather than after the remaining
    // connect budget — and so the unwinding attempt does not go on to wire the
    // very session that is being torn down here.
    abortAttempt();
    const bool wasTearingDown = m_tearingDown;
    m_tearingDown = true;
    cancelReconnect();
    unwire();
    if (m_pool)
        m_pool->disconnectFromHost();
    // Restored rather than forced false: this can run from inside a teardown
    // that is already in progress (a handler of the error() fail() emits, say),
    // and clearing the flag under that caller would let the pool and device
    // signals its remaining steps provoke be mistaken for a fresh loss.
    m_tearingDown = wasTearingDown;
    if (m_environmentTrustActive) {
        m_trustUnknownHostKeys = m_environmentTrustPrevious;
        m_environmentTrustActive = false;
    }
    // An upgrade nobody is connecting for any more is not owed to the next
    // connect. A request survives a FAILED attempt on purpose — the retry (the
    // same attempt resumed after a host-key or credential prompt, or a rung of
    // the ladder) is still the user's upgrade — but an explicit teardown is
    // them giving up on it.
    m_forceUpgrade = false;
    m_attempt = 0;
    setState(State::Disconnected);
}

void SessionBootstrap::fail(const QString& message)
{
    unwire();
    // Authentication may have succeeded before a later provisioning or exec
    // step failed. Do not leave that SSH session alive behind State::Failed.
    const bool wasTearingDown = m_tearingDown;
    m_tearingDown = true;
    if (m_pool)
        m_pool->disconnectFromHost();
    m_tearingDown = wasTearingDown;
    emit error(message);
}

void SessionBootstrap::failFatal(const QString& message)
{
    // Same teardown and the same report as fail(); the difference is that the
    // reconnect ladder must not spend nine more rungs on a condition only a
    // change on the server can clear. Set BEFORE fail(), because fail() emits
    // error() and a handler of that may look at the object.
    m_attemptFatal = true;
    fail(message);
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
                                   const QString& user,
                                   const QString& identityFile)
{
    return m_pool && m_pool->connectToHost(host, port, user, identityFile);
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
    // A verdict that landed BEFORE loop.exec(). connectToHost() may fail
    // synchronously (an unresolvable name it already has cached, an address
    // family the host has no route for), and QEventLoop::quit() called before
    // exec() is simply forgotten: entering the loop anyway would sit there for
    // the whole connect budget and then report the endpoint as MUTE rather than
    // as unreachable.
    bool settled = false;

    // The server's identification string (RFC 4253 §4.2) is sent as soon as the
    // TCP connection is up, so the first byte is a sufficient liveness proof —
    // and it is a far more robust one than parsing for "SSH-", which a server
    // may legally precede with arbitrary lines. The bytes are left unread; the
    // socket is a throwaway and libssh opens its own.
    connect(&socket, &QIODevice::readyRead, &loop, [&] {
        spoke = true;
        settled = true;
        loop.quit();
    });
    // A peer that hangs up without speaking is dead for our purposes, and so is
    // any resolve/connect failure.
    connect(&socket, &QAbstractSocket::errorOccurred, &loop,
            [&](QAbstractSocket::SocketError) {
                settled = true;
                loop.quit();
            });
    connect(&socket, &QAbstractSocket::disconnected, &loop, [&] {
        settled = true;
        loop.quit();
    });
    connect(&deadline, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        settled = true;
        loop.quit();
    });

    socket.connectToHost(host, port, QIODevice::ReadOnly);
    deadline.start(m_connectTimeoutMs);
    // Published so abortAttempt() can cut the wait short; cleared on every exit
    // path, including the exceptional one.
    m_nestedLoop = &loop;
    const auto clearLoop = qScopeGuard([this] { m_nestedLoop = nullptr; });
    // ExcludeUserInputEvents: repaints, timers and sockets keep running so the
    // shell stays alive on screen, but a second click cannot re-enter connect.
    if (!settled)
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

SshChannelDevice* SessionBootstrap::openChannelDevice(const QString& command,
                                                      const QString& role)
{
    // `role` only labels the long-lived diagnostics stream, which is wired by
    // wireChannelSignals() rather than here.
    Q_UNUSED(role);

    auto* device = new SshChannelDevice(m_pool, this);
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

bool SessionBootstrap::runRemoteScript(const QString& script, int timeoutMs,
                                       QString* output, QString* error)
{
    const QString role = QStringLiteral("codeharbor-provision");
    if (!m_pool) {
        if (error)
            *error = QStringLiteral("no SSH connection pool");
        return false;
    }
    // The attempt was already cancelled before this step began - abortAttempt()
    // can land between two provisioning steps, where there is no nested loop of
    // ours for it to quit. Entering the wait anyway would spend the whole
    // budget (three minutes for an install) on the GUI thread before noticing,
    // and would run a remote command for a session nobody wants any more.
    if (m_cancelRequested) {
        if (error)
            *error = QStringLiteral("cancelled");
        return false;
    }

    // Stack-owned, and handed back to the pool by closeChannel() below rather
    // than left for the destructor: an SSH server caps concurrent channels per
    // connection, and this runs up to three times per connect.
    SshChannelDevice device(m_pool);
    QString diagnostics;
    connect(&device, &SshChannelDevice::channelError, &device,
            [this, &diagnostics, role](const QString& text) {
                const QString trimmed = text.trimmed();
                if (trimmed.isEmpty())
                    return;
                if (!diagnostics.isEmpty())
                    diagnostics += QStringLiteral("; ");
                diagnostics += trimmed;
                emit channelDiagnostic(role, trimmed);
            });

    // Quoted as one argument to `sh -c` so the script only relies on POSIX sh,
    // whatever login shell the remote account happens to use — the same rule
    // rpcCommand()/bridgeCommand() follow.
    if (!device.startExec(QStringLiteral("sh -c ") + shellQuote(script))) {
        if (error)
            *error = diagnostics.isEmpty()
                         ? QStringLiteral("could not open an SSH channel")
                         : diagnostics;
        return false;
    }

    QEventLoop loop;
    QByteArray collected;
    qsizetype published = 0;
    bool finished = false;
    bool timedOut = false;

    // Drain whatever has arrived and republish every COMPLETE progress line.
    // `published` is how far the republishing has got, so a line split across
    // two reads is emitted once and whole.
    const auto drain = [this, &device, &collected, &published] {
        collected += device.readAll();
        qsizetype newline = collected.indexOf('\n', published);
        while (newline >= 0) {
            const QString line =
                QString::fromUtf8(collected.mid(published, newline - published))
                    .trimmed();
            published = newline + 1;
            if (line.startsWith(progressPrefix()))
                emit provisioning(line.mid(progressPrefix().size()));
            newline = collected.indexOf('\n', published);
        }
    };

    QTimer poll;
    poll.setInterval(20);
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);

    connect(&device, &QIODevice::readyRead, &loop, drain);
    // Belt and braces beside readyRead: SshChannelDevice's own read pump is a
    // timer, so a poll of the same order costs nothing and means this wait
    // cannot hang on a missed signal edge.
    connect(&poll, &QTimer::timeout, &loop, drain);
    connect(&device, &SshChannelDevice::readChannelFinished, &loop, [&] {
        finished = true;
        loop.quit();
    });
    connect(&deadline, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });

    poll.start();
    if (timeoutMs > 0)
        deadline.start(timeoutMs);
    // Published so abortAttempt() can cut a multi-minute download short the
    // moment the user asks to disconnect; cleared on every exit path.
    m_nestedLoop = &loop;
    const auto clearLoop = qScopeGuard([this] { m_nestedLoop = nullptr; });
    // ExcludeUserInputEvents, exactly as probeEndpoint() does it: the shell
    // keeps repainting (which is the whole point of reporting progress) but a
    // second click cannot re-enter the connect path.
    //
    // Not entered at all when the verdict is already in. startExec() publishes
    // the remote's first stderr line synchronously, and a handler of the
    // channelDiagnostic() that carries it may call disconnectSession() — which
    // finds no nested loop of ours to quit and only raises the cancel flag.
    // QEventLoop::quit() before exec() is forgotten, so entering anyway would
    // spend the whole budget (three minutes for an install) frozen on the GUI
    // thread, for a session nobody wants any more.
    if (!m_cancelRequested && !finished && !timedOut)
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    poll.stop();
    // Whatever landed together with the EOF that ended the loop, and then
    // whatever the first drain's own read left behind. Twice rather than a bare
    // readAll(): readAll() would append those bytes to `collected` without ever
    // scanning them, so the last progress line of an install - the "installed"
    // verdict itself - would never reach the user.
    drain();
    drain();
    device.closeChannel();

    if (output)
        *output = QString::fromUtf8(collected);

    if (m_cancelRequested) {
        if (error)
            *error = QStringLiteral("cancelled");
        return false;
    }
    if (timedOut) {
        if (error) {
            *error = QStringLiteral("the remote command did not finish within "
                                    "%1 ms").arg(timeoutMs);
            if (!diagnostics.isEmpty())
                *error += QStringLiteral(": ") + diagnostics;
        }
        return false;
    }
    if (!finished) {
        if (error)
            *error = diagnostics.isEmpty()
                         ? QStringLiteral("the remote command produced nothing")
                         : diagnostics;
        return false;
    }
    // EOF happened, so the remote command RAN — but an SSH exec channel does not
    // hand its exit status to SshChannelDevice, so "ran" is not "succeeded".
    // Callers decide from the output; the stderr comes back either way, because
    // it is the only explanation a failed script leaves behind.
    if (error)
        *error = diagnostics;
    return true;
}

bool SessionBootstrap::keepExistingService(const QString& reason, bool forced,
                                           const QString& entry)
{
    const QString message =
        tr("%1 The CodeHarbor remote service already installed at %2 on %3 was "
           "left exactly as it was, and this session is using it.")
            .arg(reason, entry, m_host);
    if (forced) {
        // The user asked for this update, so its failure is the news — and
        // error() cannot carry that news, because it is HELD while a connect
        // attempt is in flight and then dropped when the attempt succeeds,
        // which is precisely what this attempt is about to do.
        emit upgradeFailed(message);
    } else {
        // Nobody asked: the client merely noticed that the release it would
        // install is not the one that is there. That is a log line, not a toast
        // on a session which is coming up fine — the same treatment an old node
        // under an installation that needs no update already gets.
        emit channelDiagnostic(QStringLiteral("codeharbor-provision"), message);
    }
    return true;
}

bool SessionBootstrap::ensureRemoteService()
{
    const QString role = QStringLiteral("codeharbor-provision");
    // Whatever the connect was doing before: Connecting for a user-initiated
    // attempt, Reconnecting for a rung of the ladder. Restored on every exit so
    // provisioning never relabels the attempt it interrupted.
    const State resume = m_state;
    // Consumed here and cleared whatever happens below, so a forced upgrade
    // that fails (or that the user cancels) applies to THIS attempt only and
    // never turns every later reconnect into a reinstall.
    const bool forced = std::exchange(m_forceUpgrade, false);

    QString report;
    QString reportError;
    const bool asked = runRemoteScript(remoteInspectScript(m_nodePath, m_repoRoot),
                                       kInspectTimeoutMs, &report, &reportError);
    if (!asked && m_cancelRequested)
        return false;
    const RemoteInspection info = asked ? parseInspection(report)
                                        : RemoteInspection{};
    if (!info.reported) {
        // FAIL-SOFT, and this is the important design decision in this
        // function. The inspection is a DIAGNOSTIC: it exists so a missing
        // prerequisite is named instead of surfacing as "codeharbord channel
        // closed". If the diagnostic itself cannot run — the server caps
        // channels, a login banner ate the report, sh is something exotic — the
        // only defensible answer is to carry on down the exec path that worked
        // before this feature existed. Refusing to connect because a probe
        // failed would turn servers that work today into unreachable ones.
        if (forced) {
            // The user asked us to write to their server and we cannot even
            // read what is there, so nothing is written. Loud, not a
            // diagnostic: an update that silently did not happen is worse than
            // one that says so. The connect still proceeds, because the
            // installation that was already there may well work.
            emit upgradeFailed(
                tr("Could not update the CodeHarbor remote service on %1: the "
                   "server did not report what it has (%2). Nothing was changed "
                   "under \"%3\".")
                    .arg(m_host,
                         reportError.isEmpty() ? tr("no report came back")
                                               : reportError,
                         m_repoRoot));
            return true;
        }
        emit channelDiagnostic(
            role, QStringLiteral("could not read the server's prerequisites "
                                 "(%1); connecting without provisioning")
                      .arg(reportError.isEmpty()
                               ? QStringLiteral("no report came back")
                               : reportError));
        return true;
    }

    if (!info.nodePresent) {
        // Not a soft warning even when something is installed: without node
        // nothing under repoRoot can start, and "sh: node: not found" on a dead
        // channel is precisely the unactionable failure this replaces.
        failFatal(tr("\"%1\" is not a runnable Node.js on %2. The CodeHarbor remote "
                "service needs Node %3.%4 or newer there. Install it (nodesource "
                "or nvm) and set the profile's node path to its ABSOLUTE "
                "location — a non-interactive SSH session has none of a version "
                "manager's PATH.")
                .arg(m_nodePath, m_host)
                .arg(kMinimumRemoteNodeMajor)
                .arg(kMinimumRemoteNodeMinor));
        return false;
    }

    const QString url = remoteArtifactUrl();
    const bool nodeOk = nodeVersionIsSupported(info.nodeVersion);

    // Three reasons to install:
    //   * nothing usable is there at all;
    //   * the copy WE installed is not the one this client would install now,
    //     which is how a client upgrade drags the remote side along instead of
    //     driving a service from another release; or
    //   * the user asked for one outright (requestRemoteUpgrade()).
    // Without that third reason, an install with NO marker belongs to a human —
    // a git checkout, a hand-unpacked tarball — and is never overwritten. An
    // incompatible one of those is still caught, by AppController's
    // kMinimumServerSchemaVersion check against server.info, which reports it
    // without destroying the tree.
    const bool needsInstall =
        forced || info.entry.isEmpty()
        || (!info.marker.isEmpty() && info.marker != url);

    if (!needsInstall) {
        if (!nodeOk) {
            // Deliberately not a refusal: entryCandidates() prefers built
            // dist/*.js, which an older node runs fine, so a server that works
            // today must keep working. Only PROVISIONING demands the floor.
            emit channelDiagnostic(
                role, QStringLiteral("node %1 on %2 is older than the %3.%4 the "
                                     "remote service declares; using the "
                                     "existing installation at %5 unchanged")
                          .arg(info.nodeVersion, m_host)
                          .arg(kMinimumRemoteNodeMajor)
                          .arg(kMinimumRemoteNodeMinor)
                          .arg(info.entry));
        }
        return true;
    }

    // ---- the one place a forced upgrade stops -----------------------------
    //
    // A service is running from a SOURCE CHECKOUT when the entry point the
    // server reported is not the release layout's `<root>/dist/codeharbord.js`
    // but one of the checkout paths under `<root>/remote/`. Unpacking a release
    // tarball there would not replace that checkout: it would drop a `dist/`
    // beside it that entryCandidates() then PREFERS, so the user's own build
    // silently stops being what runs, inside a directory they maintain. Say so
    // instead, and name the two ways forward.
    //
    // Restricted to the forced path rather than left unconditional: an ordinary
    // connect must keep behaving exactly as it did, and the only way it reaches
    // an install at all with an entry already present is a marker of our own
    // naming a different release, which by construction is the release layout.
    const QString releaseEntry =
        entryCandidates(m_repoRoot, QStringLiteral("codeharbord")).at(0);
    if (forced && !info.entry.isEmpty() && info.entry != releaseEntry) {
        // Reported, not fatal. Nothing was written, so the checkout the user
        // maintains is still there and still runnable; refusing the session as
        // well would answer a mistaken click by taking their workspace away.
        return keepExistingService(
            tr("\"%1\" on %2 is a source checkout, not an installed release: it "
               "runs %3. Update it there (git pull, then build), or point this "
               "profile's repository root at a directory of its own to install "
               "releases into. Nothing was changed.")
                .arg(m_repoRoot, m_host, info.entry),
            forced, info.entry);
    }

    // ---- everything below writes to somebody else's machine ---------------
    //
    // A prerequisite of INSTALLING that the server does not meet is not a
    // reason to refuse the session. Every one of the checks below therefore
    // ends in declineInstall(): when something launchable is already under
    // repoRoot the user is told what did not happen — loudly when they asked
    // for it — and the session comes up on the copy that was working before.
    // Only a server with NOTHING installed is refused, because there is then
    // nothing to fall back to.
    //
    // That distinction is the whole point: "your update did not happen" is a
    // toast, while "your workspace is unreachable until you upgrade Node on the
    // server" is what this client used to do to a working installation the
    // moment a client upgrade made the release marker stale.
    const auto declineInstall = [this, &info, forced](const QString& reason) {
        if (!info.entry.isEmpty())
            return keepExistingService(reason, forced, info.entry);
        failFatal(reason);
        return false;
    };

    if (!nodeOk) {
        return declineInstall(
            tr("Node %1 on %2 is too old to run the CodeHarbor remote service, "
               "which needs %3.%4 or newer. Install a current Node.js there and "
               "point the profile's node path at it. Nothing was written to "
               "\"%5\".")
                .arg(info.nodeVersion, m_host)
                .arg(kMinimumRemoteNodeMajor)
                .arg(kMinimumRemoteNodeMinor)
                .arg(m_repoRoot));
    }
    if (m_repoRoot.isEmpty() || m_repoRoot == QLatin1String("/")
        || m_repoRoot.startsWith(QLatin1Char('~'))) {
        // "~" is refused rather than expanded: every path this client sends is
        // shell-quoted (it must be — repoRoot is user input), so an unexpanded
        // tilde would create a directory literally named "~" and install into
        // it. Reading such a path merely fails; WRITING to it makes a mess in
        // the user's home.
        return declineInstall(
            tr("Cannot install the CodeHarbor remote service into \"%1\". Give "
               "the profile an absolute directory of its own on %2, for example "
               "/home/<user>/codeharbor; \"~\" is not expanded because every "
               "path sent to the server is quoted.")
                .arg(m_repoRoot, m_host));
    }
    // "has no service" is a lie during an upgrade, where one is running right
    // now; the missing prerequisite is the same either way, so only the lead-in
    // changes.
    const QString lacks =
        info.entry.isEmpty()
            ? tr("%1 has no CodeHarbor remote service under \"%2\"")
                  .arg(m_host, m_repoRoot)
            : tr("The CodeHarbor remote service under \"%2\" on %1 cannot be "
                 "updated")
                  .arg(m_host, m_repoRoot);
    if (url.isEmpty()) {
        return declineInstall(
            tr("%1, and this build cannot tell which release matches it (it "
               "reports no version). Unpack codeharbor-remote.tar.gz there "
               "yourself, or set CH_REMOTE_ARTIFACT_URL to the tarball to "
               "install.")
                .arg(lacks));
    }
    const QString staged = stagedArtifactPath(url);
    if (staged.isEmpty() && info.fetcher == QLatin1String("none")) {
        return declineInstall(
            tr("%1, and neither curl nor wget is installed there to download "
               "%2. Install one of them, or unpack codeharbor-remote.tar.gz "
               "into \"%3\" by hand, or stage the tarball on the server and set "
               "CH_REMOTE_ARTIFACT_URL to its path.")
                .arg(lacks, url, m_repoRoot));
    }
    if (!info.tar) {
        return declineInstall(
            tr("%1: there is no `tar` there to unpack one. Install tar, or "
               "unpack codeharbor-remote.tar.gz into \"%2\" by hand.")
                .arg(lacks, m_repoRoot));
    }

    setState(State::Provisioning);
    emit provisioning(info.entry.isEmpty()
                          ? tr("Installing the CodeHarbor remote service into "
                               "%1 on %2")
                                .arg(m_repoRoot, m_host)
                          : tr("Updating the CodeHarbor remote service in %1 on "
                               "%2")
                                .arg(m_repoRoot, m_host));

    QString installLog;
    QString installError;
    const bool ran =
        runRemoteScript(remoteProvisionScript(m_repoRoot, url, info.fetcher),
                        kProvisionTimeoutMs, &installLog, &installError);
    // Cancel is checked BEFORE the state is restored: disconnectSession() runs
    // from inside runRemoteScript()'s nested loop and has already put us in
    // Disconnected, so restoring `resume` here would resurrect Connecting on an
    // attempt the user just abandoned.
    if (!ran && m_cancelRequested)
        return false;
    setState(resume);
    if (!ran || !installLog.contains(installedMarker())) {
        const QString reason =
            tr("Could not install the CodeHarbor remote service into \"%1\" on "
               "%2 from %3: %4")
                .arg(m_repoRoot, m_host, url,
                     installError.isEmpty()
                         ? tr("the install did not report success")
                         : installError);
        // remoteProvisionScript() stages the archive, proves it holds an entry
        // point and only then swaps it in, undoing the swap on any failure — so
        // repoRoot SHOULD now hold exactly what it held before. "Should" is not
        // something to connect on: a script killed outright (channel dropped,
        // budget spent) never reaches its own rollback. So the survivor is READ
        // BACK, and only a directory that really still holds a launchable entry
        // point is treated as a fallback.
        QString leftReport;
        QString leftError;
        const RemoteInspection left =
            runRemoteScript(remoteInspectScript(m_nodePath, m_repoRoot),
                            kInspectTimeoutMs, &leftReport, &leftError)
                ? parseInspection(leftReport)
                : RemoteInspection{};
        if (m_cancelRequested)
            return false;
        if (!left.entry.isEmpty())
            return keepExistingService(reason, forced, left.entry);
        fail(reason);
        return false;
    }

    // Ask the SERVER again rather than believing the script's own verdict. This
    // is the difference between "the install said it worked" and "there is now
    // something here that this client can launch", and it is what makes a
    // silently empty install loud.
    QString verifyReport;
    QString verifyError;
    if (!runRemoteScript(remoteInspectScript(m_nodePath, m_repoRoot),
                         kInspectTimeoutMs, &verifyReport, &verifyError)) {
        if (m_cancelRequested)
            return false;
        fail(tr("Installed the CodeHarbor remote service into \"%1\" on %2 but "
                "could not confirm it: %3")
                .arg(m_repoRoot, m_host, verifyError));
        return false;
    }
    const RemoteInspection after = parseInspection(verifyReport);
    if (after.entry.isEmpty()) {
        failFatal(tr("Unpacked %1 into \"%2\" on %3, but there is no codeharbord entry "
                "point there afterwards, so the archive is not a "
                "codeharbor-remote release.")
                .arg(url, m_repoRoot, m_host));
        return false;
    }

    emit provisioning(tr("Installed the CodeHarbor remote service at %1")
                          .arg(after.entry));
    return true;
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

QString SessionBootstrap::withLastPoolError(const QString& message) const
{
    if (m_lastPoolError.isEmpty())
        return message;
    return message + QStringLiteral(": ") + m_lastPoolError;
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
    m_attemptFatal = false;
    m_lastDiagnostic.clear();
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
    // An UNKNOWN key needs a decision, and SPEC 12.1 says the decision is the
    // USER's: the pool asks the installed host-key callback, and AppController
    // installs one that refuses the attempt and raises the fingerprint prompt.
    // If NO callback is installed we must not invent one that says yes — that is
    // blind trust-on-first-use: whatever key the server presented would be
    // trusted, written into the known_hosts store and connected to, with nobody
    // ever asked. So the attempt is refused here instead, before any handshake.
    //
    // Accepting an unknown key unasked is available only where there is nobody
    // to ask and refusing means "cannot connect at all" — the CH_LIVE_* harness
    // path and live tests — and only when that caller has explicitly opted in
    // via setTrustUnknownHostKeys().
    KnownHosts hosts;
    const QFileInfo storeInfo(m_knownHostsPath);
    const bool storeExists = storeInfo.exists();
    QFile store(m_knownHostsPath);
    if (storeExists) {
        if (!store.open(QIODevice::ReadOnly | QIODevice::Text)) {
            emit error(QStringLiteral(
                           "refusing to connect to %1:%2: could not read the "
                           "trusted-host store %3 (%4)")
                           .arg(m_host)
                           .arg(m_port)
                           .arg(m_knownHostsPath, store.errorString()));
            return false;
        }
        const QByteArray contents = store.readAll();
        if (store.error() != QFileDevice::NoError) {
            emit error(QStringLiteral(
                           "refusing to connect to %1:%2: could not read the "
                           "trusted-host store %3 (%4)")
                           .arg(m_host)
                           .arg(m_port)
                           .arg(m_knownHostsPath, store.errorString()));
            return false;
        }
        hosts = KnownHosts::parse(QString::fromUtf8(contents));
        store.close();
        // Owner-only, on the way IN. This file is the whole record of which
        // server keys the user has approved, and an account that can WRITE it
        // inserts a key of its own and host verification stops meaning
        // anything - the presented key then matches, so nobody is ever asked.
        // Best effort, exactly like ServerProfiles::restrictPermissions().
        QFile::setPermissions(m_knownHostsPath,
                              QFile::ReadOwner | QFile::WriteOwner);
    }
    const int knownBefore = hosts.entries().size();
    m_pool->setKnownHosts(hosts);
    // The unattended auto-accept policy is installed for THIS attempt only and
    // taken straight back off again. The pool is shared and outlives the
    // attempt, so leaving it behind would be a one-way door: every later
    // attempt would find a callback already installed, skip the check above
    // entirely, and keep trusting unknown keys blindly even after
    // setTrustUnknownHostKeys(false). Restoring what was there is also correct
    // for the ordinary case, where AppController's prompting callback is the
    // previous value and must survive untouched.
    const bool installedAutoAccept =
        !m_pool->hostKeyCallback() && m_trustUnknownHostKeys;
    if (!m_pool->hostKeyCallback() && !m_trustUnknownHostKeys) {
        emit error(QStringLiteral(
                       "refusing to connect to %1:%2: no host-key decision "
                       "policy is installed, so an unknown host key could "
                       "not be shown to you for approval")
                       .arg(m_host)
                       .arg(m_port));
        return false;
    }
    if (installedAutoAccept) {
        m_pool->setHostKeyCallback([](const QString&, const QString&,
                                      const QByteArray&, KnownHosts::Verdict) {
            return SshConnectionPool::HostKeyDecision::Accept;
        });
    }
    const auto dropAutoAccept = qScopeGuard([this, installedAutoAccept] {
        if (installedAutoAccept)
            m_pool->setHostKeyCallback({});
    });

    m_lastPoolError.clear();
    if (!connectPool(m_host, m_port, m_user, m_identityFile)) {
        // Same rule probeEndpoint() follows above: a cancellation is the user's
        // own doing, not a fault to report. This one is reachable because the
        // handshake is synchronous and the pool delivers its progress, its
        // host-key question and its credential prompt from inside it, so a
        // handler of any of those can call disconnectSession(). The pool then
        // aborts the handshake between libssh calls and reports it exactly like
        // a failure - a bare false - and the user got a "SSH connection failed"
        // toast for pressing Disconnect.
        if (!m_cancelRequested)
            emit error(withLastPoolError(
                QStringLiteral("SSH connection to %1:%2 failed")
                    .arg(m_host)
                    .arg(m_port)));
        return false;
    }

    if (m_pool->knownHosts().entries().size() != knownBefore) {
        // Reported rather than swallowed. Silently failing to record a key the
        // user has just approved is not cosmetic: the decision is gone, so the
        // very next connect presents the same fingerprint prompt again, for
        // ever, and nothing on screen ever explains why. The session itself is
        // already up, so this is the one error() that does not abort anything.
        const QFileInfo info(m_knownHostsPath);
        QDir().mkpath(info.absolutePath());
        // Serialize the reread, merge and atomic replacement with other
        // CodeHarbor processes. QSaveFile prevents torn writes but cannot
        // prevent two writers from both reading the same old file and the
        // later rename losing the first writer's approval.
        const QString lockPath = m_knownHostsPath + QStringLiteral(".merge-lock");
        QLockFile lock(lockPath);
        lock.setStaleLockTime(30000);
        if (!lock.tryLock(1500)) {
            emit error(QStringLiteral(
                           "connected, but could not lock the trusted-host "
                           "store %1; the approved key was not recorded")
                           .arg(m_knownHostsPath));
        } else {
            const auto unlock = qScopeGuard([&lock] { lock.unlock(); });
            // MERGE, do not overwrite. `hosts` is the store as it stood when
            // this attempt STARTED, and the pool's copy is that snapshot plus
            // whatever this attempt approved. Serialising the snapshot would
            // throw away a key another window recorded while this handshake
            // was running, so reread now and add only this attempt's keys.
            const bool fileExists = QFileInfo::exists(m_knownHostsPath);
            QByteArray currentData;
            bool currentReadable = !fileExists;
            if (fileExists) {
                QFile current(m_knownHostsPath);
                if (current.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    currentData = current.readAll();
                    currentReadable = current.error() == QFileDevice::NoError;
                }
            }
            if (!currentReadable) {
                emit error(QStringLiteral(
                               "connected, but could not read the trusted-host "
                               "store %1; the approved key was not recorded")
                               .arg(m_knownHostsPath));
            } else {
                // A missing file is the only case where an empty merge base is
                // safe. An existing file that cannot be read must never be
                // replaced with a partial store containing only this key.
                KnownHosts merged = KnownHosts::parse(currentData);
                int addedHere = 0;
                for (const KnownHosts::Entry& entry : m_pool->knownHosts().entries()) {
                    // A hashed, wildcard or @marker entry cannot be re-added as
                    // a plain line; the reread already carries whatever the file has.
                    if (!entry.supported)
                        continue;
                    if (hosts.verify(entry.host, entry.keyType, entry.key)
                        != KnownHosts::Verdict::Unknown)
                        continue;
                    merged.add(entry.host, entry.keyType, entry.key);
                    ++addedHere;
                }
                // Leave the file untouched when another writer already recorded
                // every key this attempt approved.
                if (addedHere > 0) {
                    // QSaveFile writes beside the original and renames only on
                    // commit(), so a failed write leaves the previous store intact.
                    QSaveFile out(m_knownHostsPath);
                    const QByteArray serialized = merged.serialize();
                    if (!out.open(QIODevice::WriteOnly)
                        || out.write(serialized) != serialized.size()
                        || !out.commit()) {
                        emit error(QStringLiteral(
                                       "connected, but could not record the host key of "
                                       "%1:%2 in %3 (%4); you will be asked to approve it "
                                       "again on the next connect")
                                       .arg(m_host)
                                       .arg(m_port)
                                       .arg(m_knownHostsPath, out.errorString()));
                    } else {
                        QFile::setPermissions(m_knownHostsPath,
                                              QFile::ReadOwner | QFile::WriteOwner);
                    }
                }
            }
        }
    }

    // A cancel that was raised DURING the handshake and that the pool did not
    // manage to abort itself: it checks between libssh calls, so a disconnect
    // asked for late enough leaves connectToHost() returning true with a live,
    // authenticated session. The canceller has already run its whole teardown -
    // including m_pool->disconnectFromHost() - but it ran BEFORE this session
    // existed, so nothing has dropped it, and the object would sit in
    // State::Disconnected with an SSH session still up on the server.
    //
    // Checked here rather than before the known-hosts write above, so a key the
    // user approved during that same handshake is still recorded: the approval
    // was real and the cancel does not retract it.
    if (m_cancelRequested) {
        const bool wasTearingDown = m_tearingDown;
        m_tearingDown = true;
        m_pool->disconnectFromHost();
        m_tearingDown = wasTearingDown;
        return false;
    }

    // The session is authenticated; before anything is exec'd on it, make sure
    // there is something at the configured location TO exec. Only a failure the
    // user has to act on (no node, no download tool, an install that did not
    // land) stops the attempt here, and ensureRemoteService() has already
    // reported it. See its comment for why a failed INSPECTION does not.
    if (!ensureRemoteService())
        return false;
    // Provisioning can park this attempt in a nested event loop for minutes,
    // which is exactly where a "disconnect" lands. ensureRemoteService() reports
    // that as a plain false only when a step of its own failed; a cancel that
    // arrived between its steps leaves it succeeding. Answering the user's
    // teardown by wiring the very session they abandoned is the one outcome
    // that must not happen, so the flag is the last thing checked before
    // anything is exec'd.
    if (m_cancelRequested)
        return false;

    m_rpcDevice = openChannelDevice(rpcCommand(m_nodePath, m_repoRoot),
                                    QStringLiteral("codeharbord"));
    if (!m_rpcDevice) {
        fail(withLastDiagnostic(
            QStringLiteral("could not start codeharbord over SSH")));
        return false;
    }
    wireChannelSignals(m_rpcDevice, QStringLiteral("codeharbord"));
    if (m_client) {
        // Turn the transport heartbeat on HERE, at the one place in the shipped
        // app that binds a real SSH channel to the client. A wedged
        // `codeharbord` — a stuck handler, a filesystem call that never
        // returns, a half-open channel libssh has not noticed — leaves the
        // channel readable and writable forever, so nothing else in this file
        // would ever notice it: no EOF, no channelError(), no reconnect. The
        // heartbeat turns that silence into the ordinary transport-loss path,
        // which the reconnect ladder below already handles, instead of a
        // viewer pane whose text editor sits on "saving…" for the rest of the
        // session.
        // Idempotent, so re-wiring after a reconnect simply re-arms it.
        m_client->enableHeartbeat();
        m_client->setTransport(m_rpcDevice);
    }

    m_agentDevice = openChannelDevice(bridgeCommand(m_nodePath, m_repoRoot),
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
    // A stateChanged()/wired() handler is allowed to tear this session down
    // again — AppController runs its identity handshake from wired() and a
    // Disconnect can land while these signals are being delivered. Reporting
    // success afterwards would have connectAndWire()'s caller tell the UI the
    // session is up while this object already says Disconnected.
    return m_state == State::Wired;
}

bool SessionBootstrap::connectAndWire(const QString& host, quint16 port,
                                      const QString& user,
                                      const QString& nodePath,
                                      const QString& repoRoot,
                                      const QString& identityFile)
{
    // A connect already in flight owns m_host/m_port and is parked in
    // probeEndpoint()'s nested event loop. Overwriting the target underneath it
    // would wire a session to one host and report it as another, so a second
    // request is refused rather than interleaved.
    if (m_attempting || m_connectRequested) {
        emit error(QStringLiteral("a connection attempt is already in progress"));
        return false;
    }
    // m_attempting alone does not cover this whole function: setState() below
    // delivers stateChanged() to consumers BEFORE attemptWire() raises that
    // flag, and a handler that called back in here would overwrite the target
    // the outer call is one line away from connecting to.
    m_connectRequested = true;
    const auto clearRequest =
        qScopeGuard([this] { m_connectRequested = false; });
    if (m_environmentTrustActive && !m_environmentConnectInProgress) {
        // The environment entry point's unattended trust is scoped to its
        // session. A later attended connect must start from the previous policy.
        m_trustUnknownHostKeys = m_environmentTrustPrevious;
        m_environmentTrustActive = false;
    }

    // Remember the target: every automatic retry replays exactly this call.
    m_host = host;
    m_port = port;
    m_user = user;
    m_nodePath = nodePath;
    m_repoRoot = repoRoot;
    m_identityFile = identityFile;

    cancelReconnect();
    m_attempt = 0;
    // The user asked for this one, so the ladder switch may not abandon it.
    m_attemptIsRetry = false;
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
    const QString identityFile = qEnvironmentVariable("CH_LIVE_IDENTITY");
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

    // This entry point IS the unattended one: it only runs when CH_LIVE_SSH is
    // set, which no ordinary desktop launch does, and it has no user interface to
    // raise a host-key prompt in. Keep the opt-in for automatic retries of this
    // session, but restore the attended policy when the session is torn down.
    const bool tookTrustScope = !m_environmentTrustActive;
    if (tookTrustScope)
        m_environmentTrustPrevious = m_trustUnknownHostKeys;
    m_environmentTrustActive = true;
    m_trustUnknownHostKeys = true;
    m_environmentConnectInProgress = true;
    const bool connected =
        connectAndWire(host, static_cast<quint16>(port), user, nodePath, repoRoot,
                       identityFile);
    m_environmentConnectInProgress = false;
    if (!connected && tookTrustScope && m_environmentTrustActive) {
        m_trustUnknownHostKeys = m_environmentTrustPrevious;
        m_environmentTrustActive = false;
    }
    return connected;
}

} // namespace ch
