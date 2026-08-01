#include "CodeharbordClient.h"
#include "InternalUrlSchemeHandler.h"
#include "RpcTypes.h"
#include "ViewerHandlerRegistry.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"

#include <QtWebEngineQuick/QQuickWebEngineProfile>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QWebEngineUrlScheme>
#include <QtTest/QtTest>
#include <QtWebEngineQuick/QtWebEngineQuick>

using ch::CodeharbordClient;
using ch::InternalUrlMap;
using ch::InternalUrlSchemeHandler;
using ch::ViewerHandlerRegistry;
using ch::ViewerModel;
using ch::ViewerProfiles;
using ch::ViewerResolution;

namespace {

// A CodeharbordClient wired to a QLocalSocket pair with the TEST playing the
// remote codeharbord: requests written by the client are read off one end and
// canned JSON-RPC responses are written back. Same shape as the harness in
// tst_rpcclient / tst_editorcontroller, kept local because ViewerModel needs
// only a handful of round trips.
class RpcPair {
public:
    // Wire the pair up. Returns false if the socket pair could not be made,
    // which the caller turns into a QVERIFY failure.
    bool listen()
    {
        static int seq = 0;
        const QString name = QStringLiteral("ch_viewers_test_%1_%2")
                                 .arg(QCoreApplication::applicationPid())
                                 .arg(++seq);
        QLocalServer::removeServer(name);
        if (!m_server.listen(name))
            return false;
        m_clientSide.connectToServer(name);
        if (!m_clientSide.waitForConnected(2000))
            return false;
        if (!m_server.waitForNewConnection(2000))
            return false;
        m_serverSide = m_server.nextPendingConnection();
        if (!m_serverSide)
            return false;
        m_client.setTransport(&m_clientSide);
        return true;
    }

    CodeharbordClient *client() { return &m_client; }

    // Next framed request the client wrote, or an empty object on timeout. The
    // event loop is pumped first every iteration so the client can flush.
    QJsonObject nextRequest(int timeoutMs = 3000)
    {
        QDeadlineTimer deadline(timeoutMs);
        forever {
            QTest::qWait(5);
            if (m_serverSide)
                m_buffer += m_serverSide->readAll();
            const int nl = m_buffer.indexOf('\n');
            if (nl >= 0) {
                const QByteArray raw = m_buffer.left(nl);
                m_buffer.remove(0, nl + 1);
                return QJsonDocument::fromJson(raw).object();
            }
            if (deadline.hasExpired())
                break;
        }
        return {};
    }

    void respondResult(const QJsonObject &request, const QJsonObject &result)
    {
        write(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                          {QStringLiteral("id"), request.value(QStringLiteral("id"))},
                          {QStringLiteral("result"), result}});
    }

    void respondError(const QJsonObject &request, int code, const QString &message)
    {
        write(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), request.value(QStringLiteral("id"))},
            {QStringLiteral("error"),
             QJsonObject{{QStringLiteral("code"), code},
                         {QStringLiteral("message"), message}}}});
    }

private:
    void write(const QJsonObject &message)
    {
        m_serverSide->write(QJsonDocument(message).toJson(QJsonDocument::Compact)
                            + '\n');
        m_serverSide->flush();
    }

    QLocalServer m_server;
    QLocalSocket m_clientSide;
    QLocalSocket *m_serverSide = nullptr;  // owned by m_server
    QByteArray m_buffer;
    // Declared last so it is destroyed FIRST and unbinds from m_clientSide
    // while that socket is still alive.
    CodeharbordClient m_client;
};

// The path a file.readFile request asks for.
QString requestPath(const QJsonObject &request)
{
    return request.value(QStringLiteral("params"))
        .toObject()
        .value(QStringLiteral("path"))
        .toString();
}

// A successful file.readFile result for `path`.
QJsonObject readResult(const QString &path, const QString &content,
                       const QString &encoding = QStringLiteral("utf-8"),
                       bool truncated = false)
{
    return QJsonObject{{QStringLiteral("path"), path},
                       {QStringLiteral("encoding"), encoding},
                       {QStringLiteral("content"), content},
                       {QStringLiteral("revision"), QStringLiteral("r1")},
                       {QStringLiteral("truncated"), truncated}};
}

} // namespace

