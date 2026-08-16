#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include "TerminalBridge.h"
#include "TerminalController.h"
#include "WorkspaceDb.h"

namespace ch {

class AgentStatusMonitor;
class CodeharbordClient;
class SshChannelDevice;
class SshConnectionPool;

// Per-pane TerminalController factory and PTY attach service (SPEC 5.1-5.3),
// mirroring ch::EditorFactory. Exposed to QML as the `terminalFactory` context
// property.
//
// Each terminal pane owns its OWN controller: a shared one would splice two
// panes' input, output and geometry into a single stream. create() therefore
// parents the controller to the requesting pane so it dies with the pane, and
// createBridge() gives that controller the WebChannel face the xterm.js page
// talks to.
//
// attach() is the production version of what tst_liveterminal does by hand:
// open a Pty channel on the shared SSH session, run the pane's attach-or-create
// tmux command in it, and bind the channel to the controller as its transport.
// The command comes from TerminalController::tmuxNewSessionCommand(), which is
// the shell-injection-hardened one (SPEC 5.2); the TARGET it names comes from
// resolveTarget() below, i.e. from the server.
//
// It deliberately does NOT subscribe to SshConnectionPool::stateChanged to push
// panes to Disconnected when the session goes down, because the channel already
// does it, per pane and earlier: SshConnectionPool::closeSession() emits
// sessionClosing() BEFORE it frees the session's channels, every
// ch::SshChannelDevice closes on that signal, closing emits
// readChannelFinished(), and TerminalController::onTransportFinished() turns
// that into Disconnected for any pane that was live (SPEC 5.6). A session that
// dies at the network level with no local disconnect reaches the same place from
// the other end: the device's read pump sees the error/EOF and reports the same
// end-of-stream. A second, pool-level path would only race the first one, and it
// could not do better than per-pane: a pane whose channel ended for its own
// reasons must not be re-reported when the session later goes down.
class TerminalFactory : public QObject {
    Q_OBJECT
public:
    explicit TerminalFactory(SshConnectionPool* pool, QObject* parent = nullptr);

    // Create a controller owned by `owner` (the QML pane Item), so its buffers
    // and timers are released when the pane is destroyed. With no owner the
    // factory adopts it, so the returned object is never unparented.
    Q_INVOKABLE ch::TerminalController* create(QObject* owner = nullptr);

    // WebChannel face for `controller`, owned by `owner` (the pane) as well.
    Q_INVOKABLE ch::TerminalBridge* createBridge(ch::TerminalController* controller,
                                                 QObject* owner = nullptr);

    // True when the shared SSH session can carry a new channel; false makes
    // attach() a no-op, so a pane can say "no server" instead of hanging.
    //
    // Virtual for ONE reason: it is the only environmental gate on
    // resolveTarget(), and ch::SshConnectionPool cannot be brought to its
    // Connected state without a real handshake against a real server. Every
    // rule about which answer a pane may adopt as its identity therefore lived
    // behind a door no unit test could open. A test subclass answers true and
    // exercises the real resolution code; nothing in production overrides it.
    Q_INVOKABLE virtual bool connected() const;

