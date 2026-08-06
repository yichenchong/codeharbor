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
    if (!fileUrl.isValid())
        return {};
    // The opaque origin is exclusively a bridge for remote file URLs.  Do not
    // mint entries for arbitrary schemes: those entries can never be served by
    // this handler (which rejects them), but would still consume an LRU slot
    // and make the model hand QML a URL that is guaranteed to fail.
    if (fileUrl.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive)
        != 0
        || fileUrl.path().isEmpty())
        return {};
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

QString InternalUrlMap::idOf(const QString &internalUrl)
{
    const QUrl u(internalUrl);
    if (u.scheme().compare(scheme(), Qt::CaseInsensitive) != 0) {
        // Not a URL of this scheme: treat the argument as a bare id.
        return internalUrl;
    }
    // codeharbor-internal://file/<id>: host is "file", path is "/<id>".
    // The authority is checked, not assumed. InternalUrlSchemeHandler refuses
    // any other host (a differing host is a differing web origin), and every
    // entry point here has to agree with it: an accepting lookup next to a
    // refusing handler is precisely the sort of split that lets a URL be
    // "valid" on one side of the subsystem and rejected on the other.
    if (u.host().compare(QLatin1String("file"), Qt::CaseInsensitive) != 0
        || u.port() != -1 || !u.userInfo().isEmpty())
        return QString();
    // Ports and user-info are part of the URL authority too, so accepting
    // them would create alternate origins for the same opaque resource.
    QString id = u.path();
    if (id.startsWith(QLatin1Char('/')))
        id = id.mid(1);
    return id;
}

QUrl InternalUrlMap::fileUrlFor(const QString &internalUrl) const
{
    const QString id = idOf(internalUrl);
    if (id.isEmpty())
        return QUrl();
    return fileUrlForId(id);
}

QUrl InternalUrlMap::fileUrlForId(const QString &id) const
{
    QMutexLocker lock(&m_mutex);
    const auto it = m_idToFile.constFind(id);
    if (it == m_idToFile.constEnd())
        return QUrl();
    // Mark as recently used so an actively-displayed file resists eviction.
    touch(id);
    return it.value();
}

bool InternalUrlMap::retain(const QString &internalUrl)
{
    const QString id = idOf(internalUrl);
    if (id.isEmpty())
        return false;
    QMutexLocker lock(&m_mutex);
    if (!m_idToFile.contains(id))
        return false;
    ++m_pins[id];
    // A newly pinned entry is being displayed right now, so it is the most
    // recently used one by definition.
    touch(id);
    return true;
}

void InternalUrlMap::release(const QString &internalUrl)
{
    const QString id = idOf(internalUrl);
    if (id.isEmpty())
        return;
    QMutexLocker lock(&m_mutex);
    const auto it = m_pins.find(id);
    if (it == m_pins.end())
        return;
    if (--it.value() <= 0)
        m_pins.erase(it);
    // The map may have been over the cap the whole time this entry was pinned.
    evictIfNeeded();
}

int InternalUrlMap::size() const
{
    QMutexLocker lock(&m_mutex);
    // QHash::size() is qsizetype; the cap these are compared against is an int,
    // so the narrowing is stated rather than left to an implicit conversion.
    return static_cast<int>(m_idToFile.size());
}

