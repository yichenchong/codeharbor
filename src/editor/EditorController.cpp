#include "EditorController.h"

#include "CodeharbordClient.h"
#include "RpcTypes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

#include <utility>

namespace ch {

namespace {

// JSON-RPC param object helpers keyed to the frozen RpcTypes shapes.
QJsonObject readParams(const QString& path)
{
    return QJsonObject{{QStringLiteral("path"), path}};
}

// file.readFile params. ALWAYS a bounded byte window: see
// EditorController::kMaxEditableReadBytes for why an unbounded read is both a
// memory hazard on the server and unable to report that it hit a limit.
QJsonObject readFileParams(const QString& path)
{
    return QJsonObject{
        {QStringLiteral("path"), path},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("length"), EditorController::kMaxEditableReadBytes}};
}

QJsonObject writeParams(const QString& path, const QString& content,
                        const QString& expectedRevision,
                        std::optional<int> mode = std::nullopt)
{
    QJsonObject params{{QStringLiteral("path"), path},
                       {QStringLiteral("content"), content},
                       {QStringLiteral("expectedRevision"), expectedRevision}};
    // C1: an optional POSIX file mode. remote/src/files.ts leaves the finished
    // file at exactly this mode when present (used for the 0600 recovery
    // snapshot); omitted for ordinary saves, which keep the file's own mode.
    if (mode.has_value())
        params.insert(QStringLiteral("mode"), *mode);
    return params;
}

QJsonObject unwatchParams(const QString& subscriptionId)
{
    return QJsonObject{{QStringLiteral("subscriptionId"), subscriptionId}};
}

// Restrictive mode for a recovery snapshot (SPEC 11.3): read/write for the
// owner only. Passed through writeFile's C1 `mode` param so the snapshot never
// inherits a permissive umask default.
constexpr int kRecoveryFileMode = 0600;

// A recovery snapshot is a small JSON envelope, not the raw buffer, so it can
// record WHICH file it holds. There is one physical snapshot file per pane
// (recoveryPath()), reused as the pane switches files, and this recorded path
// is what lets checkRecovery() refuse to offer a snapshot belonging to a file
// the pane has since left.
QString serializeRecovery(const QString& path, const QString& content)
{
    const QJsonObject obj{{QStringLiteral("path"), path},
                          {QStringLiteral("content"), content}};
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// Parse a snapshot envelope. Returns false (leaving the outputs untouched) for
// an empty tombstone, a truncated/prefix read, or any non-envelope bytes — each
// of which means "nothing this pane can offer".
bool parseRecovery(const QString& raw, QString& outPath, QString& outContent)
{
    if (raw.isEmpty())
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isObject())
        return false;
    const QJsonObject obj = doc.object();
    const QJsonValue path = obj.value(QStringLiteral("path"));
    const QJsonValue content = obj.value(QStringLiteral("content"));
    if (!path.isString() || !content.isString())
        return false;
    outPath = path.toString();
    outContent = content.toString();
    return true;
}

// POSIX write bits (owner|group|other) of the file.stat result's `mode` field,
// which remote/src/files.ts mints from fs.lstat(). Used ONLY as a fallback for
// a server too old to report the authoritative `writable` flag (see below).
constexpr int kWriteBits = 0222;

// Does the server's stat PROVE the open file cannot be written?
//
// The authoritative signal is the file.stat result's `writable` flag (C2,
// remote/src/rpc-types.ts): the server computes it with fs.access(resolvedPath,
// W_OK) on the LINK-FOLLOWED target, so it answers the exact question a save
// asks — can THIS process write the bytes readFile/writeFile actually touch —
// and it is true iff writable, false on any error. When present it is trusted
// directly, which also closes the old symlink hole: fs.access follows the link,
// so a file opened through a symlink now reports the target's real writability
// instead of the link's perpetual 0777.
//
// A directory (or a socket/fifo) is never a writable text buffer regardless.
//
// FALLBACK — a server that predates the `writable` flag reports only `mode`,
// which comes from lstat and carries no uid/gid (and the client does not know
// the remote euid), so a SET write bit proves nothing. The one thing mode alone
// proves is the negative: with NO write bit set anywhere, no user but root can
// write the file. Read-only is claimed on that proof only, and every ambiguous
// case stays editable so the SPEC 8.6 write path can report the real errno.
// That fallback is also symlink-blind — a symlink's own mode is always 0777 on
// Linux — so against an old server a file opened through a link can be typed
// into but fails to save with the server's EACCES via saveError. Deliberately
// kept for that older peer only; a current server never reaches it.
bool statSaysUnwritable(const QJsonObject& stat)
{
    const QString kind = stat.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("directory") || kind == QLatin1String("other"))
        return true;
    // Authoritative when present (C2): fs.access(W_OK) on the link-followed
    // target. Absent only from a server too old to report it.
    const QJsonValue writable = stat.value(QStringLiteral("writable"));
    if (writable.isBool())
        return !writable.toBool();
    const QJsonValue mode = stat.value(QStringLiteral("mode"));
    if (!mode.isDouble())
        return false;  // neither field present: unknown, not the same as unwritable
    return (mode.toInt() & kWriteBits) == 0;
}

} // namespace

EditorController::EditorController(CodeharbordClient* client, QString recoveryId,
                                  QObject* parent)
    : QObject(parent), m_client(client), m_recoveryId(std::move(recoveryId))
{
    if (m_client) {
        connect(m_client, &CodeharbordClient::notificationReceived, this,
                &EditorController::onNotification);
        // SPEC 5.6: the session can be dropped and re-wired underneath us. Both
        // edges matter — the close tells us the subscription we hold died with
        // its process, the bind tells us there is a new process to subscribe on.
        connect(m_client, &CodeharbordClient::transportClosed, this,
                &EditorController::onTransportClosed);
        connect(m_client, &CodeharbordClient::transportBound, this,
                &EditorController::onTransportBound);
    }
}

EditorController::~EditorController()
{
    // Release the server-side watcher subscribed in open() (SPEC 8.7). The
    // client is borrowed and may already be gone (m_client is a QPointer, and
    // unwatchCurrent() checks it), in which case there is nothing to release:
    // the subscription died with the process that minted it. The response is
    // irrelevant either way — we are gone — so the callback is an empty no-op.
    unwatchCurrent();
}

void EditorController::unwatchCurrent()
{
    if (!m_client || m_watchSubscriptionId.isEmpty())
        return;
    m_client->call(QString::fromLatin1(rpc::kMethodUnwatch),
                   unwatchParams(m_watchSubscriptionId),
                   [](QJsonValue, std::optional<RpcError>) {});
    m_watchSubscriptionId.clear();
}

void EditorController::subscribeWatch()
{
    if (!m_client || m_path.isEmpty())
        return;
    // A subscribe is already outstanding for this generation; a second one
    // would create a duplicate watcher and double every watchEvent.
    if (m_watchPending)
        return;
    // An id we still hold means no EOF was seen on the way here, so the server
    // that minted it may well be the one we are about to ask. Release it first:
    // that is the only way a rebind can never leave two live watchers on the
    // same path (SPEC 8.7). After a real reconnect the id was already dropped
    // by onTransportClosed(), so this costs nothing on that path.
    unwatchCurrent();

    const quint64 generation = ++m_watchGeneration;
    const QString path = m_path;
    m_watchPending = true;

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodWatch), readParams(path),
        [self, client = m_client, path, generation](
            QJsonValue watchRes, std::optional<RpcError> watchErr) {
            const QString subId =
                watchErr.has_value()
                    ? QString()
                    : watchRes.toObject()
                          .value(QStringLiteral("subscriptionId"))
                          .toString();

            // Superseded: the controller was destroyed (pane closed), the file
            // was switched, or the transport was replaced while this was in
            // flight. The server may have created a subscription nobody tracks,
            // so release it through the BORROWED client — captured separately
            // because `self` may be gone — and NEVER touch m_watchPending,
            // which now belongs to a newer attempt. The capture is a QPointer:
            // if the client itself has been destroyed there is nobody left to
            // tell, and the subscription died with its process anyway.
            if (!self || generation != self->m_watchGeneration
                || self->m_path != path) {
                if (!subId.isEmpty() && client)
                    client->call(QString::fromLatin1(rpc::kMethodUnwatch),
                                 unwatchParams(subId),
                                 [](QJsonValue, std::optional<RpcError>) {});
                return;
            }

            self->m_watchPending = false;
            if (subId.isEmpty())
                return;  // no subscription was created
            self->m_watchSubscriptionId = subId;
        });
}

