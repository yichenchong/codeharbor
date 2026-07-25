// LIVE gate for the viewer workstream (SPEC 7.4/7.5, docs/PLAN.md V).
//
// Everything here runs against a REAL sshd and a REAL remote codeharbord: the
// bytes rendered by Chromium and listed by the directory pane are read off the
// remote filesystem over an SSH channel by the production stack
// (SessionBootstrap -> SshChannelDevice -> CodeharbordClient ->
// InternalUrlSchemeHandler / ViewerModel). No mocks, no fixture-in-a-QBuffer.
//
// It proves four things the unit suite structurally cannot:
//   (a) a codeharbor-internal:// URL loaded into a WebEngineView on the
//       privileged profile really delivers remote file bytes into the DOM —
//       verified by reading them back out with runJavaScript();
//   (b) the REAL recursive src/qml/ViewerRegion.qml lays a branch tree out as a
//       genuine split: every pane non-zero, tiling the parent along the split
//       axis and spanning it across — for 2 and 3 children, both orientations,
//       even division and explicit persisted `ratios`, plus a pane added to an
//       ALREADY laid-out split (the one-shot sizing latch's reset path);
//   (c) the handler registry dispatches real remote URLs to the right view kind
//       and the resulting pane populates from live server data — the directory
//       pane lists the remote entries this test created, over file.listDirectory
//       (schema v2), right down to the rendered delegate strings;
//   (d) the tree actually RASTERISES: a grabbed frame is non-empty and not one
//       flat colour, and the PNG is written under the build directory.
//
// Skipped wholesale unless CH_LIVE_SSH is set, so the default suite stays green
// on a machine with no fixture. The ctest registration pins the headless recipe
// (offscreen platform, software Quick backend, Chromium sandbox/GPU off).

#include "CodeharbordClient.h"
#include "InternalUrlSchemeHandler.h"
#include "SessionBootstrap.h"
#include "SshChannelDevice.h"
#include "SshConnectionPool.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"

#include <QtTest>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>
#include <QtQuickControls2/QQuickStyle>
#include <QtWebEngineQuick/QtWebEngineQuick>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

using ch::CodeharbordClient;
using ch::InternalUrlSchemeHandler;
using ch::RpcError;
using ch::SessionBootstrap;
using ch::SshChannelDevice;
using ch::SshConnectionPool;
using ch::ViewerModel;
using ch::ViewerProfiles;

namespace {

// SSH connect + a remote node cold start (which also type-strips the TypeScript
// entry point) is measured in seconds, not milliseconds.
constexpr int kExecTimeoutMs = 30000;
constexpr int kRpcTimeoutMs = 60000;
// Chromium's first navigation pays for the zygote and renderer start.
constexpr int kLoadTimeoutMs = 60000;
constexpr int kJsTimeoutMs = 20000;

// Widest plausible SplitView handle; the split-axis arithmetic tolerates it.
constexpr qreal kMaxHandleExtent = 16.0;
// Sub-pixel slack for layout arithmetic done in qreal.
constexpr qreal kEpsilon = 1.0;

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 640;

// Field separator for the JS -> C++ round trip. Plain ASCII (a control
// character would need a C++ universal-character-name, which is ill-formed
// below U+00A0) and absent from every string this test puts on the wire.
constexpr auto kJsSeparator = "@@CH@@";

QString sq(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

// ---------------------------------------------------------------------------
// QML shells. Each hosts a REAL production QML file loaded from the CodeHarbor
// module's qrc — the same URL the shipped binary resolves — inside a real
// Window so a scene graph exists and frames are actually produced.
// ---------------------------------------------------------------------------

constexpr auto kInternalSchemeShell = R"QML(
import QtQuick
import QtQuick.Window
import QtWebEngine

Window {
    id: win
    width: 800
    height: 600
    visible: true

    property url pageUrl: ""

    WebEngineView {
        id: view
        objectName: "internalView"
        anchors.fill: parent

        // The privileged profile — the one carrying InternalUrlSchemeHandler.
        profile: viewers.internalProfile()
        // Scripting on so the test can read the delivered bytes back out of the
        // DOM. The production viewer views disable it; this is the probe.
        settings.javascriptEnabled: true
        url: win.pageUrl

        property bool finished: false
        property bool succeeded: false
        property string failureText: ""
        property bool jsFinished: false
        property string jsResult: ""

        onLoadingChanged: function(request) {
            if (request.status === WebEngineView.LoadSucceededStatus) {
                view.succeeded = true;
                view.finished = true;
            } else if (request.status === WebEngineView.LoadFailedStatus) {
                view.succeeded = false;
                view.failureText = request.errorString + " (" + request.errorCode + ")";
                view.finished = true;
            }
        }

        function readDocument() {
            view.runJavaScript(
                "[document.title," +
                " document.body ? document.body.innerText.length : -1," +
                " document.body ? document.body.innerText : ''].join('@@CH@@')",
                function(result) {
                    view.jsResult = String(result);
                    view.jsFinished = true;
                });
        }
    }
}
)QML";

constexpr auto kSplitTreeShell = R"QML(
import QtQuick
import QtQuick.Window

Window {
    id: win
    width: 960
    height: 640
    visible: true
    color: "#11111b"

    property var treeNode: null

    Loader {
        id: regionLoader
        objectName: "regionLoader"
        anchors.fill: parent
        // `node` must ride along with setSource: the recursive region type is
        // inert until it has one, and a declarative source would complete the
        // child tree before its node arrived.
        Component.onCompleted: setSource("qrc:/qt/qml/CodeHarbor/ViewerRegion.qml",
                                         { node: win.treeNode })
    }
}
)QML";

constexpr auto kTextViewShell = R"QML(
import QtQuick
import QtQuick.Window