    // Resolve the SERVER-MINTED tmux target this pane's layout leaf owns, and
    // report it through targetResolved().
    //
    // This is where a terminal's identity comes from, and the ONLY place the
    // client learns it. The pane IS a row in the server's `terminal_panes`
    // table, and codeharbord mints that row's `tmuxTarget` from its UUID id, so
    // the binding holds across app restarts and across two client machines
    // sharing a Dev Session.
    //
    // TWO addressing modes, and which one is used is the caller's decision
    // because only the caller can see the layout leaf:
    //
    //   * `terminalPaneId` non-empty — the row id the leaf carries
    //     (SplitNode::terminalPaneId). A pure lookup, and the normal case. A
    //     row id is minted once and never recycled, so this cannot land on
    //     another pane's shell.
    //
    //   * `terminalPaneId` EMPTY — resolve by `paneName`, the layout slot
    //     label, which is lookup-or-create. ONLY legitimate for a leaf stored
    //     before layouts carried a row id, where the label is genuinely the
    //     historical key. Slot labels are minted per Dev Session PER CLIENT and
    //     are recycled, so a caller that takes this path for a NEW pane can
    //     hand it a shell an earlier pane left running (closing a pane leaves
    //     the shell alive on purpose). paneRowResolved() reports the row this
    //     found so the leaf can be backfilled and never ask by label again.
    //
    // Returns false when no resolution was started — no controller, no Dev
    // Session, no pane name for a legacy label lookup, or no server to ask —
    // A lookup and a create both need the server, so this refuses in exactly
    // the cases attach() does rather than inventing a target locally.
    //
    // True means targetResolved() WILL be emitted for this controller, always
    // asynchronously (a cached answer is posted, not delivered inline) so a
    // caller can finish assigning its own state before the answer arrives.
    // Idempotent per pane: a second call while one is in flight joins it rather
    // than creating a second row, and an answer once known is remembered for
    // the rest of the process — including across a reconnect, so re-attaching
    // never duplicates a row.
    Q_INVOKABLE bool resolveTarget(ch::TerminalController* controller,
                                   const QString& devSessionId,
                                   const QString& paneName,
                                   const QString& terminalPaneId,
                                   const QString& workingDir);

    // Cache key for one LEGACY layout slot: "<devSessionId>/<layout pane id>".
    // Public as a test seam: the remembered-target cache is keyed by it, and a
    // caller that cannot build the key cannot check what was remembered. A pane
    // resolved by row id is remembered under the bare row id instead, which can
    // never collide with this shape — a UUID contains no "/".
    static QString paneKey(const QString& devSessionId, const QString& paneName);

    // The lookup the pane's identity produces: by `terminalPaneId` when the
    // layout leaf carries one (a pure lookup on the row that IS this terminal),
    // by `paneName` when it does not (the legacy slot label, lookup-or-create).
    // `workingDir` rides along only on the legacy path, because only that path
    // can create a row and `tmux new-session -c` applies only at creation.
    //
    // Static and public because THIS is the fix — which of the two keys a pane
    // is addressed by decides whether a brand new pane can end up on a closed
    // pane's shell — and it deserves a test that does not need an SSH session
    // to run. Same reasoning as tmuxKillSessionCommand() below.
    static ResolveTerminalPaneParams resolveParamsFor(const QString& serverId,
                                                      const QString& devSessionId,
                                                      const QString& paneName,
                                                      const QString& terminalPaneId,
                                                      const QString& workingDir);

    // The workspace repository resolveTarget() reads and writes rows through,
    // and the server whose rows they are. Both are injected (main.cpp) rather
    // than constructed here: the repository is the application's, and the
    // server id is only known once server.info has answered. Ownership stays
    // with the caller. Changing the server id forgets every remembered answer —
    // rows belong to a server, so BOTH halves of a remembered answer, the tmux
    // target and the `terminal_panes` row id that is the pane's agent-status
    // identity, are meaningless on another one.
    void setWorkspace(WorkspaceDb* workspace);
    void setServerId(const QString& serverId);

    // The RPC peer the out-of-band `tmux.listSessions` diagnostic is asked
    // over; injected, not owned, and nullptr simply turns that diagnostic off.
    // It is deliberately NOT the workspace repository this class already holds:
    // ch::WorkspaceDb is the `workspace.*` CRUD surface and nothing else, while
    // `tmux.*` is its own RPC group about the host's tmux server.
    void setRpcClient(CodeharbordClient* client);

    // The viewer control socket THIS window's daemon reported through
    // server.info (SPEC 6.8), exported into every pane this factory attaches so
    // an agent there drives the window that owns it.
    //
    // Set on every wire, because it belongs to ONE daemon: a reconnect is a new
    // daemon with a new path, and a pane that kept the old one would point at a
    // socket nothing is serving. Empty when the server reported none, which
    // unsets it in the pane rather than leaving a stale value.
    void setControlSocket(const QString& socketPath);
    QString controlSocket() const { return m_controlSocket; }


