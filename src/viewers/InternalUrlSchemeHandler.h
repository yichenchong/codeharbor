#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMultiMap>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QWebEngineUrlSchemeHandler>

#include <optional>

// Full definition (not a forward declaration): m_client is a QPointer, whose
// QObject static_cast needs the complete CodeharbordClient (a QObject) type.
#include "CodeharbordClient.h"

namespace ch {



// Free-standing, WebEngine-independent bidirectional map between remote
// file:// URLs and opaque codeharbor-internal://file/<id> URLs (SPEC 7.4).
//
// Remote file:// URLs must never reach Chromium directly (it would treat them
// as client-machine paths), so a privileged internal viewer references remote
// resources through an opaque internal URL. The mapping is:
//   * unguessable — ids are random 128-bit tokens, never sequential, so a
//     compromised page cannot enumerate other opened files by guessing ids;
//   * stable  — a live file URL always maps back to the same id (until evicted);
//   * injective — distinct file URLs map to distinct ids;
//   * invertible — fileUrlFor(internalUrlFor(u)) == u while u is retained.
// The map is LRU-bounded (maxEntries): minting past the cap evicts the
// least-recently-used entry so it never grows without bound. It holds no
// WebEngine state, so it is unit-testable on its own.
class InternalUrlMap {
public:
    // Default upper bound on retained entries before LRU eviction kicks in.
    static constexpr int kDefaultMaxEntries = 1024;

    // Scheme name and the fixed "codeharbor-internal://file/" URL prefix.
    static QString scheme();
    static QString prefix();

    // A process-wide shared instance so the scheme handler (which resolves
    // incoming internal requests) and the ViewerModel / QML (which mint internal
    // URLs) agree on the same id space without explicit wiring.
    static InternalUrlMap &shared();

    // `maxEntries` bounds the number of retained mappings (LRU-evicted beyond
    // it). Values < 1 are clamped to 1.
    explicit InternalUrlMap(int maxEntries = kDefaultMaxEntries);

    // Mint (or reuse) the opaque internal URL for a remote file URL. Minting a
    // new entry may evict the least-recently-used one when over the cap.
    QString internalUrlFor(const QUrl &fileUrl);

    // Inverse of internalUrlFor. Accepts either a full internal URL or a bare
    // id; returns an invalid QUrl when the id is unknown, and equally when a
    // full internal URL names an authority other than "file" — the same URL
    // InternalUrlSchemeHandler would refuse. Marks the entry as recently used
    // so actively-displayed files resist eviction.
    QUrl fileUrlFor(const QString &internalUrl) const;

    // Resolve just the opaque id component back to its file URL. Marks the entry
    // as recently used.
    QUrl fileUrlForId(const QString &id) const;

    // Pin an entry against LRU eviction for as long as a pane is DISPLAYING it.
    //
    // Recency alone cannot express this. An entry is touched when it is minted
    // and when the browser resolves it, and a pane that loaded its image hours
    // ago does neither again — so after `maxEntries` other files are opened the
    // identifier the pane is still showing ages out, and the pane's next reload
    // (or the browser's next subresource fetch) fails with UrlNotFound on a URL
    // that is visibly on screen. Pinning is the only signal that carries
    // "still on screen" into the map.
    //
    // Reference counted: N retains need N releases, so two panes showing the
    // same file cannot unpin each other. Accepts a full internal URL or a bare
    // id, under the same authority rule as fileUrlFor(). Returns false — and
    // pins nothing — for an unknown or malformed argument, so a caller can tell
    // "kept alive" from "already gone".
    //
    // Pinned entries are neither evicted nor counted against the cap, which
    // governs the unpinned remainder. Counting them would be worse than
    // useless: with every entry pinned, a fresh mint would be evicted the
    // instant it was handed out. The number of pins is bounded by the number of
    // live panes, orders of magnitude below maxEntries, so a caller that leaks
    // a retain costs one entry, not the bound.
    bool retain(const QString &internalUrl);

