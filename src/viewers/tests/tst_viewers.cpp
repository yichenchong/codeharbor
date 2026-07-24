#include "InternalUrlSchemeHandler.h"
#include "ViewerHandlerRegistry.h"
#include "ViewerProfiles.h"

#include <QtWebEngineQuick/QQuickWebEngineProfile>

#include <QGuiApplication>
#include <QString>
#include <QUrl>
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
    void mimeForPathByExtension();
    void profileIsolation();
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
          QStringLiteral("yml"), QStringLiteral("toml")}) {
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

int main(int argc, char *argv[])
{
    // Use the offscreen platform when none is set so the GUI application (needed
    // by WebEngine) starts without a display in CI.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));

    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    TstViewers tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_viewers.moc"