// Unit tests for the viewer handler registry (SPEC 7.5), the file <-> internal
// URL mapping (SPEC 7.4), the remote text-read plumbing, and the
// external/internal WebEngine profile isolation invariant (M3). The registry,
// URL-mapping and read-plumbing assertions require no WebEngine runtime and run
// unconditionally; only the live profile checks may QSKIP.
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
    void urlMappingBareId();
    void viewKindStringsMatchQmlContract();
    void viewerModelWithoutClientReportsErrors();

    // ONE ViewerModel serves every viewer pane (it is a single QML context
    // property), and two panes may be showing the SAME file, so a read's
    // identity is its token and never its path. These pin that no pane can
    // strand or cancel another pane's read, in either direction.
    void textReadsOfDifferentPathsAreIndependent();
    void cancelTextFileDropsOnlyItsOwnRead();
    void twoConcurrentReadsOfOnePathBothSettle();
    void cancelOfOneReadOfAPathLeavesTheOtherAlone();
    void aRefreshingPaneCancelsItsOwnEarlierReadOfOnePath();
    void textReadBookkeepingDoesNotLeak();
    // What the text pane is told when the server's answer is not plain text.
    void textReadFailureModes();
    // A read the client cannot even put on the wire still has to reach the pane
    // that asked for it, and reach it AFTER the token naming it.
    void textReadOnADeadTransportStillReachesItsCaller();

    // The order and the shape of the entries the directory pane renders.
    void directoryListingIsSortedDirectoriesFirst();

    // SPEC 9: an out-of-project path stays openable, but the pane has to be
    // TOLD it is out of project. These pin the flag's trip from the reply to
    // the signal the pane binds to.
    void resolvePathCarriesTheRepositoryRootFlag();
    void resolvePathFailuresLeaveTheFlagUndetermined();
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

    // Widely-used source and config extensions must also land in the text
    // editor: falling through to Download hides readable files behind the
    // binary pane, which was the pre-existing gap.
    for (const QString &ext :
         {QStringLiteral("jsx"), QStringLiteral("cjs"), QStringLiteral("mts"),
          QStringLiteral("java"), QStringLiteral("rb"), QStringLiteral("php"),
          QStringLiteral("kt"), QStringLiteral("swift"), QStringLiteral("cs"),
          QStringLiteral("lua"), QStringLiteral("bash"), QStringLiteral("zsh"),
          QStringLiteral("scss"), QStringLiteral("htm"), QStringLiteral("ini"),
          QStringLiteral("conf"), QStringLiteral("env"), QStringLiteral("sql"),
          QStringLiteral("csv"), QStringLiteral("log"), QStringLiteral("diff"),
          QStringLiteral("patch"), QStringLiteral("cmake"),
          QStringLiteral("hxx"), QStringLiteral("cxx")}) {
        QCOMPARE(ViewerHandlerRegistry::resolveByExtension(ext),
                 ViewerResolution::TextEditor);
    }

    // Build, infra and less-common source formats a developer reads as text.
    // These are files this very repository (and any repository it is used on)
    // is full of; each one landing in the binary Download pane is a file the
    // user simply cannot read.
    for (const QString &ext :
         {QStringLiteral("qml"), QStringLiteral("qrc"), QStringLiteral("proto"),
          QStringLiteral("graphql"), QStringLiteral("tf"), QStringLiteral("nix"),
          QStringLiteral("bzl"), QStringLiteral("ninja"), QStringLiteral("m4"),
          QStringLiteral("ac"), QStringLiteral("am"), QStringLiteral("mod"),
          QStringLiteral("sum"), QStringLiteral("lock"), QStringLiteral("rst"),
          QStringLiteral("tex"), QStringLiteral("ps1"), QStringLiteral("bat"),
          QStringLiteral("dart"), QStringLiteral("jl"), QStringLiteral("hs"),
          QStringLiteral("ex"), QStringLiteral("erl"), QStringLiteral("zig"),
          QStringLiteral("asm"), QStringLiteral("ipynb"),
          QStringLiteral("desktop"), QStringLiteral("service")}) {
        QCOMPARE(ViewerHandlerRegistry::resolveByExtension(ext),
                 ViewerResolution::TextEditor);
    }

    // Images.
    for (const QString &ext :
         {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
          QStringLiteral("gif"), QStringLiteral("svg"), QStringLiteral("webp"),
          // Raster formats Chromium decodes natively; without these an .ico or
          // a .bmp opened as a download instead of showing the picture.
          QStringLiteral("bmp"), QStringLiteral("ico"), QStringLiteral("avif"),
          QStringLiteral("apng")}) {
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
    // A format Chromium cannot decode stays a download rather than becoming an
    // image pane stuck on a broken-image icon.
    QCOMPARE(ViewerHandlerRegistry::resolveByExtension(QStringLiteral("tiff")),
             ViewerResolution::Download);
    QCOMPARE(ViewerHandlerRegistry::resolveByExtension(QStringLiteral("psd")),
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

    // Well-known extensionless files and dotfiles -> text viewer (basename
    // table), while truly binary/unknown content still -> download.
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/Makefile"))),
             ViewerResolution::TextEditor);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/Dockerfile"))),
             ViewerResolution::TextEditor);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/LICENSE"))),
             ViewerResolution::TextEditor);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/.bashrc"))),
             ViewerResolution::TextEditor);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/.gitignore"))),
             ViewerResolution::TextEditor);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/somebinary"))),
             ViewerResolution::Download);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/archive.zip"))),
             ViewerResolution::Download);

    // The name table is matched CASE-INSENSITIVELY. "makefile" all lower case
    // is what GNU make itself looks for and what countless repositories ship;
    // a case-sensitive table showed it as an undecipherable binary.
    for (const QString &name :
         {QStringLiteral("makefile"), QStringLiteral("MAKEFILE"),
          QStringLiteral("GNUmakefile"), QStringLiteral("readme"),
          QStringLiteral("License"), QStringLiteral("Jenkinsfile"),
          QStringLiteral("Rakefile"), QStringLiteral("CODEOWNERS"),
          QStringLiteral(".dockerignore"), QStringLiteral(".clang-format"),
          QStringLiteral(".npmrc")}) {
        QCOMPARE(ViewerHandlerRegistry::resolve(
                     QUrl(QStringLiteral("file:///home/yc/") + name)),
                 ViewerResolution::TextEditor);
    }

    // A well-known name with a MEANINGLESS suffix bolted on is still that file.
    // These spellings are everywhere and each one used to open as a binary.
    for (const QString &name :
         {QStringLiteral("Dockerfile.dev"), QStringLiteral("Makefile.am"),
          QStringLiteral("Makefile.in"), QStringLiteral(".env.local"),
          QStringLiteral(".env.production"), QStringLiteral("README.old")}) {
        QCOMPARE(ViewerHandlerRegistry::resolve(
                     QUrl(QStringLiteral("file:///home/yc/") + name)),
                 ViewerResolution::TextEditor);
    }

    // The suffix rule must not swallow ordinary binaries: the stem has to be a
    // known text name, and a KNOWN extension still wins outright.
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/notmakefile.bin"))),
             ViewerResolution::Download);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/README.png"))),
             ViewerResolution::ImageViewer);
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/README.md"))),
             ViewerResolution::InternalHtmlRenderer);

    // A directory whose own name is a well-known text name is still a
    // directory: the trailing slash is decided first.
    QCOMPARE(ViewerHandlerRegistry::resolve(
                 QUrl(QStringLiteral("file:///home/yc/LICENSE/"))),
             ViewerResolution::DirectoryViewer);

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