void EditorController::onTransportClosed()
{
    // That codeharbord is unreachable for good, and its subscription registry
    // died with the process (remote/src/files.ts). Forget the id in silence: an
    // unwatch issued later would be addressed to the REPLACEMENT server, which
    // never created it. The buffer, its revision and its dirty flag are the
    // user's and are deliberately left alone.
    m_watchSubscriptionId.clear();

    // Nothing is open, or the drop has already been reported.
    if (m_path.isEmpty() || m_fileState == FileState::Disconnected)
        return;

    // SPEC 8.2: with no transport the file can be neither read nor written, so
    // the pane reports that instead of advertising whatever state it happened to
    // be in. Conflict, ExternallyModified and ReadOnly are REMEMBERED: the first
    // two describe the file on the server diverging from the buffer and only the
    // user can resolve that, and ReadOnly describes the bytes this pane holds —
    // a truncated read of a file over kMaxEditableReadBytes, which a reconnect
    // does not make whole. Every other state described an operation this drop
    // just voided (CodeharbordClient::failAllPending() has already failed its
    // callback), so the buffer itself decides where the reconnect lands.
    if (m_fileState == FileState::Conflict
        || m_fileState == FileState::ExternallyModified
        || m_fileState == FileState::ReadOnly)
        m_resumeState = m_fileState;
    else
        m_resumeState.reset();
    setFileState(FileState::Disconnected);
}

void EditorController::onTransportBound()
{
    // Whatever was in flight belonged to the previous process and can never be
    // answered by this one; supersede it before issuing anything new so a late
    // reply cannot install a dead subscription id or clear the guard below.
    ++m_watchGeneration;
    m_watchPending = false;

    if (m_path.isEmpty())
        return;  // nothing open in this pane

    // Leave a transient Disconnected or transport Error state behind BEFORE
    // issuing anything: reconciliation may move the state again (to
    // ExternallyModified, or through a reload), and it has to start truthful.
    if (m_fileState == FileState::Disconnected
        || m_fileState == FileState::Error)
        setFileState(m_resumeState.value_or(m_dirty ? FileState::Modified
                                                     : FileState::Clean));
    m_resumeState.reset();

    // A page that reloaded while the transport was down is still empty and is
    // owed its buffer (see ready()). Serving it IS the reconciliation — it
    // fetches the file's current bytes — so it replaces the stat below rather
    // than running next to it. Only the reload branch needs a watch of its own:
    // open() subscribes on its way through.
    if (m_pageNeedsContent) {
        if (!m_dirty)
            subscribeWatch();
        refetchForReloadedPage();
        return;
    }

    // Order matters: subscribe FIRST so a change landing during the
    // reconciliation round trip is still announced, then close the window the
    // outage opened.
    subscribeWatch();
    reconcileAfterReconnect();
}

void EditorController::reconcileAfterReconnect()
{
    if (!m_client || m_path.isEmpty())
        return;

    const QString path = m_path;
    // Only act if the world still looks the way it does right now: an open(),
    // save() or reload() that settles before this stat answers is authoritative
    // and must not be undone by a snapshot taken before it ran.
    const QString baseline = m_revision;

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodStat), readParams(path),
        [self, path, baseline](QJsonValue statRes, std::optional<RpcError> statErr) {
            if (!self)
                return;
            if (self->m_path != path || self->m_revision != baseline)
                return;  // superseded

            if (statErr.has_value()) {
                // The file cannot be stat'd at all. The transport was just bound
                // and this request WAS answered, so the fault is the path, not
                // the link: the file was deleted while the session was down (the
                // common case — the old codeharbord died before it could emit a
                // watchEvent, and the replacement's fresh subscription baselines
                // at whatever is on disk when it is created), or its directory
                // stopped being traversable. Returning here would re-advertise
                // the pre-outage state, so the pane would claim "clean" for a
                // file that no longer exists and the user's next save would be
                // refused as a revision conflict for a revision they never saw
                // change.
                //
                // The revision baseline is deliberately LEFT ALONE. It is what
                // turns that next save into a guarded write the server refuses
                // (assertRevisionMatches in remote/src/files.ts) rather than an
                // unguarded create that silently resurrects the file — or writes
                // over whatever else now holds the path. The server's revision
                // token embeds the inode, so no replacement file can ever match
                // it by accident, and the create-only retry the page's
                // "overwrite" affordance issues is refused too if something else
                // took the path. Recreating a file the user deliberately deleted
                // must stay an explicit choice.
                if (self->m_fileState == FileState::Saving)
                    return;  // the in-flight write decides this file's next state
                if (self->m_dirty) {
                    // Unsaved work over a file that is not there: only the user
                    // can resolve it, exactly as for any other divergence
                    // (SPEC 8.7), and an existing Conflict already says more.
                    if (self->m_fileState != FileState::Conflict)
                        self->setFileState(FileState::ExternallyModified);
                    return;
                }
                // A clean buffer has nothing to lose, so re-READ instead of
                // guessing: the read either succeeds (the stat failure was
                // transient; the buffer re-baselines and settles Clean) or fails
                // too and leaves the pane in FileState::Error — the same place a
                // "deleted" watchEvent leaves it on a live transport.
                self->reload(FileState::ExternallyModified);
                return;
            }

            // One conversion, two readers: `statRes` is a QJsonValue and every
            // toObject() on it rebuilds the object.
            const QJsonObject stat = statRes.toObject();

            // This stat is also the reconnect's permission refresh: the file
            // may have been chmod'd while the session was down, and no watch
            // event exists to tell us. Free — the round trip is already made —
            // and it covers the dirty-buffer branch below, which never reloads
            // and so would otherwise keep the pre-outage verdict forever.
            self->applyStatPermissions(stat);

            if (self->m_fileState == FileState::Saving)
                return;  // the in-flight write decides this file's next state

            const QString revision =
                stat.value(QStringLiteral("revision")).toString();
            if (revision.isEmpty() || revision == baseline)
                return;  // nothing changed while we were away

            if (self->m_dirty) {
                // Unsaved edits: NEVER clobber them (SPEC 8.7). Same rule
                // onNotification() applies to a live watch event, including the
                // refusal to downgrade an existing Conflict.
                if (self->m_fileState != FileState::Conflict)
                    self->setFileState(FileState::ExternallyModified);
                return;
            }
            self->reload(FileState::ExternallyModified);
        });
}

void EditorController::setFileState(FileState state)
{
    if (m_fileState == state)
        return;
    m_fileState = state;
    emit fileStateChanged(toString(state));
}

void EditorController::setReadOnly(bool readOnly)
{
    if (m_readOnly == readOnly)
        return;
    m_readOnly = readOnly;
    emit readOnlyChanged(readOnly);

    // Becoming read-only retires any recovery snapshot we are still holding
    // (SPEC 11.3). A snapshot only pays off if it can be SAVED back one day;
    // for a file the user can no longer write it is dead weight that re-prompts
    // "unsaved changes" on every reopen and can never be applied. This normally
    // has nothing to do — the snapshot is written by reportContent(), which the
    // guard below refuses once read-only — but a debounced report from the page
    // can land in the round trip before the derivation completes.
    if (readOnly)
        clearRecovery();
}

void EditorController::applyStatPermissions(const QJsonObject& stat)
{
    m_pathReadOnly = statSaysUnwritable(stat);
    updateReadOnly();
}

