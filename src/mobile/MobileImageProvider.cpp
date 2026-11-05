#include "MobileImageProvider.h"

#include <QBuffer>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStringView>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ch {

namespace {

bool hasOnlyLocalCssUrls(QStringView css)
{
    // QtSvg resolves paint-server URLs locally, but do not let stylesheet
    // imports or escaped spellings introduce a second resource grammar here.
    if (css.contains(QLatin1Char('\\'))
        || css.contains(QLatin1String("@import"), Qt::CaseInsensitive))
        return false;
    static const QRegularExpression url(
        QStringLiteral("url\\s*\\(([^)]*)\\)"),
        QRegularExpression::CaseInsensitiveOption);
    auto matches = url.globalMatchView(css);
    while (matches.hasNext()) {
        const auto match = matches.next();
        QStringView target = match.capturedView(1).trimmed();
        if (target.size() >= 2
            && ((target.front() == QLatin1Char('"') && target.back() == QLatin1Char('"'))
                || (target.front() == QLatin1Char('\'') && target.back() == QLatin1Char('\''))))
            target = target.mid(1, target.size() - 2).trimmed();
        if (!target.startsWith(QLatin1Char('#')))
            return false;
    }
    return true;
}

struct SvgRasterBudget {
    // At most 16 Mi pixels (64 MiB at 32 bpp) and 128 image decodes per document.
    // Charge every reference, including invisible/zero-geometry image elements.
    qint64 pixels = 16 * qint64(1024 * 1024);
    int images = 128;
};

bool isEmbeddedRaster(QStringView reference, SvgRasterBudget *budget)
{
    // QtSvg tests the original spelling case-sensitively before decoding a
    // data URL. QUrl alone would normalize DATA: and authorize a filename read.
    if (!reference.startsWith(QLatin1String("data:")) || --budget->images < 0)
        return false;
    const QUrl url(reference.toString());
    if (url.scheme() != QLatin1String("data") || !url.host().isEmpty())
        return false;
    // Match Qt's data-URL decoding order: locate the comma before percent
    // decoding, then inspect the decoded media-type suffix for base64.
    const QByteArray encoded = url.toEncoded(QUrl::RemoveScheme);
    const qsizetype comma = encoded.indexOf(',');
    if (comma < 0)
        return false;
    const QByteArray type = QByteArray::fromPercentEncoding(encoded.first(comma)).trimmed();
    QByteArray bytes = QByteArray::fromPercentEncoding(encoded.mid(comma + 1));
    if (QLatin1StringView(type).endsWith(QLatin1String(";base64"), Qt::CaseInsensitive)) {
        const auto decoded = QByteArray::fromBase64Encoding(
            bytes, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded)
            return false;
        bytes = *decoded;
    }
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return false;
    QImageReader reader(&buffer);
    const QByteArray format = reader.format();
    // MIME types are not evidence: an image/png data URL can contain another
    // SVG, which would otherwise start an unrestricted recursive resource load.
    if (format != "png" && format != "jpeg" && format != "gif"
        && format != "webp" && format != "bmp" && format != "ico"
        && format != "tiff")
        return false;
    const QSize dimensions = reader.size();
    if (!dimensions.isValid() || dimensions.isEmpty())
        return false;
    const qint64 pixels = qint64(dimensions.width()) * dimensions.height();
    if (pixels > budget->pixels)
        return false;
    budget->pixels -= pixels;
    // QtSvg falls back to opening the href as a filename if data decoding
    // fails. Verify the raster is decodable, not only that its header sniffed.
    return !reader.read().isNull();
}

bool isSelfContainedSvg(const QByteArray &bytes)
{
    QXmlStreamReader xml(bytes);
    bool foundRoot = false;
    SvgRasterBudget rasterBudget;
    int depth = 0;
    int styleDepth = 0;
    QString styleText;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference)
            return false;
        if (token == QXmlStreamReader::ProcessingInstruction
            && xml.processingInstructionTarget().compare(
                   QLatin1String("xml-stylesheet"), Qt::CaseInsensitive) == 0)
            return false; // QtSvg reads external CSS files from this instruction.
        if (token == QXmlStreamReader::Characters && styleDepth > 0) {
            styleText.append(xml.text());
            continue;
        }
        if (token == QXmlStreamReader::EndElement) {
            if (depth == styleDepth) {
                if (!hasOnlyLocalCssUrls(styleText))
                    return false;
                styleDepth = 0;
                styleText.clear();
            }
            --depth;
            continue;
        }
        if (token != QXmlStreamReader::StartElement)
            continue;
        ++depth;
        if (styleDepth > 0)
            return false; // nested markup in a style element is unsupported
        if (xml.name() == QLatin1String("style"))
            styleDepth = depth;
        if (!foundRoot) {
            if (xml.name() != QLatin1String("svg"))
                return false;
            foundRoot = true;
        }
        for (const auto &attribute : xml.attributes()) {
            const QStringView value = attribute.value();
            if (attribute.name() == QLatin1String("href")) {
                const QStringView reference = value.trimmed();
                if (reference.isEmpty())
                    continue;
                // A fragment is local for <use>/gradients, but QtSvg's <image>
                // treats even "#id" as a filename rather than an SVG reference.
                if (xml.name() != QLatin1String("image")
                    && reference.startsWith(QLatin1Char('#')))
                    continue;
                if (xml.name() != QLatin1String("image")
                    || !isEmbeddedRaster(value, &rasterBudget))
                    return false;
            } else if (!hasOnlyLocalCssUrls(value)) {
                return false;
            }
        }
        // Do not use readElementText() here: it skips processing instructions
        // that QtSvg would still process (and can use to read a local CSS file).
    }
    // Compressed SVG is deliberately not passed on unchecked: the XML gate
    // cannot inspect it, so it is outside this self-contained SVG subset.
    return foundRoot && !xml.hasError();
}

} // namespace

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
    // size() already makes QtSvg parse the document and follow image/CSS
    // resources, so the resource boundary must be checked BEFORE that accessor.
    const bool svg = reader.format().startsWith("svg");
    if (svg && !isSelfContainedSvg(bytes))
        return {};
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
    if (!storedSize.isValid() || storedSize.isEmpty())
        return {}; // Without dimensions there is no pre-decode memory bound.
    if (!svg && qint64(storedSize.width()) * storedSize.height() > kMaxSourcePixels)
        return {};
    const bool transposed = reader.transformation().testFlag(
        QImageIOHandler::TransformationRotate90);
    const QSize sourceSize = transposed ? storedSize.transposed() : storedSize;
    // Respect one-axis sourceSize requests too. Unspecified axes impose no
    // additional restriction; an empty request still has the output budget.
    double scale = 1;
    if (requestedSize.width() > 0 || requestedSize.height() > 0) {
        scale = std::numeric_limits<double>::max();
        if (requestedSize.width() > 0)
            scale = double(requestedSize.width()) / sourceSize.width();
        if (requestedSize.height() > 0)
            scale = std::min(scale, double(requestedSize.height()) / sourceSize.height());
    }
    scale = std::min({scale,
                      double(kMaxDecodedDimension) / sourceSize.width(),
                      double(kMaxDecodedDimension) / sourceSize.height(),
                      std::sqrt(double(kMaxDecodedPixels) / sourceSize.width()
                                / sourceSize.height())});
    const QSize scaled(std::max(1, int(std::floor(sourceSize.width() * scale))),
                       std::max(1, int(std::floor(sourceSize.height() * scale))));
    reader.setScaledSize(transposed ? scaled.transposed() : scaled);

    QImage image = reader.read();
    if (image.isNull())
        return {}; // bytes are not an image format this build can decode
    if (image.width() > kMaxDecodedDimension || image.height() > kMaxDecodedDimension
        || qint64(image.width()) * image.height() > kMaxDecodedPixels)
        return {}; // A decoder must not ignore the requested output bound.
    if (size)
        *size = sourceSize;
    return image;
}

} // namespace ch
