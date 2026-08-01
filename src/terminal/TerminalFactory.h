#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include "TerminalBridge.h"
#include "TerminalController.h"
#include "WorkspaceDb.h"

namespace ch {

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
    // and timers are released when the pane is destroyed.
    Q_INVOKABLE ch::TerminalController* create(QObject* owner = nullptr);

    // WebChannel face for `controller`, owned by `owner` (the pane) as well.
    Q_INVOKABLE ch::TerminalBridge* createBridge(ch::TerminalController* controller,
                                                 QObject* owner = nullptr);

    // True when the shared SSH session can carry a new channel; false makes
    // attach() a no-op, so a pane can say "no server" instead of hanging.
    Q_INVOKABLE bool connected() const;

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
    // Returns false when no resolution was started — no controller, no pane
    // name, or no server to ask — having reported the reason through error().
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
    // with the caller. Changing the server id forgets every remembered target —
    // rows belong to a server, and a target read from one is meaningless on
    // another.
    void setWorkspace(WorkspaceDb* workspace);
    void setServerId(const QString& serverId);

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
    // CALLER CONTRACT — `tmuxTarget` must be the one resolveTarget() answered
    // with for this pane, i.e. a `terminal_panes.tmuxTarget` value. The attach
    // command is `tmux new-session -A`, "attach if it already exists, create
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
    // or not that attach succeeded, and empty before the first one (or after a
    // kill that ran). Survives detach() so kill() still knows what to destroy.
    Q_INVOKABLE QString targetFor(ch::TerminalController* controller) const;

    // The command kill() runs on its own Exec channel:
    // tmux kill-session -t '=<target>'. Two independent hardenings, both
    // load-bearing: the POSIX single-quote rule keeps an id carrying shell
    // metacharacters from escaping into the remote shell (SPEC 5.2, the same
    // rule as TerminalController's attach command), and tmux's `=` exact-match
    // sigil keeps a glob- or prefix-shaped id from resolving to somebody else's
    // session. Static and public because both guarantees deserve their own test.
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
    void paneRowResolved(const QString& devSessionId, const QString& paneName,
                         const QString& terminalPaneId);

private:
    struct Attachment {
        // Weak: the device is parented to the controller, so a pane destroyed
        // out from under us takes it along.
        QPointer<SshChannelDevice> device;
        QString target;
    };

    // Create-or-update the pane's entry with the tmux target it is aiming at,
    // wiring the destroyed() cleanup the first time. Never returns or keeps an
    // iterator: callers re-find the entry after any call that can emit.
    void rememberTarget(ch::TerminalController* controller, const QString& target);

    // One pane's outstanding resolveTarget() calls: every controller waiting on
    // the same resolution key (a row id, or a legacy "<devSession>/<label>").
    // Weak, because a pane can be closed while its lookup is in flight.
    using Waiters = QList<QPointer<TerminalController>>;

    // Hand `target` (empty = failed) to everything waiting on `key` and forget
    // the key's in-flight state. `message` is reported through error() first,
    // and only on failure.
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
    // Resolution key -> server-minted tmux target: the `terminal_panes` row id
    // for a pane addressed by id, or paneKey() for one addressed by its legacy
    // slot label. A label-addressed answer is remembered under BOTH, so the
    // backfill that follows it costs no second round trip. Kept across a
    // disconnect on purpose: the rows are the server's and do not change while
    // we are away, and re-reading them is exactly the round trip a reconnect
    // should not pay.
    QHash<QString, QString> m_targets;
    // Keys with a lookup or create in flight, and who is waiting on each.
    QHash<QString, Waiters> m_resolving;
};

} // namespace ch
