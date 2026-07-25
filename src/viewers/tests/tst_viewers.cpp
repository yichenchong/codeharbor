#include "InternalUrlSchemeHandler.h"
#include "ViewerHandlerRegistry.h"
#include "ViewerProfiles.h"

#include <QtWebEngineQuick/QQuickWebEngineProfile>

#include <QGuiApplication>
#include <QString>
#include <QUrl>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QWebEngineUrlScheme>
#include <QtTest/QtTest>
#include <QtWebEngineQuick/QtWebEngineQuick>

using ch::InternalUrlMap;
using ch::InternalUrlSchemeHandler;
using ch::ViewerHandlerRegistry;
using ch::ViewerProfiles;
using ch::ViewerResolution;

// Unit tests for the viewer handler registry (SPEC 7.5), the file <-> internal
// URL mapping (SPEC 7.4), and the external/internal WebEngine profile isolation
// invariant (M3). The registry and URL-mapping assertions require no WebEngine
// runtime and run unconditionally; only the live profile checks may QSKIP.
class TstViewers : public QObject {
    Q_OBJECT
private slots:
    void resolveByExtensionTable();
    void resolveUrlTable();
    void urlMappingRoundTrip();
    void urlMappingStableAndDistinct();
    void urlMappingIdsUnguessable();
    void urlMappingLruEviction();
    void mimeForPathByExtension();
    void profileIsolation();
    void schemeFlags();
    void activeContentMimeGate();
    void activeContentMimeFromExtension();
    void urlMappingRemintAfterEviction();
};

void TstViewers::resolveByExtensionTable()
{
    // Markdown -> internal HTML renderer.
    QCOMPARE(ViewerHandlerRegistry::resolveByExtension(QStringLiteral("md")),
             ViewerResolution::InternalHtmlRenderer);
    QCOMPARE(ViewerHandlerRegistry::resolveByExtension(QStringLiteral("markdown")),
             ViewerResolution::InternalHtmlRenderer);

    // Source / text / structured -> text editor.
    for (const QString &ext :
         {QStringLiteral("txt"), QStringLiteral("ts"), QStringLiteral("tsx"),
          QStringLiteral("js"), QStringLiteral("mjs"), QStringLiteral("c"),
          QStringLiteral("cc"), QStringLiteral("cpp"), QStringLiteral("h"),
          QStringLiteral("hpp"), QStringLiteral("py"), QStringLiteral("rs"),
          QStringLiteral("go"), QStringLiteral("sh"), QStringLiteral("css"),
          QStringLiteral("html"), QStringLiteral("json"), QStringLiteral("yaml"),
          QStringLiteral("yml"), QStringLiteral("toml"), QStringLiteral("xml")}) {
        QCOMPARE(ViewerHandlerRegistry::resolveByExtension(ext),
                 ViewerResolution::TextEditor);
    }

    // Images.
    for (const QString &ext :
         {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
          QStringLiteral("gif"), QStringLiteral("svg"), QStringLiteral("webp")}) {
        QCOMPARE(ViewerHandlerRegistry::resolveByExtension(ext),
                 ViewerResolution::ImageViewer);
    }

    // PDF.
    QCOMPARE(ViewerHandlerRegistry::resolveByExtension(QStringLiteral("pdf")),
             ViewerResolution::PdfViewer);

    // Case insensitivity.
    QCOMPARE(ViewerHandlerRegistry::resolveByExtension(QStringLiteral("PNG")),
             ViewerResolution::ImageViewer);

    // Unknown / known-binary -> download.
    QCOMPARE(ViewerHandlerRegistry::resolveByExtension(QStringLiteral("bin")),
             ViewerResolution::Download);
    QCOMPARE(ViewerHandlerRegistry::resolveByExtension(QStringLiteral("exe")),
             ViewerResolution::Download);
    QCOMPARE(ViewerHandlerRegistry::resolveByExtension(QString()),
             ViewerResolution::Download);
}

