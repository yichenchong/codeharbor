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
    // The order and the shape of the entries the directory pane renders.
    void directoryListingIsSortedDirectoriesFirst();
    void openAsKindsFollowRegistryClaims();
    void applicationSchemeValidationAndEscaping();

    // SPEC 9: an out-of-project path stays openable, but the pane has to be
    // TOLD it is out of project. These pin the flag's trip from the reply to
    // the signal the pane binds to.
    void resolvePathCarriesTheRepositoryRootFlag();
    void resolvePathFailuresLeaveTheFlagUndetermined();
    void pinnedInternalUrlSurvivesEviction();
    void pinsAreCountedAndReleasable();
    void responseHeadersLetSvgStyleItself();
    void refusalMessagesExplainThemselves();
    void internalRequestFailuresReachQml();
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
void TstViewers::openAsKindsFollowRegistryClaims()
{
    QCOMPARE(ViewerHandlerRegistry::applicableViewKinds(
                 QUrl(QStringLiteral("file:///repo/main.cpp"))),
             QStringList({QStringLiteral("editor"), QStringLiteral("text")}));
    QCOMPARE(ViewerHandlerRegistry::applicableViewKinds(
                 QUrl(QStringLiteral("file:///repo/index.html"))),
             QStringList({QStringLiteral("editor"), QStringLiteral("text"),
                          QStringLiteral("web")}));
    QCOMPARE(ViewerHandlerRegistry::applicableViewKinds(
                 QUrl(QStringLiteral("file:///repo/logo.png"))),
             QStringList({QStringLiteral("image")}));
    QCOMPARE(ViewerHandlerRegistry::applicableViewKinds(
                 QUrl(QStringLiteral("file:///repo/archive.bin"))),
             QStringList({QStringLiteral("binary")}));
    QCOMPARE(ViewerHandlerRegistry::applicableViewKinds(
                 QUrl(QStringLiteral("file:///repo/src/"))),
             QStringList({QStringLiteral("directory")}));
    QCOMPARE(ViewerHandlerRegistry::applicableViewKinds(
                 QUrl(QStringLiteral("https://example.test/"))),
             QStringList({QStringLiteral("web")}));
}

