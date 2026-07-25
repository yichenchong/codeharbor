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

} // namespace

EditorController::EditorController(CodeharbordClient* client, QObject* parent)
    : QObject(parent), m_client(client)
{
    if (m_client) {
        connect(m_client, &CodeharbordClient::notificationReceived, this,
                &EditorController::onNotification);
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
    // watcher first so subscriptions are never leaked or duplicated (SPEC 8.7).
    unwatchCurrent();
    m_path = path;
    m_dirty = false;
    m_recoveryRevision.clear();
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
                       emit self->contentLoaded(content, revision);
                       self->setFileState(FileState::Clean);

                       // Subscribe to external-change notifications (SPEC 8.7).
                       self->m_client->call(
                           QString::fromLatin1(rpc::kMethodWatch),
                           readParams(path),
                           [self, client = self->m_client, path](QJsonValue watchRes, std::optional<RpcError> watchErr) {
                               if (watchErr.has_value())
                                   return; // no subscription was created
                               const QString subId =
                                   watchRes.toObject()
                                       .value(QStringLiteral("subscriptionId"))
                                       .toString();
                               // If the controller was destroyed (pane closed)
                               // or switched files before this watch resolved,
                               // the server created a subscription for a path we
                               // no longer track. Release it through the BORROWED
                               // client (it outlives the controller per the ctor
                               // contract) so a server-side watcher is NEVER
                               // leaked (SPEC 8.7) — even when `self` is gone.
                               if (!self || self->m_path != path) {
                                   if (!subId.isEmpty())
                                       client->call(
                                           QString::fromLatin1(rpc::kMethodUnwatch),
                                           unwatchParams(subId),
                                           [](QJsonValue, std::optional<RpcError>) {});
                                   return;
                               }
                               self->m_watchSubscriptionId = subId;
                           });

                       // Offer a crash-recovery snapshot if one exists and
                       // differs from the freshly loaded file (SPEC 11.3).
                       self->checkRecovery(content);
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
                    if (recovered != loadedContent)
                        emit self->recoveryAvailable(recovered);
                });
        });
}

void EditorController::save(QString content, QString expectedRevision)
{
    if (!m_client || m_path.isEmpty())
        return;

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
            emit self->contentLoaded(content, revision);
            self->setFileState(FileState::Clean);
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