void TstViewers::resolveUrlTable()
{
    // Schemes first.
    QCOMPARE(ViewerHandlerRegistry::resolve(QUrl(QStringLiteral("http://example.com/x"))),
             ViewerResolution::DirectWebNavigation);
    QCOMPARE(ViewerHandlerRegistry::resolve(QUrl(QStringLiteral("https://example.com/x"))),
             ViewerResolution::DirectWebNavigation);

    // Opaque internal URL (no extension) -> internal HTML renderer.
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("codeharbor-internal://file/f1"))),
             ViewerResolution::InternalHtmlRenderer);
    // Internal URL that carries a recognizable extension resolves by it.
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("codeharbor-internal://file/f1.png"))),
             ViewerResolution::ImageViewer);

    // file:// by extension across every class.
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/README.md"))),
             ViewerResolution::InternalHtmlRenderer);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/main.cpp"))),
             ViewerResolution::TextEditor);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/config.toml"))),
             ViewerResolution::TextEditor);
    // XML is source/structured data (not an unpreviewable binary download).
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/data.xml"))),
             ViewerResolution::TextEditor);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/logo.svg"))),
             ViewerResolution::ImageViewer);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/manual.pdf"))),
             ViewerResolution::PdfViewer);

    // Trailing slash -> directory.
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/project/"))),
             ViewerResolution::DirectoryViewer);

    // Extensionless / known-binary file -> download.
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/Makefile"))),
             ViewerResolution::Download);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/archive.zip"))),
             ViewerResolution::Download);

    // Unknown scheme -> error.
    QCOMPARE(ViewerHandlerRegistry::resolve(QUrl(QStringLiteral("ftp://host/x"))),
             ViewerResolution::Error);
}

void TstViewers::urlMappingRoundTrip()
{
    InternalUrlMap map;
    const QList<QUrl> files = {
        QUrl(QStringLiteral("file:///home/yc/project/README.md")),
        QUrl(QStringLiteral("file:///home/yc/project/src/main.cpp")),
        QUrl(QStringLiteral("file:///tmp/space in name.txt")),
        QUrl::fromLocalFile(QStringLiteral("/etc/hosts")),
    };
    for (const QUrl &file : files) {
        const QString internal = map.internalUrlFor(file);
        QVERIFY(internal.startsWith(InternalUrlMap::prefix()));
        // fileUrlFor is the exact inverse of internalUrlFor.
        QCOMPARE(map.fileUrlFor(internal), file);
    }

    // Unknown id resolves to an invalid URL.
    QVERIFY(!map.fileUrlFor(QStringLiteral("codeharbor-internal://file/unknown"))
                 .isValid());
}

void TstViewers::urlMappingStableAndDistinct()
{
    InternalUrlMap map;
    const QUrl a(QStringLiteral("file:///a.txt"));
    const QUrl b(QStringLiteral("file:///b.txt"));

    const QString a1 = map.internalUrlFor(a);
    const QString a2 = map.internalUrlFor(a);
    const QString b1 = map.internalUrlFor(b);

    // Stable id for the same file.
    QCOMPARE(a1, a2);
    // Distinct ids for distinct files.
    QVERIFY(a1 != b1);
    // Both remain individually invertible.
    QCOMPARE(map.fileUrlFor(a1), a);
    QCOMPARE(map.fileUrlFor(b1), b);
}

