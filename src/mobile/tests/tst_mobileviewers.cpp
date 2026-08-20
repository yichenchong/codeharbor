#include "MarkdownModel.h"
#include "MobileImageProvider.h"
#include "MobileViewerService.h"
#include "RpcTypes.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QUrl>
#include <QtTest/QtTest>

#include <cstring>
#include <memory>
#include <utility>

using ch::CodeharbordClient;
using ch::MobileImageCache;
using ch::MobileImageProvider;
using ch::MobileViewerService;

namespace {

QByteArray jsonLine(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

// A QIODevice the test drives byte for byte, standing in for the SSH RPC channel.
// The same technique as src/remote/tests/tst_rpcclient.cpp's ScriptedDevice,
// reduced to what these tests need: deliver a frame synchronously, and read back
// what the client wrote. No moc is needed — only inherited QIODevice signals are
// emitted.
class ScriptedDevice : public QIODevice {
public:
    ScriptedDevice() { open(QIODevice::ReadWrite); }

    void deliver(const QByteArray &bytes)
    {
        m_in += bytes;
        emit readyRead();
    }
    QByteArray takeWritten() { return std::exchange(m_out, QByteArray()); }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override
    {
        return m_in.size() + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 n = qMin<qint64>(maxSize, m_in.size());
        std::memcpy(data, m_in.constData(), size_t(n));
        m_in.remove(0, n);
        return n;
    }
    qint64 writeData(const char *data, qint64 maxSize) override
    {
        m_out.append(data, maxSize);
        return maxSize;
    }

private:
    QByteArray m_in;
    QByteArray m_out;
};

// A tiny real PNG, so the provider is exercised against bytes an image decoder
// actually accepts rather than a placeholder that would make a null QImage
// indistinguishable from a cache miss.
QByteArray pngBytes()
{
    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return buffer.data();
}

// A 400x200 JPEG carrying an EXIF orientation of 6 ("rotate 90"), i.e. exactly
// what a phone camera writes for a portrait photograph: the pixels are stored
// landscape and the viewer is expected to turn them. Built here rather than
// checked in as a binary, because the whole point is that the tag is present and
// a fixture file makes that invisible.
//
// The APP1 segment is assembled by hand: Qt's JPEG writer does not emit
// orientation metadata, so there is no way to ask for one.
QByteArray rotatedJpegBytes()
{
    QImage image(400, 200, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 90);
    const QByteArray jpeg = buffer.data();
    Q_ASSERT(jpeg.startsWith(QByteArrayLiteral("\xFF\xD8")));

    // "Exif\0\0", then a little-endian TIFF header, then a one-entry IFD holding
    // tag 0x0112 (Orientation) = 6, then a zero next-IFD offset.
    static const char kExif[] =
        "\x45\x78\x69\x66\x00\x00"          // "Exif\0\0"
        "\x49\x49\x2A\x00\x08\x00\x00\x00"  // "II", 42, first IFD at offset 8
        "\x01\x00"                          // one entry
        "\x12\x01\x03\x00\x01\x00\x00\x00"  // tag 0x0112, SHORT, count 1
        "\x06\x00\x00\x00"                  // value 6, padded to four bytes
        "\x00\x00\x00\x00";                 // no next IFD
    const QByteArray payload(kExif, sizeof(kExif) - 1);
    QByteArray app1(QByteArrayLiteral("\xFF\xE1"));
    const int length = int(payload.size()) + 2; // the length field counts itself
    app1.append(char((length >> 8) & 0xFF));    // big-endian, as JPEG segments are
    app1.append(char(length & 0xFF));
    app1.append(payload);
    return jpeg.left(2) + app1 + jpeg.mid(2);
}

} // namespace

class TstMobileViewers : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void cleanup();

    void readsAreBoundedByTheSameCapTheDesktopApplies();
    void utf8TextAndBase64BytesAreDecodedDifferently();
    void anUntrustworthyReadReplyIsRefusedRatherThanRendered();
    void aWriteEchoesItsRevisionAndReportsTheNewOne();
    void aWriteConfirmedWithoutARevisionIsAFailure();
    void aReadWithoutARevisionIsRefusedBeforeItBecomesABuffer();
    void aReplyLargerThanTheCapIsRefused();
    void aDirectoryListingIsOrderedDirectoriesFirstThenByName();
    void aMalformedDirectoryEntryFailsTheWholeListing();
    void aStatReplyIsForwardedAsAMapAndCheckedFirst();
    void resolvePathReportsOutsideTheRepositoryRoot();
    void aResolveReplyMissingTheFlagIsAFailureNotAFalse();
    void theImageHandshakeCachesBytesBeforeAnnouncingTheUrl();
    void aSecondRequestForACachedImageCostsNoRoundTrip();
    void anOverCapImageReadIsRefusedAndNothingIsCached();
    void theProviderReportsTheSizeTheCallerWillActuallyGet();
    void theImageProviderAnswersOnlyFromTheCache();
    void theImageCacheEvictsTheLeastRecentlyUsed();
    void imageUrlsAreOpaqueAndRoundTrip();
    void viewKindsSpeakTheDesktopVocabulary();
    void everyOperationDegradesWithoutATransport();
    void thePdfSpoolIsAppPrivateDigestNamedAndEmptied();
    void aReplyThatOutlivesTheServiceIsDropped();
    void aReleasedRequestStillInFlightLeavesNothingBehind();

private:
    // The one request the client has written since the last call. Fails the test
    // when there is not exactly one.
    QJsonObject takeRequest();
    void respondResult(qint64 id, const QJsonObject &result);
    void respondError(qint64 id, int code, const QString &message);

