#include "MobileViewerService.h"

#include "RpcTypes.h"
#include "ViewerHandlerRegistry.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPointer>
#include <QStandardPaths>

#include <algorithm>

namespace ch {

namespace {

// The kind word for a registry resolution. The mapping is the desktop's
// (ViewerModel.cpp's viewKindFor), restated because that one is a file-static
// helper inside a WebEngine-linked translation unit; the WORDS are the shared
// contract and they must not drift, which is what
// mobileViewKindsMatchTheDesktopVocabulary in tests/tst_mobileviewers.cpp pins.
QString kindWordFor(ViewerResolution resolution)
{
    switch (resolution) {
    case ViewerResolution::DirectWebNavigation:
        return QStringLiteral("web");
    case ViewerResolution::InternalHtmlRenderer:
        return QStringLiteral("markdown");
    case ViewerResolution::TextEditor:
        return QStringLiteral("text");
    case ViewerResolution::ImageViewer:
        return QStringLiteral("image");
    case ViewerResolution::PdfViewer:
        return QStringLiteral("pdf");
    case ViewerResolution::DirectoryViewer:
        return QStringLiteral("directory");
    case ViewerResolution::Download:
    case ViewerResolution::Error:
        break;
    }
    // Download and Error both land on "binary", the unclaimed-resource
    // disposition of SPEC 7.5. On mobile that is ViewerUnsupportedPage, which
    // says the kind and the reason rather than offering a download: there is
    // nowhere on a phone to download a remote file TO that would not mean
    // exporting it out of the app sandbox.
    return QStringLiteral("binary");
}

// One remote directory entry on its way to a {name, kind} map. Sorting these
// rather than finished QVariantMaps keeps QVariant::toMap() and QMap lookups out
// of the comparator, which would otherwise pay for them four times per compare.
struct DirectoryEntry {
    QString name;
    QString kind;
};

// Directories first, then case-insensitively by name with a case-sensitive
// tie-break. Byte-for-byte the desktop's directoryEntryLess (ViewerModel.cpp):
// one directory must not list in two different orders depending on which client
// the user opened it in.
bool directoryEntryLess(const DirectoryEntry &a, const DirectoryEntry &b)
{
    const bool aDir = a.kind == QLatin1String("directory");
    const bool bDir = b.kind == QLatin1String("directory");
    if (aDir != bDir)
        return aDir;
    const int folded = a.name.compare(b.name, Qt::CaseInsensitive);
    if (folded != 0)
        return folded < 0;
    return a.name.compare(b.name, Qt::CaseSensitive) < 0;
}

QString noClientMessage()
{
    return QStringLiteral("no remote client is connected");
}

// Whether the reply carrying `epoch` is still the one anyone is waiting for,
// retiring the bookkeeping entry when it is.
//
// The two operations whose reply WRITES something — requestImage() fills the
// byte cache, requestPdf() writes a file into the app-private spool — both have
// a matching release call the page makes when it stops showing that path. Those
// release calls used to look only at what had ALREADY been written, which is
// nothing at all while the read is still in flight: a page retargeted to another
// document called releasePdf(), found nothing, and the reply then landed and
// spooled a file (up to kMaxInlineReadBytes) that no page held any more and
// nothing would ever release. Swiping through a directory of PDFs faster than
// the network answers left one abandoned file per skipped document in a cache
// directory the user cannot see, until the process exited.
//
// So each request records an epoch, and only the reply whose epoch is still
// current is allowed to write anything. A release drops the entry; a SECOND
// request for the same path mints a NEWER epoch and overwrites it. That is what
// makes the awkward ordering come out right: release-then-request-again leaves
// the first (older) reply unclaimed — it writes nothing and announces nothing —
// while the second reply is claimed and produces the file the live request
// needs. Nothing is ever deleted out from under a live request, because a reply
// that is not current never creates the thing in the first place.
//
// The map cannot grow: an entry exists only between a request and its reply or
// its release, and CodeharbordClient guarantees every request's callback runs
// exactly once, destruction included.
bool claimRequest(QHash<QString, qint64> *requests, const QString &path,
                  qint64 epoch)
{
    const auto it = requests->constFind(path);
    if (it == requests->constEnd() || *it != epoch)
        return false;
    requests->erase(it);
    return true;
}

} // namespace

MobileViewerService::MobileViewerService(CodeharbordClient *client,
                                        QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_imageCache(std::make_shared<MobileImageCache>())
{
    // A mobile app is normally KILLED rather than shut down, so the destructor
    // below is not a guarantee. Clearing the spool on the way in is what makes
    // sure yesterday's document is not still sitting in the cache directory.
    purgePdfSpool();
}

MobileViewerService::~MobileViewerService()
{
    purgePdfSpool();
}

QString MobileViewerService::pdfSpoolDir() const
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
        return {};
    const QString dir = base + QLatin1String("/pdf-spool");
    if (!QDir().mkpath(dir))
        return {};
    return dir;
}

void MobileViewerService::purgePdfSpool()
{
    m_pdfSpool.clear();
    const QString dir = pdfSpoolDir();
    if (dir.isEmpty())
        return;
    // removeRecursively() on the directory itself, not per remembered file: the
    // point of the startup purge is the files this process does NOT remember.
    QDir(dir).removeRecursively();
}

MarkdownModel *MobileViewerService::createMarkdownModel(QObject *owner)
{
    return new MarkdownModel(owner ? owner : this);
}

MobileImageProvider *MobileViewerService::createImageProvider()
{
    return new MobileImageProvider(m_imageCache);
}

bool MobileViewerService::decodeRead(const QJsonObject &result, ReadReply *out,
                                     QString *error)
{
    const QJsonValue encodingValue = result.value(QStringLiteral("encoding"));
    const QJsonValue contentValue = result.value(QStringLiteral("content"));
    if (!encodingValue.isString() || !contentValue.isString()) {
        *error = QStringLiteral("the server's reply carried no readable file "
                                "content");
        return false;
    }

    // CHECKED, never coerced: QJsonValue::toBool() on an absent member is false,
    // which would present a prefix of an over-cap file as the whole file.
    const QJsonValue truncatedValue = result.value(QStringLiteral("truncated"));
    if (!truncatedValue.isUndefined() && !truncatedValue.isBool()) {
        *error = QStringLiteral("the server's reply carried an untrustworthy "
                                "truncation flag");
        return false;
    }
    out->truncated = truncatedValue.toBool(false);

    const QJsonValue revisionValue = result.value(QStringLiteral("revision"));
    out->revision = revisionValue.isString() ? revisionValue.toString()
                                             : QString();

    const QString encoding = encodingValue.toString();
    const QString content = contentValue.toString();
    if (encoding == QLatin1String("utf-8")) {
        out->text = content;
        out->bytes = content.toUtf8();
        out->binary = false;
    } else if (encoding != QLatin1String("base64")) {
        // An encoding this build does not know is a server bug or a corrupted
        // frame. Decoding it as UTF-8 anyway would show the user a payload in
        // an alphabet nobody chose; the write side refuses an unlisted encoding
        // for the same reason (writeFileLocked in remote/src/files.ts).
        *error = QStringLiteral("the server sent the file in an encoding this "
                                "client does not understand");
        return false;
    } else {
        const auto decoded = QByteArray::fromBase64Encoding(
            content.toUtf8(),
            QByteArray::Base64Encoding
                | QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            *error = QStringLiteral("the server sent a malformed base64 "
                                    "payload");
            return false;
        }
        out->bytes = *decoded;
        // The bytes stay bytes. A base64 reply means the daemon's STRICT UTF-8
        // decoder refused this file, so there is no text to show and none is
        // invented — see the note on readFile().
        out->text.clear();
        out->binary = true;
    }

    // The cap is enforced on the REPLY as well as on the request. Nothing
    // obliges a peer to honour the `length` we asked for, and every consumer
    // downstream is sized against this one number: MobileImageCache's byte
    // bound is literally three times it, and the whole payload is held in this
    // process, in the cache, and again in the view. A reply that ignored the
    // window is a broken or hostile peer, not a bigger file, so it is refused
    // rather than admitted past a bound this class documents as absolute.
    if (out->bytes.size() > kMaxInlineReadBytes) {
        *error = QStringLiteral("the server returned more data than this client "
                                "asked for");
        return false;
    }
    return true;
}

void MobileViewerService::readFile(const QString &path)
{
    if (!m_client) {
        emit fileError(path, noClientMessage());
        return;
    }

    const QJsonObject params{
        {QStringLiteral("path"), path},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("length"), kMaxInlineReadBytes},
    };
    QPointer<MobileViewerService> guard(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodReadFile), params,
        [guard, path](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            if (error) {
                emit guard->fileError(path, error->message);
                return;
            }
            ReadReply reply;
            QString message;
            if (!decodeRead(result.toObject(), &reply, &message)) {
                emit guard->fileError(path, message);
                return;
            }
            // The revision is required HERE and not in decodeRead(), because
            // it is this path — and only this path — whose result becomes an
            // editable buffer. remote/src/files.ts mints one from the same
            // descriptor it read, so an absent token is a broken peer; and an
            // empty token means CREATE-ONLY to file.writeFile, so accepting one
            // would let the user type for ten minutes and then have their first
            // save refused with "file already exists". The image and PDF paths
            // never save anything and are deliberately not held to this.
            if (reply.revision.isEmpty()) {
                emit guard->fileError(
                    path,
                    QStringLiteral("the server sent this file without a "
                                   "revision, so an edit of it could not be "
                                   "saved safely"));
                return;
            }
            emit guard->fileRead(path, reply.text, reply.binary, reply.revision,
                                 reply.truncated);
        });
}