void EditorController::refreshPermissions(std::function<void()> then)
{
    if (!m_client || m_path.isEmpty()) {
        if (then)
            then();
        return;
    }

    const QString path = m_path;
    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodStat), readParams(path),
        [self, path, then](QJsonValue res, std::optional<RpcError> err) {
            if (!self)
                return;
            // Superseded by a file switch: this answer describes the old file
            // and must not decide the new one's editability.
            if (self->m_path != path)
                return;
            // A stat that failed says nothing; keep the last derivation rather
            // than guessing in either direction.
            if (!err.has_value())
                self->applyStatPermissions(res.toObject());
            if (then)
                then();
        });
}

void EditorController::deliverContent(const QString& content, const QString& revision)
{
    if (!m_ready) {
        // The WebChannel page has not attached its handlers yet. Emitting now
        // would drop the buffer on the floor and leave the pane blank forever,
        // so hold the LATEST load and replay it from ready().
        m_pendingContent = content;
        m_pendingRevision = revision;
        return;
    }
    emit contentLoaded(content, revision);
}

void EditorController::ready()
{
    const bool reconnected = m_ready;
    m_ready = true;

    // Every signal emitted before the page attached its handlers went into the
    // void, so the current file's state is replayed here. Without it a page that
    // connected after the load renders an empty status bar and — far worse for a
    // read-only buffer — a freely editable Monaco over a file it can never
    // write. The defaults (nothing open, not read-only) need no signal.
    if (!m_path.isEmpty())
        emit fileStateChanged(toString(m_fileState));
    if (m_readOnly)
        emit readOnlyChanged(true);

    if (m_pendingContent.has_value()) {
        const QString content = *m_pendingContent;
        const QString revision = m_pendingRevision;
        // Consume BEFORE emitting: a handler that re-enters (e.g. requestReload)
        // must not see the buffer a second time.
        m_pendingContent.reset();
        m_pendingRevision.clear();
        emit contentLoaded(content, revision);
        return;
    }

    // Nothing held. A repeat ready() is a page RELOAD: the Monaco page threw
    // its buffer away (a renderer crash, a bundle reload), so replaying is not
    // an option and the file must be fetched again.
    if (!reconnected || m_path.isEmpty())
        return;
    // ...but not over a dead transport. Every fetch below mutates pane state
    // before it can know the request will fail — open() in particular clears
    // the dirty flag, the revision baseline and the whole recovery slot — and
    // with nothing bound the call fails synchronously, so the pane would end up
    // blank, claiming "clean" over work that exists only in this pane's
    // crash-recovery snapshot, and nothing would ever probe that snapshot
    // again. Record the debt instead and pay it from onTransportBound().
    if (!m_client || m_fileState == FileState::Disconnected) {
        m_pageNeedsContent = true;
        return;
    }
    refetchForReloadedPage();
}

// Serve a page that reloaded and lost its buffer. Split out of ready() because
// a reload arriving during an outage has to wait for a transport and then run
// exactly this (see the m_pageNeedsContent note in ready()).
void EditorController::refetchForReloadedPage()
{
    m_pageNeedsContent = false;
    if (m_dirty) {
        // The buffer the page just lost held unsaved work. A plain reload would
        // silently replace it with the server's bytes and the only surviving
        // copy — this pane's crash-recovery snapshot (SPEC 11.3) — would never
        // be mentioned again, because only open() probes for it. Re-open the
        // file instead: the fresh page is treated exactly like a first open, so
        // the snapshot is offered back through recoveryAvailable and the user
        // decides. open() also re-subscribes the watch on its own.
        open(m_path);
        return;
    }
    reload(FileState::Loading, /*discardLocalEdits=*/true);
}

QString EditorController::recoveryPath() const
{
    // One snapshot file per pane (SPEC 11.3), named by the pane's stable id
    // inside the recovery directory the SERVER reports (server.info.recoveryDir,
    // fed in via setRecoveryDir()). NOT a sibling of the edited file, so a
    // snapshot never pollutes the repository, and two panes editing one path
    // keep independent snapshots. The base is a REMOTE absolute path chosen by
    // the server, so it is correct across hosts — unlike a client-derived path,
    // which would name a directory that need not exist or belong to the same
    // user on the server. An empty base (an older server that does not report
    // one, or none pushed in yet) or an empty pane id yields an empty path,
    // which DISABLES recovery rather than sharing one unkeyed file between panes.
    if (m_recoveryId.isEmpty() || m_recoveryDir.isEmpty())
        return QString();
    return m_recoveryDir + QLatin1Char('/') + m_recoveryId;
}

// A recovery key or file change retires every in-flight operation for the old
// slot. The old write may still land on its old path, but its reply must not
// adopt that slot's revision into the new one, decrement the new slot's
// in-flight count, or carry out a truncate the new slot never asked for — the
// generation bump is what makes every outstanding reply recognise itself as
// stale.
void EditorController::resetRecoverySlot()
{
    m_recoveryRevision.clear();
    m_recoveryHasContent = false;
    m_recoveryWritesInFlight = 0;
    m_recoveryClearInFlight = false;
    m_recoveryCheckInFlight = false;
    m_recoveryClearPending = false;
    ++m_recoveryGeneration;
}

// A recovery key can settle AFTER the file is already on screen, so the slot it
// names has never been probed. Probe it now rather than making the user close
// and reopen the pane — but only under the conditions open() itself requires
// before it offers a snapshot: a file is open, its load finished (Clean), the
// buffer is the file's bytes, and the file is one the user could save a
// restored snapshot back to (SPEC 11.3).
void EditorController::probeRecoveryForOpenFile()
{
    if (!m_path.isEmpty() && !m_dirty && m_fileState == FileState::Clean
        && !m_readOnly)
        checkRecovery(m_loadedContent, m_loadGeneration);
}

void EditorController::setRecoveryDir(const QString& dir)
{
    // The server-reported recovery base (server.info.recoveryDir), pushed in by
    // EditorFactory once server.info answers. It can arrive AFTER this pane and
    // its controller already exist, so it is a mutable setter rather than a
    // constructor argument.
    if (m_recoveryDir == dir)
        return;
    m_recoveryDir = dir;
    resetRecoverySlot();
    probeRecoveryForOpenFile();
}

void EditorController::setRecoveryId(const QString& id)
{
    // The pane's stable layout id can settle after this controller exists. A
    // changed id is a new recovery slot, not merely a different spelling of
    // the old path, so old replies must be retired just like on open().
    if (m_recoveryId == id)
        return;
    m_recoveryId = id;
    resetRecoverySlot();
    probeRecoveryForOpenFile();
}