    ScriptedDevice *m_device = nullptr;
    CodeharbordClient *m_client = nullptr;
    MobileViewerService *m_service = nullptr;
};

void TstMobileViewers::initTestCase()
{
    // Keeps ch::MobileViewerService's PDF spool — and the purge it runs at
    // construction — inside a throwaway location instead of the developer's real
    // cache directory.
    QStandardPaths::setTestModeEnabled(true);
}

void TstMobileViewers::init()
{
    m_device = new ScriptedDevice;
    m_client = new CodeharbordClient;
    m_client->setTransport(m_device);
    m_service = new MobileViewerService(m_client);
}

void TstMobileViewers::cleanup()
{
    delete m_service;
    m_service = nullptr;
    delete m_client;
    m_client = nullptr;
    delete m_device;
    m_device = nullptr;
}

QJsonObject TstMobileViewers::takeRequest()
{
    const QList<QByteArray> lines = m_device->takeWritten().split('\n');
    QList<QJsonObject> requests;
    for (const QByteArray &line : lines) {
        if (line.isEmpty())
            continue;
        requests.append(QJsonDocument::fromJson(line).object());
    }
    if (requests.size() != 1) {
        QTest::qFail(qPrintable(QStringLiteral("expected exactly one request, saw %1")
                                    .arg(requests.size())),
                     __FILE__, __LINE__);
        return {};
    }
    return requests.first();
}

void TstMobileViewers::respondResult(qint64 id, const QJsonObject &result)
{
    m_device->deliver(jsonLine(
        {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}));
}

void TstMobileViewers::respondError(qint64 id, int code, const QString &message)
{
    m_device->deliver(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", id},
         {"error", QJsonObject{{"code", code}, {"message", message}}}}));
}

// The cap is not decoration: remote/src/files.ts derives `truncated` FROM the
// requested length, so an unranged read cannot report that the file was too big
// — and 8 MiB is the number the desktop's inline viewer and its editor both
// apply, so a file must not be viewable on one client and not the other.
void TstMobileViewers::readsAreBoundedByTheSameCapTheDesktopApplies()
{
    QSignalSpy reads(m_service, &MobileViewerService::fileRead);
    m_service->readFile(QStringLiteral("/repo/big.txt"));

    const QJsonObject request = takeRequest();
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodReadFile));
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("path")).toString(),
             QStringLiteral("/repo/big.txt"));
    QCOMPARE(params.value(QStringLiteral("offset")).toInteger(), qint64(0));
    QCOMPARE(params.value(QStringLiteral("length")).toInteger(),
             qint64(MobileViewerService::kMaxInlineReadBytes));
    QCOMPARE(qint64(MobileViewerService::kMaxInlineReadBytes),
             qint64(8 * 1024 * 1024));

    respondResult(request.value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "utf-8"},
                   {"content", "prefix"},
                   {"revision", "r1"},
                   {"truncated", true}});
    QCOMPARE(reads.size(), 1);
    // Forwarded VERBATIM, never inferred from the payload's length.
    QCOMPARE(reads.at(0).at(4).toBool(), true);
    QCOMPARE(reads.at(0).at(1).toString(), QStringLiteral("prefix"));
}

void TstMobileViewers::utf8TextAndBase64BytesAreDecodedDifferently()
{
    QSignalSpy reads(m_service, &MobileViewerService::fileRead);

    m_service->readFile(QStringLiteral("/repo/a.txt"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "utf-8"},
                   {"content", "héllo"},
                   {"revision", "r1"}});
    QCOMPARE(reads.size(), 1);
    QCOMPARE(reads.at(0).at(1).toString(), QStringLiteral("héllo"));
    QCOMPARE(reads.at(0).at(2).toBool(), false); // not binary
    QCOMPARE(reads.at(0).at(3).toString(), QStringLiteral("r1"));
    QCOMPARE(reads.at(0).at(4).toBool(), false);

    // A base64 reply means the daemon's STRICT UTF-8 decoder refused the file.
    // It is reported as binary with NO text: neither the base64 alphabet nor a
    // wall of U+FFFD may be shown as though it were the file.
    m_service->readFile(QStringLiteral("/repo/a.bin"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(
                                   QByteArray("\x00\x01\xff", 3).toBase64())},
                   {"revision", "r2"}});
    QCOMPARE(reads.size(), 2);
    QCOMPARE(reads.at(1).at(1).toString(), QString());
    QCOMPARE(reads.at(1).at(2).toBool(), true);
}