void TstViewers::urlMappingBareId()
{
    // fileUrlFor() documents that it accepts either a full internal URL or a
    // bare id. The scheme handler resolves the bare id itself, but QML and the
    // live tests hand the whole URL back; both spellings must agree.
    InternalUrlMap map;
    const QUrl file(QStringLiteral("file:///home/yc/project/README.md"));
    const QString internal = map.internalUrlFor(file);
    const QString id = internal.mid(InternalUrlMap::prefix().size());

    QCOMPARE(map.fileUrlFor(internal), file);
    QCOMPARE(map.fileUrlFor(id), file);
    QCOMPARE(map.fileUrlForId(id), file);
    // An unknown bare id is not silently aliased onto some other entry.
    QVERIFY(!map.fileUrlForId(QStringLiteral("00000000000000000000000000000000"))
                 .isValid());

    // A full internal URL naming a DIFFERENT authority is refused, exactly as
    // InternalUrlSchemeHandler refuses it. In a browser a differing host is a
    // differing origin, so an inverse that quietly accepted one would disagree
    // with the handler about whether the very same URL is legitimate.
    QVERIFY(!map.fileUrlFor(QStringLiteral("codeharbor-internal://evil/") + id)
                 .isValid());
    QVERIFY(!map.fileUrlFor(QStringLiteral("codeharbor-internal://files/") + id)
                 .isValid());
    // Host matching stays case-insensitive, which is how URLs work.
    QCOMPARE(map.fileUrlFor(QStringLiteral("codeharbor-internal://FILE/") + id),
             file);
}

void TstViewers::viewKindStringsMatchQmlContract()
{
    // ViewerPane.qml switches on these exact strings to pick a view; a rename
    // here silently degrades every pane to the fallback. This pins the mapping
    // from resolution to QML view kind (SPEC 7.5).
    ViewerModel viewers;
    QCOMPARE(viewers.viewKind(QUrl(QStringLiteral("https://example.com/"))),
             QStringLiteral("web"));
    QCOMPARE(viewers.viewKind(QUrl(QStringLiteral("file:///p/README.md"))),
             QStringLiteral("markdown"));
    QCOMPARE(viewers.viewKind(QUrl(QStringLiteral("file:///p/main.cpp"))),
             QStringLiteral("text"));
    QCOMPARE(viewers.viewKind(QUrl(QStringLiteral("file:///p/logo.png"))),
             QStringLiteral("image"));
    QCOMPARE(viewers.viewKind(QUrl(QStringLiteral("file:///p/manual.pdf"))),
             QStringLiteral("pdf"));
    QCOMPARE(viewers.viewKind(QUrl(QStringLiteral("file:///p/sub/"))),
             QStringLiteral("directory"));
    // Download / OpenExternally / Error all collapse onto the binary pane.
    QCOMPARE(viewers.viewKind(QUrl(QStringLiteral("file:///p/blob.bin"))),
             QStringLiteral("binary"));
    QCOMPARE(viewers.viewKind(QUrl(QStringLiteral("ftp://host/x"))),
             QStringLiteral("binary"));
}

void TstViewers::viewerModelWithoutClientReportsErrors()
{
    // Before a Dev Session is connected there is no remote client. The QML
    // views bind unconditionally, so both calls must answer with an error
    // signal rather than doing nothing and leaving a permanently blank pane.
    ViewerModel viewers;
    QSignalSpy textErrors(&viewers, &ViewerModel::textFileError);
    QSignalSpy dirErrors(&viewers, &ViewerModel::directoryError);
    QSignalSpy textReads(&viewers, &ViewerModel::textFileRead);
    QSignalSpy dirLists(&viewers, &ViewerModel::directoryListed);

    const QString token = viewers.readTextFile(QStringLiteral("/p/README.md"));
    viewers.listDirectory(QStringLiteral("/p"));

    // The read failure is DEFERRED: emitting it from inside readTextFile would
    // deliver a token the caller has not been handed yet and cannot match.
    QVERIFY(!token.isEmpty());
    QCOMPARE(textErrors.size(), 0);
    QTRY_COMPARE(textErrors.size(), 1);
    QCOMPARE(dirErrors.size(), 1);
    QCOMPARE(textReads.size(), 0);
    QCOMPARE(dirLists.size(), 0);
    // The failing read names itself, so the view that issued it can tell the
    // error is about its own request, and echoes the path for the same reason.
    QCOMPARE(textErrors.first().at(0).toString(), token);
    QCOMPARE(textErrors.first().at(1).toString(), QStringLiteral("/p/README.md"));
    QCOMPARE(dirErrors.first().at(0).toString(), QStringLiteral("/p"));
    // Even a read that never reached the wire is forgotten once it settles.
    QCOMPARE(viewers.inFlightTextReadCount(), 0);
}

