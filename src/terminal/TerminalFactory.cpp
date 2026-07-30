#include "TerminalFactory.h"

#include "Ids.h"
#include "SessionState.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"

namespace {

// The pane's tmux target is app-minted, but it still reaches a remote shell as
// part of a command string, so it is interpolated with the same POSIX
// single-quote rule the hardened helpers use (see the identical rule in
// TerminalController.cpp and SessionBootstrap.cpp): wrap in single quotes and
// rewrite every embedded quote as '\'' so nothing can break out of the quoting.
// The attach command itself is NOT built here — TerminalController's own
// tmuxNewSessionCommand() produces it (SPEC 5.2).
QString shellSingleQuote(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

// A PTY with a zero dimension is not a size, it is a bug waiting to surface as
// an unusable remote window. A pane that has not been laid out yet has no real
// geometry to offer, so fall back to whatever the controller already recorded
// (a reconnect knows the pane size even when the caller does not) and only then
// to the conventional default.
constexpr int kDefaultColumns = 80;
constexpr int kDefaultRows = 24;

} // namespace

namespace ch {

TerminalFactory::TerminalFactory(SshConnectionPool* pool, QObject* parent)
    : QObject(parent), m_pool(pool)
{
}

TerminalController* TerminalFactory::create(QObject* owner)
{
    // Parented to the pane: destroyed with it (no leaked flush timer, buffers
    // or transport connections).
    auto* controller = new TerminalController(owner);

    // The controller bounds the silent part of an attach itself and moves the
    // pane to Error when the window expires (TerminalController::kAttachTimeoutMs).
    // It has no way to say WHY, though: error() is the only message channel the
    // pane's chrome listens to (src/qml/TerminalPaneView.qml onError), so the
    // sentence is minted here. Connected once at creation rather than per
    // attach(), so a pane that re-attaches does not accumulate handlers and
    // report the same stall several times.
    connect(controller, &TerminalController::attachTimedOut, this, [this, controller]() {
        const QString target = targetFor(controller);
        const QString waited = QString::number(controller->attachTimeoutMs() / 1000);
        emit error(controller,
                   target.isEmpty()
                       ? QStringLiteral("The remote terminal sent nothing for %1 s; "
                                        "the tmux attach did not complete. Try Retry.")
                             .arg(waited)
                       : QStringLiteral("The remote terminal sent nothing for %1 s; "
                                        "the tmux session %2 did not finish attaching. "
                                        "Try Retry.")
                             .arg(waited, target));
    });

    return controller;
}

TerminalBridge* TerminalFactory::createBridge(TerminalController* controller, QObject* owner)
{
    if (!controller)
        return nullptr;
    return new TerminalBridge(controller, owner ? owner : controller);
}

bool TerminalFactory::connected() const
{
    return m_pool && m_pool->state() == SshConnectionPool::State::Connected;
}

QString TerminalFactory::targetFor(TerminalController* controller) const
{
    return m_attached.value(controller).target;
}

QString TerminalFactory::tmuxKillSessionCommand(const QString& target)
{
    // Two layers, and BOTH are needed.
    //
    //  1. Shell quoting stops the target breaking out of the command string.
    //
    //  2. tmux's own `=` exact-match sigil stops it hitting the wrong SESSION.
    //     A bare `-t <name>` is a tmux TARGET, and tmux's target grammar falls
    //     back from an exact match to a prefix match and then to fnmatch — so
    //     `-t 'ch_a_t1'` happily kills `ch_a_t10` once `ch_a_t1` is gone, and a
    //     devSessionId of `*` (the ids come from server data) makes
    //     `-t 'ch_*_t1'` destroy whatever session it lands on. Verified against
    //     tmux 3.6: `kill-session -t 'ch_*_t1'` killed `ch_victim_t1`, while
    //     `-t '=ch_*_t1'` refused with "can't find session". The remote
    //     `tmux.*` RPC group already pins its targets this way
    //     (remote/src/tmux.ts killSession); this is the same rule for the
    //     client-side command.
    //
    // The sigil goes INSIDE the quotes: it is tmux syntax, not shell syntax.
    return QStringLiteral("tmux kill-session -t %1")
        .arg(shellSingleQuote(QLatin1Char('=') + target));
}

bool TerminalFactory::attach(TerminalController* controller,
                             const QString& devSessionId,
                             const QString& terminalId,
                             const QString& workingDir,
                             int cols,
                             int rows)
{
    if (!controller)
        return false;
    if (!connected()) {
        emit error(controller, QStringLiteral("no SSH connection"));
        return false;
    }

    // A re-attach must not leave the previous channel behind.
    detach(controller);

    // 0 from the caller means "the renderer has not reported a size yet". It
    // must NOT downgrade a size the pane already established: a reconnect that
    // arrives without geometry would otherwise snap a 200x60 pane back to 80x24.
    const int columns = cols > 0 ? cols
                                 : (controller->columns() > 0 ? controller->columns()
                                                              : kDefaultColumns);
    const int lines = rows > 0 ? rows
                               : (controller->rows() > 0 ? controller->rows() : kDefaultRows);

    const DevSessionId devSession{devSessionId};
    const TerminalId terminal{terminalId};
    // Both come from the hardened SPEC 5.2 helpers: stable ids, shell-safe
    // quoting. Nothing about the command is assembled here.
    const QString target = TerminalController::tmuxTarget(devSession, terminal);
    const QString command =
        TerminalController::tmuxNewSessionCommand(devSession, terminal, workingDir);

    auto* device = new SshChannelDevice(m_pool, SshConnectionPool::ChannelKind::Pty, controller);
    connect(device, &SshChannelDevice::channelError, this,
            [this, controller](const QString& message) { emit error(controller, message); });

    controller->setState(TerminalState::OpeningChannel);
    // Bind BEFORE the PTY runs: tmux redraws the whole pane the moment it
    // attaches, and a controller wired up afterwards would miss that first
    // screenful.
    controller->setTransport(device);

    if (!device->startPty(QStringLiteral("xterm-256color"), columns, lines, command)) {
        controller->setTransport(nullptr);
        delete device;
        controller->setState(TerminalState::Error);
        // No second error() here on purpose. EVERY startPty() failure path in
        // SshChannelDevice emits channelError first (abortStart(), the
        // "channel already started"/"no SSH connection pool"/"could not open SSH
        // channel" refusals, and the built-without-libssh stub), and the
        // connection above has already forwarded that specific reason. A generic
        // "could not open a PTY channel for <target>" fired afterwards would
        // simply overwrite it in the pane's chrome, which shows the LAST message
        // it was given (src/qml/TerminalPaneView.qml onError).
        return false;
    }

    // Record the geometry in the controller too: it is the only thing that
    // still knows the pane size when a reconnect opens a fresh PTY at the
    // channel default (SPEC 5.6). Costs one window-change request at attach.
    controller->resize(columns, lines);
    controller->setState(TerminalState::AttachingTmux);

    // The pane's first bytes are tmux drawing itself: that is what Ready means
    // (SPEC 5.6). Connected after the controller's own transport hookup, so the
    // batch is ingested before the transition is reported.
    connect(device, &QIODevice::readyRead, controller, [controller]() {
        if (controller->state() == TerminalState::OpeningChannel
            || controller->state() == TerminalState::AttachingTmux) {
            controller->setState(TerminalState::Ready);
        }
    });

    auto it = m_attached.find(controller);
    if (it == m_attached.end()) {
        it = m_attached.insert(controller, Attachment{});
        // The key must never dangle: entries die with their pane.
        connect(controller, &QObject::destroyed, this,
                [this](QObject* dead) { m_attached.remove(dead); });
    }
    it->device = device;
    it->target = target;
    return true;
}

void TerminalFactory::detach(TerminalController* controller)
{
    if (!controller)
        return;
    auto it = m_attached.find(controller);
    if (it == m_attached.end())
        return;

    SshChannelDevice* device = it->device.data();
    it->device = nullptr;  // the target stays: kill() still needs it
    if (!device)
        return;

    // Close first so the controller sees the channel end and reports the drop
    // through its own state machine, then unbind and release the device.
    device->disconnect(this);  // no late channelError for a pane we just dropped
    device->closeChannel();
    controller->setTransport(nullptr);
    device->deleteLater();

    if (TerminalController::isLiveState(controller->state()))
        controller->setState(TerminalState::Disconnected);
}

void TerminalFactory::kill(TerminalController* controller)
{
    if (!controller)
        return;
    // Read the target out BEFORE detaching, and re-find the entry afterwards
    // rather than holding the iterator across the call. detach() closes the SSH
    // channel, which drives the controller's state machine, which reaches the
    // WebChannel bridge and the QML pane; anything there is free to attach or
    // detach another pane on this factory, and a single insert into m_attached
    // can rehash it and turn a held iterator into a dangling write.
    const QString target = targetFor(controller);

    detach(controller);

    if (auto it = m_attached.find(controller); it != m_attached.end())
        it->target.clear();  // the session is gone; nothing left to kill twice

    if (target.isEmpty() || !connected())
        return;

    // Out-of-band on its own Exec channel: the pane's own PTY channel is the
    // thing being destroyed, so it cannot carry its own kill.
    //
    // channelError is deliberately NOT forwarded to error() from this channel,
    // unlike the PTY channel in attach(). This command is fire-and-forget: the
    // pane has already been told it was killed, and tmux writing "can't find
    // session" to stderr some milliseconds later (which is the expected outcome
    // when another client got there first) would replace that with an alarming
    // message about a pane that is gone anyway. The refusal below is reported,
    // because that one means the command never ran at all.
    auto* exec = new SshChannelDevice(m_pool, SshConnectionPool::ChannelKind::Exec, this);
    connect(exec, &SshChannelDevice::readChannelFinished, exec, &QObject::deleteLater);
    if (!exec->startExec(tmuxKillSessionCommand(target))) {
        emit error(controller, QStringLiteral("could not kill the tmux session %1").arg(target));
        delete exec;
    }
}

} // namespace ch