    // A pane's harness changed on the server, so the remembered answer for it
    // is out of date.
    //
    // The resolution cache deliberately survives a disconnect (see m_resolved),
    // and a remembered answer carries the harness it was resolved with. Without
    // this, changing a pane's harness and then reconnecting would re-bind the
    // OLD value from the cache and silently undo the change — the pane would go
    // back to reporting (or not reporting) exactly as it did before, with
    // nothing on screen to say why. Cheap and idempotent: a pane whose harness
    // has not changed, and a pane that has never been resolved, both cost a
    // lookup and no write.
    void noteHarnessChanged(const QString& terminalPaneId, const QString& harness);

    // Feed SPEC 6.6 activity detection for the adapterless "generic" harness.
    // Not owned; nullptr disables the reporting entirely, which is what every
    // test that does not care about agent state gets.
    //
    // This class is where the two halves meet, and nowhere else does: it is the
    // only place that knows a controller's `terminal_panes` row id (from
    // resolveTarget) AND owns the PTY channel its bytes arrive on. So it is
    // what tells ch::AgentStatusMonitor that a pane attached and that a pane
    // produced output — the two observations a generic harness offers, since it
    // publishes no lifecycle events for an adapter to map. Only the FACT of
    // output crosses over, never a byte of it.
    void setAgentMonitor(AgentStatusMonitor* monitor);

    // Open a PTY channel attached to the tmux session `tmuxTarget` and wire it
    // to `controller`. Re-attaching releases the previous channel first. False
    // if there is no connection, no target, or the channel/PTY could not be
    // started; the reason is reported through error().
    //
    // `cols`/`rows` at or below 0 mean "the renderer has not reported a size
    // yet": the geometry the controller already recorded is used instead, and
    // only a controller that has never been sized falls back to 80x24. A
    // reconnect therefore never shrinks a pane that is already laid out.
    //
    // CALLER CONTRACT — in production `tmuxTarget` must be the one
    // resolveTarget() answered with for this pane, i.e. a
    // `terminal_panes.tmuxTarget` value. attach() enforces that provenance
    // whenever a workspace is configured; a factory without one is only the
    // lower-level PTY test seam used by the live attach gate.
    // The attach command is `tmux new-session -A`, "attach if it already exists, create
    // otherwise", which is exactly what makes a terminal survive a disconnect,
    // an app restart and a session switch — and it is also why a target used by
    // a second pane silently adopts the first pane's shell, scrollback and
    // running processes (and `workingDir` is ignored, because -c only applies
    // when the session is created). This class cannot detect that: "reconnect
    // to my terminal" and "give me a new terminal that happens to reuse a
    // target" arrive here as byte-identical calls. Uniqueness is guaranteed
    // upstream instead — one server-side minting site, plus UNIQUE
    // (tmux_target) on the table (schema v3).
    Q_INVOKABLE bool attach(ch::TerminalController* controller,
                            const QString& tmuxTarget,
                            const QString& workingDir,
                            int cols,
                            int rows);

    // Release the pane's channel without touching the remote tmux session, so
    // a later attach() (or another client) finds the work still running.
    Q_INVOKABLE void detach(ch::TerminalController* controller);

    // Detach and destroy the remote tmux session for this pane: the pane's
    // processes go away with it. The recorded target is only forgotten once the
    // kill command has actually been handed to the server; a kill that could
    // not run (no connection, or the exec channel was refused) reports through
    // error() and leaves targetFor() intact, so the pane can try again instead
    // of stranding a running session nothing can name any more.
    Q_INVOKABLE void kill(ch::TerminalController* controller);

    // tmux target for `controller`: the one its LAST attach() aimed at, whether
    // or not that attach succeeded, and empty before the first one (or after
    // a kill that ran). A target is only returned while the factory is still
    // looking at the server that supplied it; it cannot be used to kill a
    // same-named session on a different server after a profile switch.
    Q_INVOKABLE QString targetFor(ch::TerminalController* controller) const;

    // The command kill() runs on its own Exec channel:
    // tmux kill-session -t '=<target>'. Two independent hardenings, both
    // load-bearing: the POSIX single-quote rule keeps an id carrying shell
    // metacharacters from escaping into the remote shell (SPEC 5.2, the same
    // rule as TerminalController's attach command), and tmux's `=` exact-match
    // sigil keeps a glob- or prefix-shaped id from resolving to somebody else's
    // session. Static and public because both guarantees deserve their own test.
    //
    // Neither is a substitute for the THIRD one, which is not in this function:
    // `=` pins an exact NAME match but does not stop tmux reading an ID sigil
    // first, so a target beginning `$` still selects the session holding that
    // id. kill() therefore refuses a target that fails
    // ch::TerminalController::isSafeTmuxTarget() before it gets here.
    static QString tmuxKillSessionCommand(const QString& target);

signals:
    // Attach failures and mid-session channel diagnostics, tagged with the pane
    // they belong to so one pane's failure is not reported on all of them.
    void error(ch::TerminalController* controller, const QString& message);