void TstViewers::textReadsOfDifferentPathsAreIndependent()
{
    // Two viewer panes, two files, one shared ViewerModel. Both reads are in
    // flight at once and their replies can arrive in either order; each pane
    // must get its own answer. A single global "latest read wins" counter drops
    // the first pane's reply, leaving that pane stuck on "Loading…" forever
    // with no error to show.
    RpcPair pair;
    QVERIFY(pair.listen());
    ViewerModel viewers(pair.client());
    QSignalSpy reads(&viewers, &ViewerModel::textFileRead);
    QSignalSpy errors(&viewers, &ViewerModel::textFileError);

    const QString tokenA = viewers.readTextFile(QStringLiteral("/p/a.txt"));
    const QJsonObject requestA = pair.nextRequest();
    QCOMPARE(requestA.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodReadFile));
    QCOMPARE(requestPath(requestA), QStringLiteral("/p/a.txt"));

    const QString tokenB = viewers.readTextFile(QStringLiteral("/p/b.txt"));
    const QJsonObject requestB = pair.nextRequest();
    QCOMPARE(requestPath(requestB), QStringLiteral("/p/b.txt"));
    QVERIFY(tokenA != tokenB);

    // Out of order: the SECOND pane's file answers first.
    pair.respondResult(requestB,
                       readResult(QStringLiteral("/p/b.txt"), QStringLiteral("B")));
    pair.respondResult(requestA,
                       readResult(QStringLiteral("/p/a.txt"), QStringLiteral("A")));

    QTRY_COMPARE(reads.size(), 2);
    QCOMPARE(errors.size(), 0);
    // Keyed by TOKEN: that is what a pane matches on, so that is what the test
    // checks the content was attributed to.
    QVariantMap byToken;
    QVariantMap pathByToken;
    for (const QList<QVariant> &args : reads) {
        byToken.insert(args.at(0).toString(), args.at(2));
        pathByToken.insert(args.at(0).toString(), args.at(1));
    }
    QCOMPARE(byToken.value(tokenA).toString(), QStringLiteral("A"));
    QCOMPARE(byToken.value(tokenB).toString(), QStringLiteral("B"));
    QCOMPARE(pathByToken.value(tokenA).toString(), QStringLiteral("/p/a.txt"));
    QCOMPARE(pathByToken.value(tokenB).toString(), QStringLiteral("/p/b.txt"));
    QCOMPARE(viewers.inFlightTextReadCount(), 0);
}

void TstViewers::cancelTextFileDropsOnlyItsOwnRead()
{
    // A pane whose URL clears cancels ITS read. Every other pane's read must
    // survive: they share this one ViewerModel. This is the cross-path case.
    RpcPair pair;
    QVERIFY(pair.listen());
    ViewerModel viewers(pair.client());
    QSignalSpy reads(&viewers, &ViewerModel::textFileRead);
    QSignalSpy errors(&viewers, &ViewerModel::textFileError);

    const QString keepToken = viewers.readTextFile(QStringLiteral("/p/keep.txt"));
    const QJsonObject keep = pair.nextRequest();
    QCOMPARE(requestPath(keep), QStringLiteral("/p/keep.txt"));
    const QString dropToken = viewers.readTextFile(QStringLiteral("/p/drop.txt"));
    const QJsonObject drop = pair.nextRequest();
    QCOMPARE(requestPath(drop), QStringLiteral("/p/drop.txt"));

    viewers.cancelTextFile(dropToken);

    pair.respondResult(drop, readResult(QStringLiteral("/p/drop.txt"),
                                        QStringLiteral("dropped")));
    pair.respondResult(keep, readResult(QStringLiteral("/p/keep.txt"),
                                        QStringLiteral("kept")));

    QTRY_COMPARE(reads.size(), 1);
    QCOMPARE(reads.first().at(0).toString(), keepToken);
    QCOMPARE(reads.first().at(1).toString(), QStringLiteral("/p/keep.txt"));
    QCOMPARE(reads.first().at(2).toString(), QStringLiteral("kept"));
    QCOMPARE(errors.size(), 0);

    // Cancelling a token with nothing in flight — including the empty token a
    // caller passes when it has nothing to cancel, and a token that already
    // settled — disturbs nothing.
    viewers.cancelTextFile();
    viewers.cancelTextFile(keepToken);
    viewers.cancelTextFile(QStringLiteral("not-a-token"));
    viewers.readTextFile(QStringLiteral("/p/after.txt"));
    const QJsonObject after = pair.nextRequest();
    pair.respondResult(after, readResult(QStringLiteral("/p/after.txt"),
                                         QStringLiteral("after")));
    QTRY_COMPARE(reads.size(), 2);
    QCOMPARE(reads.at(1).at(2).toString(), QStringLiteral("after"));
}