void EditorController::open(QString path)
{
    if (!m_client)
        return;

    // Switching files (or re-opening the same one): release the previous file's
    // watcher first so subscriptions are never leaked or duplicated (SPEC 8.7),
    // and supersede a subscribe still in flight for the OLD path — otherwise
    // its late reply would keep the in-flight guard raised forever and the new
    // file would never get a watcher at all.
    unwatchCurrent();
    ++m_watchGeneration;
    m_watchPending = false;
    m_path = path;
    m_revision.clear();

    m_loadedContent.clear();
    m_dirty = false;
    // A clear intended for the file being left behind is not owed to the new one,
    // and no reply from that file may touch this bookkeeping again.
    resetRecoverySlot();
    // The save chain belongs to the file being left behind: a write still on the
    // wire for it must not make a save on the NEW file queue behind it, and the
    // bytes queued for it are not owed to the new one. Bumping the generation is
    // what stops that write's reply from clearing the flag or consuming the
    // queue of the chain started here.
    ++m_saveGeneration;
    m_saveInFlight = false;
    m_inFlightSaveContent.clear();
    m_queuedSaveContent.reset();
    // Read-only is per-FILE and re-derived below; carrying the previous file's
    // verdict over would either lock an editable file or, worse, leave an
    // unwritable one editable.
    m_pathReadOnly = false;
    m_bufferReadOnly = false;
    updateReadOnly();
    // A load for the PREVIOUS file must never be replayed into the page after
    // a switch; the new load supersedes it.
    m_pendingContent.reset();
    m_pendingRevision.clear();
    // This load serves the page itself, so a refetch owed to a page that
    // reloaded during an outage is settled here rather than fired again on the
    // next rebind (which would re-open the file underneath this one).
    m_pageNeedsContent = false;
    // Where an outage that is still unresolved wanted to come back to. It
    // described the file being left behind (a conflict over ITS bytes, a
    // truncated read of IT), so a rebind must not restore it over the file
    // opening now; this load decides the new file's state on its own.
    m_resumeState.reset();
    // Supersede any read still in flight: its answer describes the file we are
    // leaving, and applying it would push the wrong bytes at the page and adopt
    // the wrong revision as this buffer's save guard.
    const quint64 generation = ++m_loadGeneration;
    setFileState(FileState::Loading);

    QPointer<EditorController> self(this);
    m_client->call(QString::fromLatin1(rpc::kMethodReadFile), readFileParams(path),
                   [self, generation](QJsonValue result,
                                      std::optional<RpcError> error) {
                       if (!self || generation != self->m_loadGeneration)
                           return;  // gone, or superseded by a newer load
                       if (error.has_value()) {
                           self->setFileState(FileState::Error);
                           return;
                       }
                       const QJsonObject obj = result.toObject();
                       // base64 means the daemon's STRICT UTF-8 decode refused
                       // these bytes and sent them exactly as they are
                       // (remote/src/files.ts). Decode before showing anything:
                       // the pane must display the file, with replacement
                       // characters where the invalid sequences are, not a wall
                       // of base64. The decode itself is rpc::decodeFileContent
                       // (src/remote/RpcTypes.h), which lives with the reply
                       // shape rather than here so the next consumer of a
                       // file.readFile reply does not rediscover it.
                       const std::optional<QString> decoded =
                           rpc::decodeFileContent(obj);
                       if (!decoded) {
                           // A base64 payload that is not valid base64: a server
                           // bug or a corrupted frame. Nothing legible exists to
                           // show and the raw payload would be worse, so this is
                           // an honest read failure. Nothing has been mutated
                           // yet, so the pane keeps the file it had.
                           self->setFileState(FileState::Error);
                           return;
                       }
                       const QString content = *decoded;
                       const QString revision =
                           obj.value(QStringLiteral("revision")).toString();
                       if (revision.isEmpty()) {
                           // A readable buffer without the server's revision
                           // token cannot be safely saved later.
                           self->setFileState(FileState::Error);
                           return;
                       }
                       self->m_revision = revision;
                       self->m_loadedContent = content;
                       // The buffer is not the file's bytes when the read came
                       // back base64 (binary: save() sends utf-8, so writing it
                       // back would destroy the file) or truncated (a PREFIX:
                       // writing it back would delete everything past it).
                       // Either way the buffer cannot be saved (SPEC 8.2).
                       const bool truncated =
                           obj.value(QStringLiteral("truncated")).toBool();
                       self->m_bufferReadOnly =
                           obj.value(QStringLiteral("encoding")).toString()
                               == QLatin1String("base64")
                           || truncated;
                       self->updateReadOnly();
                       self->deliverContent(content, revision);
                       // A file over kMaxEditableReadBytes is NOT opened for
                       // editing: FileState::Clean asserts "the buffer is what
                       // the server has", and for a prefix that is false, so the
                       // pane would advertise a saveable buffer over a file whose
                       // tail it never read. ReadOnly is the honest SPEC 8.2
                       // state — the head of the file is still shown, which is
                       // what makes a huge log useful — and it is a state no
                       // save can be issued from. Not Error: the read SUCCEEDED,
                       // and Error would leave the pane blank for a file we can
                       // legitimately display.
                       self->setFileState(truncated ? FileState::ReadOnly
                                                    : FileState::Clean);

                       // Subscribe to external-change notifications (SPEC 8.7).
                       self->subscribeWatch();

                       // Ask whether the file is writable, and only THEN look
                       // for a crash-recovery snapshot: a file the user can
                       // never save must not be offered unsaved changes it
                       // could never apply (SPEC 11.3). Chaining also makes the
                       // order deterministic — the alternative, a parallel
                       // stat, races the recovery prompt it is meant to gate.
                       self->refreshPermissions([self, content, generation]() {
                           if (self && !self->m_readOnly
                               && generation == self->m_loadGeneration)
                               self->checkRecovery(content, generation);
                       });
                   });
}

void EditorController::checkRecovery(const QString& loadedContent, quint64 generation)
{
    if (!m_client)
        return;
    const QString recoveryPath = this->recoveryPath();
    if (recoveryPath.isEmpty())
        return;  // no stable pane id / data location: recovery disabled
    // A permission-refresh callback can arrive after setRecoveryDir() already
    // started this same probe. Keep the stat/read pair as one logical operation
    // so that callback cannot issue a duplicate stat.
    if (m_recoveryCheckInFlight)
        return;
    const quint64 recoveryGeneration = m_recoveryGeneration;
    // Where this pane's slot stood when the probe started; see the check in the
    // read reply below.
    const quint64 slotSerial = m_recoverySlotSerial;
    m_recoveryCheckInFlight = true;

    QPointer<EditorController> self(this);
    // Stat first: absence (error) simply means there is nothing to recover.
    m_client->call(
        QString::fromLatin1(rpc::kMethodStat), readParams(recoveryPath),
        [self, recoveryPath, loadedContent, generation, recoveryGeneration,
         slotSerial](
            QJsonValue statRes, std::optional<RpcError> statErr) {
            if (!self)
                return;
            // A key change reset the guard for the new slot; this old reply
            // must not clear that new probe's guard.
            if (recoveryGeneration != self->m_recoveryGeneration)
                return;
            if (!self->m_client || statErr.has_value()) {
                self->m_recoveryCheckInFlight = false;
                return;  // clientless or no snapshot present
            }
            // Superseded: this check belongs to a load that is no longer on
            // screen.
            if (generation != self->m_loadGeneration) {
                self->m_recoveryCheckInFlight = false;
                return;
            }
            const QString snapshotRevision =
                statRes.toObject().value(QStringLiteral("revision")).toString();
            self->m_client->call(
                QString::fromLatin1(rpc::kMethodReadFile),
                readFileParams(recoveryPath),
                [self, snapshotRevision, loadedContent, generation,
                 recoveryGeneration, slotSerial](
                    QJsonValue readRes, std::optional<RpcError> readErr) {
                    if (!self)
                        return;
                    if (recoveryGeneration != self->m_recoveryGeneration)
                        return;
                    self->m_recoveryCheckInFlight = false;
                    if (readErr.has_value()
                        || generation != self->m_loadGeneration)
                        return;
                    const QJsonObject snapshot = readRes.toObject();
                    // A snapshot write, its retry, or a truncate has installed a
                    // newer revision for this slot since the probe started, so
                    // this stat's revision — and the "does the slot hold
                    // content" verdict below — describe the slot as it was
                    // BEFORE that write. Adopting either would guard the next
                    // write with a revision the server has already replaced, and
                    // a REFUSED truncate leaves a stale snapshot standing behind
                    // a successful save: the next open then offers to restore
                    // text that save already superseded.
                    const bool slotUntouched =
                        slotSerial == self->m_recoverySlotSerial;
                    // Otherwise adopt the snapshot's revision, so the next write
                    // to this pane's slot is a guarded overwrite even when
                    // nothing below is offered.
                    if (slotUntouched)
                        self->m_recoveryRevision = snapshotRevision;
                    // The snapshot is read through the same wire shape as any
                    // other file, so it is decoded the same way (open() and
                    // reload() both do): the daemon sends base64 for bytes its
                    // strict UTF-8 decoder refused, and reading that payload as
                    // if it were the text would hand parseRecovery() a wall of
                    // base64 and silently discard the user's unsaved work.
                    //
                    // A snapshot over kMaxEditableReadBytes came back a prefix,
                    // so its envelope will not parse; a tombstone (empty) does
                    // not parse either, and neither does an undecodable
                    // payload. All three fold into "nothing to offer".
                    const std::optional<QString> payload =
                        rpc::decodeFileContent(snapshot);
                    QString snapshotPath;
                    QString recovered;
                    const bool parsed =
                        payload.has_value()
                        && !snapshot.value(QStringLiteral("truncated")).toBool()
                        && parseRecovery(*payload, snapshotPath, recovered);
                    // This pane's single slot is REUSED as it switches files,
                    // so a snapshot the envelope records against a different
                    // path is this pane's snapshot of a PREVIOUS file and must
                    // never be handed back as the current file's unsaved work.
                    const bool belongsHere =
                        parsed && snapshotPath == self->m_path;
                    if (slotUntouched)
                        self->m_recoveryHasContent = belongsHere;
                    if (belongsHere && recovered != loadedContent)
                        emit self->recoveryAvailable(recovered);
                });
        });
}

