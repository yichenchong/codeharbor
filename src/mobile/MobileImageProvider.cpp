#include "MobileImageProvider.h"

#include <QBuffer>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>
#include <QMutexLocker>
#include <QUrl>

#include <utility>

namespace ch {

void MobileImageCache::insert(const QString &remotePath, const QByteArray &bytes)
{
    QMutexLocker locker(&m_mutex);
    auto existing = m_bytes.find(remotePath);
    if (existing != m_bytes.end()) {
        m_totalBytes -= existing->size();
        *existing = bytes;
    } else {
        m_bytes.insert(remotePath, bytes);
        m_lru.append(remotePath);
    }
    m_totalBytes += bytes.size();
    touch(remotePath);
    evictIfNeeded();
}

QByteArray MobileImageCache::bytes(const QString &remotePath) const
{
    QMutexLocker locker(&m_mutex);
    const auto it = m_bytes.constFind(remotePath);
    if (it == m_bytes.constEnd())
        return {};
    touch(remotePath);
    return *it;
}

bool MobileImageCache::contains(const QString &remotePath) const
{
    QMutexLocker locker(&m_mutex);
    return m_bytes.contains(remotePath);
}

void MobileImageCache::remove(const QString &remotePath)
{
    QMutexLocker locker(&m_mutex);
    const auto it = m_bytes.constFind(remotePath);
    if (it == m_bytes.constEnd())
        return;
    m_totalBytes -= it->size();
    m_bytes.erase(it);
    m_lru.removeAll(remotePath);
}

void MobileImageCache::clear()
{
    QMutexLocker locker(&m_mutex);
    m_bytes.clear();
    m_lru.clear();
    m_totalBytes = 0;
}

int MobileImageCache::size() const
{
    QMutexLocker locker(&m_mutex);
    return int(m_bytes.size());
}

qint64 MobileImageCache::byteSize() const
{
    QMutexLocker locker(&m_mutex);
    return m_totalBytes;
}

void MobileImageCache::touch(const QString &remotePath) const
{
    m_lru.removeAll(remotePath);
    m_lru.append(remotePath);
}

void MobileImageCache::evictIfNeeded()
{
    // "> 1" rather than "> 0": the entry just inserted is the one the caller is
    // about to display, so a single payload larger than kMaxBytes is kept
    // rather than evicted the instant it arrives. Refusing it would mean a file
    // the service read successfully could never be shown, which is a worse
    // failure than one oversized resident buffer that the next insert retires.
    while (m_lru.size() > 1
           && (m_lru.size() > kMaxEntries || m_totalBytes > kMaxBytes)) {
        const QString victim = m_lru.takeFirst();
        const auto it = m_bytes.constFind(victim);
        if (it == m_bytes.constEnd())
            continue;
        m_totalBytes -= it->size();
        m_bytes.erase(it);
    }
}

QString MobileImageProvider::providerId()
{
    return QStringLiteral("chremote");
}

MobileImageProvider::MobileImageProvider(std::shared_ptr<MobileImageCache> cache)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_cache(std::move(cache))
{
}

QString MobileImageProvider::remotePathFor(const QString &id,
                                           const MobileImageCache &cache)
{
    const QString decoded = QUrl::fromPercentEncoding(id.toUtf8());
    if (cache.contains(decoded))
        return decoded;
    // Not a fallback for convenience: a path whose characters are all
    // unreserved arrives unencoded, and decoding it is a no-op, so the two
    // spellings are the same string and this costs nothing. It matters only for
    // the mixed case documented in the header, where the decode is what would
    // be wrong.
    if (cache.contains(id))
        return id;
    // Neither spelling is cached. Report the decoded one: it is the spelling
    // MobileViewerService keys by, so a caller logging the miss names the path
    // the user asked for rather than its escaped form.
    return decoded;
}

QImage MobileImageProvider::requestImage(const QString &id, QSize *size,
                                         const QSize &requestedSize)
{
    if (size)
        *size = QSize();
    if (!m_cache)
        return {};

    const QByteArray bytes = m_cache->bytes(remotePathFor(id, *m_cache));
    if (bytes.isEmpty())
        return {}; // cache miss: Image.status becomes Error. Never a fetch.

    // QImageReader rather than QImage::loadFromData(): the reader can be told
    // the target size BEFORE decoding, so a 40-megapixel photograph is scaled
    // by the decoder instead of being fully materialised in a phone's memory
    // first and downscaled afterwards.
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer);
    // The format is decided by CONTENT, never by the remote file's extension: a
    // server-controlled name must not be able to steer the decoder.
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    // The dimensions the CALLER will see, which are not necessarily the ones
    // the file stores. setAutoTransform(true) makes read() apply the file's
    // EXIF orientation after the handler has decoded, but
    // QImageReader::size() reports the size as STORED — the accessor that
    // accounts for the rotation, effectiveSize(), is Qt 6.12 and this build
    // targets 6.10. The two disagree for exactly the most common image a phone
    // user opens: a portrait photograph, which cameras store landscape with a
    // "rotate 90" tag. Reporting the stored size would hand Qt Quick a
    // transposed Image.sourceSize and lay the photograph out at the inverse of
    // its real aspect ratio.
    const QSize storedSize = reader.size();
    const bool transposed = reader.transformation().testFlag(
        QImageIOHandler::TransformationRotate90);
    const QSize sourceSize = transposed && storedSize.isValid()
                                 ? storedSize.transposed()
                                 : storedSize;
    if (requestedSize.isValid() && !requestedSize.isEmpty()) {
        // setScaledSize() is in the decoder's PRE-transform coordinates, so a
        // target computed against the post-transform size has to be transposed
        // back. Without that, a 400x200-stored/200x400-displayed photograph
        // asked to fit 100x40 was scaled to 80x40 and then rotated to 40x80 —
        // twice the requested height, and fitted against the wrong aspect.
        if (sourceSize.isValid() && !sourceSize.isEmpty()) {
            QSize scaled = sourceSize;
            scaled.scale(requestedSize, Qt::KeepAspectRatio);
            reader.setScaledSize(transposed ? scaled.transposed() : scaled);
        } else {
            reader.setScaledSize(transposed ? requestedSize.transposed()
                                            : requestedSize);
        }
    }

    QImage image = reader.read();
    if (image.isNull())
        return {}; // bytes are not an image format this build can decode
    if (size)
        *size = sourceSize.isValid() && !sourceSize.isEmpty() ? sourceSize
                                                             : image.size();
    return image;
}

} // namespace ch