void TstMobileViewers::anUntrustworthyReadReplyIsRefusedRatherThanRendered()
{
    QSignalSpy reads(m_service, &MobileViewerService::fileRead);
    QSignalSpy errors(m_service, &MobileViewerService::fileError);

    // (1) An encoding this build does not know. Decoding it as UTF-8 anyway is
    // how a base64 payload ends up on screen as the base64 alphabet.
    m_service->readFile(QStringLiteral("/repo/x"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "latin-1"}, {"content", "abc"}});
    QCOMPARE(errors.size(), 1);

    // (2) A base64 payload that is not valid base64.
    m_service->readFile(QStringLiteral("/repo/y"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "base64"}, {"content", "!!!not base64!!!"}});
    QCOMPARE(errors.size(), 2);

    // (3) A `truncated` that is not a boolean. Coerced with toBool() it would
    // read as false and a PREFIX would be presented as the whole file.
    m_service->readFile(QStringLiteral("/repo/z"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "utf-8"},
                   {"content", "abc"},
                   {"truncated", "yes"}});
    QCOMPARE(errors.size(), 3);

    // (4) No content at all.
    m_service->readFile(QStringLiteral("/repo/w"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "utf-8"}});
    QCOMPARE(errors.size(), 4);

    // (5) A transport/server error is forwarded with the server's own wording.
    m_service->readFile(QStringLiteral("/repo/v"));
    respondError(takeRequest().value(QStringLiteral("id")).toInteger(),
                 ch::rpc::kResourceLimit,
                 QStringLiteral("file is larger than MAX_FILE_READ_BYTES"));
    QCOMPARE(errors.size(), 5);
    QCOMPARE(errors.at(4).at(1).toString(),
             QStringLiteral("file is larger than MAX_FILE_READ_BYTES"));

    // Not once did any of these produce a rendered file.
    QCOMPARE(reads.size(), 0);
}

void TstMobileViewers::aWriteEchoesItsRevisionAndReportsTheNewOne()
{
    QSignalSpy written(m_service, &MobileViewerService::fileWritten);
    QSignalSpy errors(m_service, &MobileViewerService::fileError);

    m_service->writeFile(QStringLiteral("/repo/a.txt"),
                         QStringLiteral("body"),
                         QStringLiteral("rev-opaque-1"));
    const QJsonObject request = takeRequest();
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodWriteFile));
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();
    // VERBATIM. Revision tokens are opaque server data (SPEC 8.4) and this client
    // never parses, derives or reformats one.
    QCOMPARE(params.value(QStringLiteral("expectedRevision")).toString(),
             QStringLiteral("rev-opaque-1"));
    QCOMPARE(params.value(QStringLiteral("content")).toString(),
             QStringLiteral("body"));
    QCOMPARE(params.value(QStringLiteral("encoding")).toString(),
             QStringLiteral("utf-8"));

    respondResult(request.value(QStringLiteral("id")).toInteger(),
                  {{"revision", "rev-opaque-2"}});
    QCOMPARE(written.size(), 1);
    QCOMPARE(written.at(0).at(1).toString(), QStringLiteral("rev-opaque-2"));

    // An empty baseline is SENT, not omitted. `file.writeFile` validates
    // `expectedRevision` with requireString (remote/src/files.ts), so omitting
    // it did not mean "unguarded" — it meant the request came back as a
    // JSON-RPC invalid-params error and a save of a file that had never been
    // loaded could not succeed at all. The empty token has a MEANING on the
    // server: create-only, which is precisely the guard such a buffer wants.
    m_service->writeFile(QStringLiteral("/repo/new.txt"), QStringLiteral("x"),
                         QString());
    const QJsonObject fresh = takeRequest();
    const QJsonObject freshParams =
        fresh.value(QStringLiteral("params")).toObject();
    QVERIFY(freshParams.contains(QStringLiteral("expectedRevision")));
    QCOMPARE(freshParams.value(QStringLiteral("expectedRevision")).toString(),
             QString());

    // A revision mismatch reaches the user through the generic error path with
    // the server's own message; nothing is retried and nothing is overwritten.
    respondError(fresh.value(QStringLiteral("id")).toInteger(),
                 ch::rpc::kRevisionMismatch,
                 QStringLiteral("the file changed on disk"));
    QCOMPARE(errors.size(), 1);
    QCOMPARE(errors.at(0).at(1).toString(),
             QStringLiteral("the file changed on disk"));
    QCOMPARE(written.size(), 1);
}

void TstMobileViewers::aWriteConfirmedWithoutARevisionIsAFailure()
{
    QSignalSpy written(m_service, &MobileViewerService::fileWritten);
    QSignalSpy errors(m_service, &MobileViewerService::fileError);

    m_service->writeFile(QStringLiteral("/repo/a.txt"), QStringLiteral("b"),
                         QStringLiteral("r1"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"ok", true}});
    // Reporting success would leave the pane guarding its next save with a
    // revision the server has already replaced, which is a silent overwrite
    // waiting to happen (SPEC 8.6).
    QCOMPARE(written.size(), 0);
    QCOMPARE(errors.size(), 1);
}

void TstMobileViewers::aDirectoryListingIsOrderedDirectoriesFirstThenByName()
{
    QSignalSpy listed(m_service, &MobileViewerService::directoryListed);
    m_service->listDirectory(QStringLiteral("/repo"));
    const QJsonObject request = takeRequest();
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodListDirectory));

    // Deliberately shuffled, and containing two names that differ only in case:
    // server order is unspecified, and "equal" names must still land in a stable
    // order rather than whatever std::sort happens to do.
    const QJsonArray entries{
        QJsonObject{{"name", "readme"}, {"kind", "file"}},
        QJsonObject{{"name", "src"}, {"kind", "directory"}},
        QJsonObject{{"name", "Readme"}, {"kind", "file"}},
        QJsonObject{{"name", "Makefile"}, {"kind", "file"}},
        QJsonObject{{"name", "docs"}, {"kind", "directory"}},
    };
    respondResult(request.value(QStringLiteral("id")).toInteger(),
                  {{"entries", entries}});
    QCOMPARE(listed.size(), 1);

    const QVariantList rows = listed.at(0).at(1).toList();
    QStringList names;
    for (const QVariant &row : rows)
        names.append(row.toMap().value(QStringLiteral("name")).toString());
    // Directories first, then case-insensitively by name with a case-sensitive
    // tie-break — byte for byte the desktop's ordering.
    QCOMPARE(names,
             QStringList({QStringLiteral("docs"), QStringLiteral("src"),
                          QStringLiteral("Makefile"), QStringLiteral("Readme"),
                          QStringLiteral("readme")}));
}