void EditorController::save(QString content, QString expectedRevision)
{
    // Every refusal below SAYS SO. The page's save path is fire-and-forget: it
    // hands the buffer over, cancels the crash-recovery snapshot timer it had
    // armed, and waits for one of saved/saveConflict/saveError. A silent return
    // leaves the user believing the file was written when nothing was even sent.
    if (!m_client) {
        // The borrowed RPC client has been destroyed (the session is being torn
        // down). Nothing can be written and nothing ever will be on this client.
        emit saveError(QStringLiteral(
            "Disconnected from the server; the buffer was not written."));
        return;
    }
    if (m_path.isEmpty()) {
        // No file has been opened in this pane, so there is nothing to write
        // the buffer to. Reachable from a host or a page driving a bare
        // controller; the file's own open() is what makes a save meaningful.
        emit saveError(
            QStringLiteral("No file is open; the buffer was not written."));
        return;
    }

    // A disconnected pane cannot issue a meaningful save. CodeharbordClient
    // reports an unbound call synchronously; allowing that callback to run
    // would replace truthful Disconnected with a generic Error and prevent
    // reconnect recovery.
    if (m_fileState == FileState::Disconnected) {
        emit saveError(QStringLiteral(
            "Disconnected from the server; the buffer was not written."));
        return;
    }

    // The page may still be showing the previous file while this path is
    // Loading. A save at that point would pair old bytes with the new path (or
    // race a read of the same path), so refuse it before dirtying the buffer or
    // starting a write chain.
    if (m_fileState == FileState::Loading) {
        emit saveError(QStringLiteral(
            "File is still loading; the buffer was not written."));
        return;
    }

    // The buffer is not writable (SPEC 8.2), so this write can only end as an
    // EACCES from the server or as a UTF-8 overwrite of a binary file. Refuse it
    // here, with a reason the page can show, and leave FileState alone: nothing
    // failed and nothing changed — the save simply never happened.
    //
    // The editor page guards Ctrl/Cmd+S the same way and the two must agree
    // (src/web/editor/src/index.ts). This is the AUTHORITATIVE half: the page's
    // conflict/error notices call bridge.save() directly, bypassing its own
    // guard, and any host can invoke this slot.
    if (m_readOnly) {
        emit saveError(QStringLiteral(
            "File is read-only; the buffer was not written."));
        return;
    }

    // Two saves inside one round trip are ordinary: the save key is far faster
    // than an SSH hop, so a user who presses it twice puts two writes on the
    // wire carrying the SAME expectedRevision. The first replaces that revision
    // and the second is refused against it, and the pane then reports "file
    // changed on disk" for a change nobody but this client made. That is not a
    // conflict, and the user must not be shown one for it.
    //
    // So the writes are serialised rather than raced. If one is already on the
    // wire, this save either IS that write (identical bytes: the user pressed
    // the key again on an unchanged buffer, and re-sending would only move the
    // file's mtime and wake every other watcher for nothing) or it is the next
    // one, queued and issued by that write's reply guarded by the revision it
    // produces — which is the guard this save should have carried and could not
    // know. Nothing is dropped and nothing new is shown: the pane stays in
    // Saving until the last write of the chain answers, and that one reply is
    // the single outcome the page sees.
    if (m_saveInFlight) {
        if (content == m_inFlightSaveContent) {
            // If a newer save is already queued, this request is the latest
            // buffer observation and must replace it with the in-flight bytes.
            // Otherwise the write already on the wire will produce exactly
            // these bytes, so no second write is needed.
            if (m_queuedSaveContent.has_value())
                m_queuedSaveContent = content;
            else
                m_queuedSaveContent.reset();
        } else {
            // Keep only the newest distinct buffer; an intermediate save is
            // superseded by the content currently shown by the page.
            m_queuedSaveContent = std::optional<QString>(std::move(content));
        }
        return;
    }

    issueSave(std::move(content), std::move(expectedRevision));
}