int InternalUrlMap::retainedCount() const
{
    QMutexLocker lock(&m_mutex);
    return static_cast<int>(m_pins.size());
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
    //
    // The cap governs the UNPINNED entries. A pin means a pane is showing that
    // resource right now, and dropping it would fail a URL that is visibly on
    // screen — so pinned entries are neither evicted nor counted. Counting them
    // would be worse than useless: with the cap reached and every entry pinned,
    // the next mint would be evicted the instant it was handed out, killing the
    // URL a pane had just been given. The number of pins is bounded by the
    // number of live panes, which is orders of magnitude below maxEntries, so
    // the table stays bounded either way.
    if (m_lru.size() <= m_maxEntries)
        return; // cheap common case: not even the pins could push it over
    qsizetype unpinned = m_lru.size() - m_pins.size();
    for (qsizetype i = 0; i < m_lru.size() && unpinned > m_maxEntries;) {
        const QString candidate = m_lru.at(i);
        if (m_pins.contains(candidate)) {
            ++i;
            continue;
        }
        const QUrl file = m_idToFile.take(candidate);
        m_fileToId.remove(file.toString());
        m_lru.removeAt(i);
        --unpinned;
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
    // RSS, Atom, ...). "+xml-compressed" is shared-mime-info's spelling for a
    // gzipped member of the same family (image/svg+xml-compressed, i.e. .svgz);
    // it does not end in "+xml" and so has to be named separately, or the one
    // SVG spelling that is compressed would slip past the gate.
    if (m == QByteArrayLiteral("application/xml")
        || m == QByteArrayLiteral("text/xml")
        || m.endsWith(QByteArrayLiteral("+xml"))
        || m.endsWith(QByteArrayLiteral("+xml-compressed")))
        return true;

    return m == QByteArrayLiteral("text/html")
        || m == QByteArrayLiteral("text/xsl") // standalone XSLT stylesheet
        // MHTML archives reconstruct a whole page (scripts + subresources
        // inlined) from a single file.
        || m == QByteArrayLiteral("multipart/related")
        || m == QByteArrayLiteral("message/rfc822")
        || m == QByteArrayLiteral("application/x-mimearchive");
}

bool InternalUrlSchemeHandler::isTextualMime(const QByteArray &mime)
{
    QByteArray m = mime.toLower();
    const qsizetype semi = m.indexOf(';');
    if (semi >= 0)
        m = m.left(semi).trimmed();

    if (m.startsWith(QByteArrayLiteral("text/")))
        return true;
    // The two structured suffixes RFC 6839 registers. Everything spelled
    // "*+xml" or "*+json" is a text document by construction.
    if (m.endsWith(QByteArrayLiteral("+xml")) || m.endsWith(QByteArrayLiteral("+json")))
        return true;
    // application/* types that are plainly text but carry no structured suffix.
    // Missing one costs a mojibake rendering of a non-ASCII file, so the list
    // covers the spellings shared-mime-info actually produces for source and
    // configuration files (.json, .yaml, .toml, .js).
    return m == QByteArrayLiteral("application/xml")
        || m == QByteArrayLiteral("application/json")
        || m == QByteArrayLiteral("application/yaml")
        || m == QByteArrayLiteral("application/x-yaml")
        || m == QByteArrayLiteral("application/toml")
        || m == QByteArrayLiteral("application/javascript")
        || m == QByteArrayLiteral("application/ecmascript")
        || m == QByteArrayLiteral("application/sql")
        || m == QByteArrayLiteral("application/x-sh")
        || m == QByteArrayLiteral("application/x-shellscript");
}

QString InternalUrlSchemeHandler::failureMessage(Failure reason,
                                                 const QString &detail)
{
    switch (reason) {
    case Failure::MethodNotAllowed:
        return QStringLiteral("the internal viewer origin is read-only");
    case Failure::UnknownHost:
        return QStringLiteral("not an internal viewer address");
    case Failure::UnknownResource:
        // The eviction case is the one a user can actually hit, so it is named:
        // a pane that has been showing one file while a thousand others were
        // opened can find its own address expired.
        return QStringLiteral(
            "this viewer address is no longer valid; reopen the file");
    case Failure::NotARemoteFile:
        return QStringLiteral("this viewer address does not name a remote file");
    case Failure::EmptyPath:
        return QStringLiteral("this viewer address names no file");
    case Failure::NoClient:
        return QStringLiteral("no remote client is connected");
    case Failure::ReadFailed:
        return detail.isEmpty()
                   ? QStringLiteral("the file could not be read")
                   : QStringLiteral("the file could not be read: %1").arg(detail);
    case Failure::TooLarge:
        // One 8 MB rule, one sentence. The text handler presents an oversized
        // file as a read-only prefix (EditorController); the image, PDF and
        // binary panes cannot show a prefix of anything, so they get told.
        return QStringLiteral("file is too large to display inline");
    case Failure::UndecodableContent:
        return QStringLiteral("file contents could not be decoded");
    case Failure::MalformedReply:
        // Distinct from UndecodableContent: there the payload was the right
        // SHAPE and only the base64 was corrupt; here the server answered with
        // fields this build cannot trust at all, so nothing was even attempted.
        return QStringLiteral(
            "the server sent a reply this viewer cannot interpret");
    }
    return QStringLiteral("the file could not be displayed");
}

QMultiMap<QByteArray, QByteArray>
InternalUrlSchemeHandler::responseHeadersFor(const QByteArray &mime)
{
    QMultiMap<QByteArray, QByteArray> headers;
    // Pin the declared Content-Type: without nosniff, Chromium may
    // content-sniff the untrusted bytes into a different type (e.g. HTML) and
    // render them as an active document, dodging the active-content CSP gate
    // below (which keys off the DECLARED mime).
    headers.insert(QByteArrayLiteral("X-Content-Type-Options"),
                   QByteArrayLiteral("nosniff"));
    // Every reply is a whole buffer served under an implicit 200. Chromium's
    // custom-scheme job interface has no way to answer 206 Partial Content —
    // QWebEngineUrlRequestJob::reply() takes a content type and a device and
    // nothing else — so honouring a Range request would hand the browser a
    // fragment labelled as the complete resource. Say so instead of staying
    // silent, which invites a range-capable consumer to assume seekability.
    headers.insert(QByteArrayLiteral("Accept-Ranges"), QByteArrayLiteral("none"));
    // An internal URL is STABLE for as long as its file is retained: the same
    // id is handed back for the same remote path every time. The file behind it
    // is not stable — it is a live file on a server the user is also editing on
    // — so a cached response would show the picture as it was when the pane
    // first loaded it and go on showing that through every reload. Nothing here
    // can revalidate (there is no ETag and no conditional-request path in the
    // job interface), so the only honest answer is to keep no copy at all.
    headers.insert(QByteArrayLiteral("Cache-Control"),
                   QByteArrayLiteral("no-store"));
    if (isActiveContentMime(mime)) {
        // Defense-in-depth alongside the internal profile's JS-disabled
        // WebEngineViews: sandbox the document and forbid every
        // script/subresource/fetch so an .svg/.html/.xml/MHTML served top-level
        // on the privileged origin cannot read and exfiltrate other files.
        //
        // style-src 'unsafe-inline' is the ONE relaxation. Without it
        // default-src 'none' also forbids the <style> block and style="..."
        // attributes INSIDE the document, so an ordinary styled SVG — the most
        // common active-content file a user opens here — renders unstyled, with
        // no indication why. Inline CSS cannot execute script, and every way it
        // could smuggle data out is a resource load (background-image: url(),
        // @import, font-src) which default-src 'none' still refuses; the
        // sandbox still applies. The exfiltration defence is therefore
        // unchanged.
        headers.insert(
            QByteArrayLiteral("Content-Security-Policy"),
            QByteArrayLiteral("default-src 'none'; style-src 'unsafe-inline'; "
                              "sandbox"));
    }
    return headers;
}

std::optional<InternalUrlSchemeHandler::Failure>
InternalUrlSchemeHandler::decodeReadReply(const QJsonObject &reply,
                                          QByteArray *bytes)
{
    // `truncated` is the server's answer to "is this the WHOLE file?" and this
    // handler has no way to serve a prefix honestly — every reply goes out as a
    // complete resource under an implicit 200. QJsonValue::toBool() on a value
    // that is absent, null, or a string reads as FALSE, so coercing it would
    // turn "the server did not say" into "the file is complete" and hand
    // Chromium the first 8 MB of a video as if that were the whole document.
    const QJsonValue truncated = reply.value(QStringLiteral("truncated"));
    if (!truncated.isBool())
        return Failure::MalformedReply;
    if (truncated.toBool()) {
        // File exceeds the inline render cap; fail cleanly rather than serve
        // partial bytes as a complete document.
        return Failure::TooLarge;
    }

    const QJsonValue content = reply.value(QStringLiteral("content"));
    if (!content.isString())
        return Failure::MalformedReply;

    // The encoding decides how the string is turned back into bytes, so an
    // unrecognised spelling cannot be defaulted: treating a base64 payload as
    // UTF-8 text serves the base64 alphabet itself where the image should be,
    // and reports it as a successful read. The write side refuses an unlisted
    // encoding for the same reason (writeFileLocked, remote/src/files.ts).
    const QJsonValue encoding = reply.value(QStringLiteral("encoding"));
    if (!encoding.isString())
        return Failure::MalformedReply;
    const QString encodingName = encoding.toString();

    if (encodingName == QLatin1String("base64")) {
        // Strict decode. QByteArray::fromBase64() silently yields an
        // empty/garbled result for a malformed payload, which would be served
        // to Chromium as a successful but empty document; abort instead so the
        // viewer reports a failure.
        QByteArray encoded = content.toString().toUtf8();
        const auto decoded = QByteArray::fromBase64Encoding(
            std::move(encoded),
            QByteArray::Base64Encoding
                | QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded)
            return Failure::UndecodableContent;
        *bytes = *decoded;
        return std::nullopt;
    }
    if (encodingName == QLatin1String("utf-8")) {
        *bytes = content.toString().toUtf8();
        return std::nullopt;
    }
    return Failure::MalformedReply;
}

void InternalUrlSchemeHandler::requestStarted(QWebEngineUrlRequestJob *job)
{
    if (!job)
        return;

    const QUrl requestUrl = job->requestUrl();
    // Report the reason before failing the job: fail() carries only Chromium's
    // coarse Error enum, and a pane showing a blank failed page has nothing to
    // tell the user without this.
    const auto refuse = [this, job, requestUrl](
                            Failure reason, QWebEngineUrlRequestJob::Error error,
                            const QString &detail = QString()) {
        // A requestFailed() slot can synchronously tear down the view (and
        // therefore the request job). Re-check through QPointer before calling
        // fail(), instead of dereferencing a job that the signal just deleted.
        QPointer<QWebEngineUrlRequestJob> guard(job);
        emit requestFailed(requestUrl, reason, failureMessage(reason, detail));
        if (guard)
            guard->fail(error);
    };

    // The privileged origin is strictly read-only. A rendered document could
    // still submit a form or issue a non-GET fetch; answer only GET rather than
    // silently treating every method as a read.
    if (job->requestMethod().compare(QByteArrayLiteral("GET"), Qt::CaseInsensitive)
        != 0) {
        refuse(Failure::MethodNotAllowed, QWebEngineUrlRequestJob::RequestDenied);
        return;
    }

    if (requestUrl.scheme().compare(InternalUrlMap::scheme(),
                                    Qt::CaseInsensitive)
            != 0
        || requestUrl.host().compare(QLatin1String("file"),
                                     Qt::CaseInsensitive)
               != 0
        || requestUrl.port() != -1 || !requestUrl.userInfo().isEmpty()) {
        refuse(Failure::UnknownHost, QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    QString id = requestUrl.path();
    if (id.startsWith(QLatin1Char('/')))
        id = id.mid(1);

    const QUrl fileUrl = m_map->fileUrlForId(id);
    if (!fileUrl.isValid()) {
        refuse(Failure::UnknownResource, QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }
    // Every id must stand for a REMOTE FILE. The map itself accepts any QUrl,
    // so this is the one place that pins the invariant: without it an id minted
    // for, say, an http URL would be turned into a server path below and read
    // off the remote filesystem — an attacker-chosen path smuggled in through a
    // URL the app was merely asked to display.
    if (fileUrl.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) != 0) {
        refuse(Failure::NotARemoteFile, QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    // Remote path for the read: a remote file:// URL's path is the server path.
    // Checked BEFORE the client, so that a malformed address is reported as the
    // malformed address it is. With the two the other way round, every refusal
    // while offline came back as "no remote client is connected" — which for a
    // URL naming no file at all sent the user looking at their connection.
    const QString localPath = fileUrl.toLocalFile();
    const QString path = localPath.isEmpty() ? fileUrl.path() : localPath;
    if (path.isEmpty()) {
        // A file URL with no path at all names nothing on the server; asking
        // the file service to read "" would only produce a confusing error.
        refuse(Failure::EmptyPath, QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    if (!m_client) {
        refuse(Failure::NoClient, QWebEngineUrlRequestJob::RequestFailed);
        return;
    }

    // Guard against the job being destroyed before the async reply arrives, and
    // separately against this handler being gone by then: the reply may land
    // after the internal profile that owns the handler has been torn down.
    QPointer<QWebEngineUrlRequestJob> guard(job);
    QPointer<InternalUrlSchemeHandler> self(this);
    const QJsonObject params{
        {QStringLiteral("path"), path},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("length"), kMaxInlineReadBytes},
    };
    m_client->call(
        QString::fromLatin1(rpc::kMethodReadFile), params,
        [guard, self, requestUrl,
         path](QJsonValue result, std::optional<RpcError> error) {
            const auto refuseLate = [&guard, &self, &requestUrl](
                                        Failure reason,
                                        const QString &detail = QString()) {
                if (self)
                    emit self->requestFailed(requestUrl, reason,
                                             failureMessage(reason, detail));
                // The signal may synchronously destroy the WebEngine view.
                // QPointer turns that cancellation into a no-op instead of a
                // use-after-free (or a null dereference).
                if (guard)
                    guard->fail(QWebEngineUrlRequestJob::RequestFailed);
            };
            if (error) {
                refuseLate(Failure::ReadFailed, error->message);
                return;
            }
            QByteArray bytes;
            if (const std::optional<Failure> refusal =
                    decodeReadReply(result.toObject(), &bytes)) {
                refuseLate(*refusal);
                return;
            }

            // The request may have been cancelled while the read was in
            // flight — a pane navigating away, a split closing, a reload —
            // in which case Chromium has already destroyed the job and the
            // QPointer is null. There is nothing left to reply to and nobody
            // to report a failure to, so stop here: parenting a QBuffer to a
            // null owner would leak it, and every call below would dereference
            // a null job. The refusal paths above are guarded the same way.
            if (!guard)
                return;

            auto *buffer = new QBuffer(guard);
            buffer->setData(bytes);
            buffer->open(QIODevice::ReadOnly);

            const QByteArray mime = InternalUrlSchemeHandler::mimeForPath(path);
            // Declare the encoding for textual payloads. The bytes are the raw
            // remote file bytes, served as UTF-8 by the file service; without an
            // explicit charset Chromium falls back to a locale-derived guess and
            // a UTF-8 file with non-ASCII characters renders as mojibake. The
            // CSP gate in responseHeadersFor() deliberately keys off the bare
            // type, and isActiveContentMime() strips any parameter anyway.
            const QByteArray contentType =
                isTextualMime(mime) ? mime + QByteArrayLiteral("; charset=utf-8")
                                    : mime;
            guard->setAdditionalResponseHeaders(
                InternalUrlSchemeHandler::responseHeadersFor(mime));
            guard->reply(contentType, buffer);
        });
}

} // namespace ch