void MobileViewerService::writeFile(const QString &path, const QString &content,
                                    const QString &expectedRevision)
{
    if (!m_client) {
        emit fileError(path, noClientMessage());
        return;
    }

    const QJsonObject params{
        {QStringLiteral("path"), path},
        {QStringLiteral("content"), content},
        {QStringLiteral("encoding"), QStringLiteral("utf-8")},
        // ALWAYS sent, the empty token included. `file.writeFile` REQUIRES the
        // field (requireString, remote/src/files.ts), so omitting it did not
        // mean "no guard" — it meant the request was rejected outright with a
        // JSON-RPC invalid-params error, and a save of a file that had no
        // baseline could never succeed. An empty token is not "any revision"
        // either: the server reads it as CREATE-ONLY, which is exactly the
        // guard a buffer with no baseline wants, and it still refuses to
        // overwrite a file that appeared meanwhile (SPEC 8.6). The desktop
        // sends it the same way (EditorController's writeParams()).
        {QStringLiteral("expectedRevision"), expectedRevision},
    };

    QPointer<MobileViewerService> guard(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodWriteFile), params,
        [guard, path](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            if (error) {
                // Includes ch::rpc::kRevisionMismatch. Deliberately NOT special
                // cased: the server's message is written for a person, and this
                // client must not silently overwrite (SPEC 8.6).
                emit guard->fileError(path, error->message);
                return;
            }
            const QJsonValue revision =
                result.toObject().value(QStringLiteral("revision"));
            if (!revision.isString()) {
                emit guard->fileError(
                    path,
                    QStringLiteral("the server confirmed the write without a "
                                   "revision, so the buffer can no longer be "
                                   "guarded — reload before editing again"));
                return;
            }
            emit guard->fileWritten(path, revision.toString());
        });
}