void TstViewers::urlMappingIdsUnguessable()
{
    // Ids must be unguessable, high-entropy tokens (not the old sequential
    // f1,f2,... counter) so a compromised page cannot enumerate other opened
    // files by walking ids (SPEC 7.4). They remain injective and invertible.
    InternalUrlMap map;
    const QString prefix = InternalUrlMap::prefix();
    const QRegularExpression sequential(QStringLiteral("^f\\d+$"));
    const QRegularExpression token128(QStringLiteral("^[0-9a-f]{32}$"));
    QList<QString> ids;
    for (int i = 0; i < 8; ++i) {
        const QUrl f(QStringLiteral("file:///dir/file%1.bin").arg(i));
        const QString internal = map.internalUrlFor(f);
        QVERIFY(internal.startsWith(prefix));
        const QString id = internal.mid(prefix.size());
        // Never the guessable sequential scheme.
        QVERIFY2(!sequential.match(id).hasMatch(),
                 qPrintable(QStringLiteral("sequential id leaked: %1").arg(id)));
        // A 128-bit random token (QUuid::Id128 -> 32 lowercase hex chars).
        QVERIFY2(token128.match(id).hasMatch(),
                 qPrintable(QStringLiteral("id not a 128-bit token: %1").arg(id)));
        // Still invertible.
        QCOMPARE(map.fileUrlFor(internal), f);
        ids.append(id);
    }
    // Injective: every id is distinct.
    QCOMPARE(QSet<QString>(ids.cbegin(), ids.cend()).size(), ids.size());
}

void TstViewers::urlMappingLruEviction()
{
    // The map is LRU-bounded so it never grows without limit; minting past the
    // cap evicts the least-recently-used entry (SPEC 7.4 follow-up).
    const QUrl a(QStringLiteral("file:///a.txt"));
    const QUrl b(QStringLiteral("file:///b.txt"));
    const QUrl c(QStringLiteral("file:///c.txt"));
    const QUrl d(QStringLiteral("file:///d.txt"));

    InternalUrlMap map(3);
    QCOMPARE(map.maxEntries(), 3);
    const QString ia = map.internalUrlFor(a);
    const QString ib = map.internalUrlFor(b);
    const QString ic = map.internalUrlFor(c);
    QCOMPARE(map.size(), 3);

    // A fourth mint pushes over the cap: the least-recently-used (a) is evicted.
    const QString id = map.internalUrlFor(d);
    QCOMPARE(map.size(), 3); // still bounded
    QVERIFY(!map.fileUrlFor(ia).isValid());
    QCOMPARE(map.fileUrlFor(ib), b);
    QCOMPARE(map.fileUrlFor(ic), c);
    QCOMPARE(map.fileUrlFor(id), d);

    // Accessing an entry marks it recently-used, so it survives the next
    // eviction while a stale neighbour is dropped instead.
    InternalUrlMap map2(3);
    const QString ja = map2.internalUrlFor(a);
    const QString jb = map2.internalUrlFor(b);
    const QString jc = map2.internalUrlFor(c);
    QCOMPARE(map2.fileUrlFor(ja), a); // bump a to most-recently-used
    map2.internalUrlFor(d);           // evicts LRU (b), not a
    QVERIFY(!map2.fileUrlFor(jb).isValid());
    QCOMPARE(map2.fileUrlFor(ja), a);
    QCOMPARE(map2.fileUrlFor(jc), c);
}

void TstViewers::schemeFlags()
{
    // The internal scheme is registered in main() before WebEngine init. It
    // must NOT carry LocalAccessAllowed: the privileged origin serves every
    // resource via CodeharbordClient/readFile and must never reach client
    // file:// resources (SPEC 2.4/7). It stays a secure origin.
    const QWebEngineUrlScheme s =
        QWebEngineUrlScheme::schemeByName(QByteArrayLiteral("codeharbor-internal"));
    QCOMPARE(s.name(), QByteArrayLiteral("codeharbor-internal"));
    QVERIFY(!s.flags().testFlag(QWebEngineUrlScheme::LocalAccessAllowed));
    QVERIFY(s.flags().testFlag(QWebEngineUrlScheme::SecureScheme));
}