    // The answer to a resolveTarget() call, tagged with the pane it is for.
    // An EMPTY target means the resolution failed; error() carried the reason
    // just before this, and the pane is expected to drop out of "resolving"
    // rather than wait forever for an answer that is not coming.
    void targetResolved(ch::TerminalController* controller, const QString& target);

    // A LEGACY, label-addressed resolution found (or created) a row: this Dev
    // Session's slot `paneName` is the `terminal_panes` row `terminalPaneId`.
    // Emitted only on that path and only on success, because it exists for one
    // job — letting ch::SessionLayouts write the id into the layout leaf and
    // persist it, so the leaf never has to ask by label again. A pane resolved
    // by row id already knows the answer and emits nothing.
    //
    // Emitted EVERY time such a resolution succeeds, including the ones this
    // factory answers from its own cache without a round trip. It is the LEAF
    // that has to end up holding the id, and this factory cannot see whether it
    // does; reporting once and never again meant a report that failed to land
    // for any reason at all was the only one there would ever be, and the leaf
    // kept naming its terminal by the recyclable slot label for the rest of the
    // process. Repeats are free: bindTerminalPaneRow() returns immediately when
    // the leaf already carries this id.
    void paneRowResolved(const QString& devSessionId, const QString& paneName,
                         const QString& terminalPaneId);