void TstMobileViewers::aMalformedDirectoryEntryFailsTheWholeListing()
{
    QSignalSpy listed(m_service, &MobileViewerService::directoryListed);
    QSignalSpy errors(m_service, &MobileViewerService::directoryError);

    m_service->listDirectory(QStringLiteral("/repo"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"entries",
                    QJsonArray{QJsonObject{{"name", "a"}, {"kind", "file"}},
                               QJsonObject{{"name", 42}}}}});
    // Showing the rows that happened to parse would present a partial directory
    // as a complete one.
    QCOMPARE(listed.size(), 0);
    QCOMPARE(errors.size(), 1);

    m_service->listDirectory(QStringLiteral("/repo"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"ok", true}});
    QCOMPARE(errors.size(), 2);
}

void TstMobileViewers::resolvePathReportsOutsideTheRepositoryRoot()
{
    QSignalSpy resolved(m_service, &MobileViewerService::pathResolved);

    m_service->resolvePath(QStringLiteral("/etc/hosts"),
                           QStringLiteral("/srv/repo"));
    const QJsonObject request = takeRequest();
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodResolvePath));
    QCOMPARE(request.value(QStringLiteral("params"))
                 .toObject()
                 .value(QStringLiteral("base"))
                 .toString(),
             QStringLiteral("/srv/repo"));

    respondResult(request.value(QStringLiteral("id")).toInteger(),
                  {{"path", "/etc/hosts"}, {"insideRepositoryRoot", false}});
    QCOMPARE(resolved.size(), 1);
    // The ASKED-for path is echoed, so a page can tell an answer about the file
    // it shows from one about the file it has navigated away from.
    QCOMPARE(resolved.at(0).at(0).toString(), QStringLiteral("/etc/hosts"));
    QCOMPARE(resolved.at(0).at(2).toBool(), false);

    // An empty base is OMITTED: the server would resolve it against its own
    // working directory, and everything is inside the filesystem root.
    m_service->resolvePath(QStringLiteral("/etc/hosts"), QString());
    QVERIFY(!takeRequest()
                 .value(QStringLiteral("params"))
                 .toObject()
                 .contains(QStringLiteral("base")));
}

void TstMobileViewers::aResolveReplyMissingTheFlagIsAFailureNotAFalse()
{
    QSignalSpy resolved(m_service, &MobileViewerService::pathResolved);
    QSignalSpy errors(m_service, &MobileViewerService::fileError);

    m_service->resolvePath(QStringLiteral("/repo/a"), QStringLiteral("/repo"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"path", "/repo/a"}});
    // toBool() on an absent value is false, which would mark every path as
    // outside the project — and in the markdown viewer that silently stops every
    // legitimate image from loading.
    QCOMPARE(resolved.size(), 0);
    QCOMPARE(errors.size(), 1);

    m_service->resolvePath(QStringLiteral("/repo/b"), QStringLiteral("/repo"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"insideRepositoryRoot", true}});
    QCOMPARE(resolved.size(), 0);
    QCOMPARE(errors.size(), 2);
}

// The handshake documented in MobileImageProvider.h. Getting the ORDER wrong is
// what deadlocks a render thread, so the order is what is asserted: bytes in the
// cache first, announcement second.
void TstMobileViewers::theImageHandshakeCachesBytesBeforeAnnouncingTheUrl()
{
    const QByteArray png = pngBytes();
    const QString path = QStringLiteral("/repo/doc/logo.png");

    bool cachedWhenAnnounced = false;
    connect(m_service, &MobileViewerService::imageReady, this,
            [&](const QString &, const QUrl &) {
                cachedWhenAnnounced =
                    !m_service->cachedImageBytes(path).isEmpty();
            });

    QSignalSpy ready(m_service, &MobileViewerService::imageReady);
    m_service->requestImage(path);
    const QJsonObject request = takeRequest();
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodReadFile));

    respondResult(request.value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(png.toBase64())},
                   {"truncated", false}});

    QCOMPARE(ready.size(), 1);
    QVERIFY2(cachedWhenAnnounced,
             "imageReady was emitted before the bytes were cached: the provider "
             "answers on a worker thread and cannot wait for them");
    QCOMPARE(m_service->cachedImageBytes(path), png);
    QCOMPARE(ready.at(0).at(1).toUrl(), m_service->imageUrl(path));
}

void TstMobileViewers::aSecondRequestForACachedImageCostsNoRoundTrip()
{
    const QString path = QStringLiteral("/repo/logo.png");
    m_service->requestImage(path);
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(pngBytes().toBase64())}});

    QSignalSpy ready(m_service, &MobileViewerService::imageReady);
    m_service->requestImage(path);
    QCOMPARE(ready.size(), 1); // answered synchronously, from the cache
    QCOMPARE(m_device->takeWritten(), QByteArray());

    // Handing the bytes back really does hand them back.
    m_service->forgetImage(path);
    QVERIFY(m_service->cachedImageBytes(path).isEmpty());
    m_service->requestImage(path);
    QVERIFY(!m_device->takeWritten().isEmpty());
}