void TstViewers::mimeForPathByExtension()
{
    // MIME is derived from the extension only (no filesystem access), so these
    // are stable regardless of whether the file exists. Drives the content-type
    // the internal scheme handler serves to Chromium.
    QCOMPARE(InternalUrlSchemeHandler::mimeForPath(QStringLiteral("/a/logo.png")),
             QByteArrayLiteral("image/png"));
    QCOMPARE(InternalUrlSchemeHandler::mimeForPath(QStringLiteral("/a/logo.svg")),
             QByteArrayLiteral("image/svg+xml"));
    QCOMPARE(InternalUrlSchemeHandler::mimeForPath(QStringLiteral("/a/doc.pdf")),
             QByteArrayLiteral("application/pdf"));
    QCOMPARE(InternalUrlSchemeHandler::mimeForPath(QStringLiteral("/a/notes.txt")),
             QByteArrayLiteral("text/plain"));
    // Case-insensitive extension matching.
    QCOMPARE(InternalUrlSchemeHandler::mimeForPath(QStringLiteral("/a/LOGO.PNG")),
             QByteArrayLiteral("image/png"));
    // Unknown extension falls back to the binary octet stream.
    QCOMPARE(
        InternalUrlSchemeHandler::mimeForPath(QStringLiteral("/a/blob.zzqq")),
        QByteArrayLiteral("application/octet-stream"));
}

void TstViewers::profileIsolation()
{
    // Profile construction needs a GUI application + WebEngine; skip only this
    // live part when unavailable (the registry / URL-mapping asserts above are
    // unconditional).
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        QSKIP("WebEngine profiles require a QGuiApplication");

    ViewerProfiles profiles(nullptr);
    QQuickWebEngineProfile *external = profiles.externalProfile();
    QQuickWebEngineProfile *internal = profiles.internalProfile();
    QVERIFY(external != nullptr);
    QVERIFY(internal != nullptr);

    // The profiles MUST be the QML profile type so `WebEngineView.profile:
    // viewers.internalProfile()` binds at runtime (M3). If a regression reverts
    // to the Core QWebEngineProfile these casts/inherits checks fail.
    QVERIFY(external->inherits("QQuickWebEngineProfile"));
    QVERIFY(internal->inherits("QQuickWebEngineProfile"));
    // The internal profile is off-the-record (ephemeral), the external one is
    // persistent (named storage).
    QCOMPARE(internal->isOffTheRecord(), true);
    QCOMPARE(external->isOffTheRecord(), false);

    // M3 isolation invariant: only the internal profile carries the internal
    // scheme handler; the external (arbitrary-site) profile must not.
    QCOMPARE(profiles.externalHasInternalScheme(), false);
    QCOMPARE(profiles.internalHasInternalScheme(), true);
}

void TstViewers::activeContentMimeGate()
{
    // The security gate deciding whether a scheme-handler reply is locked down
    // with the restrictive CSP (default-src 'none'; sandbox). Untrusted file
    // bytes served as any of these on the privileged internal origin can run
    // script or pull subresources as a top-level document — a cross-file
    // exfiltration vector (SPEC 7.2) — so EVERY active/scriptable type MUST be
    // flagged, including the XML-family and MHTML edge types.
    const QList<QByteArray> active = {
        QByteArrayLiteral("text/html"),
        QByteArrayLiteral("application/xhtml+xml"),
        QByteArrayLiteral("image/svg+xml"),
        QByteArrayLiteral("application/xml"),
        QByteArrayLiteral("text/xml"),
        QByteArrayLiteral("application/xslt+xml"),
        QByteArrayLiteral("text/xsl"),
        QByteArrayLiteral("application/rss+xml"),
        QByteArrayLiteral("application/atom+xml"),
        QByteArrayLiteral("multipart/related"),
        QByteArrayLiteral("message/rfc822"),
        QByteArrayLiteral("application/x-mimearchive"),
    };
    for (const QByteArray &m : active)
        QVERIFY2(InternalUrlSchemeHandler::isActiveContentMime(m), m.constData());

    // Case-insensitive and tolerant of a trailing charset parameter.
    QVERIFY(InternalUrlSchemeHandler::isActiveContentMime(
        QByteArrayLiteral("TEXT/HTML")));
    QVERIFY(InternalUrlSchemeHandler::isActiveContentMime(
        QByteArrayLiteral("text/html; charset=utf-8")));

    // Passive, inert content that MUST render normally (never CSP-locked, or the
    // image/pdf viewers would break).
    const QList<QByteArray> passive = {
        QByteArrayLiteral("image/png"),
        QByteArrayLiteral("image/jpeg"),
        QByteArrayLiteral("image/gif"),
        QByteArrayLiteral("image/webp"),
        QByteArrayLiteral("application/pdf"),
        QByteArrayLiteral("text/plain"),
        QByteArrayLiteral("application/json"),
        QByteArrayLiteral("application/octet-stream"),
    };
    for (const QByteArray &m : passive)
        QVERIFY2(!InternalUrlSchemeHandler::isActiveContentMime(m), m.constData());
}