Window {
    id: win
    width: 800
    height: 500
    visible: true

    property url fileUrl: ""

    Loader {
        id: textLoader
        objectName: "textLoader"
        anchors.fill: parent
        Component.onCompleted: setSource("qrc:/qt/qml/CodeHarbor/ViewerTextView.qml",
                                         { url: win.fileUrl })
    }
}
)QML";

// ---------------------------------------------------------------------------
// Tree walking. Structural, not name-based: the generated QML metaobjects are
// called "ViewerPane_QMLTYPE_37" and friends. Both the QObject child list and
// the QQuickItem visual child list must be walked, because QML populates
// Loader/Repeater content through paths that skip QObject re-parenting.
// ---------------------------------------------------------------------------

using Predicate = std::function<bool(QObject *)>;

void collectInto(QObject *root, const Predicate &match, QSet<QObject *> &visited,
                 QList<QObject *> &out)
{
    if (!root || visited.contains(root))
        return;
    visited.insert(root);

    if (match(root))
        out.append(root);

    const auto objectChildren = root->children();
    for (QObject *child : objectChildren)
        collectInto(child, match, visited, out);

    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        const auto itemChildren = item->childItems();
        for (QQuickItem *child : itemChildren)
            collectInto(child, match, visited, out);
    }
}

QList<QObject *> collect(QObject *root, const Predicate &match)
{
    QSet<QObject *> visited;
    QList<QObject *> out;
    collectInto(root, match, visited, out);
    return out;
}

bool hasProperty(const QObject *object, const char *name)
{
    return object->metaObject()->indexOfProperty(name) >= 0;
}

// A leaf pane (ViewerPane): carries `paneId` but not `node`. ViewerRegion,
// which carries the node, is excluded by the second half.
bool isLeafPane(QObject *object)
{
    return hasProperty(object, "paneId") && !hasProperty(object, "node");
}

// ViewerDirectoryView: the only viewer type exposing this exact triple.
bool isDirectoryView(QObject *object)
{
    return hasProperty(object, "entries") && hasProperty(object, "errorText")
           && hasProperty(object, "url");
}

// Flat, geometry-annotated dump printed only when an assertion fails.
QString dumpTree(QObject *root)
{
    QString out;
    const QList<QObject *> all = collect(root, [](QObject *) { return true; });
    for (QObject *object : all) {
        out += QLatin1String("  ") + QString::fromLatin1(object->metaObject()->className());
        if (auto *item = qobject_cast<QQuickItem *>(object)) {
            out += QStringLiteral(" [%1x%2 @%3,%4 visible=%5]")
                       .arg(item->width())
                       .arg(item->height())
                       .arg(item->x())
                       .arg(item->y())
                       .arg(item->isVisible() ? 1 : 0);
        }
        out += QLatin1Char('\n');
    }
    return out;
}

// Spin the event loop until `ready` or the deadline elapses. QTRY_* covers the
// assertions; this is for plumbing steps whose failure needs a custom message.
bool waitFor(const std::function<bool()> &ready, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeoutMs)
        QTest::qWait(20);
    return ready();
}

// Number of DISTINCT colours in an image, capped so a rich frame does not build
// an unbounded set. Exactly one colour == nothing rasterised past the clear.
int distinctColours(const QImage &image, int cap = 4096)
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    QSet<QRgb> seen;
    for (int y = 0; y < rgb.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); ++x) {
            seen.insert(line[x]);
            if (seen.size() >= cap)
                return seen.size();
        }
    }
    return static_cast<int>(seen.size());
}

// The inner SplitView built by ViewerRegion's branchComponent, identified by the
// one-shot sizing latch it owns. Structural, so it survives the generated
// "SplitView_QMLTYPE_n" metaobject names.
bool isRegionSplitView(QObject *object)
{
    return hasProperty(object, "ratiosApplied") && hasProperty(object, "orientation");
}

// One measured split: the panes ordered along the split axis, their extent on
// that axis, their extent across it, and the gaps (drag handles) between them.
struct SplitMeasurement {
    QList<QQuickItem *> panes;
    QList<QRectF> rects;
    QList<qreal> extents;
    QList<qreal> crossExtents;
    QList<qreal> gaps; // panes.size() - 1 entries
    qreal total = 0;
    qreal crossTotal = 0;
    qreal firstPos = 0;

    qreal gapSum() const
    {
        qreal sum = 0;
        for (qreal gap : gaps)
            sum += gap;
        return sum;
    }

    qreal extentSum() const
    {
        qreal sum = 0;
        for (qreal extent : extents)
            sum += extent;
        return sum;
    }

    QString describe() const
    {
        QString out = QStringLiteral("parent=%1 (cross %2)").arg(total).arg(crossTotal);
        for (int i = 0; i < rects.size(); ++i) {
            out += QStringLiteral(" pane%1=(%2,%3 %4x%5)")
                       .arg(i)
                       .arg(rects.at(i).x())
                       .arg(rects.at(i).y())
                       .arg(rects.at(i).width())
                       .arg(rects.at(i).height());
        }
        for (int i = 0; i < gaps.size(); ++i)
            out += QStringLiteral(" handle%1=%2").arg(i).arg(gaps.at(i));
        return out;
    }
};

