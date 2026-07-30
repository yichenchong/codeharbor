#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QWebEngineUrlSchemeHandler>

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
    // id; returns an invalid QUrl when the id is unknown. Marks the entry as
    // recently used so actively-displayed files resist eviction.
    QUrl fileUrlFor(const QString &internalUrl) const;

    // Resolve just the opaque id component back to its file URL. Marks the entry
    // as recently used.
    QUrl fileUrlForId(const QString &id) const;

    // Number of currently retained mappings, and the configured cap. For tests.
    int size() const;
    int maxEntries() const { return m_maxEntries; }

private:
    // Move `id` to the most-recently-used end. Requires m_mutex held.
    void touch(const QString &id) const;
    // Evict least-recently-used entries while over the cap. Requires m_mutex.
    void evictIfNeeded();

    mutable QMutex m_mutex;
    QHash<QString, QString> m_fileToId; // file url string -> id
    QHash<QString, QUrl> m_idToFile;    // id -> original file url
    mutable QList<QString> m_lru;       // ids, front = LRU, back = MRU
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
class InternalUrlSchemeHandler : public QWebEngineUrlSchemeHandler {
    Q_OBJECT
public:
    // Upper bound on the bytes fetched for a single inline viewer render. A
    // file larger than this is failed (never truncated-and-served) so the RPC
    // frame stays bounded and no consumer receives partial content as if it
    // were complete. ViewerModel::readTextFile applies the same cap.
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
    // text/html, any XML document (application/xml, text/xml, and every "*+xml"
    // such as SVG, XHTML, XSLT, RSS, Atom), standalone XSLT, and MHTML archives.
    // Replies for such types are locked down with a restrictive CSP. Exposed so
    // the security gate is unit-testable without a live WebEngine job.
    static bool isActiveContentMime(const QByteArray &mime);

private:
    QPointer<CodeharbordClient> m_client;
    InternalUrlMap *m_map;
};

} // namespace ch
