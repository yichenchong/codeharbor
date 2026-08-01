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

    // Leave the Disconnected state the drop parked us in BEFORE issuing
    // anything: the reconciliation below may move the state again (to
    // ExternallyModified, or through a reload), and it has to move it from a
    // truthful starting point.
    if (m_fileState == FileState::Disconnected)
        setFileState(m_resumeState.value_or(m_dirty ? FileState::Modified
                                                    : FileState::Clean));
    m_resumeState.reset();

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
    if (m_dirty) {
        // The buffer the page just lost held unsaved work. A plain reload would
        // silently replace it with the server's bytes and the only surviving
        // copy — this pane's crash-recovery snapshot (SPEC 11.3) — would never
        // be mentioned again, because only open() probes for it. Re-open the
        // file instead: the fresh page is treated exactly like a first open, so
        // the snapshot is offered back through recoveryAvailable and the user
        // decides.
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

void EditorController::setRecoveryDir(const QString& dir)
{
    // The server-reported recovery base (server.info.recoveryDir), pushed in by
    // EditorFactory once server.info answers. It can arrive AFTER this pane and
    // its controller already exist, so it is a mutable setter rather than a
    // constructor argument: recoveryPath() reads whatever is current at the time
    // a snapshot is written or probed.
    m_recoveryDir = dir;
}

void EditorController::setRecoveryId(const QString& id)
{
    // The pane's stable layout id, pushed in by EditorPaneView. It can settle
    // after this controller exists (the region assigns a pane's paneId as part
    // of building/rebuilding the layout), so this is a setter rather than a
    // fixed constructor value; recoveryPath() reads whatever is current when a
    // snapshot is written or probed. Keyed per pane so two panes on one file
    // never share a snapshot (SPEC 11.3).
    m_recoveryId = id;
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
    m_dirty = false;
    m_recoveryRevision.clear();
    m_recoveryHasContent = false;
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
                       const QString content =
                           obj.value(QStringLiteral("content")).toString();
                       const QString revision =
                           obj.value(QStringLiteral("revision")).toString();
                       self->m_revision = revision;
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

    QPointer<EditorController> self(this);
    // Stat first: absence (error) simply means there is nothing to recover.
    m_client->call(
        QString::fromLatin1(rpc::kMethodStat), readParams(recoveryPath),
        [self, recoveryPath, loadedContent, generation](
            QJsonValue statRes, std::optional<RpcError> statErr) {
            if (!self || !self->m_client || statErr.has_value())
                return; // gone, clientless, or no snapshot present
            // Superseded: this check belongs to a load that is no longer the
            // one on screen.
            if (generation != self->m_loadGeneration)
                return;
            const QString snapshotRevision =
                statRes.toObject().value(QStringLiteral("revision")).toString();
            self->m_client->call(
                QString::fromLatin1(rpc::kMethodReadFile),
                readFileParams(recoveryPath),
                [self, snapshotRevision, loadedContent, generation](
                    QJsonValue readRes, std::optional<RpcError> readErr) {
                    if (!self || readErr.has_value())
                        return;
                    if (generation != self->m_loadGeneration)
                        return;
                    const QJsonObject snapshot = readRes.toObject();
                    // Adopt the snapshot's revision unconditionally so the next
                    // write to this pane's slot is a guarded overwrite, even
                    // when nothing below is offered.
                    self->m_recoveryRevision = snapshotRevision;
                    // A snapshot over kMaxEditableReadBytes came back a prefix,
                    // so its envelope will not parse; a tombstone (empty) does
                    // not parse either. parseRecovery() folds both into "nothing
                    // to offer".
                    QString snapshotPath;
                    QString recovered;
                    const bool parsed =
                        !snapshot.value(QStringLiteral("truncated")).toBool()
                        && parseRecovery(
                               snapshot.value(QStringLiteral("content")).toString(),
                               snapshotPath, recovered);
                    // This pane's single slot is REUSED as it switches files, so
                    // a snapshot the envelope records against a different path is
                    // this pane's snapshot of a PREVIOUS file and must never be
                    // handed back as the current file's unsaved work (SPEC 11.3).
                    const bool belongsHere =
                        parsed && snapshotPath == self->m_path
                        && !recovered.isEmpty();
                    self->m_recoveryHasContent = belongsHere;
                    if (belongsHere && recovered != loadedContent)
                        emit self->recoveryAvailable(recovered);
                });
        });
}

void EditorController::save(QString content, QString expectedRevision)
{
    if (!m_client || m_path.isEmpty())
        return;

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

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodWriteFile),
        writeParams(path, content, expectedRevision),
        [self, path, editSerial](QJsonValue result, std::optional<RpcError> error) {
            if (!self)
                return;
            // The pane switched files while the write was in flight. This
            // outcome belongs to a file this controller no longer holds, so it
            // must not re-baseline the new file's revision, clear the new file's
            // recovery snapshot, or report a save the page never asked for.
            if (self->m_path != path)
                return;
            if (!error.has_value()) {
                const QString newRevision =
                    result.toObject().value(QStringLiteral("revision")).toString();
                self->m_revision = newRevision;
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
                    [self, path](QJsonValue statRes, std::optional<RpcError> statErr) {
                        if (!self || self->m_path != path)
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

    ++m_editSerial;
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
    // tell whose file it holds; the pane's current path is captured separately
    // to detect a file switch landing during this round trip.
    const QString path = m_path;
    const QString payload = serializeRecovery(path, content);

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodWriteFile),
        writeParams(recoveryPath, payload, m_recoveryRevision, kRecoveryFileMode),
        [self, content, path, recoveryPath, retryOnMismatch](
            QJsonValue result, std::optional<RpcError> error) {
            if (!self)
                return;
            // The pane switched files while this was in flight: open() has
            // already reset the recovery bookkeeping for the new file, so this
            // stale write's revision is not ours to adopt.
            if (self->m_path != path)
                return;
            if (!error.has_value()) {
                // Track the returned revision so the next snapshot is a guarded
                // overwrite rather than a create.
                self->m_recoveryRevision =
                    result.toObject().value(QStringLiteral("revision")).toString();
                self->m_recoveryHasContent = !content.isEmpty();
                return;
            }
            // A stale create-only guard means a snapshot survives from a prior
            // session; adopt its current revision and retry once. Recovery is
            // best-effort — other errors are swallowed (never surfaced as a
            // save failure).
            if (retryOnMismatch && self->m_client
                && error->code == rpc::kRevisionMismatch) {
                self->m_client->call(
                    QString::fromLatin1(rpc::kMethodStat), readParams(recoveryPath),
                    [self, content, path](QJsonValue statRes,
                                          std::optional<RpcError> statErr) {
                        if (!self || statErr.has_value())
                            return;
                        if (self->m_path != path)
                            return;
                        self->m_recoveryRevision =
                            statRes.toObject()
                                .value(QStringLiteral("revision"))
                                .toString();
                        self->writeRecovery(content, /*retryOnMismatch=*/false);
                    });
            }
        });
}

void EditorController::clearRecovery()
{
    // Nothing was ever snapshotted for the open file (or it is already the empty
    // tombstone): writing again would only burn a round trip.
    if (!m_client || m_path.isEmpty() || !m_recoveryHasContent)
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
    const QString path = m_path;
    const QString guard = m_recoveryRevision;

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodWriteFile),
        writeParams(recoveryPath, QString(), guard, kRecoveryFileMode),
        [self, path, recoveryPath](QJsonValue result, std::optional<RpcError> error) {
            if (!self || error.has_value())
                return; // best-effort: a stale snapshot is a prompt, not data loss
            // The pane switched files; this bookkeeping is not ours to write.
            if (self->m_path != path)
                return;
            self->m_recoveryRevision =
                result.toObject().value(QStringLiteral("revision")).toString();
            self->m_recoveryHasContent = false;
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
    // Where the buffer stands right now. A report landing during the round trip
    // bumps this, which is how the reply below notices that the bytes it is
    // about to install would destroy keystrokes typed since it was issued.
    const quint64 editSerial = m_editSerial;

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodReadFile), readFileParams(path),
        [self, generation, editSerial, discardLocalEdits](
            QJsonValue result, std::optional<RpcError> error) {
            if (!self || generation != self->m_loadGeneration)
                return;
            if (error.has_value()) {
                self->setFileState(FileState::Error);
                return;
            }
            // The user typed while this read was in flight. Only a reload the
            // user ASKED for may overwrite that: for a system-initiated reload
            // (a watch event, a reconnect reconciliation) the buffer is now
            // unsaved work that exists nowhere else, and pushing the server's
            // bytes at the page would silently delete it. Drop the fetched
            // bytes and flag the divergence instead — the same answer
            // onNotification() gives for a change that arrives against an
            // already-dirty buffer (SPEC 8.7).
            if (!discardLocalEdits && self->m_editSerial != editSerial) {
                if (self->m_fileState != FileState::Conflict)
                    self->setFileState(FileState::ExternallyModified);
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
            // m_editSerial cannot notice this on its own: a save carries bytes
            // the controller may never have seen a reportContent for, because
            // Ctrl/Cmd+S beats the page's 500 ms debounce.
            if (!discardLocalEdits && self->m_fileState == FileState::Saving)
                return;
            const QJsonObject obj = result.toObject();
            const QString content = obj.value(QStringLiteral("content")).toString();
            const QString revision = obj.value(QStringLiteral("revision")).toString();
            self->m_revision = revision;
            self->m_dirty = false;
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