// Measure `panes` in `region`'s coordinate space, ordered along the split axis.
SplitMeasurement measureSplit(QQuickItem *region, bool horizontal,
                              const QList<QObject *> &panes)
{
    SplitMeasurement m;
    for (QObject *object : panes) {
        if (auto *item = qobject_cast<QQuickItem *>(object))
            m.panes.append(item);
    }
    // Repeater order is not a layout guarantee; position on the axis is.
    std::sort(m.panes.begin(), m.panes.end(), [region, horizontal](QQuickItem *a, QQuickItem *b) {
        const QPointF pa = a->mapToItem(region, QPointF(0, 0));
        const QPointF pb = b->mapToItem(region, QPointF(0, 0));
        return horizontal ? pa.x() < pb.x() : pa.y() < pb.y();
    });

    m.total = horizontal ? region->width() : region->height();
    m.crossTotal = horizontal ? region->height() : region->width();
    for (QQuickItem *pane : std::as_const(m.panes)) {
        const QRectF rect =
            pane->mapRectToItem(region, QRectF(0, 0, pane->width(), pane->height()));
        m.rects.append(rect);
        m.extents.append(horizontal ? rect.width() : rect.height());
        m.crossExtents.append(horizontal ? rect.height() : rect.width());
    }
    if (!m.rects.isEmpty())
        m.firstPos = horizontal ? m.rects.first().x() : m.rects.first().y();
    for (int i = 1; i < m.rects.size(); ++i) {
        const qreal prevEnd = horizontal ? m.rects.at(i - 1).right() : m.rects.at(i - 1).bottom();
        const qreal start = horizontal ? m.rects.at(i).x() : m.rects.at(i).y();
        m.gaps.append(start - prevEnd);
    }
    return m;
}

} // namespace

class TstLiveViewers : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // (a) Real remote bytes reach the DOM through codeharbor-internal://.
    void remoteBytesRenderThroughInternalScheme();

    // (c1) The registry dispatches REAL remote URLs to the right view kind, and
    //      the live server agrees about what each entry is.
    void registryResolvesLiveRemoteUrls();

    // (c2) A real text view populates from live file.readFile bytes.
    void textViewPopulatesFromLiveServer();

    // (b) + (c3) + (d) The real recursive region lays out a genuine split whose
    //      panes populate from live file.listDirectory data and rasterise.
    //      Covers 2 and 3 children, both orientations, even and explicit ratios.
    void splitTreeRendersLiveDirectoryPanes_data();
    void splitTreeRendersLiveDirectoryPanes();

    // (b) The path the one-shot sizing latch's reset defends: a pane added to
    //      an ALREADY laid-out split must be sized too, not come up zero-extent.
    void splitTreeSizesPaneAddedAfterFirstLayout();

private:
    bool remoteExec(const QString &command, QByteArray *stdoutText = nullptr,
                    QString *stderrText = nullptr);
    QString dirUrl() const;
    QString alphaPath() const { return m_remoteDir + QStringLiteral("/ch-live-alpha.txt"); }
    QString betaPath() const { return m_remoteDir + QStringLiteral("/ch-live-beta.json"); }

    // Shared by both split tests: builds the shell window, waits for the real
    // ViewerRegion to load, and hands back the window plus the region root.
    // Returns nullptr (after recording a test failure) if either never appears.
    QObject *openSplitShell(const QVariantMap &node, std::unique_ptr<QObject> *keepAlive,
                            QQuickWindow **window, QQuickItem **region);

    // Declaration order IS construction order; the engine is declared last so
    // it is destroyed FIRST and no QML tree unbinds against a dead ViewerModel.
    SshConnectionPool m_pool;
    CodeharbordClient m_client;
    ViewerProfiles m_profiles{&m_client};
    ViewerModel m_viewers{&m_client};
    std::unique_ptr<SessionBootstrap> m_bootstrap;
    std::unique_ptr<QQmlEngine> m_engine;

    QString m_remoteDir;
    QString m_marker;
    QStringList m_bootstrapErrors;
    bool m_live = false;
};

void TstLiveViewers::initTestCase()
{
    if (qEnvironmentVariableIsEmpty("CH_LIVE_SSH"))
        QSKIP("CH_LIVE_SSH is not set; live viewer gate skipped");
    if (!SshConnectionPool::libsshAvailable())
        QSKIP("built without libssh; live viewer gate skipped");

    m_viewers.setProfiles(&m_profiles);

    // The production seam: pool connect -> codeharbord over an SSH Rpc channel
    // -> client->setTransport(). A null AgentStatusMonitor is tolerated (that is
    // workstream A's gate), but the bridge channel is opened regardless, exactly
    // as the shipped app opens it.
    m_bootstrap = std::make_unique<SessionBootstrap>(&m_pool, &m_client, nullptr);
    connect(m_bootstrap.get(), &SessionBootstrap::error, this,
            [this](const QString &message) { m_bootstrapErrors.append(message); });

    QVERIFY2(m_bootstrap->connectAndWireFromEnvironment(),
             qPrintable(QStringLiteral("SessionBootstrap could not wire a live session: %1")
                            .arg(m_bootstrapErrors.join(QStringLiteral(" | ")))));
    QCOMPARE(m_pool.state(), SshConnectionPool::State::Connected);
    QVERIFY(m_bootstrap->rpcDevice() != nullptr);
    QCOMPARE(m_client.transport(), static_cast<QIODevice *>(m_bootstrap->rpcDevice()));

    // Prove the RPC service actually answers before anything renders, so a dead
    // server fails here with a clear message instead of as a blank page.
    QJsonValue info;
    std::optional<RpcError> rpcError;
    bool answered = false;
    m_client.call(QStringLiteral("server.info"), QJsonValue(),
                  [&](QJsonValue value, std::optional<RpcError> err) {
                      info = value;
                      rpcError = err;
                      answered = true;
                  });
    QVERIFY2(waitFor([&] { return answered; }, kRpcTimeoutMs),
             qPrintable(QStringLiteral("server.info never answered. bootstrap: %1")
                            .arg(m_bootstrapErrors.join(QStringLiteral(" | ")))));
    QVERIFY2(!rpcError.has_value(), qPrintable(rpcError ? rpcError->message : QString()));
    QCOMPARE(info.toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("codeharbord"));
    QVERIFY(info.toObject().value(QStringLiteral("schemaVersion")).toInt(-1) >= 2);

    // Remote fixture: a directory with two files and a sub-directory, created
    // ON THE SERVER over a second SSH channel.
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    m_remoteDir = QStringLiteral("/tmp/ch-live-viewers-%1").arg(token);
    m_marker = QStringLiteral("CODEHARBOR_LIVE_VIEWER_MARKER_%1").arg(token.toUpper());

    const QString setup =
        QStringList{
            QStringLiteral("mkdir -p"),
            sq(m_remoteDir + QStringLiteral("/ch-live-subdir")),
            QStringLiteral("&& printf '%s\\n'"),
            sq(m_marker),
            sq(QStringLiteral("second line: these bytes crossed a real ssh channel")),
            QStringLiteral(">"),
            sq(alphaPath()),
            QStringLiteral("&& printf '%s\\n'"),
            sq(QStringLiteral("{\"marker\":\"") + m_marker + QStringLiteral("\"}")),
            QStringLiteral(">"),
            sq(betaPath()),
            QStringLiteral("&& echo SETUP_OK"),
        }
            .join(QLatin1Char(' '));

    QByteArray setupOut;
    QString setupErr;
    QVERIFY2(remoteExec(setup, &setupOut, &setupErr),
             qPrintable(QStringLiteral("remote fixture setup timed out: %1").arg(setupErr)));
    QVERIFY2(setupOut.contains("SETUP_OK"),
             qPrintable(QStringLiteral("remote fixture setup failed: out=%1 err=%2")
                            .arg(QString::fromUtf8(setupOut), setupErr)));

    m_engine = std::make_unique<QQmlEngine>();
    m_engine->rootContext()->setContextProperty(QStringLiteral("viewers"), &m_viewers);

    m_live = true;
    qInfo("live viewer fixture ready: remoteDir=%s marker=%s", qPrintable(m_remoteDir),
          qPrintable(m_marker));
}

