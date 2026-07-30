#include "InternalUrlSchemeHandler.h"

#include "CodeharbordClient.h"
#include "RpcTypes.h"

#include <QBuffer>
#include <QByteArray>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QMimeType>
#include <QMultiMap>
#include <QMutexLocker>
#include <QPointer>
#include <QUuid>
#include <QWebEngineUrlRequestJob>

#include <utility>

namespace ch {

QString InternalUrlMap::scheme()
{
    return QStringLiteral("codeharbor-internal");
}

QString InternalUrlMap::prefix()
{
    return QStringLiteral("codeharbor-internal://file/");
}

InternalUrlMap &InternalUrlMap::shared()
{
    static InternalUrlMap instance;
    return instance;
}

InternalUrlMap::InternalUrlMap(int maxEntries)
    : m_maxEntries(maxEntries < 1 ? 1 : maxEntries)
{
}

QString InternalUrlMap::internalUrlFor(const QUrl &fileUrl)
{
    const QString key = fileUrl.toString();
    QMutexLocker lock(&m_mutex);
    const auto it = m_fileToId.constFind(key);
    if (it != m_fileToId.constEnd()) {
        touch(it.value());
        return prefix() + it.value();
    }

    // Mint an unguessable, non-sequential id (a random 128-bit token) so a
    // compromised page cannot enumerate other opened files by walking a counter
    // (SPEC 7.4). QUuid draws from the platform CSPRNG. Retry on the
    // astronomically unlikely collision.
    QString id;
    do {
        id = QUuid::createUuid().toString(QUuid::Id128);
    } while (m_idToFile.contains(id));
    m_fileToId.insert(key, id);
    m_idToFile.insert(id, fileUrl);
    m_lru.append(id);
    evictIfNeeded();
    return prefix() + id;
}

QUrl InternalUrlMap::fileUrlFor(const QString &internalUrl) const
{
    const QUrl u(internalUrl);
    QString id;
    if (u.scheme().compare(scheme(), Qt::CaseInsensitive) == 0) {
        // codeharbor-internal://file/<id>: host is "file", path is "/<id>".
        id = u.path();
        if (id.startsWith(QLatin1Char('/')))
            id = id.mid(1);
    } else {
        // Treat the argument as a bare id.
        id = internalUrl;
    }
    return fileUrlForId(id);
}

QUrl InternalUrlMap::fileUrlForId(const QString &id) const
{
    QMutexLocker lock(&m_mutex);
    const auto it = m_idToFile.constFind(id);
    if (it == m_idToFile.constEnd())
        return QUrl();
    // Mark as recently used so an actively-displayed file survives eviction.
    touch(id);
    return it.value();
}

int InternalUrlMap::size() const
{
    QMutexLocker lock(&m_mutex);
    return m_idToFile.size();
}

void InternalUrlMap::touch(const QString &id) const
{
    // m_mutex is held by the caller. Move id to the MRU (back) end.
    const qsizetype idx = m_lru.indexOf(id);
    if (idx >= 0)
        m_lru.removeAt(idx);
    m_lru.append(id);
}

void InternalUrlMap::evictIfNeeded()
{
    // m_mutex is held by the caller. Drop least-recently-used entries so the
    // map stays bounded and never grows without limit.
    while (m_lru.size() > m_maxEntries) {
        const QString victim = m_lru.takeFirst();
        const QUrl file = m_idToFile.take(victim);
        m_fileToId.remove(file.toString());
    }
}

InternalUrlSchemeHandler::InternalUrlSchemeHandler(CodeharbordClient *client,
                                                   InternalUrlMap *map,
                                                   QObject *parent)
    : QWebEngineUrlSchemeHandler(parent)
    , m_client(client)
    , m_map(map ? map : &InternalUrlMap::shared())
{
}

QByteArray InternalUrlSchemeHandler::mimeForPath(const QString &path)
{
    QMimeDatabase db;
    const QMimeType mt = db.mimeTypeForFile(path, QMimeDatabase::MatchExtension);
    if (mt.isValid() && !mt.name().isEmpty())
        return mt.name().toUtf8();
    return QByteArrayLiteral("application/octet-stream");
}

bool InternalUrlSchemeHandler::isActiveContentMime(const QByteArray &mime)
{
    // Active-content MIME types can execute script or load subresources when a
    // browser renders them as a top-level document (SPEC 7.2). Serving
    // untrusted file bytes as one of these on the privileged internal origin is
    // a cross-file exfiltration vector, so such replies are locked down with a
    // restrictive Content-Security-Policy.
    QByteArray m = mime.toLower();
    // Defensive: strip any "; charset=..." parameter (QMimeType::name never
    // carries one today, but a future MIME source might).
    const qsizetype semi = m.indexOf(';');
    if (semi >= 0)
        m = m.left(semi).trimmed();

    // Any XML-family document renders as an active document and can carry inline
    // script, an xml-stylesheet PI pulling an XSLT transform, or foreign HTML.
    // Catches application/xml, text/xml, and every "*+xml" (SVG, XHTML, XSLT,
    // RSS, Atom, ...).
    if (m == QByteArrayLiteral("application/xml")
        || m == QByteArrayLiteral("text/xml")
        || m.endsWith(QByteArrayLiteral("+xml")))
        return true;

    return m == QByteArrayLiteral("text/html")
        || m == QByteArrayLiteral("text/xsl") // standalone XSLT stylesheet
        // MHTML archives reconstruct a whole page (scripts + subresources
        // inlined) from a single file.
        || m == QByteArrayLiteral("multipart/related")
        || m == QByteArrayLiteral("message/rfc822")
        || m == QByteArrayLiteral("application/x-mimearchive");
}

void InternalUrlSchemeHandler::requestStarted(QWebEngineUrlRequestJob *job)
{
    if (!job)
        return;

    // The privileged origin is strictly read-only. A rendered document could
    // still submit a form or issue a non-GET fetch; answer only GET rather than
    // silently treating every method as a read.
    if (job->requestMethod().compare(QByteArrayLiteral("GET"), Qt::CaseInsensitive)
        != 0) {
        job->fail(QWebEngineUrlRequestJob::RequestDenied);
        return;
    }

    const QUrl requestUrl = job->requestUrl();
    // The one documented shape is codeharbor-internal://file/<id> (SPEC 7.4).
    // Reject any other authority instead of accepting it as an alias for
    // "file": a differing host is a different web origin, and serving the same
    // bytes under several origins would hand a rendered page extra origins to
    // play same-origin games with.
    if (requestUrl.host().compare(QLatin1String("file"), Qt::CaseInsensitive) != 0) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    QString id = requestUrl.path();
    if (id.startsWith(QLatin1Char('/')))
        id = id.mid(1);

    const QUrl fileUrl = m_map->fileUrlForId(id);
    if (!fileUrl.isValid()) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    if (!m_client) {
        job->fail(QWebEngineUrlRequestJob::RequestFailed);
        return;
    }

    // Remote path for the read: a remote file:// URL's path is the server path.
    const QString localPath = fileUrl.toLocalFile();
    const QString path = localPath.isEmpty() ? fileUrl.path() : localPath;

    // Guard against the job being destroyed before the async reply arrives.
    QPointer<QWebEngineUrlRequestJob> guard(job);
    const QJsonObject params{
        {QStringLiteral("path"), path},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("length"), kMaxInlineReadBytes},
    };
    m_client->call(
        QString::fromLatin1(rpc::kMethodReadFile), params,
        [guard, path](QJsonValue result, std::optional<RpcError> error) {
            if (!guard)
                return;
            if (error) {
                guard->fail(QWebEngineUrlRequestJob::RequestFailed);
                return;
            }
            const QJsonObject obj = result.toObject();
            if (obj.value(QStringLiteral("truncated")).toBool()) {
                // File exceeds the inline render cap; fail cleanly rather than
                // serve partial bytes as a complete document.
                guard->fail(QWebEngineUrlRequestJob::RequestFailed);
                return;
            }
            const QString encoding = obj.value(QStringLiteral("encoding")).toString();
            const QString content = obj.value(QStringLiteral("content")).toString();
            QByteArray bytes;
            if (encoding == QLatin1String("base64")) {
                // Strict decode. QByteArray::fromBase64() silently yields an
                // empty/garbled result for a malformed payload, which would be
                // served to Chromium as a successful but empty document; abort
                // instead so the viewer reports a failure.
                QByteArray encoded = content.toUtf8();
                const auto decoded = QByteArray::fromBase64Encoding(
                    std::move(encoded),
                    QByteArray::Base64Encoding
                        | QByteArray::AbortOnBase64DecodingErrors);
                if (!decoded) {
                    guard->fail(QWebEngineUrlRequestJob::RequestFailed);
                    return;
                }
                bytes = *decoded;
            } else {
                bytes = content.toUtf8();
            }

            auto *buffer = new QBuffer(guard);
            buffer->setData(bytes);
            buffer->open(QIODevice::ReadOnly);

            const QByteArray mime = InternalUrlSchemeHandler::mimeForPath(path);
            QMultiMap<QByteArray, QByteArray> headers;
            // Pin the declared Content-Type: without nosniff, Chromium may
            // content-sniff the untrusted bytes into a different type (e.g.
            // HTML) and render them as an active document, dodging the
            // active-content CSP gate below (which keys off the DECLARED mime).
            headers.insert(QByteArrayLiteral("X-Content-Type-Options"),
                           QByteArrayLiteral("nosniff"));
            if (InternalUrlSchemeHandler::isActiveContentMime(mime)) {
                // Defense-in-depth alongside the internal profile's JS-disabled
                // WebEngineViews: fully sandbox the document and forbid every
                // script/subresource/fetch so an .svg/.html/.xml/MHTML served
                // top-level on the privileged origin cannot read and exfiltrate
                // other files.
                headers.insert(
                    QByteArrayLiteral("Content-Security-Policy"),
                    QByteArrayLiteral("default-src 'none'; sandbox"));
            }
            // Declare the encoding for textual payloads. The bytes are the raw
            // remote file bytes, served as UTF-8 by the file service; without an
            // explicit charset Chromium falls back to a locale-derived guess and
            // a UTF-8 file with non-ASCII characters renders as mojibake. The
            // CSP gate above deliberately keys off the bare type, and
            // isActiveContentMime() strips any parameter anyway.
            const bool textual = mime.startsWith(QByteArrayLiteral("text/"))
                || mime.endsWith(QByteArrayLiteral("+xml"))
                || mime == QByteArrayLiteral("application/xml")
                || mime == QByteArrayLiteral("application/json");
            const QByteArray contentType =
                textual ? mime + QByteArrayLiteral("; charset=utf-8") : mime;
            guard->setAdditionalResponseHeaders(headers);
            guard->reply(contentType, buffer);
        });
}

} // namespace ch