void TstMobileViewers::anOverCapImageReadIsRefusedAndNothingIsCached()
{
    QSignalSpy ready(m_service, &MobileViewerService::imageReady);
    QSignalSpy failed(m_service, &MobileViewerService::imageError);

    const QString path = QStringLiteral("/repo/huge.png");
    m_service->requestImage(path);
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(pngBytes().toBase64())},
                   {"truncated", true}});

    // A prefix of an image is a corrupt image, not a smaller one. Refused,
    // exactly as InternalUrlSchemeHandler::TooLarge refuses it on the desktop.
    QCOMPARE(ready.size(), 0);
    QCOMPARE(failed.size(), 1);
    QVERIFY(m_service->cachedImageBytes(path).isEmpty());
    QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("8 MiB")));
}

void TstMobileViewers::theImageProviderAnswersOnlyFromTheCache()
{
    const QString path = QStringLiteral("/repo/pic with spaces.png");
    m_service->requestImage(path);
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(pngBytes().toBase64())}});
    QCOMPARE(m_service->cachedImageBytes(path), pngBytes());

    // The engine owns a provider; the cache is shared with it.
    MobileImageProvider *provider = m_service->createImageProvider();
    const QUrl url = m_service->imageUrl(path);
    QCOMPARE(url.host(), MobileImageProvider::providerId());

    // Qt hands the provider the id in QUrl's PrettyDecoded spelling. Both the
    // fully encoded and the pretty spelling must resolve to the same bytes.
    const QString encodedId = url.toString(QUrl::RemoveScheme
                                           | QUrl::RemoveAuthority)
                                  .mid(1);
    QSize size;
    const QImage decoded = provider->requestImage(encodedId, &size, QSize());
    QVERIFY(!decoded.isNull());
    QCOMPARE(size, QSize(4, 4));

    // A MISS is a null image, never a fetch: nothing may put this thread on the
    // network.
    QSize missSize;
    const QImage miss =
        provider->requestImage(QStringLiteral("%2Fnot%2Fcached.png"), &missSize,
                               QSize());
    QVERIFY(miss.isNull());
    QCOMPARE(m_device->takeWritten(), QByteArray());

    // Bytes that are not an image at all also decode to null rather than to
    // something the view would draw. Fed through a provider of their own, over a
    // cache built by hand, so the failure is the DECODE and not a miss.
    auto junkCache = std::make_shared<MobileImageCache>();
    junkCache->insert(QStringLiteral("/x/not-an-image"),
                      QByteArrayLiteral("<html>this is not a picture</html>"));
    MobileImageProvider junkProvider(junkCache);
    QSize junkSize;
    QVERIFY(junkProvider
                .requestImage(QStringLiteral("%2Fx%2Fnot-an-image"), &junkSize,
                              QSize())
                .isNull());

    delete provider;
}

void TstMobileViewers::theImageCacheEvictsTheLeastRecentlyUsed()
{
    MobileImageCache cache;
    // One byte per entry, so only the ENTRY bound can bite here.
    for (int i = 0; i <= MobileImageCache::kMaxEntries; ++i) {
        cache.insert(QStringLiteral("/p/%1").arg(i), QByteArrayLiteral("x"));
    }
    QCOMPARE(cache.size(), MobileImageCache::kMaxEntries);
    QVERIFY(!cache.contains(QStringLiteral("/p/0"))); // the oldest went
    QVERIFY(cache.contains(
        QStringLiteral("/p/%1").arg(MobileImageCache::kMaxEntries)));

    // Reading an entry makes it recent, so it survives the next eviction while a
    // never-touched neighbour does not.
    cache.clear();
    cache.insert(QStringLiteral("/a"), QByteArrayLiteral("1"));
    cache.insert(QStringLiteral("/b"), QByteArrayLiteral("2"));
    for (int i = 0; i < MobileImageCache::kMaxEntries; ++i) {
        QCOMPARE(cache.bytes(QStringLiteral("/a")), QByteArrayLiteral("1"));
        cache.insert(QStringLiteral("/filler/%1").arg(i),
                     QByteArrayLiteral("f"));
    }
    QVERIFY(cache.contains(QStringLiteral("/a")));
    QVERIFY(!cache.contains(QStringLiteral("/b")));

    // The BYTE bound is separate, and one oversized entry is retained rather
    // than evicted the instant it arrives — an image that was read successfully
    // has to be displayable.
    cache.clear();
    cache.insert(QStringLiteral("/huge"),
                 QByteArray(int(MobileImageCache::kMaxBytes) + 1024, 'z'));
    QCOMPARE(cache.size(), 1);
    QVERIFY(cache.byteSize() > MobileImageCache::kMaxBytes);
    // ...and the next insert retires it.
    cache.insert(QStringLiteral("/small"), QByteArrayLiteral("s"));
    QVERIFY(!cache.contains(QStringLiteral("/huge")));
    QVERIFY(cache.contains(QStringLiteral("/small")));
}