void TstLiveViewers::cleanupTestCase()
{
    // Engine first: the QML trees must unbind while `viewers` is still alive.
    m_engine.reset();

    if (m_live && !m_remoteDir.isEmpty()) {
        QByteArray out;
        QString err;
        const bool ok =
            remoteExec(QStringLiteral("rm -rf ") + sq(m_remoteDir)
                           + QStringLiteral(" && echo CLEANUP_OK"),
                       &out, &err);
        if (!ok || !out.contains("CLEANUP_OK")) {
            qWarning("remote cleanup of %s may have failed: out=%s err=%s",
                     qPrintable(m_remoteDir), out.constData(), qPrintable(err));
        } else {
            qInfo("removed remote fixture %s", qPrintable(m_remoteDir));
        }
    }

    m_bootstrap.reset();
    m_pool.disconnectFromHost();
}

QString TstLiveViewers::dirUrl() const
{
    // A remote directory is expressed as a file URL with a trailing slash
    // (ViewerHandlerRegistry::resolve).
    return QUrl::fromLocalFile(m_remoteDir).toString() + QLatin1Char('/');
}

bool TstLiveViewers::remoteExec(const QString &command, QByteArray *stdoutText,
                                QString *stderrText)
{
    SshChannelDevice device(&m_pool, SshConnectionPool::ChannelKind::Exec);
    QByteArray out;
    QString err;
    bool finished = false;
    connect(&device, &QIODevice::readyRead, &device, [&] { out += device.readAll(); });
    connect(&device, &SshChannelDevice::channelError, &device,
            [&](const QString &text) { err += text; });
    connect(&device, &QIODevice::readChannelFinished, &device, [&] { finished = true; });

    const bool started = device.startExec(command);
    if (started)
        waitFor([&] { return finished; }, kExecTimeoutMs);
    device.closeChannel();

    if (stdoutText)
        *stdoutText = out;
    if (stderrText)
        *stderrText = err;
    return started && finished;
}