    // Undo one retain(). Releasing an entry that is not pinned does nothing;
    // the entry becomes evictable again once its last pin is gone.
    void release(const QString &internalUrl);

    // Number of stored mappings, how many of them are pinned, and the
    // configured cap. For tests.
    int size() const;
    int retainedCount() const;
    int maxEntries() const { return m_maxEntries; }

private:
    // Id component of a full internal URL, or the argument itself when it is
    // not a URL of this scheme (a bare id). Empty when the argument is an
    // internal URL whose authority is not "file" — the one InternalUrlSchemeHandler
    // refuses, and which every entry point here has to refuse identically.
    static QString idOf(const QString &internalUrl);

    // Move `id` to the most-recently-used end. Requires m_mutex held.
    void touch(const QString &id) const;
    // Evict least-recently-used UNPINNED entries while over the cap. Requires
    // m_mutex.
    void evictIfNeeded();

    mutable QMutex m_mutex;
    QHash<QString, QString> m_fileToId; // file url string -> id
    QHash<QString, QUrl> m_idToFile;    // id -> original file url
    mutable QList<QString> m_lru;       // ids, front = LRU, back = MRU
    QHash<QString, int> m_pins;         // id -> outstanding retain() count
    int m_maxEntries;
};

// Custom URL scheme handler for the privileged internal profile (SPEC 7.4).
// Translates codeharbor-internal://file/<id> requests back to the remote file
// URL and streams the file's bytes fetched over the injected CodeharbordClient
// (file.readFile), replying with a MIME type derived from the extension. Text
// is served verbatim (utf-8); binary/image content is base64-decoded first.
//
// The origin is strictly read-only: only GET requests whose authority is the
// documented "file" host are answered; everything else is refused without ever
// touching the remote server.
//
// THREADS. Qt 6 calls requestStarted() on the Qt UI thread — the thread the
// profile and this handler live on — so it may talk to the CodeharbordClient
// directly and emit its signals directly, both of which it does. (Qt 5's docs
// warned this ran on Chromium's IO thread; that is no longer the case, and any
// build where it were would need the client call marshalled instead.) Only the
// QIODevice handed to QWebEngineUrlRequestJob::reply() is touched from another
// thread: Chromium reads it there until the job is destroyed, which is why the
// buffer is parented to the job and nothing else ever reads or writes it.
class InternalUrlSchemeHandler : public QWebEngineUrlSchemeHandler {
    Q_OBJECT
public:
    // Upper bound on the bytes fetched for a single inline viewer render. A
    // file larger than this is failed (never truncated-and-served) so the RPC
    // frame stays bounded and no consumer receives partial content as if it
    // were complete. EditorController::kMaxEditableReadBytes is the matching
    // cap on the text handler's own reads.
    static constexpr int kMaxInlineReadBytes = 8 * 1024 * 1024;

    // `client` performs the remote reads; `map` resolves opaque ids (defaults to
    // InternalUrlMap::shared()). Neither is owned.
    explicit InternalUrlSchemeHandler(CodeharbordClient *client,
                                      InternalUrlMap *map = nullptr,
                                      QObject *parent = nullptr);

    void requestStarted(QWebEngineUrlRequestJob *job) override;

    // MIME type for a path, derived from its extension only (no filesystem
    // access). Falls back to application/octet-stream. This is the bare type
    // with no parameters; the handler appends "; charset=utf-8" for textual
    // types when it actually replies.
    static QByteArray mimeForPath(const QString &path);

    // Whether a served MIME type is "active content": a type Chromium renders
    // as a top-level document that can execute script or pull subresources —
    // text/html, any XML document (application/xml, text/xml, every "*+xml"
    // such as SVG, XHTML, XSLT, RSS, Atom, and the gzipped "*+xml-compressed"
    // spelling shared-mime-info uses for .svgz), standalone XSLT, and MHTML
    // archives. Replies for such types are locked down with a restrictive CSP.
    // Exposed so the security gate is unit-testable without a live WebEngine
    // job.
    static bool isActiveContentMime(const QByteArray &mime);