void TstViewers::applicationSchemeValidationAndEscaping()
{
    for (const QString &scheme :
         {QStringLiteral("zed"), QStringLiteral("my-app"),
          QStringLiteral("org.example.viewer"), QStringLiteral("x+tool2")})
        QVERIFY(ViewerHandlerRegistry::isValidApplicationScheme(scheme));

    for (const QString &scheme :
         {QString(), QStringLiteral("1app"), QStringLiteral("-app"),
          QStringLiteral("app_name"), QStringLiteral("app://"),
          QStringLiteral("http"), QStringLiteral("FILE"),
          QStringLiteral("codeharbor-internal")})
        QVERIFY2(!ViewerHandlerRegistry::isValidApplicationScheme(scheme),
                 qPrintable(scheme));

    const QUrl escaped = ViewerHandlerRegistry::applicationUrl(
        QStringLiteral("my-app"),
        QStringLiteral("/srv/repo/notes #1?draft=1%done"));
    QVERIFY(escaped.isValid());
    QCOMPARE(escaped.scheme(), QStringLiteral("my-app"));
    QCOMPARE(escaped.path(), QStringLiteral("/srv/repo/notes #1?draft=1%done"));
    QVERIFY(escaped.query().isEmpty());
    QVERIFY(escaped.fragment().isEmpty());
    QVERIFY(!ViewerHandlerRegistry::applicationUrl(
                 QStringLiteral("codeharbor-internal"),
                 QStringLiteral("/srv/repo/file")).isValid());
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
    // Download and Error both collapse onto the binary pane.
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
    //
    // Both answer SYNCHRONOUSLY, and that is deliberate rather than accidental.
    // Each is identified by a PATH the caller already composed before calling,
    // and every caller sets its matching state before it calls
    // (ViewerDirectoryView.reload, ViewerPane.submitAddress /
    // ViewerPane.checkRepoRoot), so a reply delivered inside the call is still
    // matched correctly.
    ViewerModel viewers;
    QSignalSpy dirErrors(&viewers, &ViewerModel::directoryError);
    QSignalSpy dirLists(&viewers, &ViewerModel::directoryListed);
    QSignalSpy resolveErrors(&viewers, &ViewerModel::pathResolveError);
    QSignalSpy resolved(&viewers, &ViewerModel::pathResolved);

    viewers.listDirectory(QStringLiteral("/p"));
    viewers.resolvePath(QStringLiteral("/p/README.md"), QStringLiteral("/p"));

    QCOMPARE(dirErrors.size(), 1);
    QCOMPARE(resolveErrors.size(), 1);
    QCOMPARE(dirLists.size(), 0);
    QCOMPARE(resolved.size(), 0);
    // Each failure echoes the path it is about, which is how a pane tells its
    // own answer from another pane's on this shared model.
    QCOMPARE(dirErrors.first().at(0).toString(), QStringLiteral("/p"));
    QCOMPARE(resolveErrors.first().at(0).toString(),
             QStringLiteral("/p/README.md"));
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

void TstViewers::pinnedInternalUrlSurvivesEviction()
{
    // A pane that is DISPLAYING a remote image holds its internal URL and never
    // re-resolves it. Recency alone therefore cannot protect it: once cap other
    // files have been opened, the address still on screen ages out and the
    // pane's next reload fails on a URL the user can see. Pinning is the signal
    // that carries "still displayed" into the table.
    const QUrl shown(QStringLiteral("file:///p/shown.png"));

    InternalUrlMap map(4);
    const QString pinned = map.internalUrlFor(shown);
    QVERIFY(map.retain(pinned));
    QCOMPARE(map.retainedCount(), 1);

    // Twenty other files pass through a table capped at four. Under plain LRU
    // the displayed one is long gone.
    for (int i = 0; i < 20; ++i)
        map.internalUrlFor(QUrl(QStringLiteral("file:///p/other%1.txt").arg(i)));

    // Still resolvable, which is what the pane's reload needs.
    QCOMPARE(map.fileUrlFor(pinned), shown);
    // And still the SAME id: re-minting must not hand the pane a second
    // identity for a file it is already showing.
    QCOMPARE(map.internalUrlFor(shown), pinned);

    // The pin is an exemption for one entry, not a suspension of the bound: the
    // unpinned remainder is still capped, so the table cannot grow with the
    // number of files opened.
    QCOMPARE(map.size(), map.maxEntries() + 1);

    // Released, the entry is ordinary again and ordinary traffic evicts it.
    map.release(pinned);
    QCOMPARE(map.retainedCount(), 0);
    // release() re-runs eviction at once, so the entry that was over the cap
    // while pinned no longer is.
    QCOMPARE(map.size(), map.maxEntries());
    for (int i = 0; i < 5; ++i)
        map.internalUrlFor(QUrl(QStringLiteral("file:///p/after%1.txt").arg(i)));
    QVERIFY(!map.fileUrlFor(pinned).isValid());
}

void TstViewers::pinsAreCountedAndReleasable()
{
    // Two panes may show the same file. Pins are counted so one pane closing
    // cannot unpin the other's still-visible resource.
    const QUrl shared(QStringLiteral("file:///p/shared.png"));
    InternalUrlMap map(1);
    const QString url = map.internalUrlFor(shared);

    QVERIFY(map.retain(url));
    QVERIFY(map.retain(url));
    QCOMPARE(map.retainedCount(), 1); // one entry, two holders

    // The cap counts only unpinned entries, so a newly minted URL is never
    // evicted the instant it is handed out just because the table is full of
    // things panes are showing.
    const QString pressure =
        map.internalUrlFor(QUrl(QStringLiteral("file:///p/pressure.txt")));
    QCOMPARE(map.fileUrlFor(pressure),
             QUrl(QStringLiteral("file:///p/pressure.txt")));

    map.release(url); // first pane closes
    QCOMPARE(map.retainedCount(), 1); // the second pane still holds it
    map.internalUrlFor(QUrl(QStringLiteral("file:///p/pressure2.txt")));
    QCOMPARE(map.fileUrlFor(url), shared);

    map.release(url); // second pane closes
    QCOMPARE(map.retainedCount(), 0);
    map.internalUrlFor(QUrl(QStringLiteral("file:///p/pressure3.txt")));
    QVERIFY(!map.fileUrlFor(url).isValid());

    // A release with no matching retain is a no-op, not an underflow that would
    // let the next retain be cancelled by a stale one.
    const QString again = map.internalUrlFor(shared);
    map.release(again);
    map.release(again);
    QVERIFY(map.retain(again));
    map.internalUrlFor(QUrl(QStringLiteral("file:///p/pressure4.txt")));
    QCOMPARE(map.fileUrlFor(again), shared);

    // Nothing is pinned by an address the table does not know, or by one whose
    // authority the scheme handler would refuse — the caller is told so and
    // must not then issue a release.
    QVERIFY(!map.retain(QStringLiteral("codeharbor-internal://file/deadbeef")));
    QVERIFY(!map.retain(QStringLiteral("codeharbor-internal://evil/")
                        + again.mid(InternalUrlMap::prefix().size())));
    QCOMPARE(map.retainedCount(), 1);
}

void TstViewers::responseHeadersLetSvgStyleItself()
{
    // The active-content lockdown must not also break the documents it is
    // guarding. `default-src 'none'` alone forbids the <style> block and
    // style="..." attributes INSIDE an SVG, so the most common active-content
    // file a user opens here renders unstyled with nothing to say why.
    const QMultiMap<QByteArray, QByteArray> svg =
        InternalUrlSchemeHandler::responseHeadersFor(
            QByteArrayLiteral("image/svg+xml"));
    const QByteArray csp =
        svg.value(QByteArrayLiteral("Content-Security-Policy"));
    QVERIFY2(csp.contains(QByteArrayLiteral("style-src 'unsafe-inline'")),
             qPrintable(QStringLiteral("CSP blocks the SVG's own styles: %1")
                            .arg(QString::fromLatin1(csp))));
    // The exfiltration defence is unchanged: no script, no subresource, no
    // fetch, and the document still runs in a sandboxed opaque origin. Inline
    // CSS cannot execute, and every channel it could smuggle bytes out through
    // (background-image: url(), @import, font-src) is a resource load that
    // default-src still refuses.
    QVERIFY(csp.contains(QByteArrayLiteral("default-src 'none'")));
    QVERIFY(csp.contains(QByteArrayLiteral("sandbox")));
    QVERIFY(!csp.contains(QByteArrayLiteral("script-src")));
    QVERIFY(!csp.contains(QByteArrayLiteral("'unsafe-eval'")));

    // Inert types get no CSP at all, exactly as before.
    const QMultiMap<QByteArray, QByteArray> png =
        InternalUrlSchemeHandler::responseHeadersFor(
            QByteArrayLiteral("image/png"));
    QVERIFY(!png.contains(QByteArrayLiteral("Content-Security-Policy")));

    // Both carry the sniffing guard, without which Chromium could reinterpret
    // untrusted bytes as HTML and dodge the CSP gate entirely...
    for (const QMultiMap<QByteArray, QByteArray> &headers : {svg, png}) {
        QCOMPARE(headers.value(QByteArrayLiteral("X-Content-Type-Options")),
                 QByteArrayLiteral("nosniff"));
        // ...and both state that ranges are not served. Every reply is a whole
        // buffer under an implicit 200; QWebEngineUrlRequestJob cannot answer
        // 206, so honouring a Range would label a fragment as the complete
        // resource. Saying "none" is the honest answer.
        QCOMPARE(headers.value(QByteArrayLiteral("Accept-Ranges")),
                 QByteArrayLiteral("none"));
    }
}

void TstViewers::refusalMessagesExplainThemselves()
{
    using Failure = InternalUrlSchemeHandler::Failure;
    // QWebEngineUrlRequestJob::fail() carries no text, so an oversized image or
    // PDF used to reach the pane as a blank failed page. Every refusal now has
    // a sentence, and none of them may be empty.
    const QList<Failure> all = {
        Failure::MethodNotAllowed, Failure::UnknownHost,
        Failure::UnknownResource,  Failure::NotARemoteFile,
        Failure::EmptyPath,        Failure::NoClient,
        Failure::ReadFailed,       Failure::TooLarge,
        Failure::UndecodableContent,
    };
    QSet<QString> seen;
    for (Failure f : all) {
        const QString message = InternalUrlSchemeHandler::failureMessage(f);
        QVERIFY2(!message.isEmpty(), "a refusal with nothing to say");
        seen.insert(message);
    }
    // Distinct causes must not collapse onto one sentence, or the pane cannot
    // tell an evicted address from an unreadable file.
    QCOMPARE(seen.size(), all.size());

    // The one refusal with an actionable cause, so it is the one whose exact
    // wording is worth pinning: the user is told the file is too big, not that
    // "the file could not be displayed". Produced by the `truncated` branch of
    // requestStarted()'s reply and shown verbatim by the pane through
    // ViewerModel::internalResourceError. It used to have a twin in
    // ViewerModel::settleTextRead and this assertion kept the two in step; that
    // path was deleted with the read-only text view, so this is now the only
    // producer of the sentence rather than one of two.
    QCOMPARE(InternalUrlSchemeHandler::failureMessage(Failure::TooLarge),
             QStringLiteral("file is too large to display inline"));

    // A server error is quoted rather than swallowed, and an empty detail does
    // not leave a dangling colon.
    QVERIFY(InternalUrlSchemeHandler::failureMessage(Failure::ReadFailed,
                                                     QStringLiteral("EACCES"))
                .contains(QStringLiteral("EACCES")));
    QVERIFY(!InternalUrlSchemeHandler::failureMessage(Failure::ReadFailed)
                 .endsWith(QLatin1Char(':')));
}

void TstViewers::internalRequestFailuresReachQml()
{
    // The reason has to travel all the way to the pane, which only talks to
    // ViewerModel. ViewerProfiles owns the handler, so the model forwards.
    ViewerModel viewers;
    auto *profiles = new ViewerProfiles(nullptr, &viewers);
    viewers.setProfiles(profiles);
    InternalUrlSchemeHandler *const handler = profiles->internalSchemeHandler();
    QVERIFY(handler);

    QSignalSpy failures(&viewers, &ViewerModel::internalResourceError);
    const QUrl address(QStringLiteral("codeharbor-internal://file/abc123"));
    emit handler->requestFailed(
        address, InternalUrlSchemeHandler::Failure::TooLarge,
        InternalUrlSchemeHandler::failureMessage(
            InternalUrlSchemeHandler::Failure::TooLarge));
    QCOMPARE(failures.size(), 1);
    // The pane matches the address against the one it is showing, exactly as it
    // matches a path on the other reply signals.
    QCOMPARE(failures.at(0).at(0).toUrl(), address);
    QCOMPARE(failures.at(0).at(1).toString(),
             QStringLiteral("file is too large to display inline"));

    // Re-pointing the model at other profiles must not leave the old handler
    // forwarding as well: a pane would then see one failure reported twice.
    auto *replacement = new ViewerProfiles(nullptr, &viewers);
    viewers.setProfiles(replacement);
    emit handler->requestFailed(address,
                                InternalUrlSchemeHandler::Failure::NoClient,
                                QStringLiteral("no remote client is connected"));
    QCOMPARE(failures.size(), 1);
    emit replacement->internalSchemeHandler()->requestFailed(
        address, InternalUrlSchemeHandler::Failure::NoClient,
        QStringLiteral("no remote client is connected"));
    QCOMPARE(failures.size(), 2);
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