    // This pane's remote tmux session was GONE, and the attach that just
    // succeeded created a new, empty one in its place instead of re-attaching
    // the old one — so whatever was running in it has stopped. `tmuxTarget` is
    // the session name involved.
    //
    // Emitted ONLY on POSITIVE EVIDENCE of both halves of that sentence, and
    // both halves come from tmux itself (see probeForRecreatedSession and
    // Attachment::confirmedCreatedSec): a session with this name was observed
    // alive earlier, and the session with this name that the host reports NOW
    // was created later than the one that was observed. A pane that never had a
    // session — a brand new pane, or one whose earlier attach opened a channel
    // but never got a tmux session out of it — reports nothing, because nothing
    // it can observe says any work was lost.
    void sessionRecreated(ch::TerminalController* controller, const QString& tmuxTarget);
    // Per-pane connection lifecycle, keyed by the server-reported Dev Session
    // and terminal_panes row ids. The server id is included so AppController
    // can reject a queued signal from a pane that belonged to a previous
    // server after a profile switch; neither client profile ids nor layout
    // labels are valid substitutes for this identity.
    void terminalStateChanged(const QString& serverId, const QString& devSessionId,
                              const QString& terminalId, ch::TerminalState state);

protected:
    // Open the pane's PTY channel with the tmux attach command. Called only by
    // attach(), and the ONLY reason it exists as a virtual is the same one
    // connected() gives: a real channel needs libssh and a real server, so
    // everything attach() does AFTER a successful open — including the
    // recreated-session diagnostic, which is the whole point — lived
    // behind a door no unit test could open. A test subclass answers true and
    // exercises the real code; nothing in production overrides it.
    virtual bool openPty(SshChannelDevice* device, int cols, int rows,
                         const QString& command);

private:
    struct Attachment {
        // Weak: the device is parented to the controller, so a pane destroyed
        // out from under us takes it along.
        QPointer<SshChannelDevice> device;
        QString target;
        // The server whose tmux namespace contains `target`; a profile switch
        // must not let kill() send the old name to the new server.
        QString targetServerId;
        // The latest tmux target returned by resolveTarget() for this pane.
        // Production attach() accepts only this server-supplied value; the
        // target remains authorised across detach/reconnect.
        QString resolvedTarget;
        // The pane's agent-status identity: the Dev Session it belongs to and
        // its `terminal_panes` row id, i.e. exactly the pair every AgentEvent
        // is keyed by. BOTH are read off the server's answer and never off the
        // request, and both are empty whenever this pane is not entitled to
        // report output under any identity — before its first answer, while a
        // retarget is in flight, and after a resolution failed.
        QString devSessionId;
        // The server whose row ids are held above. It is captured at bind time:
        // m_serverId can move to a different profile while a retiring
        // controller is still emitting its final Disconnected transition.
        QString serverId;
        QString terminalId;
        // The resolution key of this pane's MOST RECENT resolveTarget() call.
        // An answer is adopted only if it carries this key. QML can retarget a
        // pane while a lookup is in flight, and the superseded answer describes
        // the terminal the pane has just left; adopting it is what made a
        // retargeted pane attach to the previous Dev Session's shell.
        QString pendingResolveKey;
        // The TerminalController::outputReceived -> AgentStatusMonitor
        // forwarding for this pane. Held rather than fired and forgotten so an
        // identity change can tear it down and re-make it: a second connection
        // left behind would report every batch twice, and one that outlived its
        // identity would report bytes under a pane id this controller no longer
        // owns.
        QMetaObject::Connection outputConnection;
        // tmux's own `session_created` (UNIX seconds) for `target`, as last
        // OBSERVED in a `tmux.listSessions` answer, or -1 when this pane has
        // never had a session confirmed to exist under that name.
        //
        // This is the whole basis of sessionRecreated(), and it is deliberately
        // an observation rather than an inference. The predecessor was a bool
        // meaning "an attach() has already succeeded for this pane", i.e. "a PTY
        // channel came up once" — which is NOT evidence that a tmux session ever
        // existed. `tmux new-session -A` can fail to create one for reasons the
        // channel open knows nothing about (no tmux on the host, an unusable
        // working directory, or the transport dying between the exec request and
        // the command actually running, which is exactly what a dropped network
        // does), and the pane would then announce that the user's work had been
        // destroyed the first time an attach really did create the session —
        // about a session that had never existed and work that was never lost.
        //
        // Comparing session_created values rather than racing tmux's clock
        // against ours is also what makes the verdict exact: a session whose
        // creation time is unchanged IS the session we saw before, whatever
        // either clock says, so neither clock skew between client and host nor
        // tmux's one-second granularity can turn an ordinary reconnect into a
        // false report of lost work.
        //
        // Held per SESSION, not merely per pane: rememberTarget() clears it
        // whenever the pane aims at a different target, because a retargeted
        // pane and a pane whose session the user killed both have nothing they
        // expect to find, and creating a session for them is not a loss.
        //
        // KNOWN LIMITATION, and it is not fixable from here: this memory lives
        // in the process. A session lost while the application was CLOSED is
        // therefore not reported on the next launch — the client has no record
        // that the pane ever had a session, so its first attach of the new
        // process has nothing to compare against. Reporting on that would mean
        // guessing, which is the failure this field exists to end.
        qint64 confirmedCreatedSec = -1;
    };

    // The pane's entry, created (with its destroyed() cleanup) on first use.
    // Never returned to a caller and never held across anything that can emit:
    // a single insert rehashes m_attached and turns a held reference into a
    // dangling write.
    Attachment& entryFor(ch::TerminalController* controller);

    // Ask the host, out of band, what it knows about `target` right now, record
    // the session_created it reports, and report sessionRecreated() when that
    // value proves the session this pane had is gone: a creation time STRICTLY
    // LATER than one previously confirmed for the same name is a different
    // session wearing it, so the one the pane was using ended and everything
    // running in it stopped.
    //
    // Asked on EVERY attach, not only a re-attach, because the answer is also
    // the only way the first attach can record what it found: without that
    // observation a later report would be an inference, which is what this
    // replaced. An attach that CREATED the session on a pane with nothing
    // confirmed therefore stays silent and merely remembers.
    //
    // Diagnostic only, and silent in every failure: no `tmux.listSessions`
    // answer, an RPC error, or no entry for this target reports nothing and
    // emits no error(). A failed diagnostic must never become a user-facing
    // fault on a pane that is working perfectly well. A listing with no entry
    // for this target also leaves the confirmation ALONE rather than clearing
    // it: the pane's evidence that a session once existed is still good, and
    // discarding it would silence the very report that matters if the session
    // comes back under the same name.
    void probeForRecreatedSession(ch::TerminalController* controller,
                                  const QString& target);