void TstViewers::twoConcurrentReadsOfOnePathBothSettle()
{
    // TWO PANES SHOWING THE SAME FILE. Nothing about that is exotic, and both
    // panes must be answered. Bookkeeping keyed by path has one slot per file,
    // so the second read overwrites the first and one of the two panes is left
    // on "Loading…" with no content and no error.
    RpcPair pair;
    QVERIFY(pair.listen());
    ViewerModel viewers(pair.client());
    QSignalSpy reads(&viewers, &ViewerModel::textFileRead);
    QSignalSpy errors(&viewers, &ViewerModel::textFileError);

    const QString tokenA = viewers.readTextFile(QStringLiteral("/notes.md"));
    const QJsonObject requestA = pair.nextRequest();
    const QString tokenB = viewers.readTextFile(QStringLiteral("/notes.md"));
    const QJsonObject requestB = pair.nextRequest();
    // Same path, genuinely different reads: different RPC ids, different tokens.
    QVERIFY(requestA.value(QStringLiteral("id")).toInt()
            != requestB.value(QStringLiteral("id")).toInt());
    QVERIFY(!tokenA.isEmpty());
    QVERIFY(tokenA != tokenB);
    QCOMPARE(viewers.inFlightTextReadCount(), 2);

    // Answered out of order, as two independent server round trips may be.
    pair.respondResult(requestB, readResult(QStringLiteral("/notes.md"),
                                            QStringLiteral("for pane B")));
    pair.respondResult(requestA, readResult(QStringLiteral("/notes.md"),
                                            QStringLiteral("for pane A")));

    QTRY_COMPARE(reads.size(), 2);
    QCOMPARE(errors.size(), 0);
    QVariantMap byToken;
    for (const QList<QVariant> &args : reads) {
        QCOMPARE(args.at(1).toString(), QStringLiteral("/notes.md"));
        byToken.insert(args.at(0).toString(), args.at(2));
    }
    // Each caller's reply is attributed to ITS OWN token; neither was dropped
    // as superseded by the other.
    QCOMPARE(byToken.value(tokenA).toString(), QStringLiteral("for pane A"));
    QCOMPARE(byToken.value(tokenB).toString(), QStringLiteral("for pane B"));
    QCOMPARE(viewers.inFlightTextReadCount(), 0);
}

void TstViewers::cancelOfOneReadOfAPathLeavesTheOtherAlone()
{
    // THE BUG, exactly. Pane A and pane B both open /notes.md. Pane B navigates
    // away before its reply lands and cancels its read. With bookkeeping keyed
    // by path that cancel erases the ONE entry for /notes.md, so pane A's reply
    // is dropped too and pane A hangs on "Loading…" forever.
    RpcPair pair;
    QVERIFY(pair.listen());
    ViewerModel viewers(pair.client());
    QSignalSpy reads(&viewers, &ViewerModel::textFileRead);
    QSignalSpy errors(&viewers, &ViewerModel::textFileError);

    const QString tokenA = viewers.readTextFile(QStringLiteral("/notes.md"));
    const QJsonObject requestA = pair.nextRequest();
    const QString tokenB = viewers.readTextFile(QStringLiteral("/notes.md"));
    const QJsonObject requestB = pair.nextRequest();

    viewers.cancelTextFile(tokenB);
    QCOMPARE(viewers.inFlightTextReadCount(), 1);

    pair.respondResult(requestB, readResult(QStringLiteral("/notes.md"),
                                            QStringLiteral("abandoned")));
    pair.respondResult(requestA, readResult(QStringLiteral("/notes.md"),
                                            QStringLiteral("still wanted")));

    // A is still delivered, under its own token.
    QTRY_COMPARE(reads.size(), 1);
    QCOMPARE(reads.first().at(0).toString(), tokenA);
    QCOMPARE(reads.first().at(1).toString(), QStringLiteral("/notes.md"));
    QCOMPARE(reads.first().at(2).toString(), QStringLiteral("still wanted"));
    // B produces nothing at all — not a second read, not an error.
    QTest::qWait(100);
    QCOMPARE(reads.size(), 1);
    QCOMPARE(errors.size(), 0);

    // The mirror direction: the pane that cancels first is the other one.
    reads.clear();
    const QString tokenC = viewers.readTextFile(QStringLiteral("/notes.md"));
    const QJsonObject requestC = pair.nextRequest();
    const QString tokenD = viewers.readTextFile(QStringLiteral("/notes.md"));
    const QJsonObject requestD = pair.nextRequest();
    viewers.cancelTextFile(tokenC);
    pair.respondResult(requestC, readResult(QStringLiteral("/notes.md"),
                                            QStringLiteral("abandoned")));
    pair.respondResult(requestD, readResult(QStringLiteral("/notes.md"),
                                            QStringLiteral("still wanted too")));
    QTRY_COMPARE(reads.size(), 1);
    QCOMPARE(reads.first().at(0).toString(), tokenD);
    QCOMPARE(reads.first().at(2).toString(), QStringLiteral("still wanted too"));
    QCOMPARE(errors.size(), 0);
    QCOMPARE(viewers.inFlightTextReadCount(), 0);
}

void TstViewers::aRefreshingPaneCancelsItsOwnEarlierReadOfOnePath()
{
    // One pane re-reading the SAME file (ViewerTextView.reload) must never end
    // up showing the older answer. Supersession is no longer implicit — it
    // cannot be, since a second read of a path is now a legitimate second
    // reader — so the pane cancels the token it is holding before asking again,
    // which is exactly what reload() does.
    RpcPair pair;
    QVERIFY(pair.listen());
    ViewerModel viewers(pair.client());
    QSignalSpy reads(&viewers, &ViewerModel::textFileRead);

    const QString stale = viewers.readTextFile(QStringLiteral("/p/a.txt"));
    const QJsonObject first = pair.nextRequest();
    viewers.cancelTextFile(stale);
    const QString fresh = viewers.readTextFile(QStringLiteral("/p/a.txt"));
    const QJsonObject second = pair.nextRequest();
    QVERIFY(first.value(QStringLiteral("id")).toInt()
            != second.value(QStringLiteral("id")).toInt());
    // Tokens come from one monotonic counter and are never reused, so the
    // cancelled read can never be mistaken for the new one.
    QVERIFY(stale != fresh);

    pair.respondResult(second, readResult(QStringLiteral("/p/a.txt"),
                                          QStringLiteral("newest")));
    pair.respondResult(first, readResult(QStringLiteral("/p/a.txt"),
                                         QStringLiteral("stale")));

    QTRY_COMPARE(reads.size(), 1);
    QCOMPARE(reads.first().at(0).toString(), fresh);
    QCOMPARE(reads.first().at(2).toString(), QStringLiteral("newest"));
    QTest::qWait(100);
    QCOMPARE(reads.size(), 1);
    QCOMPARE(viewers.inFlightTextReadCount(), 0);
}