void EditorController::issueSave(QString content, QString expectedRevision)
{
    // The bytes handed to us are, by definition, not yet the bytes on the
    // server. Mark the buffer dirty for the duration of the write: if it fails,
    // or the session dies with it in flight, the buffer still holds work that is
    // nowhere else, and a buffer wrongly believed clean is silently
    // auto-reloaded over by the next external change (SPEC 8.7). Only the reply
    // below clears it, and only if nothing was typed in the meantime.
    m_dirty = true;
    const quint64 editSerial = m_editSerial;

    setFileState(FileState::Saving);
    const QString path = m_path;
    const quint64 loadGeneration = m_loadGeneration;
    const quint64 saveGeneration = ++m_saveGeneration;
    m_saveInFlight = true;
    m_inFlightSaveContent = content;

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodWriteFile),
        writeParams(path, content, expectedRevision),
        [self, path, content, editSerial, loadGeneration, saveGeneration](
            QJsonValue result, std::optional<RpcError> error) {
            if (!self)
                return;
            // This write is off the wire whatever its outcome, so the chain's
            // bookkeeping is released FIRST — before any guard below returns
            // early — or a superseded save would leave the flag latched and
            // every later save would queue behind a write that already answered.
            // Guarded by the generation because open() starts a fresh chain for
            // the new file and may already have a write of its own outstanding;
            // this reply must not clear that one's flag or steal its queue.
            std::optional<QString> queued;
            if (self->m_saveGeneration == saveGeneration) {
                self->m_saveInFlight = false;
                self->m_inFlightSaveContent.clear();
                queued = std::exchange(self->m_queuedSaveContent, std::nullopt);
            }
            // The pane switched files while the write was in flight. This
            // outcome belongs to a file this controller no longer holds, so it
            // must not re-baseline the new file's revision, clear the new file's
            // recovery snapshot, or report a save the page never asked for.
            if (self->m_path != path)
                return;
            // Same reasoning, for the case that check cannot see: a load STARTED
            // while the write was in flight and left m_path equal, because it was
            // an open() of the same path or a reload(). The pane's content, its
            // revision and its FileState now belong to that load; this outcome
            // describes bytes it replaced. Silent, exactly like the path check
            // above — the load that superseded us already set this pane's state.
            //
            // Three ways in, all reachable: the page's Reload button
            // (requestReload), a page reload that finds the buffer dirty (that
            // path re-opens rather than reloads, so the recovery snapshot is
            // offered back), and a host re-opening the file already on screen.
            //
            // This compares loads STARTED, not loads that committed, which is
            // only safe because a load cannot start during a save without
            // meaning to replace the buffer: every SYSTEM-initiated reload site
            // is gated on !m_dirty, and save() holds m_dirty true for the whole
            // write. If a system reload is ever allowed to begin while Saving,
            // its reply is discarded by the guard in reload() without touching
            // anything — and this check would then wrongly swallow the write's
            // real outcome, which is the honest conflict that reply exists to
            // report (see aSaveInFlightIsNotClobberedByASystemReload). The fix
            // then is a separate counter bumped where a load actually COMMITS to
            // the buffer — the two points that assign m_revision and call
            // deliverContent, in open()'s and reload()'s replies — captured here
            // in place of m_loadGeneration.
            if (self->m_loadGeneration != loadGeneration)
                return;
            if (!error.has_value()) {
                const QString newRevision =
                    result.toObject().value(QStringLiteral("revision")).toString();
                if (newRevision.isEmpty()) {
                    emit self->saveError(
                        QStringLiteral("The server returned no revision token."));
                    self->setFileState(FileState::Error);
                    return;
                }
                self->m_revision = newRevision;
                self->m_loadedContent = content;
                if (queued.has_value()) {
                    // A second save arrived during this write carrying DIFFERENT
                    // bytes, so the file is not the buffer yet. Nothing may be
                    // reported saved, marked clean or have its recovery snapshot
                    // retired on the strength of a write those bytes supersede.
                    // Send them now, guarded by the revision this write just
                    // produced — the guard the second save could not know — and
                    // let ITS reply be the one outcome the page sees. The pane
                    // is already in Saving and stays there.
                    self->issueSave(std::move(*queued), newRevision);
                    return;
                }
                // Anything typed while the write was in flight is NOT in the
                // bytes that just landed, so the buffer is still dirty and the
                // recovery snapshot holding those edits must survive
                // (SPEC 11.3). Clearing either would let the next external
                // change auto-reload straight over them.
                const bool editedDuringSave = self->m_editSerial != editSerial;
                if (!editedDuringSave) {
                    self->m_dirty = false;
                    // The file on the server now IS the buffer, so the recovery
                    // snapshot is obsolete: drop it before anything can reopen
                    // the file and be offered a stale copy (SPEC 11.3).
                    self->clearRecovery();
                }
                emit self->saved(newRevision);
                self->setFileState(FileState::Saved);
                self->setFileState(editedDuringSave ? FileState::Modified
                                                    : FileState::Clean);
                return;
            }
            // The chain ends here: whatever was queued behind this write is
            // dropped rather than fired at a server that just refused it. No
            // work is lost — the buffer is still dirty, the failure is reported
            // honestly below, and the page's Retry / Overwrite affordances
            // re-send the buffer as it stands when they are clicked.

            // Stale revision: NEVER silently overwrite (SPEC 8.6). Surface the
            // current revision so the UI can offer reload/overwrite. Prefer the
            // revision the server attached to the error; otherwise stat the file.
            if (error->code == rpc::kRevisionMismatch) {
                // RevisionMismatchError in remote/src/files.ts attaches
                // {path, expected, currentRevision} for a stale write, and
                // {path, currentRevision} for a create-only write onto a file
                // that already exists. When the file is GONE the field is
                // present but JSON null, which reads back as an empty string —
                // indistinguishable here from a server that sent nothing, and
                // both fall through to the stat below.
                const QString current = error->data.toObject()
                                            .value(QStringLiteral("currentRevision"))
                                            .toString();
                if (!current.isEmpty()) {
                    emit self->saveConflict(current);
                    self->setFileState(FileState::Conflict);
                    return;
                }
                if (!self->m_client) {
                    // No client left to ask. Report the conflict with no
                    // revision rather than dropping it: the page must not be
                    // left believing the save is still running.
                    emit self->saveConflict(QString());
                    self->setFileState(FileState::Conflict);
                    return;
                }
                self->m_client->call(
                    QString::fromLatin1(rpc::kMethodStat), readParams(path),
                    [self, path, loadGeneration, saveGeneration](
                        QJsonValue statRes, std::optional<RpcError> statErr) {
                        // The same guards as the write's own reply, re-checked:
                        // this second round trip gives a load one more window in
                        // which to take the buffer over, and a conflict over
                        // bytes the pane no longer holds is not this pane's
                        // conflict.
                        if (!self || self->m_path != path
                            || self->m_loadGeneration != loadGeneration
                            || self->m_saveGeneration != saveGeneration)
                            return;
                        const QString rev =
                            statErr.has_value()
                                ? QString()
                                : statRes.toObject()
                                      .value(QStringLiteral("revision"))
                                      .toString();
                        // An EMPTY revision means the file could not be stat'd
                        // at all, which almost always means it was deleted under
                        // us. That is still the right thing to hand the page:
                        // its "Overwrite" affordance re-saves guarded by this
                        // revision, and an empty expectedRevision is exactly the
                        // create-only write that recreates a deleted file
                        // (assertRevisionMatches in remote/src/files.ts).
                        emit self->saveConflict(rev);
                        self->setFileState(FileState::Conflict);
                    });
                return;
            }

            emit self->saveError(error->message);
            self->setFileState(FileState::Error);
        });
}

void EditorController::reportContent(QString content)
{
    // A read-only buffer can never be saved, so a recovery snapshot of it could
    // never be applied — it would only pile up on the server and re-offer
    // "unsaved changes" on every reopen (SPEC 11.3). Monaco stops producing the
    // edits that drive this call once readOnlyChanged lands, but the bridge is
    // fire-and-forget and the page debounces by 500ms, so a report from just
    // before the derivation can still arrive. Drop it whole: the buffer is not
    // dirty in any sense the user can act on.
    if (m_readOnly)
        return;

    // A load is in flight, so the page is still showing the buffer of whatever
    // was open BEFORE it: the page debounces by 500ms, which outlives an
    // open(). Honouring this report would tag one file's edits with the file
    // now opening and mark a buffer nobody has edited dirty.
    if (m_fileState == FileState::Loading)
        return;

    // Every report is a new observation of the page buffer. This matters while
    // a save is in flight: content equal to the old baseline is a deliberate
    // revert, but the server is about to contain the bytes from that save.
    ++m_editSerial;

    // Do not use the old baseline as a clean shortcut while a write is pending.
    if (!m_saveInFlight && content == m_loadedContent) {
        m_dirty = false;
        if (m_fileState == FileState::Modified)
            setFileState(FileState::Clean);
        clearRecovery();
        return;
    }
    m_dirty = true;
    if (m_fileState == FileState::Clean)
        setFileState(FileState::Modified);
    writeRecovery(content, /*retryOnMismatch=*/true);
}

void EditorController::writeRecovery(const QString& content, bool retryOnMismatch)
{
    if (!m_client || m_path.isEmpty())
        return;
    const QString recoveryPath = this->recoveryPath();
    if (recoveryPath.isEmpty())
        return;  // no stable pane id / data location: recovery disabled
    // The path is recorded INSIDE the snapshot (envelope) so a later open can
    // tell whose file it holds. A file switch landing during this round trip is
    // detected by the recovery GENERATION below, not by re-comparing the path:
    // open() bumps that generation, and it also covers the two changes a path
    // comparison cannot see — a new recovery directory and a new pane id.
    const quint64 recoveryGeneration = m_recoveryGeneration;
    const QString payload = serializeRecovery(m_path, content);

    // See m_recoveryWritesInFlight: until this answers, clearRecovery() cannot know
    // a snapshot exists, so it must defer to this reply rather than no-op.
    ++m_recoveryWritesInFlight;
    // A FRESH report carries bytes the last save did not write, so a truncate that
    // save deferred must not be allowed to delete them. The stale-guard retry below
    // re-sends the SAME bytes, so it must not cancel anything.
    if (retryOnMismatch)
        m_recoveryClearPending = false;

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodWriteFile),
        writeParams(recoveryPath, payload, m_recoveryRevision, kRecoveryFileMode),
        [self, content, recoveryPath, retryOnMismatch, recoveryGeneration](
            QJsonValue result, std::optional<RpcError> error) {
            // The pane switched files while this was in flight: open() has already
            // reset the recovery bookkeeping for the new file, so neither this
            // write's revision nor its slot in the in-flight count is ours to touch.
            // Checked BEFORE the decrement, which is shared state.
            if (!self || recoveryGeneration != self->m_recoveryGeneration)
                return;
            if (self->m_recoveryWritesInFlight > 0)
                --self->m_recoveryWritesInFlight;
            if (!error.has_value()) {
                // Track the returned revision so the next snapshot is a guarded
                // overwrite rather than a create.
                ++self->m_recoverySlotSerial;
                self->m_recoveryRevision =
                    result.toObject().value(QStringLiteral("revision")).toString();
                // An empty buffer is valid unsaved work (the user may have
                // deleted the whole file). The envelope remains meaningful and
                // must be offered on reopen, and a later save must be able to
                // clear it just like a non-empty snapshot.
                self->m_recoveryHasContent = true;
                self->honourDeferredRecoveryClear();
                return;
            }
            // A stale create-only guard means a snapshot survives from a prior
            // session; adopt its current revision and retry once. Recovery is
            // best-effort — other errors are swallowed (never surfaced as a
            // save failure).
            if (!(retryOnMismatch && self->m_client
                  && error->code == rpc::kRevisionMismatch)) {
                // THIS snapshot never landed. Whether there is anything left to
                // truncate depends on an earlier write having succeeded, which
                // m_recoveryHasContent already records — so run the same check
                // rather than assuming either answer.
                self->honourDeferredRecoveryClear();
                return;
            }
            // The retry is a continuation of THIS snapshot write, so keep it
            // counted across the stat hop. Without this the count reads zero for
            // the duration of the stat, and a truncate deferred by a save landing
            // in that window would act on what is known now — nothing — and then be
            // undone by the retry write that follows.
            ++self->m_recoveryWritesInFlight;
            self->m_client->call(
                QString::fromLatin1(rpc::kMethodStat), readParams(recoveryPath),
                [self, content, recoveryGeneration](
                    QJsonValue statRes, std::optional<RpcError> statErr) {
                    // File switched: open() reset the recovery bookkeeping including
                    // any deferred truncate, so nothing is owed to the file being
                    // left behind — and the count now belongs to the new file.
                    if (!self || recoveryGeneration != self->m_recoveryGeneration)
                        return;
                    if (self->m_recoveryWritesInFlight > 0)
                        --self->m_recoveryWritesInFlight;
                    if (statErr.has_value()) {
                        // The chain ends here without a snapshot of its own; a
                        // truncate deferred meanwhile is still owed an answer.
                        self->honourDeferredRecoveryClear();
                        return;
                    }
                    ++self->m_recoverySlotSerial;
                    self->m_recoveryRevision =
                        statRes.toObject()
                            .value(QStringLiteral("revision"))
                            .toString();
                    // Re-counts the chain; its reply honours any deferred truncate.
                    self->writeRecovery(content, /*retryOnMismatch=*/false);
                });
        });
}