// ---------------------------------------------------------------------------
// (a) Remote bytes through the internal scheme.
// ---------------------------------------------------------------------------
void TstLiveViewers::remoteBytesRenderThroughInternalScheme()
{
    const QUrl fileUrl = QUrl::fromLocalFile(alphaPath());
    const QString internalUrl = m_viewers.internalUrlFor(fileUrl);
    QVERIFY2(internalUrl.startsWith(QStringLiteral("codeharbor-internal://file/")),
             qPrintable(internalUrl));
    // Opaque: the remote path must not leak into the URL Chromium navigates to.
    QVERIFY(!internalUrl.contains(QStringLiteral("ch-live-alpha")));
    QCOMPARE(m_viewers.fileUrlFor(internalUrl), fileUrl);

    QQmlComponent component(m_engine.get());
    component.setData(QByteArray(kInternalSchemeShell),
                      QUrl(QStringLiteral("qrc:/tst_liveviewers/internal.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));

    std::unique_ptr<QObject> root(component.createWithInitialProperties(
        {{QStringLiteral("pageUrl"), internalUrl}}));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QObject *view = root->findChild<QObject *>(QStringLiteral("internalView"));
    QVERIFY(view != nullptr);

    QVERIFY2(waitFor([&] { return view->property("finished").toBool(); }, kLoadTimeoutMs),
             "WebEngine never reported a load result for the internal URL");
    QVERIFY2(view->property("succeeded").toBool(),
             qPrintable(QStringLiteral("internal-scheme load FAILED: %1")
                            .arg(view->property("failureText").toString())));

    QVERIFY(QMetaObject::invokeMethod(view, "readDocument"));
    QVERIFY2(waitFor([&] { return view->property("jsFinished").toBool(); }, kJsTimeoutMs),
             "runJavaScript() never returned for the internal URL");

    const QStringList parts = view->property("jsResult").toString().split(
        QLatin1String(kJsSeparator));
    QCOMPARE(parts.size(), 3);
    const QString title = parts.at(0);
    const int bodyLength = parts.at(1).toInt();
    const QString body = parts.at(2);

    qInfo("internal-scheme DOM: title=\"%s\" innerText.length=%d body=\"%s\"",
          qPrintable(title), bodyLength,
          qPrintable(QString(body).trimmed().replace(QLatin1Char('\n'), QLatin1String(" / "))));

    // The whole point: the bytes in the DOM are the bytes on the remote disk.
    QVERIFY2(body.contains(m_marker),
             qPrintable(QStringLiteral("remote marker %1 is not in the rendered document: \"%2\"")
                            .arg(m_marker, body)));
    QVERIFY2(body.contains(QStringLiteral("these bytes crossed a real ssh channel")),
             qPrintable(body));
    QVERIFY(bodyLength >= m_marker.size());

    // ...and the handler declared the right type for the REMOTE path even
    // though the navigated URL is an opaque, extensionless id.
    QCOMPARE(InternalUrlSchemeHandler::mimeForPath(alphaPath()),
             QByteArrayLiteral("text/plain"));
}

// ---------------------------------------------------------------------------
// (c1) Registry dispatch on REAL remote URLs.
// ---------------------------------------------------------------------------
void TstLiveViewers::registryResolvesLiveRemoteUrls()
{
    QCOMPARE(m_viewers.viewKind(QUrl::fromLocalFile(alphaPath())), QStringLiteral("text"));
    QCOMPARE(m_viewers.viewKind(QUrl::fromLocalFile(betaPath())), QStringLiteral("text"));
    QCOMPARE(m_viewers.viewKind(QUrl(dirUrl())), QStringLiteral("directory"));

    // And the same classification taken from live server metadata rather than
    // from the string: file.listDirectory reports what each entry IS.
    QVariantList entries;
    QString listError;
    bool answered = false;
    connect(&m_viewers, &ViewerModel::directoryListed, this,
            [&](const QString &, const QVariantList &list) {
                entries = list;
                answered = true;
            });
    connect(&m_viewers, &ViewerModel::directoryError, this,
            [&](const QString &, const QString &message) {
                listError = message;
                answered = true;
            });
    m_viewers.listDirectory(m_remoteDir);
    QVERIFY2(waitFor([&] { return answered; }, kRpcTimeoutMs),
             "file.listDirectory never answered");
    disconnect(&m_viewers, nullptr, this, nullptr);
    QVERIFY2(listError.isEmpty(), qPrintable(listError));

    QStringList names;
    QString subdirKind;
    QString alphaKind;
    for (const QVariant &entry : std::as_const(entries)) {
        const QVariantMap map = entry.toMap();
        const QString name = map.value(QStringLiteral("name")).toString();
        names.append(name);
        if (name == QLatin1String("ch-live-subdir"))
            subdirKind = map.value(QStringLiteral("kind")).toString();
        if (name == QLatin1String("ch-live-alpha.txt"))
            alphaKind = map.value(QStringLiteral("kind")).toString();
    }
    qInfo("file.listDirectory(%s) -> [%s]", qPrintable(m_remoteDir),
          qPrintable(names.join(QStringLiteral(", "))));

    QCOMPARE(names.size(), 3);
    // ViewerModel sorts directories first, then by name.
    QCOMPARE(names.constFirst(), QStringLiteral("ch-live-subdir"));
    QCOMPARE(subdirKind, QStringLiteral("directory"));
    QCOMPARE(alphaKind, QStringLiteral("file"));
    QVERIFY(names.contains(QStringLiteral("ch-live-alpha.txt")));
    QVERIFY(names.contains(QStringLiteral("ch-live-beta.json")));
}

// ---------------------------------------------------------------------------
// (c2) A real text view, populated by live file.readFile bytes.
// ---------------------------------------------------------------------------
void TstLiveViewers::textViewPopulatesFromLiveServer()
{
    QQmlComponent component(m_engine.get());
    component.setData(QByteArray(kTextViewShell),
                      QUrl(QStringLiteral("qrc:/tst_liveviewers/text.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));

    std::unique_ptr<QObject> root(component.createWithInitialProperties(
        {{QStringLiteral("fileUrl"), QUrl::fromLocalFile(alphaPath())}}));
    QVERIFY2(root != nullptr, qPrintable(component.errorString()));

    QObject *loader = root->findChild<QObject *>(QStringLiteral("textLoader"));
    QVERIFY(loader != nullptr);

    QQuickItem *textView = nullptr;
    QVERIFY2(waitFor(
                 [&] {
                     textView = qvariant_cast<QQuickItem *>(loader->property("item"));
                     return textView != nullptr;
                 },
                 kExecTimeoutMs),
             "ViewerTextView.qml never loaded");

    QTRY_VERIFY_WITH_TIMEOUT(textView->property("content").toString().contains(m_marker),
                             kRpcTimeoutMs);
    QVERIFY2(textView->property("errorText").toString().isEmpty(),
             qPrintable(textView->property("errorText").toString()));

    const QString content = textView->property("content").toString();
    qInfo("ViewerTextView content (%d chars): %s", static_cast<int>(content.size()),
          qPrintable(QString(content).trimmed().replace(QLatin1Char('\n'), QLatin1String(" / "))));
    QVERIFY(content.contains(QStringLiteral("these bytes crossed a real ssh channel")));
}

// ---------------------------------------------------------------------------
// (b) + (c3) + (d) The real recursive split tree.
// ---------------------------------------------------------------------------
QObject *TstLiveViewers::openSplitShell(const QVariantMap &node,
                                        std::unique_ptr<QObject> *keepAlive,
                                        QQuickWindow **window, QQuickItem **region)
{
    QQmlComponent component(m_engine.get());
    component.setData(QByteArray(kSplitTreeShell),
                      QUrl(QStringLiteral("qrc:/tst_liveviewers/split.qml")));
    if (component.isError()) {
        QTest::qFail(qPrintable(component.errorString()), __FILE__, __LINE__);
        return nullptr;
    }

    keepAlive->reset(component.createWithInitialProperties({{QStringLiteral("treeNode"), node}}));
    QObject *root = keepAlive->get();
    if (!root) {
        QTest::qFail(qPrintable(component.errorString()), __FILE__, __LINE__);
        return nullptr;
    }

    *window = qobject_cast<QQuickWindow *>(root);
    if (!*window) {
        QTest::qFail("shell root is not a QQuickWindow", __FILE__, __LINE__);
        return nullptr;
    }

    QObject *loader = root->findChild<QObject *>(QStringLiteral("regionLoader"));
    if (!loader) {
        QTest::qFail("regionLoader is missing from the shell", __FILE__, __LINE__);
        return nullptr;
    }

    QQuickItem *loaded = nullptr;
    const bool ok = waitFor(
        [&] {
            loaded = qvariant_cast<QQuickItem *>(loader->property("item"));
            return loaded != nullptr;
        },
        kExecTimeoutMs);
    if (!ok) {
        QTest::qFail("ViewerRegion.qml never loaded", __FILE__, __LINE__);
        return nullptr;
    }
    *region = loaded;
    return root;
}

void TstLiveViewers::splitTreeRendersLiveDirectoryPanes_data()
{
    QTest::addColumn<QString>("orientation");
    QTest::addColumn<int>("paneCount");
    // Empty == no `ratios` on the node, i.e. the even-division fallback.
    QTest::addColumn<QVariantList>("ratios");

    QTest::newRow("horizontal-even") << QStringLiteral("horizontal") << 2 << QVariantList{};
    QTest::newRow("vertical-even") << QStringLiteral("vertical") << 2 << QVariantList{};
    QTest::newRow("horizontal-three") << QStringLiteral("horizontal") << 3 << QVariantList{};
    QTest::newRow("horizontal-ratios")
        << QStringLiteral("horizontal") << 3 << QVariantList{1.0, 2.0, 1.0};
    QTest::newRow("vertical-ratios")
        << QStringLiteral("vertical") << 2 << QVariantList{2.0, 1.0};
}

void TstLiveViewers::splitTreeRendersLiveDirectoryPanes()
{
    QFETCH(QString, orientation);
    QFETCH(int, paneCount);
    QFETCH(QVariantList, ratios);
    const bool horizontal = orientation == QLatin1String("horizontal");

    // A branch with `paneCount` leaves, every one pointing at the REAL remote
    // directory this test created on the server.
    QVariantList children;
    for (int i = 0; i < paneCount; ++i) {
        children.append(
            QVariantMap{{QStringLiteral("paneId"), QStringLiteral("viewer-live-%1").arg(i)},
                        {QStringLiteral("url"), dirUrl()},
                        {QStringLiteral("children"), QVariantList{}}});
    }
    QVariantMap branch{{QStringLiteral("orientation"), orientation},
                       {QStringLiteral("children"), children}};
    if (!ratios.isEmpty())
        branch.insert(QStringLiteral("ratios"), ratios);

    // Expected share of the split axis per pane: the node's ratios normalized,
    // or an even division. This mirrors SPEC 4.5 persisted split ratios.
    QList<qreal> fractions;
    qreal ratioSum = 0;
    for (const QVariant &ratio : std::as_const(ratios))
        ratioSum += ratio.toDouble();
    for (int i = 0; i < paneCount; ++i) {
        fractions.append(ratios.isEmpty() ? 1.0 / paneCount
                                          : ratios.at(i).toDouble() / ratioSum);
    }

    std::unique_ptr<QObject> shell;
    QQuickWindow *window = nullptr;
    QQuickItem *region = nullptr;
    QVERIFY(openSplitShell(branch, &shell, &window, &region) != nullptr);
    QCOMPARE(window->width(), kWindowWidth);
    QCOMPARE(window->height(), kWindowHeight);

    // The region exposes the ratio policy; check it agrees before measuring, so
    // a layout failure is never confused with a bad ratio computation.
    for (int i = 0; i < paneCount; ++i) {
        QVariant returned;
        QVERIFY(QMetaObject::invokeMethod(region, "ratioFor", Q_RETURN_ARG(QVariant, returned),
                                          Q_ARG(QVariant, i), Q_ARG(QVariant, paneCount)));
        QVERIFY2(qAbs(returned.toDouble() - fractions.at(i)) < 1e-9,
                 qPrintable(QStringLiteral("ratioFor(%1,%2)=%3, expected %4")
                                .arg(i)
                                .arg(paneCount)
                                .arg(returned.toDouble())
                                .arg(fractions.at(i))));
    }

    // Panes come up through url-sourced Loaders inside a Repeater inside a
    // SplitView: asynchronous by construction, so poll.
    const auto panes = [region] { return collect(region, isLeafPane); };
    QTRY_VERIFY2_WITH_TIMEOUT(panes().size() == paneCount,
                              qPrintable(QStringLiteral("expected %1 leaf panes, found %2\n%3")
                                             .arg(paneCount)
                                             .arg(panes().size())
                                             .arg(dumpTree(region))),
                              kExecTimeoutMs);

    // Let the layout settle and the live directory RPCs land.
    QTest::qWait(400);

    // The one-shot sizing latch must have fired; without it every pane after the
    // first collapses to a Loader's implicit size of zero.
    const QList<QObject *> splitViews = collect(region, isRegionSplitView);
    QCOMPARE(splitViews.size(), 1);
    QVERIFY2(splitViews.constFirst()->property("ratiosApplied").toBool(),
             "SplitView.ratiosApplied is still false: no preferred sizes were applied");

    const SplitMeasurement m = measureSplit(region, horizontal, panes());
    QCOMPARE(m.panes.size(), paneCount);
    qInfo("split[%s]: %s", QTest::currentDataTag(), qPrintable(m.describe()));

    // -- genuine split -----------------------------------------------------
    for (int i = 0; i < paneCount; ++i) {
        QVERIFY2(m.panes.at(i)->isVisible(), qPrintable(m.describe()));
        QVERIFY2(m.rects.at(i).width() > 0 && m.rects.at(i).height() > 0,
                 qPrintable(QStringLiteral("pane %1 has zero extent: %2")
                                .arg(i)
                                .arg(m.describe())));
        // No degenerate sliver: every pane keeps most of its intended share.
        QVERIFY2(m.extents.at(i) > m.total * fractions.at(i) * 0.5,
                 qPrintable(QStringLiteral("pane %1 extent %2 is far below its %3 share: %4")
                                .arg(i)
                                .arg(m.extents.at(i))
                                .arg(fractions.at(i))
                                .arg(m.describe())));
        // Every pane spans the parent fully ACROSS the split axis — that is what
        // makes this a split rather than N independently placed items.
        QVERIFY2(qAbs(m.crossExtents.at(i) - m.crossTotal) <= kEpsilon,
                 qPrintable(m.describe()));
    }

    // The panes tile the parent along the split axis, modulo the drag handles.
    QVERIFY2(qAbs(m.firstPos) <= kEpsilon, qPrintable(m.describe()));
    QCOMPARE(m.gaps.size(), paneCount - 1);
    for (qreal gap : m.gaps) {
        QVERIFY2(gap >= -kEpsilon && gap <= kMaxHandleExtent,
                 qPrintable(QStringLiteral("handle %1 out of range: %2").arg(gap).arg(m.describe())));
    }
    QVERIFY2(qAbs(m.extentSum() + m.gapSum() - m.total) <= kEpsilon, qPrintable(m.describe()));

    // Each pane got ITS share, not merely a share: the handle deficit is taken
    // out of one pane by SplitView, so allow exactly that much drift and no more
    // — an even split still fails a 2:1 ratio row by a wide margin.
    const qreal proportionSlack = m.gapSum() + kEpsilon;
    for (int i = 0; i < paneCount; ++i) {
        const qreal expected = m.total * fractions.at(i);
        QVERIFY2(qAbs(m.extents.at(i) - expected) <= proportionSlack,
                 qPrintable(QStringLiteral("pane %1 extent %2, expected ~%3 (+/-%4): %5")
                                .arg(i)
                                .arg(m.extents.at(i))
                                .arg(expected)
                                .arg(proportionSlack)
                                .arg(m.describe())));
    }

    // -- live data in the panes -------------------------------------------
    for (QQuickItem *pane : std::as_const(m.panes))
        QCOMPARE(pane->property("kind").toString(), QStringLiteral("directory"));

    const auto directoryViews = [region] { return collect(region, isDirectoryView); };
    QTRY_VERIFY2_WITH_TIMEOUT(directoryViews().size() == paneCount,
                              qPrintable(QStringLiteral("expected %1 directory views, found %2\n%3")
                                             .arg(paneCount)
                                             .arg(directoryViews().size())
                                             .arg(dumpTree(region))),
                              kExecTimeoutMs);

    // Every pane must not only HOLD the live listing but have RENDERED it: the
    // check walks each directory view's own subtree, so N panes each showing the
    // entries is proven per-pane rather than by a total that one pane could
    // satisfy alone. (Controls wrap a delegate's text in more than one `text`
    // item, so occurrences are matched by presence, not by count.)
    const QList<QObject *> views = directoryViews();
    QStringList lastRendered;
    for (QObject *view : views) {
        QTRY_VERIFY_WITH_TIMEOUT(view->property("entries").toList().size() == 3, kRpcTimeoutMs);
        QVERIFY2(view->property("errorText").toString().isEmpty(),
                 qPrintable(view->property("errorText").toString()));

        QStringList names;
        const QVariantList entries = view->property("entries").toList();
        for (const QVariant &entry : entries)
            names.append(entry.toMap().value(QStringLiteral("name")).toString());

        QCOMPARE(names.constFirst(), QStringLiteral("ch-live-subdir"));
        QVERIFY(names.contains(QStringLiteral("ch-live-alpha.txt")));
        QVERIFY(names.contains(QStringLiteral("ch-live-beta.json")));

        const auto renderedTexts = [view] {
            QStringList texts;
            const QList<QObject *> items = collect(view, [](QObject *object) {
                return qobject_cast<QQuickItem *>(object) != nullptr
                       && object->metaObject()->indexOfProperty("text") >= 0;
            });
            for (QObject *item : items) {
                const QString text = item->property("text").toString();
                if (!text.isEmpty() && !texts.contains(text))
                    texts.append(text);
            }
            return texts;
        };
        QTRY_VERIFY2_WITH_TIMEOUT(
            renderedTexts().contains(QStringLiteral("ch-live-subdir/")),
            qPrintable(QStringLiteral("a pane never rendered the remote entries; texts=[%1]")
                           .arg(renderedTexts().join(QStringLiteral(", ")))),
            kExecTimeoutMs);
        const QStringList rendered = renderedTexts();
        // Directories are suffixed with "/", files are not (ViewerDirectoryView).
        QVERIFY2(rendered.contains(QStringLiteral("ch-live-alpha.txt")), qPrintable(rendered.join(QStringLiteral(", "))));
        QVERIFY2(rendered.contains(QStringLiteral("ch-live-beta.json")), qPrintable(rendered.join(QStringLiteral(", "))));
        lastRendered = rendered;
    }
    qInfo("each of %d panes rendered the live remote listing: [%s]", paneCount,
          qPrintable(lastRendered.join(QStringLiteral(", "))));

    // -- (d) frame proof ---------------------------------------------------
    // QQuickWindow::grabWindow() renders synchronously through the software
    // adaptation, so it works under the offscreen platform plugin with no
    // display and no GL context. A null grab here is a real failure, not an
    // environment quirk to route around.
    QTest::qWait(300);
    const QImage frame = window->grabWindow();
    QVERIFY2(!frame.isNull(), "QQuickWindow::grabWindow() returned a null frame");
    QCOMPARE(frame.width(), kWindowWidth);
    QCOMPARE(frame.height(), kWindowHeight);

    const int colours = distinctColours(frame);
    QVERIFY2(colours > 1,
             qPrintable(QStringLiteral("frame is a single uniform colour (#%1) — nothing rasterised")
                            .arg(frame.pixel(0, 0), 8, 16, QLatin1Char('0'))));
    // Text antialiasing plus the split handles guarantee well more than two.
    QVERIFY2(colours >= 3, qPrintable(QStringLiteral("only %1 distinct colours").arg(colours)));

    const QDir outDir(QCoreApplication::applicationDirPath() + QStringLiteral("/live-frames"));
    QVERIFY(QDir().mkpath(outDir.absolutePath()));
    const QString pngPath = outDir.absoluteFilePath(
        QStringLiteral("viewer-split-%1.png").arg(QLatin1String(QTest::currentDataTag())));
    QVERIFY2(frame.save(pngPath, "PNG"), qPrintable(pngPath));

    const qint64 pngBytes = QFileInfo(pngPath).size();
    QVERIFY(pngBytes > 0);
    qInfo("frame[%s]: %dx%d, %d distinct colours -> %s (%lld bytes)", QTest::currentDataTag(),
          frame.width(), frame.height(), colours, qPrintable(pngPath),
          static_cast<long long>(pngBytes));
}

// A pane added to an ALREADY laid-out split. Before the sizing fix this pane
// came up zero-extent; the latch reset on the Repeater's model/count change is
// what makes it real, so the same SplitView instance must re-size all children.
void TstLiveViewers::splitTreeSizesPaneAddedAfterFirstLayout()
{
    const auto leaf = [this](int i) {
        return QVariantMap{{QStringLiteral("paneId"), QStringLiteral("viewer-grow-%1").arg(i)},
                           {QStringLiteral("url"), dirUrl()},
                           {QStringLiteral("children"), QVariantList{}}};
    };
    const QVariantMap twoPane{{QStringLiteral("orientation"), QStringLiteral("horizontal")},
                              {QStringLiteral("children"), QVariantList{leaf(0), leaf(1)}}};
    const QVariantMap threePane{
        {QStringLiteral("orientation"), QStringLiteral("horizontal")},
        {QStringLiteral("children"), QVariantList{leaf(0), leaf(1), leaf(2)}}};

    std::unique_ptr<QObject> shell;
    QQuickWindow *window = nullptr;
    QQuickItem *region = nullptr;
    QVERIFY(openSplitShell(twoPane, &shell, &window, &region) != nullptr);

    const auto panes = [region] { return collect(region, isLeafPane); };
    QTRY_VERIFY_WITH_TIMEOUT(panes().size() == 2, kExecTimeoutMs);
    QTest::qWait(400);

    const SplitMeasurement before = measureSplit(region, true, panes());
    qInfo("before growth: %s", qPrintable(before.describe()));
    QCOMPARE(before.panes.size(), 2);
    QVERIFY2(qAbs(before.extentSum() + before.gapSum() - before.total) <= kEpsilon,
             qPrintable(before.describe()));

    QList<QObject *> splitViews = collect(region, isRegionSplitView);
    QCOMPARE(splitViews.size(), 1);
    QObject *const splitBefore = splitViews.constFirst();
    QVERIFY(splitBefore->property("ratiosApplied").toBool());

    // Grow the tree in place, exactly as a "split this pane" command would.
    region->setProperty("node", threePane);

    QTRY_VERIFY2_WITH_TIMEOUT(panes().size() == 3,
                              qPrintable(QStringLiteral("third pane never appeared\n%1")
                                             .arg(dumpTree(region))),
                              kExecTimeoutMs);
    QTest::qWait(400);

    // The SAME SplitView must have been re-sized: if the Loader had rebuilt the
    // branch from scratch the latch reset would never have been exercised.
    splitViews = collect(region, isRegionSplitView);
    QCOMPARE(splitViews.size(), 1);
    QCOMPARE(splitViews.constFirst(), splitBefore);
    QVERIFY2(splitBefore->property("ratiosApplied").toBool(),
             "the sizing latch never re-fired after the pane was added");

    const SplitMeasurement after = measureSplit(region, true, panes());
    qInfo("after growth: %s", qPrintable(after.describe()));
    QCOMPARE(after.panes.size(), 3);

    const QStringList ids{after.panes.at(0)->property("paneId").toString(),
                          after.panes.at(1)->property("paneId").toString(),
                          after.panes.at(2)->property("paneId").toString()};
    QCOMPARE(ids, (QStringList{QStringLiteral("viewer-grow-0"), QStringLiteral("viewer-grow-1"),
                               QStringLiteral("viewer-grow-2")}));

    const qreal expected = after.total / 3.0;
    const qreal slack = after.gapSum() + kEpsilon;
    for (int i = 0; i < 3; ++i) {
        QVERIFY2(after.extents.at(i) > 0,
                 qPrintable(QStringLiteral("pane %1 came up zero-extent: %2")
                                .arg(i)
                                .arg(after.describe())));
        QVERIFY2(qAbs(after.extents.at(i) - expected) <= slack,
                 qPrintable(QStringLiteral("pane %1 extent %2, expected ~%3 (+/-%4): %5")
                                .arg(i)
                                .arg(after.extents.at(i))
                                .arg(expected)
                                .arg(slack)
                                .arg(after.describe())));
        QVERIFY2(qAbs(after.crossExtents.at(i) - after.crossTotal) <= kEpsilon,
                 qPrintable(after.describe()));
    }
    QVERIFY2(qAbs(after.firstPos) <= kEpsilon, qPrintable(after.describe()));
    QVERIFY2(qAbs(after.extentSum() + after.gapSum() - after.total) <= kEpsilon,
             qPrintable(after.describe()));
}

// QTEST_MAIN cannot be used: the internal URL scheme must be registered and
// WebEngine initialised BEFORE the QGuiApplication exists, exactly as main.cpp
// does it.
int main(int argc, char *argv[])
{
    QStandardPaths::setTestModeEnabled(true);

    ViewerProfiles::registerUrlScheme();
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setOrganizationName(QStringLiteral("CodeHarbor"));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    TstLiveViewers testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_liveviewers.moc"