void TstViewers::textReadBookkeepingDoesNotLeak()
{
    // The in-flight set is the model's only per-read state. An entry that
    // outlives its request would accumulate for the lifetime of the
    // application — one leak per file the user ever opens. Every way a read can
    // end must clear it: success, server error, and cancellation.
    RpcPair pair;
    QVERIFY(pair.listen());
    ViewerModel viewers(pair.client());
    QSignalSpy reads(&viewers, &ViewerModel::textFileRead);
    QSignalSpy errors(&viewers, &ViewerModel::textFileError);
    QCOMPARE(viewers.inFlightTextReadCount(), 0);

    const QString ok = viewers.readTextFile(QStringLiteral("/p/ok.txt"));
    const QJsonObject okRequest = pair.nextRequest();
    const QString bad = viewers.readTextFile(QStringLiteral("/p/bad.txt"));
    const QJsonObject badRequest = pair.nextRequest();
    const QString gone = viewers.readTextFile(QStringLiteral("/p/ok.txt"));
    const QJsonObject goneRequest = pair.nextRequest();
    QCOMPARE(viewers.inFlightTextReadCount(), 3);

    viewers.cancelTextFile(gone);
    QCOMPARE(viewers.inFlightTextReadCount(), 2);

    pair.respondResult(okRequest, readResult(QStringLiteral("/p/ok.txt"),
                                             QStringLiteral("fine")));
    pair.respondError(badRequest, -32603, QStringLiteral("EACCES"));
    // The cancelled read is still answered by the server; its reply must not
    // resurrect an entry.
    pair.respondResult(goneRequest, readResult(QStringLiteral("/p/ok.txt"),
                                               QStringLiteral("ignored")));

    QTRY_COMPARE(reads.size(), 1);
    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(reads.first().at(0).toString(), ok);
    QCOMPARE(errors.first().at(0).toString(), bad);
    QTest::qWait(100);
    QCOMPARE(viewers.inFlightTextReadCount(), 0);
}

void TstViewers::textReadFailureModes()
{
    // Everything the text pane can be told other than "here is your file". Each
    // must arrive as an ERROR carrying the read's token and path, never as an
    // empty document: a blank pane is indistinguishable from an empty file.
    RpcPair pair;
    QVERIFY(pair.listen());
    ViewerModel viewers(pair.client());
    QSignalSpy reads(&viewers, &ViewerModel::textFileRead);
    QSignalSpy errors(&viewers, &ViewerModel::textFileError);

    // 1. The server refused the read: its message is surfaced verbatim.
    const QString missing = viewers.readTextFile(QStringLiteral("/p/missing.txt"));
    pair.respondError(pair.nextRequest(), -32603, QStringLiteral("ENOENT"));
    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(errors.at(0).at(0).toString(), missing);
    QCOMPARE(errors.at(0).at(1).toString(), QStringLiteral("/p/missing.txt"));
    QCOMPARE(errors.at(0).at(2).toString(), QStringLiteral("ENOENT"));

    // 2. A file over the inline cap comes back as a PREFIX (truncated). Showing
    //    it as though it were the whole file would be a lie, so it is an error.
    const QString huge = viewers.readTextFile(QStringLiteral("/p/huge.log"));
    pair.respondResult(pair.nextRequest(),
                       readResult(QStringLiteral("/p/huge.log"),
                                  QStringLiteral("first window"),
                                  QStringLiteral("utf-8"), /*truncated=*/true));
    QTRY_COMPARE(errors.size(), 2);
    QCOMPARE(errors.at(1).at(0).toString(), huge);
    QCOMPARE(errors.at(1).at(1).toString(), QStringLiteral("/p/huge.log"));

    // 3. Base64 (a file the server could not decode as utf-8) is decoded here
    //    so a near-text binary still shows something legible.
    const QString bin = viewers.readTextFile(QStringLiteral("/p/bin.dat"));
    pair.respondResult(pair.nextRequest(),
                       readResult(QStringLiteral("/p/bin.dat"),
                                  QStringLiteral("aGVsbG8="),
                                  QStringLiteral("base64")));
    QTRY_COMPARE(reads.size(), 1);
    QCOMPARE(reads.at(0).at(0).toString(), bin);
    QCOMPARE(reads.at(0).at(1).toString(), QStringLiteral("/p/bin.dat"));
    QCOMPARE(reads.at(0).at(2).toString(), QStringLiteral("hello"));

    // 4. A malformed base64 payload is reported, NOT rendered as an empty file.
    const QString broken = viewers.readTextFile(QStringLiteral("/p/broken.dat"));
    pair.respondResult(pair.nextRequest(),
                       readResult(QStringLiteral("/p/broken.dat"),
                                  QStringLiteral("not base64 at all!!"),
                                  QStringLiteral("base64")));
    QTRY_COMPARE(errors.size(), 3);
    QCOMPARE(errors.at(2).at(0).toString(), broken);
    QCOMPARE(errors.at(2).at(1).toString(), QStringLiteral("/p/broken.dat"));
    QCOMPARE(reads.size(), 1);
    QCOMPARE(viewers.inFlightTextReadCount(), 0);
}