void MobileViewerService::listDirectory(const QString &path)
{
    if (!m_client) {
        emit directoryError(path, noClientMessage());
        return;
    }

    const QJsonObject params{{QStringLiteral("path"), path}};
    QPointer<MobileViewerService> guard(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodListDirectory), params,
        [guard, path](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            if (error) {
                emit guard->directoryError(path, error->message);
                return;
            }
            const QJsonValue entriesValue =
                result.toObject().value(QStringLiteral("entries"));
            if (!entriesValue.isArray()) {
                emit guard->directoryError(
                    path, QStringLiteral("reply carried no directory entries"));
                return;
            }
            const QJsonArray array = entriesValue.toArray();
            QList<DirectoryEntry> parsed;
            parsed.reserve(array.size());
            for (const QJsonValue &value : array) {
                const QJsonObject entry = value.toObject();
                const QJsonValue name = entry.value(QStringLiteral("name"));
                const QJsonValue kind = entry.value(QStringLiteral("kind"));
                if (!value.isObject() || !name.isString() || !kind.isString()) {
                    emit guard->directoryError(
                        path,
                        QStringLiteral("reply carried a malformed directory "
                                       "entry"));
                    return;
                }
                parsed.append(DirectoryEntry{name.toString(), kind.toString()});
            }
            std::sort(parsed.begin(), parsed.end(), directoryEntryLess);
            QVariantList entries;
            entries.reserve(parsed.size());
            for (const DirectoryEntry &entry : std::as_const(parsed)) {
                entries.push_back(QVariantMap{
                    {QStringLiteral("name"), entry.name},
                    {QStringLiteral("kind"), entry.kind},
                });
            }
            emit guard->directoryListed(path, entries);
        });
}