void TstMobileViewers::imageUrlsAreOpaqueAndRoundTrip()
{
    // Total percent-encoding: the path is ONE opaque component, so nothing in the
    // URL machinery can read part of it as a host, a query or a fragment.
    const QUrl url =
        m_service->imageUrl(QStringLiteral("/repo/a b/notes#1?x.png"));
    QCOMPARE(url.scheme(), QStringLiteral("image"));
    QCOMPARE(url.host(), QStringLiteral("chremote"));
    QVERIFY(!url.hasFragment());
    QVERIFY(!url.hasQuery());

    // An empty path yields no address at all, so an Image cannot be bound to a
    // provider URL that names nothing.
    QVERIFY(m_service->imageUrl(QString()).isEmpty());

    // file:// <-> path, the one conversion every page shares.
    const QUrl fileUrl = m_service->fileUrlFor(QStringLiteral("/repo/a b/x#1"));
    QCOMPARE(fileUrl.scheme(), QStringLiteral("file"));
    QCOMPARE(m_service->remotePathFor(fileUrl), QStringLiteral("/repo/a b/x#1"));
    // Anything that is not a remote file URL has no remote path.
    QCOMPARE(m_service->remotePathFor(QUrl(QStringLiteral("https://example.com/a"))),
             QString());
}

void TstMobileViewers::viewKindsSpeakTheDesktopVocabulary()
{
    const auto kind = [this](const QString &url) {
        return m_service->viewKindFor(QUrl(url));
    };
    QCOMPARE(kind(QStringLiteral("file:///repo/README.md")),
             QStringLiteral("markdown"));
    QCOMPARE(kind(QStringLiteral("file:///repo/main.cpp")),
             QStringLiteral("text"));
    QCOMPARE(kind(QStringLiteral("file:///repo/logo.png")),
             QStringLiteral("image"));
    QCOMPARE(kind(QStringLiteral("file:///repo/spec.pdf")),
             QStringLiteral("pdf"));
    QCOMPARE(kind(QStringLiteral("file:///repo/src/")),
             QStringLiteral("directory"));
    QCOMPARE(kind(QStringLiteral("https://example.com/docs")),
             QStringLiteral("web"));
    // Download AND Error both become "binary": SPEC 7.5's unclaimed-resource
    // disposition, which on mobile is the legibly-unsupported page.
    QCOMPARE(kind(QStringLiteral("file:///repo/a.o")), QStringLiteral("binary"));
    QCOMPARE(kind(QStringLiteral("gopher://example.com/x")),
             QStringLiteral("binary"));
}

void TstMobileViewers::everyOperationDegradesWithoutATransport()
{
    // The client is BORROWED and may be destroyed first; every operation must
    // then report a failure rather than crash or hang a page for ever.
    MobileViewerService orphan(nullptr);
    QSignalSpy fileErrors(&orphan, &MobileViewerService::fileError);
    QSignalSpy dirErrors(&orphan, &MobileViewerService::directoryError);
    QSignalSpy imageErrors(&orphan, &MobileViewerService::imageError);
    QSignalSpy pdfErrors(&orphan, &MobileViewerService::pdfError);

    orphan.readFile(QStringLiteral("/a"));
    orphan.writeFile(QStringLiteral("/a"), QStringLiteral("b"), QString());
    orphan.stat(QStringLiteral("/a"));
    orphan.resolvePath(QStringLiteral("/a"), QStringLiteral("/"));
    orphan.listDirectory(QStringLiteral("/a"));
    orphan.requestImage(QStringLiteral("/a.png"));
    orphan.requestPdf(QStringLiteral("/a.pdf"));

    QCOMPARE(fileErrors.size(), 4);
    QCOMPARE(dirErrors.size(), 1);
    QCOMPARE(imageErrors.size(), 1);
    QCOMPARE(pdfErrors.size(), 1);
}

// A read whose reply carries no revision is refused. The page would otherwise
// load an editable buffer whose guard token is empty, and an empty token means
// CREATE-ONLY to file.writeFile — so the user's first save would come back as
// "file already exists" long after they typed it.
void TstMobileViewers::aReadWithoutARevisionIsRefusedBeforeItBecomesABuffer()
{
    QSignalSpy reads(m_service, &MobileViewerService::fileRead);
    QSignalSpy errors(m_service, &MobileViewerService::fileError);

    m_service->readFile(QStringLiteral("/repo/a.txt"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "utf-8"}, {"content", "body"}});
    QCOMPARE(reads.size(), 0);
    QCOMPARE(errors.size(), 1);

    // A non-string revision is the same failure, not a coercion.
    m_service->readFile(QStringLiteral("/repo/b.txt"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "utf-8"}, {"content", "body"}, {"revision", 7}});
    QCOMPARE(reads.size(), 0);
    QCOMPARE(errors.size(), 2);

    // An EMPTY file is still a perfectly good read, as long as it is guarded.
    m_service->readFile(QStringLiteral("/repo/empty.txt"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "utf-8"}, {"content", ""}, {"revision", "r9"}});
    QCOMPARE(reads.size(), 1);
    QCOMPARE(reads.at(0).at(1).toString(), QString());
    QCOMPARE(reads.at(0).at(3).toString(), QStringLiteral("r9"));

    // The image and PDF paths never save anything, so they are deliberately NOT
    // held to the revision rule: a picture with no revision still displays.
    QSignalSpy ready(m_service, &MobileViewerService::imageReady);
    m_service->requestImage(QStringLiteral("/repo/logo.png"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(pngBytes().toBase64())}});
    QCOMPARE(ready.size(), 1);
}

