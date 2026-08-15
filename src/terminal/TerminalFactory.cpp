#include "TerminalFactory.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>

#include "AgentStatusMonitor.h"
#include "CodeharbordClient.h"
#include "Ids.h"
#include "RpcTypes.h"
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
// one-shot object is reclaimed regardless.
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
    // or transport connections). With no pane to parent to, the factory itself
    // takes ownership rather than returning a free-floating QObject: this is a
    // Q_INVOKABLE, so an unparented return value would be handed to QML with
    // JavaScriptOwnership and collected at a moment nothing here controls,
    // while a C++ caller that ignored the default would simply leak it. Same
    // rule, and the same shape, as createBridge() below.
    auto* controller = new TerminalController(owner ? owner : this);

    // The controller bounds the silent part of an attach itself and moves the
    // pane to Error when the window expires (TerminalController::kAttachTimeoutMs).
    // It has no way to say WHY, though: error() is the only message channel the
    // pane's chrome listens to (src/qml/TerminalPaneView.qml onError), so the
    // sentence is minted here. Connected once at creation rather than per
    // attach(), so a pane that re-attaches does not accumulate handlers and
    // report the same stall several times.
    connect(controller, &TerminalController::attachTimedOut, this, [this, controller]() {
        const QString target = targetFor(controller);
        const int timeoutMs = controller->attachTimeoutMs();
        const QString waited = timeoutMs >= 1000
            ? QStringLiteral("%1 s").arg(timeoutMs / 1000)
            : QStringLiteral("%1 ms").arg(timeoutMs);
        emit error(controller,
                   target.isEmpty()
                       ? QStringLiteral("The remote terminal sent nothing for %1; "
                                        "the tmux attach did not complete. Try Retry.")
                             .arg(waited)
                       : QStringLiteral("The remote terminal sent nothing for %1; "
                                        "the tmux session %2 did not finish attaching. "
                                        "Try Retry.")
                             .arg(waited, target));
    });
    connect(controller, &TerminalController::stateChanged, this,
            [this, controller](TerminalState state) {
                const auto it = m_attached.constFind(controller);
                if (it == m_attached.constEnd() || it->serverId.isEmpty()
                    || it->devSessionId.isEmpty() || it->terminalId.isEmpty())
                    return;
                emit terminalStateChanged(it->serverId, it->devSessionId,
                                           it->terminalId, state);
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
    const auto it = m_attached.constFind(controller);
    if (it == m_attached.constEnd() || it->targetServerId != m_serverId)
        return {};
    return it->target;
}

TerminalFactory::Attachment& TerminalFactory::entryFor(TerminalController* controller)
{
    auto it = m_attached.find(controller);
    if (it == m_attached.end()) {
        it = m_attached.insert(controller, Attachment{});
        // The key must never dangle: entries die with their pane.
        connect(controller, &QObject::destroyed, this,
                [this](QObject* dead) { m_attached.remove(dead); });
    }
    return it.value();
}

void TerminalFactory::rememberTarget(TerminalController* controller, const QString& target)
{
    Attachment& entry = entryFor(controller);
    if (entry.target != target) {
        // A DIFFERENT session than the one this pane last aimed at, so nothing
        // is expected to be there and creating it is not a loss. This covers
        // both directions that matter: a pane retargeted at another terminal
        // (or another Dev Session), whose new session may legitimately not
        // exist yet, and a pane whose session the user KILLED on purpose —
        // kill() clears the recorded target, so the Connect that follows it
        // starts over here rather than announcing the very session the user
        // just destroyed.
        entry.everAttached = false;
    }
    entry.target = target;
    entry.targetServerId = m_serverId;
}

void TerminalFactory::setAgentMonitor(AgentStatusMonitor* monitor)
{
    m_agentMonitor = monitor;
}

void TerminalFactory::setRpcClient(CodeharbordClient* client)
{
    m_rpc = client;
}

// Deliberately does NOT touch already-attached panes. tmux cannot repair the
// environment of a process that is already running, so the value reaches a pane
// the next time it is attached — which is exactly when a new daemon's path
// becomes relevant, because a reconnect re-attaches every pane.
void TerminalFactory::setControlSocket(const QString& socketPath)
{
    m_controlSocket = socketPath;
}

void TerminalFactory::beginResolution(TerminalController* controller, const QString& key)
{
    entryFor(controller);
    auto it = m_attached.find(controller);
    if (it == m_attached.end() || it->pendingResolveKey == key)
        return;
    it->pendingResolveKey = key;
    // A DIFFERENT key means the pane has been pointed at another terminal. Its
    // old channel may still be open and printing, and those bytes are no longer
    // attributable: not to the terminal the pane has left, and not to the one
    // whose answer has not arrived. Reporting stops until the new answer lands.
    const QString oldServerId = it->serverId;
    const QString oldDevSessionId = it->devSessionId;
    const QString oldTerminalId = it->terminalId;
    if (!oldServerId.isEmpty() && !oldDevSessionId.isEmpty()
        && !oldTerminalId.isEmpty()) {
        emit terminalStateChanged(oldServerId, oldDevSessionId, oldTerminalId,
                                  TerminalState::Unloaded);
    }
    // The signal above is deliberately allowed to re-enter QML. Re-find the
    // entry and only clear the identity if the same request still owns it; a
    // re-entrant retarget may already have installed a newer identity.
    it = m_attached.find(controller);
    if (it == m_attached.end() || it->pendingResolveKey != key
        || it->serverId != oldServerId || it->devSessionId != oldDevSessionId
        || it->terminalId != oldTerminalId) {
        return;
    }
    QObject::disconnect(it->outputConnection);
    it->outputConnection = {};
    it->serverId.clear();
    it->devSessionId.clear();
    it->terminalId.clear();
    it->resolvedTarget.clear();
}

void TerminalFactory::clearAgentIdentity(TerminalController* controller)
{
    auto it = m_attached.find(controller);
    if (it == m_attached.end())
        return;
    const QString oldServerId = it->serverId;
    const QString oldDevSessionId = it->devSessionId;
    const QString oldTerminalId = it->terminalId;
    if (!oldServerId.isEmpty() && !oldDevSessionId.isEmpty()
        && !oldTerminalId.isEmpty()) {
        emit terminalStateChanged(oldServerId, oldDevSessionId, oldTerminalId,
                                  TerminalState::Unloaded);
    }
    it = m_attached.find(controller);
    if (it == m_attached.end() || it->serverId != oldServerId
        || it->devSessionId != oldDevSessionId || it->terminalId != oldTerminalId) {
        return;
    }
    QObject::disconnect(it->outputConnection);
    it->outputConnection = {};
    it->serverId.clear();
    it->devSessionId.clear();
    it->terminalId.clear();
    it->resolvedTarget.clear();
}


void TerminalFactory::bindAgentIdentity(TerminalController* controller,
                                        const QString& devSessionId,
                                        const QString& terminalId,
                                        const QString& harness)
{
    if (!controller || devSessionId.isEmpty() || terminalId.isEmpty())
        return;
    Attachment& entry = entryFor(controller);
    const bool identityChanged = entry.serverId != m_serverId
        || entry.devSessionId != devSessionId || entry.terminalId != terminalId;
    if (identityChanged) {
        // beginResolution() already published Unloaded for an identity it
        // replaced. Keeping this branch free of another signal is deliberate:
        // terminalStateChanged may re-enter QML and invalidate m_attached
        // references while this bind is still installing its new identity.
        // Torn down and re-made rather than re-pointed. Re-pointing alone would
        // be enough for the forwarding below, which re-reads the identity on
        // every batch, but the connection is what carries the identity's
        // lifetime: leaving one behind on a re-bind is how a pane ends up
        // reporting each batch twice.
        QObject::disconnect(entry.outputConnection);
        entry.outputConnection = {};
        entry.serverId = m_serverId;
        entry.devSessionId = devSessionId;
        entry.terminalId = terminalId;
    }
    if (!entry.outputConnection) {
        // The identity is re-read from the entry on every batch rather than
        // captured, so a pane that loses its identity mid-flight (a retarget,
        // a failed re-resolution) stops reporting immediately instead of on the
        // next disconnect.
        entry.outputConnection =
            connect(controller, &TerminalController::outputReceived, this, [this, controller]() {
                if (!m_agentMonitor)
                    return;
                const auto it = m_attached.constFind(controller);
                if (it == m_attached.constEnd() || it->terminalId.isEmpty())
                    return;
                // The FACT of output, never a byte of it.
                m_agentMonitor->noteTerminalOutput(it->devSessionId, it->terminalId);
            });
    }
    // Which harness the pane runs decides whether the monitor derives anything
    // from that output at all, so it is registered here — with the answer that
    // produced the identity, and therefore before the pane can have attached or
    // printed anything under it.
    //
    // ch::AppController's workspace-refresh walk registers harnesses too. That
    // is NOT a duplicate of this and neither one can be dropped: this is the
    // only registration a pane resolved since the last refresh gets, and that
    // walk is the only one panes the user has never opened get — and the only
    // thing that re-registers anything after its own retainDevSessions()
    // eviction. setTerminalHarness is idempotent, so both running costs nothing.
    if (m_agentMonitor)
        m_agentMonitor->setTerminalHarness(devSessionId, terminalId, harness);
    if (identityChanged) {
        const auto it = m_attached.constFind(controller);
        if (it != m_attached.constEnd() && !it->serverId.isEmpty()) {
            emit terminalStateChanged(it->serverId, it->devSessionId, it->terminalId,
                                      controller->state());
        }
    }
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
    // Controllers can outlive the QML pane that is being replaced during a
    // profile switch. Remove their old server identity before changing the
    // factory's current id, so late stateChanged signals cannot be relabelled
    // as belonging to the new server.
    const QList<QObject*> controllers = m_attached.keys();
    for (QObject* object : controllers)
        clearAgentIdentity(static_cast<TerminalController*>(object));
    m_serverId = serverId;
    // A remembered answer names rows on a SERVER — the tmux target AND the
    // `terminal_panes` row id the pane reports its agent state under. Carrying
    // either to another server would hand a pane something belonging to a
    // workspace it is no longer looking at: the target attaches it to (or
    // creates) a foreign shell, and the row id makes it report its output as
    // some other workspace's terminal. Both go together, because they are two
    // halves of one answer.
    m_resolved.clear();
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
        params.id = TerminalId{terminalPaneId};
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
    QPointer<TerminalController> pane(controller);
    if (!pane)
        return false;
    if (devSessionId.isEmpty()
        || (terminalPaneId.isEmpty() && paneName.isEmpty())) {
        emit error(pane.data(), QStringLiteral("no Dev Session for this pane"));
        return false;
    }
    // A lookup and a create both travel over the same SSH session the PTY would
    // use, so refuse for the same reason attach() does. Inventing a target here
    // is exactly the bug this whole path replaces.
    if (!connected() || !m_workspace || m_serverId.isEmpty()) {
        emit error(pane.data(), QStringLiteral("no SSH connection"));
        return false;
    }

    const ResolveTerminalPaneParams params =
        resolveParamsFor(m_serverId, devSessionId, paneName, terminalPaneId, workingDir);
    // The row id when the leaf has one, the legacy slot address otherwise. Two
    // shapes in one map is safe: a UUID contains no "/", so no row id can ever
    // read as a "<devSession>/<label>" pair.
    const bool byRow = !params.id.value.isEmpty();
    const QString key = byRow ? params.id.value : paneKey(devSessionId, paneName);
    // Record which resolution this pane is now waiting on. NOTHING about its
    // agent-status identity is decided here: the pane's identity comes from
    // the server's ANSWER, and it is adopted in finishResolution() only if
    // this is still the resolution the pane is waiting on when that answer
    // arrives.
    // Binding at request start instead is how a pane retargeted mid-lookup ends
    // up reporting its old channel's bytes under the new pane's row id.
    beginResolution(pane.data(), key);
    if (!pane)
        return false;
    // Waiting list first, so the delivery below has somebody to deliver to and
    // a second caller for the same pane joins it instead of creating a second
    // row or receiving a duplicate targetResolved() signal. Waiting on the same
    // key from another pane is also one server request, but each controller is
    // listed only once.
    auto resolving = m_resolving.find(key);
    if (resolving != m_resolving.end()) {
        for (const QPointer<TerminalController>& waiter : resolving.value()) {
            if (waiter.data() == pane.data())
                return true;
        }
        resolving.value().append(QPointer<TerminalController>(pane.data()));
        return true;
    }
    m_resolving.insert(key, Waiters{QPointer<TerminalController>(pane.data())});

    const ResolvedPane cached = m_resolved.value(key);
    if (!cached.target.isEmpty()) {
        // Posted, never emitted inline: the caller (TerminalPaneView) assigns
        // its "resolving" flag from this function's RETURN value, and an answer
        // delivered before that assignment would be overwritten by it.
        const QString target = cached.target;
        QMetaObject::invokeMethod(
            this,
            [this, key, target, byRow, devSessionId, paneName]() {
                finishResolution(key, target, QString());
                // The row is re-reported from the CACHE too, not only from the
                // live round trip below, and that is what makes the backfill
                // retryable instead of one-shot.
                //
                // The report exists so the layout LEAF ends up holding the row
                // id; this factory holding it is not the point. A report that
                // did not land — the pane was closed while the answer
                // travelled, the layout was mid-load, the write was refused —
                // used to be the only one there would ever be, because every
                // later resolution of the same legacy label is answered from
                // here and said nothing about the row at all. The leaf then went
                // on naming its terminal by the recyclable slot label for the
                // rest of the process: the unsafe key this whole path exists to
                // retire. Re-reporting costs one signal and cannot double-write,
                // because ch::SessionLayouts::bindTerminalPaneRow() returns
                // immediately when the leaf already carries this id.
                //
                // Guarded exactly as the live path is: a pane addressed BY its
                // row id already knows the answer, so it has nothing to backfill.
                //
                // Re-read rather than captured, because setServerId() can drop
                // the whole cache while this call is queued — and a row id from
                // the previous server must not be written into anything.
                const ResolvedPane answer = m_resolved.value(key);
                if (!byRow && !answer.terminalId.isEmpty())
                    emit paneRowResolved(devSessionId, paneName, answer.terminalId);
            },
            Qt::QueuedConnection);
        return true;
    }

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
            if (!TerminalController::isSafeTmuxTarget(pane->tmuxTarget)) {
                // The answer is data, and a target is a piece of tmux GRAMMAR:
                // one beginning `$`, `@` or `%` names an ID rather than a
                // session, and the `=` exact-match prefix every call site uses
                // does not suppress that. codeharbord validates what it mints
                // (isSafeTmuxTarget in remote/src/tmux.ts) and repairs stored
                // rows in the schema v3 migration, so this covers what that
                // cannot: a row written by an older daemon, and a server that
                // is not the one we think we are talking to. Failing the
                // resolution leaves the pane with no identity and no attach
                // authorisation, which is the right outcome — nothing is
                // silently retargeted at somebody else's session.
                self->finishResolution(
                    key, QString(),
                    QStringLiteral("the server returned an unusable tmux target (%1) for "
                                   "this terminal")
                        .arg(pane->tmuxTarget));
                return;
            }
            ResolvedPane answer;
            answer.target = pane->tmuxTarget;
            answer.terminalId = pane->id.value;
            // The row's owner as the SERVER states it, not the devSessionId the
            // question carried. They agree today — the lookup is scoped to a
            // Dev Session — but the answer is the authority on which Dev
            // Session the row it returned belongs to, and the identity a pane
            // reports its agent state under must come from the same place its
            // row id does. An answer missing either half yields no identity at
            // all (bindAgentIdentity refuses it), which is the right failure:
            // the pane still attaches, it just reports nothing.
            answer.devSessionId = pane->devSessionId.value;
            answer.harness = pane->harness;
            self->m_resolved.insert(key, answer);
            if (!byRow && !answer.terminalId.isEmpty()) {
                // Remember the answer under the row id too, so the backfill
                // this is about to trigger does not make the next attach pay
                // for a second round trip under the new key.
                self->m_resolved.insert(answer.terminalId, answer);
            }
            self->finishResolution(key, answer.target, QString());
            // AFTER the waiters have their target: the backfill republishes the
            // layout, and the pane should already be attaching by then.
            if (!byRow && !answer.terminalId.isEmpty())
                emit self->paneRowResolved(devSessionId, paneName, answer.terminalId);
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
    // The answer this key produced, which is also where the waiting panes'
    // agent-status identity comes from. Empty on a failure, and on the very
    // first delivery of a legacy lookup that the server answered without a row.
    const ResolvedPane answer = m_resolved.value(key);
    for (const QPointer<TerminalController>& waiter : waiters) {
        if (!waiter)
            continue;
        // Identity is adopted HERE and nowhere else, and only for a pane whose
        // LATEST request is the one being answered. A pane retargeted while
        // this lookup was in flight is waiting on a different key by now, and
        // this answer describes the terminal it has left; giving it that
        // identity would report its bytes as the previous pane's. The pane also
        // still appears in this waiting list, which is why the check is on the
        // pane's own record rather than on the list.
        const auto it = m_attached.constFind(waiter.data());
        const bool stillWaitingOnThis =
            it != m_attached.constEnd() && it->pendingResolveKey == key;
        if (stillWaitingOnThis) {
            // A failed resolution leaves NO identity or attach authorization
            // behind: the pane is not entitled to report under the one its
            // previous answer gave it, and it has not been given a new one.
            if (target.isEmpty()) {
                clearAgentIdentity(waiter);
            } else {
                // Store provenance before bindAgentIdentity(), whose signals
                // may re-enter the factory and invalidate hash iterators.
                if (auto current = m_attached.find(waiter.data());
                    current != m_attached.end())
                    current->resolvedTarget = target;
                bindAgentIdentity(waiter, answer.devSessionId, answer.terminalId,
                                  answer.harness);
            }
        }
        if (target.isEmpty() && !message.isEmpty())
            emit error(waiter, message);
        // Re-checked once, after everything above that can reach QML:
        // bindAgentIdentity() and error() both emit, and a pane is free to
        // close itself in response.
        if (!waiter)
            continue;
        emit targetResolved(waiter, target);
    }
}

void TerminalFactory::noteHarnessChanged(const QString& terminalPaneId,
                                         const QString& harness)
{
    if (terminalPaneId.isEmpty())
        return;
    // Nothing below runs unless the value actually MOVED. The workspace refresh
    // calls this for every pane every time it runs — which is after every
    // mutation — and the work below re-announces the pane's attach. An attach
    // is what starts SPEC 6.6's clock over at "attached and silent", so doing
    // it on an unchanged pane would drag a Running pane back to Starting a few
    // times a minute and the sidebar would never settle. A no-op has to be a
    // no-op.
    bool changed = false;
    // An answer is remembered under its row id and, for a pane that was
    // addressed by its legacy slot label, under that label as well. Both copies
    // have to move together: whichever key the next resolution uses is the one
    // that will hand a rebind its harness.
    for (auto it = m_resolved.begin(); it != m_resolved.end(); ++it) {
        if (it->terminalId != terminalPaneId || it->harness == harness)
            continue;
        it->harness = harness;
        changed = true;
    }
    if (!changed || !m_agentMonitor)
        return;
    // Panes attached RIGHT NOW have already been registered under the old
    // value, and no rebind is coming for them until something else disturbs
    // them, so without this the change would not take effect until the next
    // reconnect. Re-stating the registration fixes that; re-stating the ATTACH
    // with it is what actually starts the clock, because a pane that has just
    // become generic has no attach on record — it was not generic when its
    // channel came up, so that attach was ignored, and the monitor treats
    // output from a pane it has not seen attach as belonging to nothing it is
    // watching. Only a pane holding a live channel qualifies, which is the same
    // condition the real attach path reports on.
    // Snapshotted before either call, for the reason finishResolution() gives:
    // both of these emit, anything they reach is free to attach or close a pane
    // on this factory, and a single insert rehashes m_attached and turns a held
    // iterator into a dangling read.
    struct LivePane {
        QString devSessionId;
        QString terminalId;
        bool attached;
    };
    QVector<LivePane> live;
    for (auto it = m_attached.constBegin(); it != m_attached.constEnd(); ++it) {
        if (it->terminalId == terminalPaneId && !it->devSessionId.isEmpty())
            live.push_back({it->devSessionId, it->terminalId, it->device != nullptr});
    }
    for (const LivePane& pane : live) {
        if (!m_agentMonitor)
            return;
        m_agentMonitor->setTerminalHarness(pane.devSessionId, pane.terminalId, harness);
        if (!pane.attached || !m_agentMonitor)
            continue;
        m_agentMonitor->noteTerminalAttached(pane.devSessionId, pane.terminalId);
    }
}

bool TerminalFactory::attach(TerminalController* controller,
                             const QString& tmuxTarget,
                             const QString& workingDir,
                             int cols,
                             int rows)
{
    QPointer<TerminalController> pane(controller);
    if (!pane)
        return false;
    if (!connected()) {
        emit error(pane.data(), QStringLiteral("no SSH connection"));
        return false;
    }
    if (tmuxTarget.isEmpty()) {
        // The pane has not been resolved against the server yet. Refused rather
        // than defaulted: every locally-plausible default is a name some other
        // pane may already be using.
        emit error(pane.data(), QStringLiteral("no tmux target for this pane"));
        return false;
    }
    if (!TerminalController::isSafeTmuxTarget(tmuxTarget)) {
        // The name is about to be used in TWO tmux positions that the shell
        // quoting below does nothing for: `new-session -s <target>`, where the
        // target is UNSHIELDED, and `set-option -t '=<target>:'`, where the `=`
        // exact-match prefix is not the complete shield it looks like — tmux
        // resolves an ID sigil before it looks a name up, so a target beginning
        // `$` selects the session holding THAT ID rather than the session with
        // that name (verified on tmux 3.6). Refused here, unconditionally,
        // rather than made safe: every name codeharbord mints passes, so the
        // only values this rejects are ones no pane should be attaching to.
        emit error(pane.data(),
                   QStringLiteral("the tmux target %1 is not a usable session name")
                       .arg(tmuxTarget));
        return false;
    }
    if (m_workspace) {
        const auto it = m_attached.constFind(pane.data());
        if (it == m_attached.constEnd() || it->resolvedTarget != tmuxTarget) {
            emit error(pane.data(), QStringLiteral("tmux target was not resolved by the server"));
            return false;
        }
    }

    // A re-attach must not leave the previous channel behind.
    detach(pane.data());
    if (!pane)
        return false;

    // 0 from the caller means "the renderer has not reported a size yet". It
    // must NOT downgrade a size the pane already established: a reconnect that
    // arrives without geometry would otherwise snap a 200x60 pane back to 80x24.
    const int columns = cols > 0
        ? qMin(cols, TerminalBridge::kMaxDimension)
        : qMin(pane->columns() > 0 ? pane->columns() : kDefaultColumns,
               TerminalBridge::kMaxDimension);
    const int lines = rows > 0
        ? qMin(rows, TerminalBridge::kMaxDimension)
        : qMin(pane->rows() > 0 ? pane->rows() : kDefaultRows,
               TerminalBridge::kMaxDimension);

    // Shell-safe quoting, from the hardened SPEC 5.2 helper. Nothing about the
    // command is assembled here, and nothing about the TARGET is decided here.
    //
    // The pane's identity comes out of the attachment record — the ids the
    // SERVER reported for this pane — and is handed to the builder explicitly so
    // it can export them into the tmux session for the agent hooks (SPEC 6.4).
    // A pane that has not been resolved yet (tests without a workspace) has no
    // recorded identity, and empty ids export nothing rather than a guess.
    QString paneDevSessionId;
    QString paneTerminalId;
    if (const auto it = m_attached.constFind(pane.data()); it != m_attached.constEnd()) {
        paneDevSessionId = it->devSessionId;
        paneTerminalId = it->terminalId;
    }
    const QString command = TerminalController::tmuxNewSessionCommand(
        tmuxTarget, workingDir, paneDevSessionId, paneTerminalId, m_controlSocket);

    // Record the tmux target BEFORE anything below can fail. targetFor() is
    // what kill() destroys, and it has to name the session THIS pane is now
    // pointing at. Left until after a successful open, a pane that was
    // retargeted at a different terminal (or a different Dev Session) and then
    // failed to open its channel would still answer kill() with the PREVIOUS
    // attach's target — and kill() would destroy a tmux session, processes and
    // all, belonging to a pane the user never touched.
    rememberTarget(pane, tmuxTarget);

    auto* device = new SshChannelDevice(m_pool, pane);
    QPointer<SshChannelDevice> deviceGuard(device);
    connect(device, &SshChannelDevice::channelError, this,
            [this, pane](const QString& message) {
                if (pane)
                    emit error(pane, message);
            });

    pane->setState(TerminalState::OpeningChannel);
    if (!pane)
        return false;
    // Bind BEFORE the PTY runs: tmux redraws the whole pane the moment it
    // attaches, and a controller wired up afterwards would miss that first
    // screenful. The Ready transition is also connected before startPty(), so
    // a synchronous first readyRead cannot leave the pane stuck attaching.
    pane->setTransport(device);
    if (!pane || !deviceGuard || pane->transport() != device)
        return false;
    connect(device, &QIODevice::readyRead, pane.data(), [pane]() {
        if (pane && (pane->state() == TerminalState::OpeningChannel
                     || pane->state() == TerminalState::AttachingTmux))
            pane->setState(TerminalState::Ready);
    });

    // The moment the attach is issued, in whole seconds, for the recreated-
    // session check below. Taken BEFORE the command goes out so nothing can be
    // created in between and read as pre-existing.
    const qint64 attachedAtSec = QDateTime::currentSecsSinceEpoch();
    const bool started = openPty(device, columns, lines, command);
    if (!started) {
        const bool stillOurs = pane && pane->transport() == device;
        if (stillOurs)
            pane->setTransport(nullptr);
        if (deviceGuard)
            delete device;
        if (pane && !pane->transport())
            pane->setState(TerminalState::Error);
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
    if (!pane || !deviceGuard || pane->transport() != device)
        return false;

    // Record the geometry in the controller too: it is the only thing that
    // still knows the pane size when a reconnect opens a fresh PTY at the
    // channel default (SPEC 5.6). Costs one window-change request at attach.
    pane->resize(columns, lines);
    if (!pane || pane->transport() != device)
        return false;
    // If startPty() already delivered the first bytes, the early readyRead
    // handler has promoted the pane. Do not regress that state back to
    // AttachingTmux.
    if (pane->state() == TerminalState::OpeningChannel)
        pane->setState(TerminalState::AttachingTmux);

    // Re-found rather than carried down from rememberTarget(): setState(),
    // setTransport() and startPty() all emit signals that reach QML, and
    // anything there is free to attach or detach another pane on this factory.
    // A single insert can rehash m_attached and turn a held iterator into a
    // dangling write (the same rule kill() follows).
    if (auto it = m_attached.find(pane.data()); it != m_attached.end())
        it->device = device;

    // The pane now has a live channel, so SPEC 6.6 observation starts here: for
    // a generic-harness pane this is "attached and silent", and any output age
    // remembered from the channel this attach replaced is discarded with it.
    // Read back out of the entry rather than carried down, for the same reason
    // the device is written back above.
    if (m_agentMonitor) {
        const auto it = m_attached.constFind(pane.data());
        if (it != m_attached.constEnd() && !it->terminalId.isEmpty())
            m_agentMonitor->noteTerminalAttached(it->devSessionId, it->terminalId);
    }

    // Did this attach silently CREATE the pane's session instead of finding it?
    // `tmux new-session -A` does both and says which it did nowhere, so a pane
    // whose remote session died comes back as a brand new empty shell with the
    // user's work gone and nothing on screen to say so. Asked out of band, after
    // the attach, exactly like the kill exec: the pane is already up and working,
    // and a diagnostic must never delay or gate that.
    //
    // Only a RE-attach may report. A pane attaching for the first time in this
    // process, and a genuinely new pane, both create their session legitimately
    // and have lost nothing.
    if (auto it = m_attached.find(pane.data()); it != m_attached.end()) {
        const bool reattach = it->everAttached;
        it->everAttached = true;
        if (reattach)
            probeForRecreatedSession(pane.data(), tmuxTarget, attachedAtSec);
    }
    return true;
}

bool TerminalFactory::openPty(SshChannelDevice* device, int cols, int rows,
                              const QString& command)
{
    return device->startPty(QStringLiteral("xterm-256color"), cols, rows, command);
}

void TerminalFactory::probeForRecreatedSession(TerminalController* controller,
                                               const QString& target,
                                               qint64 attachedAtSec)
{
    if (!m_rpc || target.isEmpty())
        return;
    QPointer<TerminalFactory> self(this);
    QPointer<TerminalController> pane(controller);
    const QString askedOf = m_serverId;
    // tmux.listSessions takes no parameters (remote/src/tmux.ts) and answers
    // with the whole listing; an empty one is a NORMAL host (no tmux, or no
    // server running) rather than a failure, which is the other reason nothing
    // here is ever reported as an error.
    m_rpc->call(QString::fromLatin1(rpc::kMethodListSessions), QJsonObject{},
                [self, pane, target, attachedAtSec, askedOf](QJsonValue result,
                                                            std::optional<RpcError> error) {
                    if (!self || !pane || error)
                        return;
                    // The listing describes the host the question was asked of; a
                    // profile switch since then makes it an answer about somebody
                    // else's tmux server.
                    if (self->m_serverId != askedOf)
                        return;
                    // The pane must still be on this very session. A kill or a
                    // retarget while the question travelled would turn the notice
                    // into a report about a terminal the user has already left.
                    const auto it = self->m_attached.constFind(pane.data());
                    if (it == self->m_attached.constEnd() || it->target != target)
                        return;
                    // Wire shape: an array of TmuxSession objects carrying `name`
                    // and `created`, tmux's session_created as a UNIX timestamp in
                    // SECONDS (remote/src/rpc-types.ts). No entry for this target
                    // reports nothing: the session may have died again since, and a
                    // diagnostic that cannot see the session cannot say anything
                    // about it.
                    const QJsonArray sessions = result.toArray();
                    for (const QJsonValue& entry : sessions) {
                        const QJsonObject session = entry.toObject();
                        if (session.value(QStringLiteral("name")).toString() != target)
                            continue;
                        // SECOND granularity is all tmux publishes. A session that
                        // was already there but happened to be created in the very
                        // same second as our attach therefore reads as newly
                        // created, and that is accepted rather than papered over:
                        // the only alternative is a strictly-later comparison,
                        // which misses every real loss whose replacement landed in
                        // the same second as the attach that made it. A missing or
                        // unreadable field lands below the floor and says nothing.
                        if (session.value(QStringLiteral("created")).toInteger(-1)
                            >= attachedAtSec) {
                            emit self->sessionRecreated(pane.data(), target);
                        }
                        return;
                    }
                });
}

void TerminalFactory::detach(TerminalController* controller)
{
    QPointer<TerminalController> pane(controller);
    if (!pane)
        return;
    auto it = m_attached.find(pane.data());
    if (it == m_attached.end())
        return;

    QPointer<SshChannelDevice> device = it->device;
    it->device = nullptr;  // the target stays: kill() still needs it
    if (!device)
        return;

    // Close first so the controller sees the channel end and reports the drop,
    // then unbind and release the device.
    device->disconnect(this);  // no late channelError for a pane we just dropped
    device->closeChannel();

    // closeChannel() ends the read channel, which walks the controller's state
    // machine, which reaches the WebChannel bridge and the QML pane — and
    // anything there is free to call attach() straight back on THIS pane. If
    // it did, the controller is already bound to a brand new channel and the
    // steps below must not unbind or report that fresh pane as dropped.
    const bool stillOurs = pane && device && pane->transport() == device;
    if (device)
        device->deleteLater();
    if (!stillOurs || !pane)
        return;

    pane->setTransport(nullptr);
    // setTransport() can itself re-enter QML, so do not report a new channel
    // as disconnected after a replacement was installed.
    if (!pane || pane->transport())
        return;
    if (TerminalController::isLiveState(pane->state()))
        pane->setState(TerminalState::Disconnected);

    // SPEC 6.6 observation ENDS here: the client no longer has a channel to see
    // this pane's output on, so ch::AgentStatusMonitor must stop treating it as
    // locally observable. Left un-reported, a generic pane keeps `attached` set
    // forever, the fallback clock strands it at Idle two seconds after the user
    // switches Dev Session, and the sidebar reports a working session as done.
    //
    // Re-found rather than carried down from the iterator taken at the top of
    // this function: closeChannel() and setTransport() each walk the
    // controller's state machine into the WebChannel bridge and QML, and
    // anything there is free to attach or detach a pane on this factory, which
    // rehashes m_attached. The identity is COPIED out before the call for the
    // same reason — noteTerminalDetached() emits, and a slot may rehash the
    // hash the references would point into.
    //
    // Skipped outright if something above re-attached this pane: that pane has
    // a live channel and its own noteTerminalAttached() already recorded, and
    // reporting a detach for it would blind the monitor to a pane the user is
    // watching. Guarded BEFORE each dereference, like the attach path.
    if (!pane || pane->transport() || !m_agentMonitor)
        return;
    const auto entry = m_attached.constFind(pane.data());
    if (entry == m_attached.constEnd() || entry->terminalId.isEmpty())
        return;
    const QString devSessionId = entry->devSessionId;
    const QString terminalId = entry->terminalId;
    m_agentMonitor->noteTerminalDetached(devSessionId, terminalId);
}

void TerminalFactory::kill(TerminalController* controller)
{
    QPointer<TerminalController> pane(controller);
    if (!pane)
        return;
    // Read the target out BEFORE detaching, and re-find the entry afterwards
    // rather than holding the iterator across the call. detach() closes the
    // SSH channel, which drives the controller's state machine, which reaches
    // the WebChannel bridge and the QML pane; anything there is free to attach
    // or detach another pane on this factory.
    const QString target = targetFor(pane.data());

    detach(pane.data());

    if (target.isEmpty())
        return;  // never attached: there is no remote session to destroy
    if (!TerminalController::isSafeTmuxTarget(target)) {
        // Unreachable through attach(), which refuses such a target before it
        // is ever recorded — and kept anyway, because this is the call that
        // DESTROYS a session and `kill-session -t '=<target>'` is exactly the
        // command tmux's ID sigils defeat: a stored `$0` would kill whichever
        // session happens to hold that id, processes and all. The target is not
        // cleared, so nothing is silently forgotten either.
        emit error(pane.data(),
                   QStringLiteral("the tmux target %1 is not a usable session name; "
                                  "nothing was killed")
                       .arg(target));
        return;
    }
    if (!connected()) {
        // The target is deliberately KEPT. Forgetting it here (which is what
        // this used to do, unconditionally, before the command had even been
        // attempted) stranded the remote tmux session for good: targetFor()
        // went empty, so a later kill() on the same pane became a silent no-op
        // and the user's processes kept running on the server with nothing in
        // the UI able to name them again.
        if (pane)
            emit error(pane.data(),
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
    auto* exec = new SshChannelDevice(m_pool, this);
    QPointer<SshChannelDevice> execGuard(exec);
    connect(exec, &SshChannelDevice::readChannelFinished, exec, &QObject::deleteLater);
    const bool started = exec->startExec(tmuxKillSessionCommand(target));
    if (!started) {
        // The target is kept here too: the command never ran, so the session is
        // still there and Retry has to be able to name it.
        if (pane)
            emit error(pane.data(),
                       QStringLiteral("could not kill the tmux session %1").arg(target));
        if (execGuard)
            delete exec;
        return;
    }

    // The command is on its way, so the pane may forget its target: nothing is
    // left to kill twice. Re-find rather than carried across detach() and
    // startExec(), both of which emit signals that reach QML, where anything is
    // free to attach another pane on this factory. Only clear the entry if it
    // still names the target this invocation actually sent; a re-entrant
    // attach() may have installed a different, live target in the meantime.
    if (auto it = m_attached.find(pane.data()); it != m_attached.end()
        && it->target == target && it->targetServerId == m_serverId) {
        it->target.clear();
        it->targetServerId.clear();
        it->resolvedTarget.clear();
    }

    // Watchdog: the self-deletion above is driven ONLY by the channel's own
    // readChannelFinished(). If the SSH session dies mid-kill that end-of-stream
    // may never come, and the channel would then live until the factory itself
    // is destroyed (app exit). A single-shot timer PARENTED to the channel
    // deletes it after a bounded wait, so the timer itself cannot leak either.
    // Whichever of {finished, timeout} fires first deletes the channel; because
    // the timer is a child it is destroyed with it, which cancels the other.
    if (!execGuard)
        return;
    auto* watchdog = new QTimer(exec);
    watchdog->setSingleShot(true);
    connect(watchdog, &QTimer::timeout, exec, &QObject::deleteLater);
    watchdog->start(kKillWatchdogMs);
}

} // namespace ch