void MobileViewerService::stat(const QString &path)
{
    if (!m_client) {
        emit fileError(path, noClientMessage());
        return;
    }

    const QJsonObject params{{QStringLiteral("path"), path}};
    QPointer<MobileViewerService> guard(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodStat), params,
        [guard, path](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            if (error) {
                emit guard->fileError(path, error->message);
                return;
            }
            if (!result.isObject()) {
                emit guard->fileError(
                    path, QStringLiteral("the server did not describe this "
                                         "path"));
                return;
            }
            emit guard->stated(path, result.toObject().toVariantMap());
        });
}

void MobileViewerService::resolvePath(const QString &path, const QString &base)
{
    if (!m_client) {
        emit fileError(path, noClientMessage());
        return;
    }

    QJsonObject params{{QStringLiteral("path"), path}};
    if (!base.isEmpty())
        params.insert(QStringLiteral("base"), base);
    QPointer<MobileViewerService> guard(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodResolvePath), params,
        [guard, path](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            if (error) {
                emit guard->fileError(path, error->message);
                return;
            }
            const QJsonObject obj = result.toObject();
            const QJsonValue inside =
                obj.value(QStringLiteral("insideRepositoryRoot"));
            // A server that did not answer the question is a FAILURE, never
            // defaulted: toBool() on an absent value is false, which would mark
            // every path as outside the project — and in the markdown viewer
            // that would silently stop every legitimate image from loading.
            if (!inside.isBool()) {
                emit guard->fileError(
                    path,
                    QStringLiteral("reply carried no insideRepositoryRoot "
                                   "flag"));
                return;
            }
            const QJsonValue resolved = obj.value(QStringLiteral("path"));
            if (!resolved.isString()) {
                emit guard->fileError(
                    path, QStringLiteral("reply carried no resolved path"));
                return;
            }
            emit guard->pathResolved(path, resolved.toString(), inside.toBool());
        });
}

