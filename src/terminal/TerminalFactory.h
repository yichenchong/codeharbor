#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include "TerminalBridge.h"
#include "TerminalController.h"

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
// The tmux target and command come from TerminalController's own helpers, which
// are the shell-injection-hardened ones (SPEC 5.2).
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

    // Open a PTY channel attached to the pane's tmux session and wire it to
    // `controller`. Re-attaching releases the previous channel first. False if
    // there is no connection or the channel/PTY could not be started; the
    // reason is reported through error().
    //
    // `cols`/`rows` at or below 0 mean "the renderer has not reported a size
    // yet": the geometry the controller already recorded is used instead, and
    // only a controller that has never been sized falls back to 80x24. A
    // reconnect therefore never shrinks a pane that is already laid out.
    //
    // CALLER CONTRACT — `terminalId` must be UNIQUE FOR EVER within its Dev
    // Session, not merely unique among the panes alive right now. The pair
    // (devSessionId, terminalId) is the whole tmux target, and the attach
    // command is `tmux new-session -A`, i.e. "attach if it already exists,
    // create otherwise". That is exactly what makes a terminal survive a
    // disconnect, an app restart and a session switch — and it is also why a
    // RECYCLED id silently adopts a stranger: detach() and pane closure both
    // leave the remote session running on purpose, so an id handed out a second
    // time re-attaches the first terminal's shell, scrollback and running
    // processes into what the user asked for as a brand new pane (and
    // `workingDir` is ignored, because -c only applies when the session is
    // created). This class cannot detect that: "reconnect to my terminal" and
    // "give me a new terminal that happens to reuse an id" arrive here as
    // byte-identical calls. Uniqueness is the caller's to guarantee.
    Q_INVOKABLE bool attach(ch::TerminalController* controller,
                            const QString& devSessionId,
                            const QString& terminalId,
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

    SshConnectionPool* m_pool = nullptr;
    // Keyed on the controller; entries are dropped when it is destroyed and
    // outlive detach() on purpose (the tmux target is what kill() needs).
    QHash<QObject*, Attachment> m_attached;
};

} // namespace ch