    // Whether a served MIME type carries text, and therefore needs an explicit
    // "; charset=utf-8" on its Content-Type. Without one Chromium falls back to
    // a locale-derived guess and a UTF-8 file with any non-ASCII byte renders
    // as mojibake. Covers text/*, the "+xml" and "+json" structured suffixes,
    // and the handful of application/* types that are plainly text. Exposed
    // static for the same reason as isActiveContentMime().
    static bool isTextualMime(const QByteArray &mime);

    // Why a request was refused. QWebEngineUrlRequestJob::fail() takes only
    // Chromium's coarse Error enum and carries NO text, so an oversized image
    // or PDF reaches the pane as a bare failed page with nothing to explain it
    // — unlike the text pane, which gets "file is too large to display inline"
    // from ViewerModel. The reason travels out of band instead, on
    // requestFailed(), which is the only channel the job interface leaves.
    enum class Failure {
        MethodNotAllowed,    // not a GET on the read-only origin
        UnknownHost,         // authority other than "file"
        UnknownResource,     // id not in the map (never minted, or evicted)
        NotARemoteFile,      // id resolves to something that is not a file:// URL
        EmptyPath,           // file:// URL naming no path at all
        NoClient,            // no remote client bound
        ReadFailed,          // file.readFile returned an error
        TooLarge,            // file exceeds kMaxInlineReadBytes
        UndecodableContent,  // base64 payload the server sent is malformed
        MalformedReply,      // file.readFile answered with fields we cannot trust
    };
    Q_ENUM(Failure)

    // Human-readable text for a Failure, in the same register as the messages
    // the editor handler shows for the same conditions. `detail` (a server
    // error message) is appended for ReadFailed when it is non-empty. Pure and
    // static, so the wording is unit-testable without a live WebEngine job —
    // which cannot be constructed outside Chromium.
    static QString failureMessage(Failure reason,
                                  const QString &detail = QString());

    // The response headers accompanying every successful reply for `mime`
    // (Content-Type itself excepted; reply() takes that separately). Exposed
    // static for the same reason as isActiveContentMime(): a QWebEngineUrlRequestJob
    // cannot be constructed in a unit test, so this is the only seam at which
    // the security headers are checkable.
    static QMultiMap<QByteArray, QByteArray>
    responseHeadersFor(const QByteArray &mime);

    // Turn a successful file.readFile result into the bytes to serve.
    //
    // Returns std::nullopt on success, having written the file's bytes to
    // `bytes`; otherwise the reason the reply cannot be served, and `bytes` is
    // left untouched. Every field is CHECKED rather than coerced: an absent
    // `truncated` reads as false through QJsonValue::toBool() and would let a
    // prefix of a huge file be served as though it were the whole document, and
    // an encoding this build does not know would be decoded as UTF-8 text —
    // which for a base64 payload means serving the base64 alphabet itself as if
    // it were the image. The write side already refuses an unlisted encoding
    // for exactly this reason (see writeFileLocked in remote/src/files.ts).
    //
    // Exposed static because QWebEngineUrlRequestJob cannot be constructed
    // outside Chromium, so this is the only seam at which the reply contract is
    // unit-testable.
    static std::optional<Failure> decodeReadReply(const QJsonObject &reply,
                                                  QByteArray *bytes);

signals:
    // A request for `internalUrl` was refused, with the reason the job
    // interface could not carry. `message` is failureMessage(reason, ...).
    // Emitted for EVERY fail() path in requestStarted(), immediately before the
    // job is failed, so a pane can show why its image or PDF did not appear.
    void requestFailed(const QUrl &internalUrl, Failure reason,
                       const QString &message);

private:
    QPointer<CodeharbordClient> m_client;
    InternalUrlMap *m_map;
};

} // namespace ch