void MobileViewerService::requestImage(const QString &path)
{
    if (path.isEmpty()) {
        emit imageError(path, QStringLiteral("no image path"));
        return;
    }
    // Already held: answer at once, with no round trip. Step 2 of the handshake
    // in MobileImageProvider.h.
    if (m_imageCache->contains(path)) {
        emit imageReady(path, imageUrl(path));
        return;
    }
    if (!m_client) {
        emit imageError(path, noClientMessage());
        return;
    }

    const QJsonObject params{
        {QStringLiteral("path"), path},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("length"), kMaxInlineReadBytes},
    };
    // The epoch this request is answering for. See claimRequest() and
    // m_imageRequests: a reply whose epoch is no longer the current one belongs
    // to a request the page has released (or superseded), and it must not put
    // bytes into the cache that forgetImage() has already been told to drop.
    const qint64 epoch = ++m_requestEpoch;
    m_imageRequests.insert(path, epoch);
    QPointer<MobileViewerService> guard(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodReadFile), params,
        [guard, path, epoch](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            if (!claimRequest(&guard->m_imageRequests, path, epoch))
                return;
            if (error) {
                emit guard->imageError(path, error->message);
                return;
            }
            ReadReply reply;
            QString message;
            if (!decodeRead(result.toObject(), &reply, &message)) {
                emit guard->imageError(path, message);
                return;
            }
            if (reply.truncated) {
                // A prefix of an image is a corrupt image, not a smaller one.
                // Refused rather than decoded, exactly as the desktop refuses
                // it (InternalUrlSchemeHandler::TooLarge).
                emit guard->imageError(
                    path,
                    QStringLiteral("this image is larger than the %1 MiB the "
                                   "viewer will load inline")
                        .arg(kMaxInlineReadBytes / (1024 * 1024)));
                return;
            }
            if (reply.bytes.isEmpty()) {
                emit guard->imageError(path,
                                       QStringLiteral("this file is empty"));
                return;
            }
            // The cache write happens BEFORE the announcement: the provider
            // answers on a worker thread and can never wait for this.
            guard->m_imageCache->insert(path, reply.bytes);
            emit guard->imageReady(path, guard->imageUrl(path));
        });
}

void MobileViewerService::forgetImage(const QString &path)
{
    // The pending request is retired FIRST, and unconditionally. A request
    // still in flight has nothing in the cache to remove yet, and leaving its
    // epoch current means its reply would insert up to kMaxInlineReadBytes for
    // a path the page has already stopped showing — bounded by the LRU, but
    // still resident memory a user swiping through a directory of photographs
    // faster than the network answers would accumulate 16 of.
    m_imageRequests.remove(path);
    m_imageCache->remove(path);
}

void MobileViewerService::requestPdf(const QString &path)
{
    if (path.isEmpty()) {
        emit pdfError(path, QStringLiteral("no document path"));
        return;
    }
    const auto spooled = m_pdfSpool.constFind(path);
    if (spooled != m_pdfSpool.constEnd() && QFile::exists(*spooled)) {
        emit pdfReady(path, QUrl::fromLocalFile(*spooled));
        return;
    }
    if (!m_client) {
        emit pdfError(path, noClientMessage());
        return;
    }

    const QJsonObject params{
        {QStringLiteral("path"), path},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("length"), kMaxInlineReadBytes},
    };
    // See claimRequest(): a reply for a request the page has already released
    // must not spool a file nothing will ever release again.
    const qint64 epoch = ++m_requestEpoch;
    m_pdfRequests.insert(path, epoch);
    QPointer<MobileViewerService> guard(this);
    m_client->call(
        QString::fromLatin1(rpc::kMethodReadFile), params,
        [guard, path, epoch](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            // Checked BEFORE the file is written rather than after, so an
            // abandoned document is never created on this device at all.
            if (!claimRequest(&guard->m_pdfRequests, path, epoch))
                return;
            if (error) {
                emit guard->pdfError(path, error->message);
                return;
            }
            ReadReply reply;
            QString message;
            if (!decodeRead(result.toObject(), &reply, &message)) {
                emit guard->pdfError(path, message);
                return;
            }
            if (reply.truncated) {
                // A prefix of a PDF will not open, and the half of it that does
                // would be a lie about the document. Refused, like an oversized
                // image.
                emit guard->pdfError(
                    path,
                    QStringLiteral("this document is larger than the %1 MiB the "
                                   "viewer will load inline")
                        .arg(kMaxInlineReadBytes / (1024 * 1024)));
                return;
            }
            const QString dir = guard->pdfSpoolDir();
            if (dir.isEmpty()) {
                emit guard->pdfError(
                    path, QStringLiteral("this device gave the app nowhere "
                                         "private to put the document"));
                return;
            }
            // Digest, not basename: the remote name is server-controlled and
            // must never choose a filename on this device.
            const QString digest = QString::fromLatin1(
                QCryptographicHash::hash(path.toUtf8(),
                                         QCryptographicHash::Sha256)
                    .toHex());
            const QString target = dir + QLatin1Char('/') + digest
                                   + QLatin1String(".pdf");
            QFile file(target);
            // flush() EXPLICITLY, before close(). QFile buffers, and
            // QFileDevice::close() flushes without reporting whether the flush
            // worked — so on a phone whose cache partition has just filled up
            // (the ordinary way an app-private cache fails) a half-written PDF
            // was announced as ready, and PdfDocument then either refused to
            // open it or showed a document missing its last pages as though
            // that were the document.
            const bool wrote =
                file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                && file.write(reply.bytes) == reply.bytes.size()
                && file.flush();
            file.close();
            if (!wrote) {
                file.remove();
                emit guard->pdfError(
                    path, QStringLiteral("the document could not be written to "
                                         "the app's private storage"));
                return;
            }
            // Owner-only, on the platforms that honour it: nothing outside this
            // app has any business reading a file from someone's repository.
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            guard->m_pdfSpool.insert(path, target);
            emit guard->pdfReady(path, QUrl::fromLocalFile(target));
        });
}

