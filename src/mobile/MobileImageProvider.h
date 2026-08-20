#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>

#include <memory>

namespace ch {

// Bounded in-memory store of remote image BYTES, shared by MobileViewerService
// (which fills it from file.readFile) and MobileImageProvider (which decodes
// from it on whatever thread Qt Quick asks on).
//
// It exists for the same reason ch::InternalUrlMap exists on the desktop: a
// remote path must never be handed to the image pipeline as something the
// pipeline could fetch itself, so the pipeline is given an opaque
// image://chremote/<path> address that can ONLY be answered out of bytes this
// process already holds. On the desktop the WebEngine scheme handler could do
// the fetch inline because Qt calls it on the UI thread; QQuickImageProvider is
// deliberately called on a WORKER thread (that is the whole point of
// Image.asynchronous), and a worker thread cannot talk to CodeharbordClient at
// all. So the fetch and the decode are split, and this cache is the seam.
//
// BOUNDED two ways, because one bound is not enough on a phone:
//   * kMaxEntries caps the number of retained images, so browsing a directory
//     of thumbnails cannot accumulate an unbounded map even when every file is
//     tiny;
//   * kMaxBytes caps the retained payload, because one entry may be up to
//     MobileViewerService::kMaxInlineReadBytes (8 MiB) on its own and eight of
//     those would be more resident memory than the whole rest of the client.
// Over either bound the least-recently-used entry is dropped. The mobile shell
// shows exactly ONE pane at a time, so the working set is one image; the slack
// is there so stepping back and forth between two files in a directory does not
// re-fetch over SSH every time.
//
// THREADS. Every method takes m_mutex. insert() runs on the Qt UI thread (the
// RPC reply), take()/contains() run on an image worker thread. Nothing here
// touches a QObject, a QPixmap, or the RPC client, so there is no thread
// affinity to violate.
class MobileImageCache {
public:
    // Retained-entry ceiling. 16 is roughly "a screen of files you might tap
    // through", and far below the point where the byte bound stops mattering.
    static constexpr int kMaxEntries = 16;
    // Retained-payload ceiling: three max-size inline reads. Chosen against
    // MobileViewerService::kMaxInlineReadBytes rather than picked round, so the
    // cache can always hold at least the image currently on screen plus the one
    // the user just came from, whatever their size.
    static constexpr qint64 kMaxBytes = 3 * qint64(8 * 1024 * 1024);

    MobileImageCache() = default;

    // Store (or refresh) the bytes for `remotePath`, then evict
    // least-recently-used entries until both bounds hold again. Bytes larger
    // than kMaxBytes on their own are still stored — they are the only entry
    // that survives the eviction pass — because refusing them would mean an
    // image the service successfully read could never be displayed.
    void insert(const QString &remotePath, const QByteArray &bytes);

    // Bytes for `remotePath`, marked most-recently-used. Empty when absent.
    // Deliberately a COPY: the caller decodes on another thread, and handing
    // out a reference into a container this class may evict under its own mutex
    // is a use-after-free waiting for a busy moment.
    QByteArray bytes(const QString &remotePath) const;

    bool contains(const QString &remotePath) const;

    // Drop one entry (a pane navigating away, or a read that must be redone).
    void remove(const QString &remotePath);
    void clear();

    // For tests and for the diagnostics in MobileViewerService.
    int size() const;
    qint64 byteSize() const;

private:
    // Move `remotePath` to the most-recently-used end. Requires m_mutex.
    void touch(const QString &remotePath) const;
    // Drop LRU entries while either bound is exceeded. Requires m_mutex.
    void evictIfNeeded();

    mutable QMutex m_mutex;
    QHash<QString, QByteArray> m_bytes;
    mutable QList<QString> m_lru; // front = LRU, back = MRU
    qint64 m_totalBytes = 0;
};

// Qt Quick image provider for the mobile client, registered under the provider
// id "chremote" (see providerId()). It answers image://chremote/<percent-encoded
// remote path> requests, and it answers them ONLY from MobileImageCache.
//
// THE HANDSHAKE, precisely. Getting this wrong deadlocks the render thread, so
// it is written out rather than left to be inferred:
//
//   1. The QML page calls viewerService.requestImage(path). It does NOT set
//      Image.source yet.
//   2. MobileViewerService, on the Qt UI thread, either finds the bytes already
//      cached or issues one file.readFile. Either way it eventually emits
//      imageReady(path, url) — AFTER the bytes are in the cache — or
//      imageError(path, message).
//   3. The page's onImageReady sets Image.source = url. Image.cache is false on
//      that element, so setting the same url again after a re-read really does
//      re-request instead of serving Qt's own pixmap cache.
//   4. Qt calls requestImage() below on an image worker thread. The bytes are
//      already there, so it decodes and returns.
//
// Step 4 NEVER calls back into MobileViewerService, CodeharbordClient, or any
// QObject living on the UI thread, and it never blocks waiting for one. A cache
// MISS is reported as a null QImage — Image.status becomes Error and the page
// shows its error text — not as a fetch. That asymmetry is the safety property:
// the only thing that can put bytes in the cache is a UI-thread RPC reply, so
// the worker thread can never be the thread waiting on the network.
//
// OWNERSHIP. QQmlEngine::addImageProvider() TAKES OWNERSHIP of the provider, so
// the cache cannot be a member: the service would be left holding a pointer the
// engine deletes at teardown. Both sides therefore hold a shared_ptr to the
// cache, and the provider is free to die whenever the engine says so.
class MobileImageProvider : public QQuickImageProvider {
public:
    // The provider id this class is registered under. QML addresses it as the
    // URL AUTHORITY: image://chremote/<id>.
    static QString providerId();

    explicit MobileImageProvider(std::shared_ptr<MobileImageCache> cache);

    // Decode the cached bytes for the requested remote path. Returns a null
    // QImage — never a fetch, never a block — when the path is not cached or
    // its bytes are not an image format this build can decode.
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

    // The remote path an incoming provider `id` names.
    //
    // MobileViewerService::imageUrl() percent-encodes the whole path (including
    // '/'), but Qt hands the id back through QUrl's PrettyDecoded spelling,
    // which decodes the sequences that cannot change meaning ("%20" -> " ")
    // while PRESERVING the ones that can ("%2F", "%25"). So the id may arrive
    // fully encoded, partly decoded, or — on a path made only of unreserved
    // characters — not encoded at all. Decoding it and, if that finds nothing,
    // trying it verbatim covers all three without guessing which happened.
    // Exposed static so the round trip is unit-testable without an engine.
    static QString remotePathFor(const QString &id,
                                 const MobileImageCache &cache);

private:
    std::shared_ptr<MobileImageCache> m_cache;
};

} // namespace ch