void TstViewers::textReadOnADeadTransportStillReachesItsCaller()
{
    // A Dev Session whose SSH connection dropped leaves a CodeharbordClient
    // that exists but has no usable transport. Its call() refuses the request
    // and runs the callback THERE AND THEN, inside readTextFile(), before
    // readTextFile() has returned the token that names the read.
    //
    // The pane matches replies by token (ViewerTextView.ownsReply), and it only
    // learns the token when readTextFile() returns. A failure emitted before
    // that carries a token the pane has never heard of, so it throws its own
    // error away and sits on "Loading…" for ever with nothing to show and
    // nothing to retry. The reply must therefore always land after the token.
    CodeharbordClient clientWithNoTransport;
    ViewerModel viewers(&clientWithNoTransport);
    QSignalSpy errors(&viewers, &ViewerModel::textFileError);
    QSignalSpy reads(&viewers, &ViewerModel::textFileRead);

    const QString token = viewers.readTextFile(QStringLiteral("/p/a.txt"));
    QVERIFY(!token.isEmpty());
    // NOTHING may have been emitted yet: the caller only now holds the token.
    QCOMPARE(errors.size(), 0);
    QCOMPARE(viewers.inFlightTextReadCount(), 1);

    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(errors.first().at(0).toString(), token);
    QCOMPARE(errors.first().at(1).toString(), QStringLiteral("/p/a.txt"));
    QVERIFY(!errors.first().at(2).toString().isEmpty());
    QCOMPARE(reads.size(), 0);
    // The read settled, so its bookkeeping is gone: the deferral must not turn
    // an unsendable read into a permanent in-flight entry.
    QCOMPARE(viewers.inFlightTextReadCount(), 0);

    // Cancelling before the deferred reply runs still suppresses it — the extra
    // hop must not smuggle a cancelled read's failure onto a pane that has
    // already navigated away.
    const QString cancelled = viewers.readTextFile(QStringLiteral("/p/b.txt"));
    viewers.cancelTextFile(cancelled);
    QTest::qWait(50);
    QCOMPARE(errors.size(), 1);
    QCOMPARE(viewers.inFlightTextReadCount(), 0);
}

void TstViewers::directoryListingIsSortedDirectoriesFirst()
{
    // The server states no order, so the model imposes one: directories first,
    // then names case-insensitively, with a case-sensitive tie-break so two
    // names differing only in case do not swap places between runs. The pane
    // renders this list verbatim, so this IS the on-screen order.
    RpcPair pair;
    QVERIFY(pair.listen());
    ViewerModel viewers(pair.client());
    QSignalSpy listings(&viewers, &ViewerModel::directoryListed);
    QSignalSpy failures(&viewers, &ViewerModel::directoryError);

    viewers.listDirectory(QStringLiteral("/p"));
    const QJsonObject request = pair.nextRequest();
    QCOMPARE(request.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodListDirectory));
    QCOMPARE(requestPath(request), QStringLiteral("/p"));

    const auto entry = [](const QString &name, const QString &kind) {
        return QJsonObject{{QStringLiteral("name"), name},
                           {QStringLiteral("kind"), kind},
                           // A field the model does not forward; the pane's
                           // delegate reads name/kind and nothing else.
                           {QStringLiteral("size"), 42}};
    };
    pair.respondResult(
        request,
        QJsonObject{
            {QStringLiteral("entries"),
             QJsonArray{entry(QStringLiteral("zebra.txt"), QStringLiteral("file")),
                        entry(QStringLiteral("readme"), QStringLiteral("file")),
                        entry(QStringLiteral("src"), QStringLiteral("directory")),
                        entry(QStringLiteral("Readme"), QStringLiteral("file")),
                        entry(QStringLiteral("Apps"), QStringLiteral("directory")),
                        entry(QStringLiteral("apple.txt"), QStringLiteral("file"))}}});

    QTRY_COMPARE(listings.size(), 1);
    QCOMPARE(failures.size(), 0);
    QCOMPARE(listings.first().at(0).toString(), QStringLiteral("/p"));

    QStringList names;
    const QVariantList entries = listings.first().at(1).toList();
    for (const QVariant &value : entries) {
        const QVariantMap map = value.toMap();
        names.append(map.value(QStringLiteral("name")).toString());
        // Exactly the two keys the pane consumes: nothing else leaks through.
        QCOMPARE(map.size(), 2);
        QVERIFY(map.contains(QStringLiteral("kind")));
    }
    QCOMPARE(names,
             (QStringList{QStringLiteral("Apps"), QStringLiteral("src"),
                          QStringLiteral("apple.txt"), QStringLiteral("Readme"),
                          QStringLiteral("readme"), QStringLiteral("zebra.txt")}));

    // An empty directory is a successful listing, not an error: the pane has to
    // be able to say "nothing here" rather than "something went wrong".
    viewers.listDirectory(QStringLiteral("/p/empty"));
    pair.respondResult(pair.nextRequest(),
                       QJsonObject{{QStringLiteral("entries"), QJsonArray{}}});
    QTRY_COMPARE(listings.size(), 2);
    QCOMPARE(listings.at(1).at(0).toString(), QStringLiteral("/p/empty"));
    QVERIFY(listings.at(1).at(1).toList().isEmpty());
    QCOMPARE(failures.size(), 0);
}