void MobileViewerService::releasePdf(const QString &path)
{
    // The pending request is retired FIRST, and unconditionally: while the read
    // is still in flight there is nothing in m_pdfSpool to delete, and that is
    // exactly the window in which a page retargeted to another document used to
    // leave a multi-megabyte file behind in the app cache for the rest of the
    // process's life. See claimRequest().
    m_pdfRequests.remove(path);
    const auto spooled = m_pdfSpool.constFind(path);
    if (spooled == m_pdfSpool.constEnd())
        return;
    QFile::remove(*spooled);
    m_pdfSpool.erase(spooled);
}

QUrl MobileViewerService::imageUrl(const QString &path) const
{
    if (path.isEmpty())
        return {};
    // Total percent-encoding, '/' included: the path becomes ONE opaque
    // component, so nothing downstream can read part of it as an authority, a
    // query or a fragment. QUrl::fromEncoded rather than a setPath() round
    // trip, because setPath() would re-encode and un-escape at its own
    // discretion.
    const QByteArray encoded = QUrl::toPercentEncoding(path);
    return QUrl::fromEncoded("image://" + MobileImageProvider::providerId().toUtf8()
                             + "/" + encoded);
}

QString MobileViewerService::viewKindFor(const QUrl &url) const
{
    return kindWordFor(ViewerHandlerRegistry::resolve(url));
}

QString MobileViewerService::remotePathFor(const QUrl &url) const
{
    if (url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) != 0)
        return {};
    // A remote file:// URL never carries a host, so the fully decoded path IS
    // the server-absolute path. FullyDecoded, not path(): the default spelling
    // leaves the sequences that "could change meaning" encoded, and the remote
    // file service speaks plain paths, not URLs.
    return url.path(QUrl::FullyDecoded);
}

QUrl MobileViewerService::fileUrlFor(const QString &path) const
{
    if (path.isEmpty())
        return {};
    QUrl url;
    url.setScheme(QStringLiteral("file"));
    // setPath() escapes for us, which is the point: concatenating the path into
    // a scheme string would let a file named "notes#1" silently become a URL
    // with a fragment and read the wrong file.
    url.setPath(path);
    return url;
}

QByteArray MobileViewerService::cachedImageBytes(const QString &path) const
{
    return m_imageCache->bytes(path);
}

} // namespace ch