void EditorController::honourDeferredRecoveryClear()
{
    // Only the LAST outstanding snapshot write may act: an earlier reply that
    // truncated on its own revision would be overwritten by the write still on
    // the wire, putting the snapshot back after the save that asked for it to go.
    // If a previous truncate is still on the wire, leave the intent armed; its
    // reply will call us again after the latest snapshot write has settled.
    if (!m_recoveryClearPending || m_recoveryWritesInFlight > 0
        || m_recoveryClearInFlight)
        return;
    m_recoveryClearPending = false;
    clearRecovery();
}

void EditorController::clearRecovery()
{
    if (!m_client || m_path.isEmpty())
        return;
    // A snapshot write is on the wire and we do not yet know its revision, so we
    // cannot guard a truncate against it. Record the intent; that write's reply
    // performs it. See m_recoveryWritesInFlight.
    if (m_recoveryWritesInFlight > 0) {
        m_recoveryClearPending = true;
        return;
    }
    // A clear is also asynchronous. Do not issue a second truncate while the
    // first is still outstanding: both would carry the same revision and the
    // duplicate can wake watchers and race a fresh recovery write.
    if (m_recoveryClearInFlight) {
        m_recoveryClearPending = true;
        return;
    }
    // Nothing was ever snapshotted for the open file (or it is already the empty
    // tombstone): writing again would only burn a round trip.
    if (!m_recoveryHasContent)
        return;

    // The frozen C1 catalog (src/remote/RpcTypes.h) has NO delete method, so
    // clearing is expressed with the write it does have: a zero-length,
    // revision-guarded file.writeFile. Guarding on the snapshot's own revision
    // keeps the SPEC 8.4/8.6 rule intact — if another writer touched the slot
    // after us the truncate is REFUSED rather than destroying their buffer.
    // An empty file does not parse as an envelope, so checkRecovery() reads the
    // tombstone as "nothing to recover" — equivalent to deletion for every
    // consumer. The 0600 mode is restated so the truncated file keeps its
    // restrictive permissions (C1).
    const QString recoveryPath = this->recoveryPath();
    if (recoveryPath.isEmpty())
        return;  // pane id / recovery dir withdrawn since: nothing to clear
    const quint64 recoveryGeneration = m_recoveryGeneration;
    const QString guard = m_recoveryRevision;
    m_recoveryClearInFlight = true;

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodWriteFile),
        writeParams(recoveryPath, QString(), guard, kRecoveryFileMode),
        [self, recoveryGeneration](QJsonValue result,
                                   std::optional<RpcError> error) {
            if (!self)
                return;
            // The pane switched files; this bookkeeping is not ours to write.
            if (recoveryGeneration != self->m_recoveryGeneration)
                return;
            self->m_recoveryClearInFlight = false;
            if (error.has_value()) {
                self->honourDeferredRecoveryClear();
                return; // best-effort: a stale snapshot is a prompt, not data loss
            }
            ++self->m_recoverySlotSerial;
            self->m_recoveryRevision =
                result.toObject().value(QStringLiteral("revision")).toString();
            self->m_recoveryHasContent = false;
            self->honourDeferredRecoveryClear();
        });
}

void EditorController::requestReload()
{
    // The USER asked for the file back (the page's conflict/error "Reload"
    // affordance calls this), so replacing the buffer is the whole point.
    reload(FileState::Loading, /*discardLocalEdits=*/true);
}

