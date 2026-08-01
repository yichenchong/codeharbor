#include "TerminalFactory.h"

#include <QTimer>

#include "Ids.h"
#include "SessionState.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"

namespace {

// The pane's tmux target is server-minted and validated there against tmux's
// own grammar, but it still reaches a remote shell as part of a command string,
// so it is interpolated with the same POSIX single-quote rule the hardened
// helpers use (see the identical rule in TerminalController.cpp and
// SessionBootstrap.cpp): wrap in single quotes and rewrite every embedded quote
// as '\'' so nothing can break out of the quoting.
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

// Upper bound on how long the fire-and-forget kill channel may stay alive.
// The channel self-deletes on its own end-of-stream; if the SSH session dies
// mid-kill that signal may never arrive, so this bounds the wait before the
// one-shot object is reclaimed regardless (TM13).
constexpr int kKillWatchdogMs = 30000;

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

void TerminalFactory::rememberTarget(TerminalController* controller, const QString& target)
{
    auto it = m_attached.find(controller);
    if (it == m_attached.end()) {
        it = m_attached.insert(controller, Attachment{});
        // The key must never dangle: entries die with their pane.
        connect(controller, &QObject::destroyed, this,
                [this](QObject* dead) { m_attached.remove(dead); });
    }
    it->target = target;
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

QString TerminalFactory::paneKey(const QString& devSessionId, const QString& paneName)
{
    // `/` cannot appear in a UUID, so no pair of ids can collide on it.
    return devSessionId + QLatin1Char('/') + paneName;
}

void TerminalFactory::setWorkspace(WorkspaceDb* workspace)
{
    m_workspace = workspace;
}

void TerminalFactory::setServerId(const QString& serverId)
{
    if (m_serverId == serverId)
        return;
    m_serverId = serverId;
    // Targets name rows on a SERVER. Carrying them to another one would hand a
    // pane a target belonging to a workspace it is no longer looking at.
    m_targets.clear();
    // Same reasoning for the lookups that are still on the wire: they were
    // asked of the PREVIOUS server, so their answers name rows that mean
    // nothing here and must not be cached (the callback in resolveTarget()
    // drops them for exactly that reason). Failing them now is what keeps the
    // panes waiting on them from waiting for ever: resolveTarget() promises an
    // answer for every call it accepted, and this is the one place that promise
    // could otherwise be broken. Keys are taken from a snapshot because
    // finishResolution() erases from the map, and a waiter is free to react by
    // re-entering resolveTarget() and inserting a fresh entry under the same key.
    const QList<QString> stale = m_resolving.keys();
    for (const QString& key : stale) {
        finishResolution(key, QString(),
                         QStringLiteral("the workspace server changed while this "
                                        "terminal was being resolved"));
    }
}

ResolveTerminalPaneParams TerminalFactory::resolveParamsFor(const QString& serverId,
                                                            const QString& devSessionId,
                                                            const QString& paneName,
                                                            const QString& terminalPaneId,
                                                            const QString& workingDir)
{
    ResolveTerminalPaneParams params;
    params.serverId = ServerId{serverId};
    params.devSessionId = DevSessionId{devSessionId};
    if (!terminalPaneId.isEmpty()) {
        // Pure lookup on the row that IS this terminal. It was minted when the
        // layout leaf was created and a row id is never recycled, so there is
        // nothing to create here and nothing a reused label could confuse it
        // with. The slot label is deliberately NOT sent: the server would have
        // no use for it, and a caller reading this should not be able to
        // believe it is part of the question.
        params.id = terminalPaneId;
        return params;
    }
    // Legacy leaf: no row id was ever stored for it, so its slot label is the
    // historical key and the server does lookup-or-create in ONE call.
    // Deliberately not list-then-create: those are two round trips, and between
    // them another client can insert the very row this one is about to create.
    params.name = paneName;
    // Only meaningful when the row is created: `tmux new-session -c` roots a
    // session at creation and is ignored on attach, so an existing pane keeps
    // the directory it was born with.
    if (!workingDir.isEmpty())
        params.workingDirectory = workingDir;
    return params;
}

bool TerminalFactory::resolveTarget(TerminalController* controller,
                                    const QString& devSessionId,
                                    const QString& paneName,
                                    const QString& terminalPaneId,
                                    const QString& workingDir)
{
    if (!controller)
        return false;
    if (devSessionId.isEmpty() || paneName.isEmpty()) {
        emit error(controller, QStringLiteral("no Dev Session for this pane"));
        return false;
    }
    // A lookup and a create both travel over the same SSH session the PTY would
    // use, so refuse for the same reason attach() does. Inventing a target here
    // is exactly the bug this whole path replaces.
    if (!connected() || !m_workspace || m_serverId.isEmpty()) {
        emit error(controller, QStringLiteral("no SSH connection"));
        return false;
    }

    const ResolveTerminalPaneParams params =
        resolveParamsFor(m_serverId, devSessionId, paneName, terminalPaneId, workingDir);
    // The row id when the leaf has one, the legacy slot address otherwise. Two
    // shapes in one map is safe: a UUID contains no "/", so no row id can ever
    // read as a "<devSession>/<label>" pair.
    const bool byRow = !params.id.isEmpty();
    const QString key = byRow ? params.id : paneKey(devSessionId, paneName);
    // Waiting list first, so the delivery below has somebody to deliver to and
    // a second caller for the same pane joins the flight instead of starting a
    // second one. That is a bandwidth saving, NOT the correctness guarantee: it
    // only covers this process. Two client machines racing the same LEGACY slot
    // are made safe on the server, by workspace.resolveTerminalPane doing its
    // lookup-or-create in one BEGIN IMMEDIATE transaction.
    const bool alreadyInFlight = m_resolving.contains(key);
    m_resolving[key].append(QPointer<TerminalController>(controller));

    const QString cached = m_targets.value(key);
    if (!cached.isEmpty()) {
        // Posted, never emitted inline: the caller (TerminalPaneView) assigns
        // its "resolving" flag from this function's RETURN value, and an answer
        // delivered before that assignment would be overwritten by it.
        QMetaObject::invokeMethod(
            this, [this, key, cached]() { finishResolution(key, cached, QString()); },
            Qt::QueuedConnection);
        return true;
    }
    if (alreadyInFlight)
        return true;

    QPointer<TerminalFactory> self(this);
    const QString askedOf = m_serverId;
    m_workspace->resolveTerminalPane(
        params, [self, key, byRow, askedOf, devSessionId,
                 paneName](std::optional<TerminalPane> pane, std::optional<RpcError> err) {
            if (!self)
                return;
            // The answer names a row on the server the question was asked of.
            // setServerId() may have moved this factory to another one while it
            // was on the wire, in which case caching the target would hand a
            // pane a name from a workspace it is no longer looking at — and
            // setServerId() has already answered (and forgotten) every waiter
            // this key had, so there is nobody left to deliver to either.
            if (self->m_serverId != askedOf)
                return;
            if (err) {
                self->finishResolution(key, QString(), err->message);
                return;
            }
            if (!pane || pane->tmuxTarget.isEmpty()) {
                // The server owns the mint and fills a missing target in the
                // same transaction, so this cannot happen against a codeharbord
                // of this generation. Reported rather than papered over: the
                // alternatives are a pane that silently attaches nothing, or a
                // client that starts minting names of its own again.
                self->finishResolution(
                    key, QString(),
                    QStringLiteral("the server returned this terminal without a tmux target"));
                return;
            }
            self->m_targets.insert(key, pane->tmuxTarget);
            if (!byRow && !pane->id.value.isEmpty()) {
                // Remember the answer under the row id too, so the backfill
                // this is about to trigger does not make the next attach pay
                // for a second round trip under the new key.
                self->m_targets.insert(pane->id.value, pane->tmuxTarget);
            }
            self->finishResolution(key, pane->tmuxTarget, QString());
            // AFTER the waiters have their target: the backfill republishes the
            // layout, and the pane should already be attaching by then.
            if (!byRow && !pane->id.value.isEmpty())
                emit self->paneRowResolved(devSessionId, paneName, pane->id.value);
        });
    return true;
}

void TerminalFactory::finishResolution(const QString& key, const QString& target,
                                       const QString& message)
{
    // Taken out of the map BEFORE anything is emitted: a pane that hears its
    // answer is free to call straight back into resolveTarget() (a retry after
    // a failure does exactly that), and it must start a new flight rather than
    // append to the list being drained.
    const Waiters waiters = m_resolving.take(key);
    for (const QPointer<TerminalController>& waiter : waiters) {
        if (!waiter)
            continue;
        if (target.isEmpty() && !message.isEmpty())
            emit error(waiter, message);
        emit targetResolved(waiter, target);
    }
}

bool TerminalFactory::attach(TerminalController* controller,
                             const QString& tmuxTarget,
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
    if (tmuxTarget.isEmpty()) {
        // The pane has not been resolved against the server yet. Refused rather
        // than defaulted: every locally-plausible default is a name some other
        // pane may already be using.
        emit error(controller, QStringLiteral("no tmux target for this pane"));
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

    // Shell-safe quoting, from the hardened SPEC 5.2 helper. Nothing about the
    // command is assembled here, and nothing about the TARGET is decided here.
    const QString command = TerminalController::tmuxNewSessionCommand(tmuxTarget, workingDir);

    // Record the tmux target BEFORE anything below can fail. targetFor() is
    // what kill() destroys, and it has to name the session THIS pane is now
    // pointing at. Left until after a successful open, a pane that was
    // retargeted at a different terminal (or a different Dev Session) and then
    // failed to open its channel would still answer kill() with the PREVIOUS
    // attach's target — and kill() would destroy a tmux session, processes and
    // all, belonging to a pane the user never touched.
    rememberTarget(controller, tmuxTarget);

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

    // Re-found rather than carried down from rememberTarget(): setState(),
    // setTransport() and startPty() all emit signals that reach QML, and
    // anything there is free to attach or detach another pane on this factory.
    // A single insert can rehash m_attached and turn a held iterator into a
    // dangling write (the same rule kill() follows).
    if (auto it = m_attached.find(controller); it != m_attached.end())
        it->device = device;
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

    // closeChannel() ends the read channel, which walks the controller's state
    // machine, which reaches the WebChannel bridge and the QML pane — and
    // anything there is free to call attach() straight back on THIS pane (the
    // same re-entrancy the m_attached lookups above and in kill() guard
    // against). If it did, the controller is already bound to a brand new
    // channel and the three steps below would unbind it, then report the fresh
    // pane as dropped. The device we came here to release is still released.
    const bool stillOurs = controller->transport() == device;
    device->deleteLater();
    if (!stillOurs)
        return;

    controller->setTransport(nullptr);
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

    if (target.isEmpty())
        return;  // never attached: there is no remote session to destroy
    if (!connected()) {
        // The target is deliberately KEPT. Forgetting it here (which is what
        // this used to do, unconditionally, before the command had even been
        // attempted) stranded the remote tmux session for good: targetFor()
        // went empty, so a later kill() on the same pane became a silent no-op
        // and the user's processes kept running on the server with nothing in
        // the UI able to name them again.
        emit error(controller,
                   QStringLiteral("no SSH connection: the tmux session %1 is still "
                                  "running and was not killed.")
                       .arg(target));
        return;
    }

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
        // The target is kept here too: the command never ran, so the session is
        // still there and Retry has to be able to name it.
        emit error(controller, QStringLiteral("could not kill the tmux session %1").arg(target));
        delete exec;
        return;
    }

    // The command is on its way, so the pane may forget its target: nothing is
    // left to kill twice. Re-found rather than carried across detach() and
    // startExec(), both of which emit signals that reach QML, where anything is
    // free to attach or detach another pane on this factory — and a single
    // insert can rehash m_attached and turn a held iterator into a dangling
    // write.
    if (auto it = m_attached.find(controller); it != m_attached.end())
        it->target.clear();

    // Watchdog: the self-deletion above is driven ONLY by the channel's own
    // readChannelFinished(). If the SSH session dies mid-kill that end-of-stream
    // may never come, and the channel would then live until the factory itself
    // is destroyed (app exit). A single-shot timer PARENTED to the channel
    // deletes it after a bounded wait, so the timer itself cannot leak either.
    // Whichever of {finished, timeout} fires first deletes the channel; because
    // the timer is a child it is destroyed with it, which cancels the other.
    auto* watchdog = new QTimer(exec);
    watchdog->setSingleShot(true);
    connect(watchdog, &QTimer::timeout, exec, &QObject::deleteLater);
    watchdog->start(kKillWatchdogMs);
}

} // namespace ch