void TstViewers::resolvePathCarriesTheRepositoryRootFlag()
{
    // The viewer pane marks a file that lives outside the Dev Session's
    // repository root (SPEC 9). The flag is the server's to compute; this is
    // the wire trip that carries it, including the `base` that decides what
    // "the project" even is.
    RpcPair pair;
    QVERIFY(pair.listen());
    ViewerModel viewers(pair.client());
    QSignalSpy resolved(&viewers, &ViewerModel::pathResolved);
    QSignalSpy failures(&viewers, &ViewerModel::pathResolveError);

    viewers.resolvePath(QStringLiteral("/srv/repos/app/src/main.cpp"),
                        QStringLiteral("/srv/repos/app"));
    const QJsonObject inside = pair.nextRequest();
    QCOMPARE(inside.value(QStringLiteral("method")).toString(),
             QString::fromLatin1(ch::rpc::kMethodResolvePath));
    QCOMPARE(requestPath(inside), QStringLiteral("/srv/repos/app/src/main.cpp"));
    // The base is the ACTIVE session's root, never the server's own working
    // directory: without it the server answers about the wrong project.
    QCOMPARE(inside.value(QStringLiteral("params"))
                 .toObject()
                 .value(QStringLiteral("base"))
                 .toString(),
             QStringLiteral("/srv/repos/app"));
    pair.respondResult(
        inside,
        QJsonObject{{QStringLiteral("path"),
                     QStringLiteral("/srv/repos/app/src/main.cpp")},
                    {QStringLiteral("insideRepositoryRoot"), true}});
    QTRY_COMPARE(resolved.size(), 1);
    // The ASKED-for path is echoed, which is what a pane matches its own
    // outstanding question against.
    QCOMPARE(resolved.at(0).at(0).toString(),
             QStringLiteral("/srv/repos/app/src/main.cpp"));
    QCOMPARE(resolved.at(0).at(1).toString(),
             QStringLiteral("/srv/repos/app/src/main.cpp"));
    QCOMPARE(resolved.at(0).at(2).toBool(), true);

    // The same call for a path in nobody's project. It is answered, not
    // refused: SPEC 9 allows it to be opened and this call never gates it.
    viewers.resolvePath(QStringLiteral("/etc/hosts"),
                        QStringLiteral("/srv/repos/app"));
    const QJsonObject outside = pair.nextRequest();
    QCOMPARE(requestPath(outside), QStringLiteral("/etc/hosts"));
    pair.respondResult(
        outside, QJsonObject{{QStringLiteral("path"), QStringLiteral("/etc/hosts")},
                             {QStringLiteral("insideRepositoryRoot"), false}});
    QTRY_COMPARE(resolved.size(), 2);
    QCOMPARE(resolved.at(1).at(0).toString(), QStringLiteral("/etc/hosts"));
    QCOMPARE(resolved.at(1).at(2).toBool(), false);

    QCOMPARE(failures.size(), 0);
}

void TstViewers::resolvePathFailuresLeaveTheFlagUndetermined()
{
    // Every way the question can go unanswered must arrive as an error, so the
    // pane shows NOTHING. A false invented here would brand an ordinary
    // in-project file as foreign; a true would hide a real out-of-project one.
    ViewerModel offline;
    QSignalSpy offlineResolved(&offline, &ViewerModel::pathResolved);
    QSignalSpy offlineFailures(&offline, &ViewerModel::pathResolveError);
    offline.resolvePath(QStringLiteral("/etc/hosts"), QStringLiteral("/srv/app"));
    QCOMPARE(offlineFailures.size(), 1);
    QCOMPARE(offlineFailures.at(0).at(0).toString(), QStringLiteral("/etc/hosts"));
    QCOMPARE(offlineResolved.size(), 0);

    RpcPair pair;
    QVERIFY(pair.listen());
    ViewerModel viewers(pair.client());
    QSignalSpy resolved(&viewers, &ViewerModel::pathResolved);
    QSignalSpy failures(&viewers, &ViewerModel::pathResolveError);

    // 1. The server refused: its message is surfaced verbatim.
    viewers.resolvePath(QStringLiteral("/p/a.txt"), QStringLiteral("/p"));
    pair.respondError(pair.nextRequest(), -32603, QStringLiteral("EACCES"));
    QTRY_COMPARE(failures.size(), 1);
    QCOMPARE(failures.at(0).at(0).toString(), QStringLiteral("/p/a.txt"));
    QCOMPARE(failures.at(0).at(1).toString(), QStringLiteral("EACCES"));

    // 2. A reply that simply does not carry the flag (an older daemon). An
    //    absent JSON value reads as false, so defaulting would report EVERY
    //    file as outside the project; it is a failure instead.
    viewers.resolvePath(QStringLiteral("/p/b.txt"), QStringLiteral("/p"));
    pair.respondResult(pair.nextRequest(),
                       QJsonObject{{QStringLiteral("path"),
                                    QStringLiteral("/p/b.txt")}});
    QTRY_COMPARE(failures.size(), 2);
    QCOMPARE(failures.at(1).at(0).toString(), QStringLiteral("/p/b.txt"));
    QCOMPARE(resolved.size(), 0);

    // 3. Without a base the parameter is omitted rather than sent empty: an
    //    empty base resolves against the filesystem root, and everything is
    //    inside THAT.
    viewers.resolvePath(QStringLiteral("/p/c.txt"), QString());
    const QJsonObject baseless = pair.nextRequest();
    QVERIFY(!baseless.value(QStringLiteral("params"))
                 .toObject()
                 .contains(QStringLiteral("base")));
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