    // Create-or-update the pane's entry with the tmux target it is aiming at.
    void rememberTarget(ch::TerminalController* controller, const QString& target);

    // Note that this controller is now waiting on resolution `key`, and drop
    // the agent-status identity it was carrying if that is a DIFFERENT key from
    // the one it was waiting on. Dropping it at the retarget rather than at the
    // next delivery matters: the previous PTY channel is still attached and
    // still emitting, and those bytes belong to neither the terminal the pane
    // has left nor the one it has not been given yet.
    void beginResolution(ch::TerminalController* controller, const QString& key);

    // Give the pane the identity the server's answer named, and wire the
    // output reporting for it. `harness` is the row's `harness` column, which
    // is what decides whether the monitor derives state from output at all
    // (only the adapterless "generic" harness does). Idempotent, and safe with
    // no monitor set.
    void bindAgentIdentity(ch::TerminalController* controller,
                           const QString& devSessionId, const QString& terminalId,
                           const QString& harness);

    // Forget the pane's identity and tear down its output reporting, so nothing
    // it prints from here on is attributed to anybody.
    void clearAgentIdentity(ch::TerminalController* controller);

    // One pane's outstanding resolveTarget() calls: every controller waiting on
    // the same resolution key (a row id, or a legacy "<devSession>/<label>").
    // Weak, because a pane can be closed while its lookup is in flight.
    using Waiters = QList<QPointer<TerminalController>>;

    // Hand `target` (empty = failed) to everything waiting on `key` and forget
    // the key's in-flight state. `message` is reported through error() first,
    // and only on failure. This is the SINGLE delivery point — a fresh answer,
    // a cached one, and a server change all arrive here — and therefore also
    // the single place a pane's agent-status identity is adopted.
    void finishResolution(const QString& key, const QString& target, const QString& message);

    SshConnectionPool* m_pool = nullptr;
    // Keyed on the controller; entries are dropped when it is destroyed and
    // outlive detach() on purpose (the tmux target is what kill() needs).
    QHash<QObject*, Attachment> m_attached;
    // Workspace repository and server id for resolveTarget(); injected, not
    // owned. QPointer is not available (WorkspaceDb is not a QObject), so the
    // contract is the one WorkspaceDb itself documents: the repository must
    // outlive this factory, which in main.cpp it does — it belongs to the
    // AppController declared above it.
    WorkspaceDb* m_workspace = nullptr;
    QString m_serverId;
    // The current daemon's viewer control socket; empty when it reported none.
    QString m_controlSocket;
    // RPC peer for the `tmux.listSessions` diagnostic; injected, not owned, and
    // a QPointer because it IS a QObject and nothing here has to be told when
    // it goes away — a null one simply means no diagnostic.
    QPointer<CodeharbordClient> m_rpc;

    // One resolution's answer, as the server gave it. Everything here names a
    // row on ONE server, so the whole map is dropped by setServerId(): a target
    // read from one workspace is meaningless in another, and so is a row id.
    struct ResolvedPane {
        QString target;        // terminal_panes.tmuxTarget
        QString terminalId;    // terminal_panes.id
        QString devSessionId;  // the row's owner, per the server, not per the request
        QString harness;       // terminal_panes.harness; empty when the row has none
    };
    // Resolution key -> that answer: the `terminal_panes` row id for a pane
    // addressed by id, or paneKey() for one addressed by its legacy slot label.
    // A label-addressed answer is remembered under BOTH, so the backfill that
    // follows it costs no second round trip. Kept across a DISCONNECT on
    // purpose — the rows are the server's and do not change while we are away,
    // and re-reading them is exactly the round trip a reconnect should not pay.
    QHash<QString, ResolvedPane> m_resolved;
    // SPEC 6.6 activity reporting sink; not owned, may be null.
    QPointer<AgentStatusMonitor> m_agentMonitor;
    // Keys with a lookup or create in flight, and who is waiting on each.
    QHash<QString, Waiters> m_resolving;
};

} // namespace ch
