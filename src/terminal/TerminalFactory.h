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
    // Per-pane connection lifecycle, keyed by the server-reported Dev Session
    // and terminal_panes row ids. The server id is included so AppController
    // can reject a queued signal from a pane that belonged to a previous
    // server after a profile switch; neither client profile ids nor layout
    // labels are valid substitutes for this identity.
    void terminalStateChanged(const QString& serverId, const QString& devSessionId,
                              const QString& terminalId, ch::TerminalState state);

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
    };

    // The pane's entry, created (with its destroyed() cleanup) on first use.
    // Never returned to a caller and never held across anything that can emit:
    // a single insert rehashes m_attached and turns a held reference into a
    // dangling write.
    Attachment& entryFor(ch::TerminalController* controller);

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