void TstViewers::activeContentMimeFromExtension()
{
    // The gate keys off the DECLARED mime, which the handler derives from the
    // extension (mimeForPath). These extensions can reach a rendered/navigated
    // internal view (image view for .svg; the hidden download view navigates to
    // the internal URL for Download-resolution files), so their derived mime
    // MUST trip the active-content gate. Kept to the core, universally-present
    // shared-mime-info mappings; exotic edge MIME strings (rss/atom/mhtml) are
    // covered directly by activeContentMimeGate() without a DB dependency.
    for (const QString &path : {
             QStringLiteral("/a/logo.svg"), QStringLiteral("/a/page.xhtml"),
             QStringLiteral("/a/data.xml"), QStringLiteral("/a/page.html"),
         }) {
        const QByteArray mime = InternalUrlSchemeHandler::mimeForPath(path);
        QVERIFY2(InternalUrlSchemeHandler::isActiveContentMime(mime),
                 qPrintable(QStringLiteral("%1 -> %2 not gated")
                                .arg(path, QString::fromUtf8(mime))));
    }
    // Inert media must NOT be gated so the image/pdf/text viewers render it.
    for (const QString &path : {
             QStringLiteral("/a/logo.png"), QStringLiteral("/a/photo.jpg"),
             QStringLiteral("/a/doc.pdf"), QStringLiteral("/a/notes.txt"),
         }) {
        const QByteArray mime = InternalUrlSchemeHandler::mimeForPath(path);
        QVERIFY2(!InternalUrlSchemeHandler::isActiveContentMime(mime),
                 qPrintable(QStringLiteral("%1 -> %2 wrongly gated")
                                .arg(path, QString::fromUtf8(mime))));
    }
}

void TstViewers::urlMappingRemintAfterEviction()
{
    // After LRU eviction drops an entry, the evicted id stays permanently dead
    // (never resolves) and re-minting the SAME file yields a fresh, distinct,
    // still-invertible id — the map never resurrects an id it has evicted, so a
    // resolved id always maps to the currently-retained file.
    const QUrl a(QStringLiteral("file:///a.txt"));
    const QUrl b(QStringLiteral("file:///b.txt"));

    InternalUrlMap map(1); // cap of 1 forces eviction on the next mint
    const QString ia = map.internalUrlFor(a);
    QCOMPARE(map.fileUrlFor(ia), a);

    // Minting b evicts a (cap 1); a's old id must no longer resolve.
    const QString ib = map.internalUrlFor(b);
    QCOMPARE(map.size(), 1);
    QVERIFY(!map.fileUrlFor(ia).isValid());
    QCOMPARE(map.fileUrlFor(ib), b);

    // Re-minting a produces a brand-new id (evicting b); the stale id stays
    // dead and the new one resolves.
    const QString ia2 = map.internalUrlFor(a);
    QVERIFY(ia2 != ia);
    QVERIFY(!map.fileUrlFor(ia).isValid());
    QCOMPARE(map.fileUrlFor(ia2), a);
}

int main(int argc, char *argv[])
{
    // Use the offscreen platform when none is set so the GUI application (needed
    // by WebEngine) starts without a display in CI.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));

    // Custom schemes must be registered before WebEngine init (as in main.cpp),
    // so schemeFlags() can inspect the registered flags.
    ViewerProfiles::registerUrlScheme();

    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    TstViewers tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_viewers.moc"