void EditorController::reload(FileState transitional, bool discardLocalEdits)
{
    if (!m_client || m_path.isEmpty())
        return;

    setFileState(transitional);
    const QString path = m_path;
    // Supersede any read still in flight: the replies to two overlapping loads
    // can arrive in either order, and the older one must never win.
    const quint64 generation = ++m_loadGeneration;
    // The revision the buffer is guarded at right now. A save that SETTLES
    // during the round trip moves it, which is how the reply below notices that
    // the bytes it is holding are older than the ones on the server.
    const QString baselineRevision = m_revision;

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodReadFile), readFileParams(path),
        [self, generation, baselineRevision, discardLocalEdits](
            QJsonValue result, std::optional<RpcError> error) {
            if (!self || generation != self->m_loadGeneration)
                return;
            if (error.has_value()) {
                self->setFileState(FileState::Error);
                return;
            }
            // A save is in flight. Its bytes are the user's, they are not on
            // the server yet, and only its own reply can decide this file's
            // next state — exactly the rule reconcileAfterReconnect() applies
            // to its stat. Installing the server's bytes here would replace the
            // buffer the write is carrying, re-baseline the revision that write
            // is guarded by, and clear the recovery snapshot holding those
            // bytes, one moment before the reply lands (the reply is then
            // refused as a conflict, and the only copy of the edits is gone).
            // Checked FIRST so the pane keeps reporting the write it is waiting
            // on rather than being flagged over bytes that write is carrying.
            if (!discardLocalEdits && self->m_fileState == FileState::Saving)
                return;
            // A save SETTLED while this read was in flight. The server's bytes
            // are then this pane's own bytes, and this reply describes a world
            // that is already gone: installing it would put the PRE-save file
            // back on screen and, worse, re-adopt the pre-save revision as the
            // buffer's save guard, so every later save is refused as a conflict
            // over the user's own write. Silent, like the Saving case above —
            // that save already reported this file's outcome. Nothing else here
            // can see it: a save bumps neither the load generation nor, when it
            // needed no further edits, the dirty flag.
            if (!discardLocalEdits && self->m_revision != baselineRevision)
                return;
            // The user typed while this read was in flight. Only a reload the
            // user ASKED for may overwrite that: for a system-initiated reload
            // (a watch event, a reconnect reconciliation) the buffer is now
            // unsaved work that exists nowhere else, and pushing the server's
            // bytes at the page would silently delete it. Drop the fetched
            // bytes and flag the divergence instead — the same answer
            // onNotification() gives for a change that arrives against an
            // already-dirty buffer (SPEC 8.7).
            //
            // m_dirty, not a count of reports: every reload that can reach this
            // guard was started against a CLEAN buffer (both system-initiated
            // sites are gated on !m_dirty), so a dirty flag here means the page
            // reported unsaved bytes inside the round trip. A report that merely
            // took the buffer BACK to the loaded bytes leaves it clean, and such
            // a buffer has nothing to lose — counting reports instead would park
            // the pane in ExternallyModified over a buffer that matches the file
            // and leave it there until the next external change.
            if (!discardLocalEdits && self->m_dirty) {
                if (self->m_fileState != FileState::Conflict)
                    self->setFileState(FileState::ExternallyModified);
                return;
            }
            const QJsonObject obj = result.toObject();
            // Same rule as open(): a base64 reply is the file's exact bytes,
            // sent that way because the daemon's strict decoder refused them,
            // and it is decoded before it is shown. Decided BEFORE anything is
            // mutated, so an undecodable payload leaves the buffer, its
            // revision and its recovery snapshot exactly as they were.
            const std::optional<QString> decoded = rpc::decodeFileContent(obj);
            if (!decoded) {
                self->setFileState(FileState::Error);
                return;
            }
            const QString content = *decoded;
            const QString revision = obj.value(QStringLiteral("revision")).toString();
            if (revision.isEmpty()) {
                // A readable buffer without the server's revision token cannot
                // be safely saved later.
                self->setFileState(FileState::Error);
                return;
            }
            self->m_revision = revision;
            self->m_loadedContent = content;
            self->m_dirty = false;
            // This load has taken the buffer over, so the save chain it found
            // running belongs to bytes the pane no longer holds. Retire it the
            // way open() does. Only an EXPLICIT reload can get here with a write
            // still on the wire (the guard above sends every system-initiated
            // one home), and leaving the chain standing would strand the next
            // save: save() would see m_saveInFlight, park the user's bytes in
            // m_queuedSaveContent, and the old write's reply — dropped by the
            // load-generation check below — would throw them away without a
            // saved, a saveConflict or a saveError. The buffer would then be
            // believed clean while the page still holds unsaved work, and the
            // next external change would auto-reload straight over it.
            //
            // The generation bump is what keeps that old reply from clearing a
            // flag or consuming a queue that now belongs to a later chain.
            ++self->m_saveGeneration;
            self->m_saveInFlight = false;
            self->m_inFlightSaveContent.clear();
            self->m_queuedSaveContent.reset();
            // The buffer just became the file's bytes again, so a recovery
            // snapshot of the edits this reload replaced is obsolete: drop it,
            // or reopening the pane offers unsaved changes the user already
            // discarded (SPEC 11.3). A no-op unless a snapshot holds content.
            self->clearRecovery();
            // base64 => binary, truncated => a prefix of the file. Either way
            // the bytes on screen are not the file's bytes and must not be
            // written back (SPEC 8.2); see open(), which also explains why a
            // truncated read settles at ReadOnly rather than Clean.
            const bool truncated = obj.value(QStringLiteral("truncated")).toBool();
            self->m_bufferReadOnly =
                obj.value(QStringLiteral("encoding")).toString()
                    == QLatin1String("base64")
                || truncated;
            self->updateReadOnly();
            self->deliverContent(content, revision);
            self->setFileState(truncated ? FileState::ReadOnly : FileState::Clean);
            // A chmod changes ctime, so it changes the revision, so it arrives
            // here as an ordinary external change (SPEC 8.7). Re-derive rather
            // than latch the verdict taken at open: a file made read-only under
            // the user must stop being editable, and one made writable again
            // must stop being refused.
            self->refreshPermissions();
        });
}

// True when a file.watchEventsLost payload names THIS pane's live
// subscription. Everything here is untrusted bytes off a socket, so every step
// is a shape check: a non-object params, a missing or non-array
// `subscriptionIds`, an empty array, non-string elements and ids we never
// minted all answer false. So does an empty m_watchSubscriptionId — no
// subscription is held (never subscribed, already unwatched, or the transport
// died and took the id with it), and an empty id must never be "matched" by an
// empty or absent entry in the payload.
//
// Matching is by SUBSCRIPTION ID, unlike the file.watchEvent handler below,
// which deliberately matches by PATH: this notification carries no path at all,
// and the subscription id is the only key the daemon sends
// (RPC_WATCH_EVENTS_LOST_NOTIFICATION in remote/src/rpc-types.ts).
bool EditorController::watchLossNamesThisPane(const QJsonValue& params) const
{
    if (m_watchSubscriptionId.isEmpty())
        return false;
    const QJsonValue ids = params.toObject().value(QStringLiteral("subscriptionIds"));
    if (!ids.isArray())
        return false;
    const QJsonArray array = ids.toArray();
    for (const QJsonValue& id : array) {
        if (id.isString() && id.toString() == m_watchSubscriptionId)
            return true;
    }
    return false;
}

// The single answer to "the file behind this buffer may no longer be what we
// hold": re-read it, unless that would destroy unsaved work. Shared by both
// watch notifications so the two can never drift apart, and reached only after
// each has decided the news is about THIS pane.
void EditorController::applyExternalChange()
{
    if (m_saveInFlight)
        return;

    if (m_dirty) {
        // Unsaved local edits: do NOT clobber them. Flag the divergence and let
        // the UI resolve it (SPEC 8.7). An existing Conflict is NOT downgraded:
        // it claims everything ExternallyModified does plus "a save was already
        // refused", and it is the state the page's reload/overwrite affordance
        // belongs to.
        if (m_fileState != FileState::Conflict)
            setFileState(FileState::ExternallyModified);
        return;
    }

    // Clean buffer: transition through ExternallyModified while we re-fetch,
    // then settle back to Clean on success.
    //
    // This is also the DELETED path, and the re-read is what makes it honest:
    // it fails, so the pane lands in FileState::Error instead of continuing to
    // advertise a clean buffer for a file that is gone. m_revision keeps the
    // baseline of the file we loaded — on purpose. It is the guard that makes the
    // user's next save a write the server REFUSES (assertRevisionMatches in
    // remote/src/files.ts rejects a non-empty expectedRevision for a missing
    // file, and its revision token embeds the inode, so a different file that
    // took the path cannot match either) rather than an unguarded create that
    // silently resurrects a deleted file. Recreating it stays the user's explicit
    // choice, made through the page's overwrite affordance.
    //
    // reload() is a SYSTEM-initiated reload (discardLocalEdits stays false), so
    // bytes that arrive after the user has typed into the round trip are dropped
    // and the pane is merely flagged — the dirty branch above, one moment later.
    reload(FileState::ExternallyModified);
}

void EditorController::onNotification(const QString& method, const QJsonValue& params)
{
    // Events for this subscription were DROPPED by the daemon's bounded
    // notification queue, so our picture of the file is known-stale and no
    // revision, path or event kind survives to reason about: the only correct
    // reaction is to re-read (RpcTypes.h, kWatchEventsLostNotification).
    //
    // Ordering: a re-read already in flight is not a reason to skip this one.
    // That read was issued BEFORE the loss was reported, so its bytes may
    // predate the changes the daemon dropped. reload() bumps m_loadGeneration,
    // which retires the older read's reply, and issues a fresh one — the pane
    // always ends up re-reading after the loss. A loss arriving next to an
    // ordinary change notification therefore costs at most one extra round trip
    // and can never leave the pane worse off than either alone: both funnel
    // into applyExternalChange(), whose only two outcomes are "re-read" and
    // "flag a dirty buffer", and neither is weakened by repetition.
    if (method == QString::fromLatin1(rpc::kWatchEventsLostNotification)) {
        if (watchLossNamesThisPane(params))
            applyExternalChange();
        return;
    }

    if (method != QString::fromLatin1(rpc::kWatchEventNotification))
        return;
    const QJsonObject obj = params.toObject();
    if (obj.value(QStringLiteral("path")).toString() != m_path)
        return;

    // Ignore the echo of our own write: the event revision matches the baseline
    // we just adopted, so there is nothing external to reconcile. A "deleted"
    // event carries NO revision at all (reconcile() in remote/src/files.ts), so
    // it can never be mistaken for that echo and always falls through below.
    const QString eventRevision = obj.value(QStringLiteral("revision")).toString();
    if (!eventRevision.isEmpty() && eventRevision == m_revision)
        return;

    applyExternalChange();
}

} // namespace ch
