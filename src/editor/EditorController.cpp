#include "EditorController.h"

#include "CodeharbordClient.h"
#include "RpcTypes.h"

#include <QJsonObject>
#include <QPointer>

namespace ch {

namespace {

// JSON-RPC param object helpers keyed to the frozen RpcTypes shapes.
QJsonObject readParams(const QString& path)
{
    return QJsonObject{{QStringLiteral("path"), path}};
}

QJsonObject writeParams(const QString& path, const QString& content,
                        const QString& expectedRevision)
{
    return QJsonObject{{QStringLiteral("path"), path},
                       {QStringLiteral("content"), content},
                       {QStringLiteral("expectedRevision"), expectedRevision}};
}

QJsonObject unwatchParams(const QString& subscriptionId)
{
    return QJsonObject{{QStringLiteral("subscriptionId"), subscriptionId}};
}

// POSIX write bits (owner|group|other) of StatResult.mode, which
// remote/src/files.ts mints straight from fs.lstat().
constexpr int kWriteBits = 0222;

// Does the server's stat PROVE the open file cannot be written?
//
// StatResult (remote/src/rpc-types.ts) carries `mode`, but no uid/gid — and the
// client does not know the remote process's euid either. So "some write bit is
// set" proves nothing: those bits may belong to a user we are not. The one
// thing mode alone does prove is the negative — with NO write bit set anywhere,
// no user but root can write the file. Read-only is therefore claimed on that
// proof only, and every ambiguous case stays editable so the SPEC 8.6 write
// path can report the real errno. Deliberately conservative: a false read-only
// takes the file away from a user who could have edited it.
//
// A directory (or a socket/fifo) is never a writable text buffer regardless.
// Note lstat reports the LINK for a symlink, whose 0777 mode hides an
// unwritable target; that case falls through to the save-time error.
bool statSaysUnwritable(const QJsonObject& stat)
{
    const QString kind = stat.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("directory") || kind == QLatin1String("other"))
        return true;
    const QJsonValue mode = stat.value(QStringLiteral("mode"));
    if (!mode.isDouble())
        return false;  // field absent: unknown, which is not the same as unwritable
    return (mode.toInt() & kWriteBits) == 0;
}

} // namespace