// The 8 MiB cap is asked for in the request AND checked in the reply. Nothing
// obliges a peer to honour `length`, and MobileImageCache's byte bound is
// defined as three times this number — a reply that ignored the window would
// walk straight through a bound this client documents as absolute.
void TstMobileViewers::aReplyLargerThanTheCapIsRefused()
{
    QSignalSpy reads(m_service, &MobileViewerService::fileRead);
    QSignalSpy errors(m_service, &MobileViewerService::fileError);

    // ASCII, so one QString character is exactly one UTF-8 byte and the payload
    // is one byte over the cap rather than approximately over it.
    const QString oversized(MobileViewerService::kMaxInlineReadBytes + 1,
                            QLatin1Char('a'));
    m_service->readFile(QStringLiteral("/repo/huge.txt"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "utf-8"},
                   {"content", oversized},
                   {"revision", "r1"}});
    QCOMPARE(reads.size(), 0);
    QCOMPARE(errors.size(), 1);

    // Exactly AT the cap is the largest legitimate reply and must still pass.
    const QString atCap(MobileViewerService::kMaxInlineReadBytes,
                        QLatin1Char('a'));
    m_service->readFile(QStringLiteral("/repo/big.txt"));
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "utf-8"},
                   {"content", atCap},
                   {"revision", "r2"}});
    QCOMPARE(reads.size(), 1);
    QCOMPARE(errors.size(), 1);
}

void TstMobileViewers::aStatReplyIsForwardedAsAMapAndCheckedFirst()
{
    QSignalSpy stated(m_service, &MobileViewerService::stated);
    QSignalSpy errors(m_service, &MobileViewerService::fileError);

    m_service->stat(QStringLiteral("/repo/a.txt"));
    const QJsonObject request = takeRequest();
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodStat));
    respondResult(request.value(QStringLiteral("id")).toInteger(),
                  {{"path", "/repo/a.txt"},
                   {"kind", "file"},
                   {"size", 12},
                   {"revision", "r1"}});
    QCOMPARE(stated.size(), 1);
    const QVariantMap info = stated.at(0).at(1).toMap();
    QCOMPARE(info.value(QStringLiteral("kind")).toString(),
             QStringLiteral("file"));
    QCOMPARE(info.value(QStringLiteral("size")).toInt(), 12);

    // A reply that is not an object at all is a failure, not an empty map: a
    // page cannot tell "no information" from "a file of size 0".
    m_service->stat(QStringLiteral("/repo/b.txt"));
    m_device->deliver(jsonLine(
        {{"jsonrpc", "2.0"},
         {"id", takeRequest().value(QStringLiteral("id")).toInteger()},
         {"result", QJsonValue(QJsonValue::Null)}}));
    QCOMPARE(stated.size(), 1);
    QCOMPARE(errors.size(), 1);
}

// The provider must report the size the CALLER will actually get. Qt Quick uses
// it for Image.sourceSize and its own layout arithmetic, and QImageReader::size()
// reports the size as STORED — which for a portrait phone photograph (stored
// landscape with an EXIF "rotate 90" tag, the single most common image on a
// phone) is the transpose of what read() returns.
void TstMobileViewers::theProviderReportsTheSizeTheCallerWillActuallyGet()
{
    const QString path = QStringLiteral("/repo/photo.jpg");
    m_service->requestImage(path);
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(rotatedJpegBytes().toBase64())}});
    QCOMPARE(m_service->cachedImageBytes(path), rotatedJpegBytes());

    // The provider minted by the service shares the service's cache; the engine
    // would own it, so this test owns it in the engine's place.
    std::unique_ptr<MobileImageProvider> provider(
        m_service->createImageProvider());
    const QString id = m_service->imageUrl(path)
                           .toString(QUrl::RemoveScheme | QUrl::RemoveAuthority)
                           .mid(1);

    // Unscaled: the reported size is the size of the image handed back, not the
    // 400x200 the JPEG stores.
    QSize size;
    QImage decoded = provider->requestImage(id, &size, QSize());
    QCOMPARE(decoded.size(), QSize(200, 400));
    QCOMPARE(size, QSize(200, 400));

    // Scaled: the fit is computed against the size the caller sees, and the
    // result really does fit inside what was asked for. Against the stored size
    // the arithmetic produced 40x80 for this request — twice the height.
    decoded = provider->requestImage(id, &size, QSize(100, 40));
    QCOMPARE(decoded.size(), QSize(20, 40));
    QVERIFY(decoded.width() <= 100 && decoded.height() <= 40);
    // ...while `size` stays the SOURCE size, which is its contract: reporting
    // the scaled size would make the view ask for smaller and smaller renders.
    QCOMPARE(size, QSize(200, 400));
}

// The spool is app-private, digest-named, and emptied when the page lets go.
void TstMobileViewers::thePdfSpoolIsAppPrivateDigestNamedAndEmptied()
{
    const QString path = QStringLiteral("/repo/docs/../secret report.pdf");
    QSignalSpy ready(m_service, &MobileViewerService::pdfReady);

    m_service->requestPdf(path);
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(
                                   QByteArrayLiteral("%PDF-1.7 body").toBase64())},
                   {"truncated", false}});
    QCOMPARE(ready.size(), 1);

    const QString spooled = ready.at(0).at(1).toUrl().toLocalFile();
    QVERIFY(QFile::exists(spooled));
    QCOMPARE(QFile(spooled).size(), qint64(13));

    // Under the app's own cache location, which on Android is inside the app's
    // cache dir and on iOS inside the app container. Nothing else on the device
    // reads from there.
    const QString cache =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QVERIFY(!cache.isEmpty());
    QVERIFY(spooled.startsWith(cache + QLatin1String("/pdf-spool/")));

    // Named by DIGEST, so a server-controlled name cannot choose a filename on
    // this device: no "..", no spaces, no "secret report" to read off a file
    // listing.
    const QString expected = QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
    QCOMPARE(QFileInfo(spooled).fileName(),
             expected + QLatin1String(".pdf"));

    // A second request for the same document is answered from the spool, with no
    // round trip.
    m_service->requestPdf(path);
    QCOMPARE(ready.size(), 2);
    QCOMPARE(m_device->takeWritten(), QByteArray());

    m_service->releasePdf(path);
    QVERIFY(!QFile::exists(spooled));

    // And the destructor is the backstop for a page that never released.
    m_service->requestPdf(path);
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(
                                   QByteArrayLiteral("%PDF-1.7 body").toBase64())}});
    QVERIFY(QFile::exists(spooled));
    delete m_service;
    m_service = nullptr;
    QVERIFY(!QFile::exists(spooled));
}