EditorController::EditorController(CodeharbordClient* client, QObject* parent)
    : QObject(parent), m_client(client)
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
    // client is borrowed and outlives us; the response is irrelevant (we are
    // gone), so the callback is a no-op that touches no members.
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
            // so release it through the BORROWED client — it outlives the
            // controller per the ctor contract — and NEVER touch m_watchPending,
            // which now belongs to a newer attempt.
            if (!self || generation != self->m_watchGeneration
                || self->m_path != path) {
                if (!subId.isEmpty())
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
            if (!self || statErr.has_value())
                return;  // gone, or the file cannot be stat'd: nothing to claim
            if (self->m_path != path || self->m_revision != baseline)
                return;  // superseded

            // This stat is also the reconnect's permission refresh: the file
            // may have been chmod'd while the session was down, and no watch
            // event exists to tell us. Free — the round trip is already made —
            // and it covers the dirty-buffer branch below, which never reloads
            // and so would otherwise keep the pre-outage verdict forever.
            self->applyStatPermissions(statRes.toObject());

            if (self->m_fileState == FileState::Saving)
                return;  // the in-flight write decides this file's next state

            const QString revision =
                statRes.toObject().value(QStringLiteral("revision")).toString();
            if (revision.isEmpty() || revision == baseline)
                return;  // nothing changed while we were away

            if (self->m_dirty) {
                // Unsaved edits: NEVER clobber them (SPEC 8.7). Same rule
                // onNotification() applies to a live watch event.
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

    if (m_pendingContent.has_value()) {
        const QString content = *m_pendingContent;
        const QString revision = m_pendingRevision;
        // Consume BEFORE emitting: a handler that re-enters (e.g. requestReload)
        // must not see the buffer a second time.
        m_pendingContent.reset();
        m_pendingRevision.clear();
        // A read-only buffer would otherwise render editable until the next
        // toggle; the default (false) needs no signal.
        if (m_readOnly)
            emit readOnlyChanged(true);
        emit contentLoaded(content, revision);
        return;
    }

    if (m_readOnly)
        emit readOnlyChanged(true);

    // Nothing held. A repeat ready() is a page RELOAD that lost its buffer:
    // re-fetch rather than replay, so the fresh page cannot show stale bytes.
    if (reconnected && !m_path.isEmpty())
        reload(FileState::Loading);
}

QString EditorController::recoveryPathFor(const QString& path)
{
    // POSIX path semantics regardless of the host OS: the recovery snapshot is a
    // sibling of the file inside a `.codeharbor-recovery/` directory.
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const QString dir = slash >= 0 ? path.left(slash) : QString();
    const QString name = slash >= 0 ? path.mid(slash + 1) : path;
    const QString recoveryDir = dir.isEmpty()
        ? QStringLiteral(".codeharbor-recovery")
        : dir + QStringLiteral("/.codeharbor-recovery");
    return recoveryDir + QLatin1Char('/') + name;
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
    setFileState(FileState::Loading);

    QPointer<EditorController> self(this);
    m_client->call(QString::fromLatin1(rpc::kMethodReadFile), readParams(path),
                   [self, path](QJsonValue result, std::optional<RpcError> error) {
                       if (!self)
                           return;
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
                       // A base64 read means the file is binary: the buffer the
                       // page shows is NOT the file's bytes, so writing it back
                       // (save sends utf-8) would destroy it.
                       self->m_bufferReadOnly =
                           obj.value(QStringLiteral("encoding")).toString()
                           == QLatin1String("base64");
                       self->updateReadOnly();
                       self->deliverContent(content, revision);
                       self->setFileState(FileState::Clean);

                       // Subscribe to external-change notifications (SPEC 8.7).
                       self->subscribeWatch();

                       // Ask whether the file is writable, and only THEN look
                       // for a crash-recovery snapshot: a file the user can
                       // never save must not be offered unsaved changes it
                       // could never apply (SPEC 11.3). Chaining also makes the
                       // order deterministic — the alternative, a parallel
                       // stat, races the recovery prompt it is meant to gate.
                       self->refreshPermissions([self, content]() {
                           if (self && !self->m_readOnly)
                               self->checkRecovery(content);
                       });
                   });
}

void EditorController::checkRecovery(const QString& loadedContent)
{
    if (!m_client || m_path.isEmpty())
        return;
    const QString recoveryPath = recoveryPathFor(m_path);

    QPointer<EditorController> self(this);
    // Stat first: absence (error) simply means there is nothing to recover.
    m_client->call(
        QString::fromLatin1(rpc::kMethodStat), readParams(recoveryPath),
        [self, recoveryPath, loadedContent](QJsonValue statRes,
                                            std::optional<RpcError> statErr) {
            if (!self || statErr.has_value())
                return; // no snapshot present
            // A snapshot exists; read it and compare. We treat presence +
            // content difference as "recoverable". (Simplification: rather than
            // a strict mtime comparison against the file, difference is the
            // practical signal that unsaved edits survive; the snapshot's
            // revision is adopted so subsequent reportContent overwrites it.)
            const QString snapshotRevision =
                statRes.toObject().value(QStringLiteral("revision")).toString();
            self->m_client->call(
                QString::fromLatin1(rpc::kMethodReadFile),
                readParams(recoveryPath),
                [self, snapshotRevision, loadedContent](
                    QJsonValue readRes, std::optional<RpcError> readErr) {
                    if (!self || readErr.has_value())
                        return;
                    const QString recovered =
                        readRes.toObject().value(QStringLiteral("content")).toString();
                    self->m_recoveryRevision = snapshotRevision;
                    // An EMPTY snapshot is the tombstone clearRecovery() leaves
                    // behind after a successful save: the frozen C1 catalog has
                    // no delete, so "saved, nothing to recover" is encoded as a
                    // zero-length file. Adopt its revision (the next snapshot is
                    // then a guarded overwrite) but never offer it — that is the
                    // stale "unsaved changes" prompt this exists to prevent.
                    self->m_recoveryHasContent = !recovered.isEmpty();
                    if (!recovered.isEmpty() && recovered != loadedContent)
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

    setFileState(FileState::Saving);
    const QString path = m_path;

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodWriteFile),
        writeParams(path, content, expectedRevision),
        [self, path](QJsonValue result, std::optional<RpcError> error) {
            if (!self)
                return;
            if (!error.has_value()) {
                const QString newRevision =
                    result.toObject().value(QStringLiteral("revision")).toString();
                self->m_revision = newRevision;
                self->m_dirty = false;
                // The file on the server now IS the buffer, so the recovery
                // snapshot is obsolete: drop it before anything can reopen the
                // file and be offered a stale "unsaved changes" copy (SPEC 11.3).
                self->clearRecovery();
                emit self->saved(newRevision);
                self->setFileState(FileState::Saved);
                self->setFileState(FileState::Clean);
                return;
            }

            // Stale revision: NEVER silently overwrite (SPEC 8.6). Surface the
            // current revision so the UI can offer reload/overwrite. Prefer the
            // revision the server attached to the error; otherwise stat the file.
            if (error->code == rpc::kRevisionMismatch) {
                const QString current = error->data.toObject()
                                            .value(QStringLiteral("currentRevision"))
                                            .toString();
                if (!current.isEmpty()) {
                    emit self->saveConflict(current);
                    self->setFileState(FileState::Conflict);
                    return;
                }
                self->m_client->call(
                    QString::fromLatin1(rpc::kMethodStat), readParams(path),
                    [self](QJsonValue statRes, std::optional<RpcError> statErr) {
                        if (!self)
                            return;
                        const QString rev =
                            statErr.has_value()
                                ? QString()
                                : statRes.toObject()
                                      .value(QStringLiteral("revision"))
                                      .toString();
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

    m_dirty = true;
    if (m_fileState == FileState::Clean)
        setFileState(FileState::Modified);
    writeRecovery(content, /*retryOnMismatch=*/true);
}

void EditorController::writeRecovery(const QString& content, bool retryOnMismatch)
{
    if (!m_client || m_path.isEmpty())
        return;
    const QString recoveryPath = recoveryPathFor(m_path);

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodWriteFile),
        writeParams(recoveryPath, content, m_recoveryRevision),
        [self, content, recoveryPath, retryOnMismatch](
            QJsonValue result, std::optional<RpcError> error) {
            if (!self)
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
            if (retryOnMismatch && error->code == rpc::kRevisionMismatch) {
                self->m_client->call(
                    QString::fromLatin1(rpc::kMethodStat), readParams(recoveryPath),
                    [self, content](QJsonValue statRes,
                                    std::optional<RpcError> statErr) {
                        if (!self || statErr.has_value())
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
    // Nothing was ever snapshotted (or it is already the empty tombstone):
    // writing again would only burn a round trip.
    if (!m_client || m_path.isEmpty() || !m_recoveryHasContent)
        return;

    // The frozen C1 catalog (src/remote/RpcTypes.h) has NO delete method, so
    // clearing is expressed with the write it does have: a zero-length,
    // revision-guarded file.writeFile. Guarding on the snapshot's own revision
    // keeps the SPEC 8.4/8.6 rule intact — if another pane or session wrote the
    // snapshot after us the truncate is REFUSED rather than destroying their
    // buffer. checkRecovery() reads a zero-length snapshot as "nothing to
    // recover", so the tombstone is equivalent to deletion for every consumer.
    const QString recoveryPath = recoveryPathFor(m_path);
    const QString guard = m_recoveryRevision;

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodWriteFile),
        writeParams(recoveryPath, QString(), guard),
        [self](QJsonValue result, std::optional<RpcError> error) {
            if (!self || error.has_value())
                return; // best-effort: a stale snapshot is a prompt, not data loss
            self->m_recoveryRevision =
                result.toObject().value(QStringLiteral("revision")).toString();
            self->m_recoveryHasContent = false;
        });
}

void EditorController::requestReload()
{
    reload(FileState::Loading);
}

void EditorController::reload(FileState transitional)
{
    if (!m_client || m_path.isEmpty())
        return;

    setFileState(transitional);
    const QString path = m_path;

    QPointer<EditorController> self(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodReadFile), readParams(path),
        [self](QJsonValue result, std::optional<RpcError> error) {
            if (!self)
                return;
            if (error.has_value()) {
                self->setFileState(FileState::Error);
                return;
            }
            const QJsonObject obj = result.toObject();
            const QString content = obj.value(QStringLiteral("content")).toString();
            const QString revision = obj.value(QStringLiteral("revision")).toString();
            self->m_revision = revision;
            self->m_dirty = false;
            self->m_bufferReadOnly =
                obj.value(QStringLiteral("encoding")).toString()
                == QLatin1String("base64");
            self->updateReadOnly();
            self->deliverContent(content, revision);
            self->setFileState(FileState::Clean);
            // A chmod changes ctime, so it changes the revision, so it arrives
            // here as an ordinary external change (SPEC 8.7). Re-derive rather
            // than latch the verdict taken at open: a file made read-only under
            // the user must stop being editable, and one made writable again
            // must stop being refused.
            self->refreshPermissions();
        });
}

void EditorController::onNotification(const QString& method, const QJsonValue& params)
{
    if (method != QString::fromLatin1(rpc::kWatchEventNotification))
        return;
    const QJsonObject obj = params.toObject();
    if (obj.value(QStringLiteral("path")).toString() != m_path)
        return;

    // Ignore the echo of our own write: the event revision matches the baseline
    // we just adopted, so there is nothing external to reconcile.
    const QString eventRevision = obj.value(QStringLiteral("revision")).toString();
    if (!eventRevision.isEmpty() && eventRevision == m_revision)
        return;

    if (m_dirty) {
        // Unsaved local edits: do NOT clobber them. Flag the divergence and let
        // the UI resolve it (SPEC 8.7).
        setFileState(FileState::ExternallyModified);
        return;
    }

    // Clean buffer: transition through ExternallyModified while we re-fetch,
    // then settle back to Clean on success.
    reload(FileState::ExternallyModified);
}

} // namespace ch