// A reply that arrives after the service is gone must be dropped, not delivered
// to freed memory. CodeharbordClient guarantees every callback runs exactly once
// — destruction included — so this path is reached in ordinary teardown, not
// only in theory.
void TstMobileViewers::aReplyThatOutlivesTheServiceIsDropped()
{
    m_service->readFile(QStringLiteral("/repo/a.txt"));
    const qint64 id = takeRequest().value(QStringLiteral("id")).toInteger();
    delete m_service;
    m_service = nullptr;
    respondResult(id, {{"encoding", "utf-8"},
                       {"content", "body"},
                       {"revision", "r1"}});
    // Reaching here without a crash IS the assertion; the QPointer guard is what
    // makes it hold.
    QVERIFY(true);
}

// A page that retargets its pane while a read is still in flight calls the
// release path for a path that has NOTHING written for it yet. The reply then
// lands, and used to spool a file — up to 8 MiB — that no page held any more and
// that nothing would ever release, so it sat in the app cache until the process
// exited. One per document skipped while swiping through a directory.
void TstMobileViewers::aReleasedRequestStillInFlightLeavesNothingBehind()
{
    const QString pdf = QStringLiteral("/repo/a.pdf");
    QSignalSpy pdfReady(m_service, &MobileViewerService::pdfReady);
    QSignalSpy pdfErrors(m_service, &MobileViewerService::pdfError);

    m_service->requestPdf(pdf);
    const qint64 abandoned = takeRequest().value(QStringLiteral("id")).toInteger();
    m_service->releasePdf(pdf); // the pane has been retargeted
    respondResult(abandoned,
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(
                                   QByteArrayLiteral("%PDF-1.7 x").toBase64())}});
    // Nothing announced, nothing failed, and — the point — nothing written.
    QCOMPARE(pdfReady.size(), 0);
    QCOMPARE(pdfErrors.size(), 0);
    const QString spoolDir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QLatin1String("/pdf-spool");
    QCOMPARE(QDir(spoolDir).entryList(QDir::Files).size(), 0);

    // The awkward ordering: released, requested AGAIN, and only THEN does the
    // first reply land. The stale reply must not delete the file the live
    // request needs, and the live request must still get one.
    m_service->requestPdf(pdf);
    const qint64 first = takeRequest().value(QStringLiteral("id")).toInteger();
    m_service->releasePdf(pdf);
    m_service->requestPdf(pdf);
    const qint64 second = takeRequest().value(QStringLiteral("id")).toInteger();
    respondResult(first, {{"encoding", "base64"},
                          {"content", QString::fromLatin1(
                                          QByteArrayLiteral("stale").toBase64())}});
    QCOMPARE(pdfReady.size(), 0); // the stale reply is nobody's
    respondResult(second,
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(
                                   QByteArrayLiteral("%PDF-1.7 live").toBase64())}});
    QCOMPARE(pdfReady.size(), 1);
    const QString spooled = pdfReady.at(0).at(1).toUrl().toLocalFile();
    QVERIFY(QFile::exists(spooled));
    QCOMPARE(QFile(spooled).size(), qint64(13));
    m_service->releasePdf(pdf);

    // The image cache has the same shape and the same fix: forgetImage() while
    // the read is in flight means the reply must not fill the cache behind the
    // page's back. Bounded by the LRU rather than unbounded, but 16 abandoned
    // photographs is still up to 24 MiB resident on a phone.
    const QString png = QStringLiteral("/repo/a.png");
    QSignalSpy imageReady(m_service, &MobileViewerService::imageReady);
    m_service->requestImage(png);
    const qint64 imageId = takeRequest().value(QStringLiteral("id")).toInteger();
    m_service->forgetImage(png);
    respondResult(imageId,
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(pngBytes().toBase64())}});
    QCOMPARE(imageReady.size(), 0);
    QVERIFY(m_service->cachedImageBytes(png).isEmpty());
    QCOMPARE(m_service->imageCache().size(), 0);

    // ...and a re-request after the release still ends up with usable bytes.
    m_service->requestImage(png);
    respondResult(takeRequest().value(QStringLiteral("id")).toInteger(),
                  {{"encoding", "base64"},
                   {"content", QString::fromLatin1(pngBytes().toBase64())}});
    QCOMPARE(imageReady.size(), 1);
    QCOMPARE(m_service->cachedImageBytes(png), pngBytes());
}

// QGuiApplication, not QCoreApplication: MobileImageProvider decodes into a
// QImage. Offscreen is set by the test's CMake ENVIRONMENT so no display is
// needed.
QTEST_MAIN(TstMobileViewers)
#include "tst_mobileviewers.moc"
