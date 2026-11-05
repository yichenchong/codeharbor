// Pane IDENTITY across a split (SPEC 4.5).
//
// Splitting a pane is the most ordinary thing a user does with this window:
// "give me a second terminal next to the one I am working in". The split
// republishes the whole region tree, and a region that rebuilds its panes from
// that tree silently DESTROYS the pane the user was working in — the live shell
// goes down with its controller, the xterm scrollback is gone, and an editor's
// unsaved buffer (which lives in the Monaco page inside that very item) is lost.
//
// Counting panes cannot see this: the rebuilt tree has exactly the right number
// of panes with exactly the right ids, and a rebuilt terminal even re-attaches
// to the same remote tmux session. Only IDENTITY can:
//
//   * the pane Item is the SAME QObject before and after the split,
//   * its per-pane C++ object (TerminalController / EditorController) was never
//     destroyed — QPointer, so a destroyed one reads null rather than dangling,
//   * the state that lives ONLY in that pane's web page survives: xterm's
//     scrollback, and text typed into Monaco that was never saved anywhere.
//
// The last two need Chromium; they QSKIP (never fail) when the box cannot host
// it, exactly like tst_webengine_headless. The QObject-identity tests need
// nothing but Qt and are the permanent gate.

#include "CodeharbordClient.h"
#include "EditorController.h"
#include "EditorFactory.h"
#include "InternalUrlSchemeHandler.h"
#include "RpcTypes.h"
#include "SessionLayouts.h"
#include "SshConnectionPool.h"
#include "TerminalController.h"
#include "TerminalFactory.h"
#include "UiStateStore.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"
#include "WorkspaceDb.h"

#include <QtTest>

#include <QAccessible>
#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtQuickControls2/QQuickStyle>
#include <QtWebEngineQuick/QtWebEngineQuick>

#include <functional>
#include <memory>

namespace {

constexpr auto kModuleRoot = "qrc:/qt/qml/CodeHarbor/";
constexpr int kSettleMs = 400;
// Chromium's first start (zygote + renderer) is slow on a cold cache.
constexpr int kPageLoadTimeoutMs = 60000;
constexpr int kProbeTimeoutMs = 20000;

// Host window. The region under test is the REAL qrc component, sourced through
// a Loader with its `node` supplied up front — the region types are inert until
// they have one, so a declarative `source` would complete the tree first.
constexpr auto kShellQml = R"QML(
import QtQuick
import QtQuick.Window
import QtWebEngine

Window {
    id: win
    width: 960
    height: 640
    visible: true
    color: "#11111b"

    property string regionFile: ""
    property var regionProps: ({})
    property string jsResult: ""
    property bool jsFinished: false

    Loader {
        id: regionLoader
        objectName: "regionLoader"
        anchors.fill: parent
        Component.onCompleted: setSource(win.regionFile, win.regionProps)
    }

    function regionItem() { return regionLoader.item }
    function setNode(n) { if (regionLoader.item) regionLoader.item.node = n }

    function bindViewerLayouts(facade) {
        const region = regionLoader.item;
        region.serverId = Qt.binding(() => facade.serverId);
        region.acceptedServerId = Qt.binding(() => facade.serverId);
        region.sessionId = Qt.binding(() => facade.devSessionId);
        region.layoutGeneration = Qt.binding(() => facade.generation);
        region.paneGeneration = Qt.binding(() => facade.paneGeneration);
        region.node = Qt.binding(() => facade.viewerTree);
        region.paneUrlReportedForSession.connect((session, generation, pane, url) =>
            facade.setPaneUrlForSession(session, generation, "viewer", pane, url));
        region.splitRatiosAdjustedForSession.connect((session, generation, path, ratios) =>
            facade.setRatiosForSession(session, generation, "viewer", path, ratios));
    }

    // Destroy the component under test WITHOUT destroying this window. A test
    // that wants to prove something about a view's teardown wants exactly this
    // much torn down; dropping the whole window instead also destroys the last
    // WebEngineView in the process and then rebuilds one in the next test, and
    // that sequence is fragile in Qt 6.9's WebEngine (a repeatable crash on
    // every CI platform, which never reproduced against 6.10 locally).
    function unloadRegion() { regionLoader.active = false }

    // The pane's WebEngineView, found structurally: the pane keeps it inside a
    // Loader with no objectName, and a test has no business editing that.
    function findView(item) {
        if (!item)
            return null
        if (typeof item.runJavaScript === "function")
            return item
        if (item.item) {
            var nested = findView(item.item)
            if (nested)
                return nested
        }
        for (var i = 0; i < item.children.length; ++i) {
            var found = findView(item.children[i])
            if (found)
                return found
        }
        return null
    }

    function evalIn(item, script) {
        win.jsFinished = false
        win.jsResult = ""
        var view = findView(item)
        if (!view) {
            win.jsResult = "NO_VIEW"
            win.jsFinished = true
            return
        }
        view.runJavaScript(script, function(result) {
            win.jsResult = (result === undefined || result === null) ? "" : String(result)
            win.jsFinished = true
        })
    }

    // Accessible.name and Accessible.role are attached QML properties with no
    // reachable C++ accessor, so the reading has to be taken from inside the
    // document. The role comes back as the plain QAccessible::Role number.
    function accessibleName(item) { return item ? String(item.Accessible.name) : "" }
    function accessibleRole(item) { return item ? Number(item.Accessible.role) : -1 }
}
)QML";

// xterm's rendered screen (the DOM renderer is xterm 5's default) plus its
// scrollback depth: `buffer.active.length` counts lines that have scrolled off,
// which a re-attach can never restore.
constexpr auto kJsScreen = R"JS(
(function () {
    try {
        var rows = document.querySelector(".xterm-rows");
        if (!rows) return "NO_ROWS";
        return rows.textContent.replace(/\u00a0/g, " ");
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// Has the xterm renderer actually mounted? kJsScreen answers "NO_ROWS" until it
// has, and a probe for the SCREEN TEXT cannot be waited on with a needle (an
// empty terminal legitimately renders nothing), so readiness gets its own
// boolean probe. Waiting on kJsScreen with an empty needle — which is what this
// used to do — matched every possible answer, including "NO_ROWS" and
// "INVOKE_FAILED", so the wait returned immediately and the assertion behind it
// could never fail.
constexpr auto kJsXtermMounted = R"JS(
String(!!document.querySelector(".xterm-rows"))
)JS";

// Monaco's rendered buffer text.
constexpr auto kJsMonacoText = R"JS(
(function () {
    try {
        var lines = document.querySelector(".view-lines");
        if (!lines) return "NO_LINES";
        return lines.textContent.replace(/\u00a0/g, " ");
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// The rendered Markdown document, as the page actually put it on screen.
constexpr auto kJsMarkdownBody = R"JS(
(function () {
    try {
        var root = document.getElementById("ch-markdown-root");
        if (!root) return "NO_ROOT";
        return root.textContent;
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// The page's own failure banner. Read alongside the body so a test that never
// sees the document can report WHY the page said it could not render it,
// instead of only that nothing appeared.
constexpr auto kJsMarkdownStatus = R"JS(
(function () {
    try {
        var status = document.getElementById("ch-markdown-status");
        if (!status) return "NO_STATUS";
        return status.textContent;
    } catch (e) { return "ERR:" + e; }
})()
)JS";

// A `codeharbord` that answers file.readFile out of a small in-memory table and
// refuses everything else, over the QLocalSocket pair CodeharbordClient takes as
// a transport. Same shape as the harness in src/viewers/tests/tst_viewers.cpp,
// kept local because only one test in this file needs real document bytes to
// arrive: the Markdown page fetches its document itself, from inside Chromium,
// over the codeharbor-internal scheme, so a stubbed QML model cannot stand in
// for the server the way it can everywhere else here.
class ReadFileServer : public QObject
{
public:
    ~ReadFileServer() override
    {
        // The sockets die with this object; leaving them bound would hand the
        // shared client a transport made of dangling pointers.
        if (m_client)
            m_client->setTransport(nullptr);
    }

    bool bind(ch::CodeharbordClient *client)
    {
        static int seq = 0;
        const QString name = QStringLiteral("ch_paneidentity_md_%1_%2")
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
        connect(m_serverSide, &QLocalSocket::readyRead, this, [this] { serve(); });
        m_client = client;
        client->setTransport(&m_clientSide);
        return true;
    }

    void setFile(const QString &path, const QString &content)
    {
        m_files.insert(path, content);
    }

    int readCount(const QString &path) const { return m_reads.value(path); }
    void setLayout(const QString &region, const QJsonObject &tree)
    {
        m_layouts.insert(region, tree);
    }
    int layoutReadCount() const { return m_layoutReads; }
    int layoutWriteCount() const { return m_layoutWrites; }
    const QList<QJsonObject> &fileWrites() const { return m_fileWrites; }
    QString fileContent(const QString &path) const { return m_files.value(path); }

private:
    void serve()
    {
        m_buffer += m_serverSide->readAll();
        forever {
            const qsizetype newline = m_buffer.indexOf('\n');
            if (newline < 0)
                return;
            const QJsonObject request =
                QJsonDocument::fromJson(m_buffer.left(newline)).object();
            m_buffer.remove(0, newline + 1);
            const QString method = request.value(QStringLiteral("method")).toString();
            if (method == QString::fromLatin1(ch::rpc::kMethodWorkspaceGetLayout)
                || method == QString::fromLatin1(ch::rpc::kMethodWorkspaceSetLayout)) {
                const QJsonObject params = request.value(QStringLiteral("params")).toObject();
                const QString region = params.value(QStringLiteral("region")).toString();
                if (method == QString::fromLatin1(ch::rpc::kMethodWorkspaceSetLayout)) {
                    ++m_layoutWrites;
                    m_layouts.insert(region, params.value(QStringLiteral("tree")).toObject());
                } else {
                    ++m_layoutReads;
                }
                const QJsonObject response{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), request.value(QStringLiteral("id"))},
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("id"), QStringLiteral("layout-fixture")},
                                 {QStringLiteral("tree"), m_layouts.value(region)}}}};
                m_serverSide->write(
                    QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
                m_serverSide->flush();
                continue;
            }
            if (method == QString::fromLatin1(ch::rpc::kMethodWriteFile)) {
                const QJsonObject params = request.value(QStringLiteral("params")).toObject();
                const QString path = params.value(QStringLiteral("path")).toString();
                m_fileWrites.append(params);
                m_files.insert(path, params.value(QStringLiteral("content")).toString());
                const QJsonObject response{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), request.value(QStringLiteral("id"))},
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("path"), path},
                                 {QStringLiteral("revision"), QStringLiteral("r1")}}}};
                m_serverSide->write(
                    QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
                m_serverSide->flush();
                continue;
            }
            if (method != QString::fromLatin1(ch::rpc::kMethodReadFile))
                continue;
            const QString path = request.value(QStringLiteral("params"))
                                     .toObject()
                                     .value(QStringLiteral("path"))
                                     .toString();
            ++m_reads[path];
            QJsonObject response{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                 {QStringLiteral("id"),
                                  request.value(QStringLiteral("id"))}};
            const auto it = m_files.constFind(path);
            if (it == m_files.constEnd()) {
                response.insert(
                    QStringLiteral("error"),
                    QJsonObject{{QStringLiteral("code"), -32000},
                                {QStringLiteral("message"),
                                 QStringLiteral("no such file: %1").arg(path)}});
            } else {
                response.insert(
                    QStringLiteral("result"),
                    QJsonObject{{QStringLiteral("path"), path},
                                {QStringLiteral("encoding"), QStringLiteral("utf-8")},
                                {QStringLiteral("content"), *it},
                                {QStringLiteral("revision"), QStringLiteral("r1")},
                                {QStringLiteral("truncated"), false}});
            }
            m_serverSide->write(
                QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
            m_serverSide->flush();
        }
    }

    QLocalServer m_server;
    QLocalSocket m_clientSide;
    QLocalSocket *m_serverSide = nullptr;  // owned by m_server
    QByteArray m_buffer;
    QHash<QString, QString> m_files;
    QHash<QString, int> m_reads;
    QHash<QString, QJsonObject> m_layouts;
    QList<QJsonObject> m_fileWrites;
    int m_layoutReads = 0;
    int m_layoutWrites = 0;
    QPointer<ch::CodeharbordClient> m_client;
};

QUrl moduleUrl(const QString &file)
{
    return QUrl(QLatin1String(kModuleRoot) + file);
}

// A leaf pane is any object exposing `paneId` but not `node`: ViewerPane and
// TerminalPaneView match, the region types do not. Survives the generated
// "<Type>_QMLTYPE_n" metaobject names.
bool isLeafPane(const QObject *object)
{
    const QMetaObject *mo = object->metaObject();
    return mo->indexOfProperty("paneId") >= 0 && mo->indexOfProperty("node") < 0;
}

// EditorPaneView is the only component carrying `remotePath`.
bool isEditorPane(const QObject *object)
{
    return object->metaObject()->indexOfProperty("remotePath") >= 0;
}

// ViewerDirectoryView is the only component carrying both of these.
bool isDirectoryView(const QObject *object)
{
    const QMetaObject *mo = object->metaObject();
    return mo->indexOfProperty("basePath") >= 0 && mo->indexOfProperty("rows") >= 0;
}
// ViewerBinaryView exposes the download state but no directory/editor state.
// This lets the reload contract use a non-WebEngine content kind without
// depending on a remote server or a Chromium page load.
bool isBinaryView(const QObject *object)
{
    return object->metaObject()->indexOfProperty("downloadRequested") >= 0;
}


// ViewerImageView and ViewerPdfView, the two views that PIN the internal
// address they are displaying. Exactly one of them is loaded in a pane at a
// time, so a pane-scoped search finds the one under test.
bool isPinningView(const QObject *object)
{
    const QMetaObject *mo = object->metaObject();
    return mo->indexOfProperty("internalUrl") >= 0 && mo->indexOfProperty("retained") >= 0;
}

// ViewerBinaryView's Download affordance, the third place an internal address
// is held: it is the only button in that view and it says so.
bool isDownloadButton(QObject *object)
{
    return object->metaObject()->indexOfProperty("down") >= 0
           && object->property("text").toString() == QLatin1String("Download");
}

// AppPaneHeader: the only component carrying this exact set. It is what makes a
// pane say which file or terminal it holds and whether it is the focused one,
// and — because it lives INSIDE the pane — it is also the thing a split could
// most easily take down with it.
bool isPaneHeader(const QObject *object)
{
    const QMetaObject *mo = object->metaObject();
    return mo->indexOfProperty("subtitle") >= 0 && mo->indexOfProperty("busy") >= 0
           && mo->indexOfProperty("actions") >= 0 && mo->indexOfProperty("title") >= 0;
}

// The region's own SplitView, identified by the one-shot sizing latch it and
// nothing else carries.
bool isRegionSplitView(const QObject *object)
{
    return object->metaObject()->indexOfProperty("ratiosApplied") >= 0;
}

using Predicate = std::function<bool(QObject *)>;

// Walks BOTH the QObject child list and the QQuickItem visual child list: QML
// parents Loader/Repeater content visually and deliberately skips QObject
// re-parenting on some of those paths, so neither list alone sees the tree.
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

QString describePanes(const QList<QObject *> &panes)
{
    QStringList parts;
    for (QObject *pane : panes) {
        parts.append(QStringLiteral("%1(paneId=\"%2\")@%3")
                         .arg(QString::fromLatin1(pane->metaObject()->className()),
                              pane->property("paneId").toString(),
                              QString::number(reinterpret_cast<quintptr>(pane), 16)));
    }
    return parts.isEmpty() ? QStringLiteral("<none>") : parts.join(QStringLiteral(", "));
}

QObject *paneWithId(QObject *root, const QString &paneId)
{
    const QList<QObject *> panes = collect(root, [&paneId](QObject *object) {
        return isLeafPane(object) && object->property("paneId").toString() == paneId;
    });
    return panes.size() == 1 ? panes.constFirst() : nullptr;
}

// The one descendant carrying `name`. Returns null when there is not exactly
// one, so an ambiguous match fails the test rather than picking arbitrarily.
QObject *childNamed(QObject *root, const QString &name)
{
    const QList<QObject *> found = collect(root, [&name](QObject *object) {
        return object->objectName() == name;
    });
    return found.size() == 1 ? found.constFirst() : nullptr;
}

// A QML `var` property holding a QObject comes back either as a direct pointer
// or wrapped in a QJSValue, depending on how the engine boxed it.
QObject *asObject(const QVariant &value)
{
    if (QObject *direct = value.value<QObject *>())
        return direct;
    return value.value<QJSValue>().toQObject();
}

// Same story for a QML `var` holding a JS ARRAY: a signal argument or a property
// read may hand it over already converted to a QVariantList, or still boxed in a
// QJSValue.
QVariantList asList(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QJSValue>())
        return value.value<QJSValue>().toVariant().toList();
    return value.toList();
}

QVariantMap asMap(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QJSValue>())
        return value.value<QJSValue>().toVariant().toMap();
    return value.toMap();
}

// The index path a region reports its ratios under, printed so a failure names
// it: [] is the root branch, ["1"] its second child.
QString pathText(const QVariantList &path)
{
    QStringList parts;
    for (const QVariant &step : path)
        parts.append(step.toString());
    return QLatin1Char('[') + parts.join(QLatin1Char(',')) + QLatin1Char(']');
}

QVariantMap leafNode(const QString &paneId, const QString &url = QString(),
                     const QString &customTitle = QString())
{
    QVariantMap leaf{{QStringLiteral("paneId"), paneId},
                     {QStringLiteral("children"), QVariantList{}}};
    if (!url.isEmpty())
        leaf.insert(QStringLiteral("url"), url);
    if (!customTitle.isEmpty())
        leaf.insert(QStringLiteral("customTitle"), customTitle);
    return leaf;
}

// The exact shape SessionLayouts::splitPane() publishes: the ORIGINAL leaf is
// copied into a new branch as its first child, with equal starting ratios.
QVariantMap branchNode(const QString &orientation, const QVariantList &children)
{
    return QVariantMap{{QStringLiteral("orientation"), orientation},
                       {QStringLiteral("children"), children},
                       {QStringLiteral("ratios"), QVariantList{1.0, 1.0}}};
}

// The `app` context property (ch::AppController), cut down to the two things a
// pane reaches for on it.
//
// The first is the ACTIVE Dev Session's repository root, which a viewer pane
// reads: that root is what "outside the project" (SPEC 9) is measured against,
// so without it a pane has nothing to be outside OF and asks nothing. Mirrors
// AppController's property name and its NOTIFY-on-session-change shape.
//
// The second is the harness stored against a terminal pane's `terminal_panes`
// row — what the pane RUNS, which is what the sidebar's per-session status is
// computed from (SPEC 6.6). Both halves of the real pair are here: a reading,
// so a dialog can open on what is stored, and a WRITE that is recorded rather
// than performed, because "the user's choice reached the controller intact" is
// the whole assertion.
class AppControllerStub : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString activeSessionRepoRoot READ activeSessionRepoRoot
                       NOTIFY activeSessionChanged)

public:
    struct HarnessWrite {
        QString terminalPaneId;
        QString harness;
    };

    QString activeSessionRepoRoot() const { return m_repoRoot; }

    void setActiveSessionRepoRoot(const QString &root)
    {
        if (m_repoRoot == root)
            return;
        m_repoRoot = root;
        emit activeSessionChanged();
    }

    Q_INVOKABLE QString terminalPaneHarness(const QString &terminalPaneId) const
    {
        return m_harness.value(terminalPaneId);
    }

    Q_INVOKABLE void setTerminalPaneHarness(const QString &terminalPaneId,
                                            const QString &harness)
    {
        m_writes.append({terminalPaneId, harness});
        m_harness.insert(terminalPaneId, harness);
    }

    // What the server is pretending to hold already, so a dialog can be opened
    // on a pane that HAS a harness without that seeding counting as a write.
    void seedTerminalPaneHarness(const QString &terminalPaneId, const QString &harness)
    {
        m_harness.insert(terminalPaneId, harness);
    }

    const QList<HarnessWrite> &harnessWrites() const { return m_writes; }

    void clearHarness()
    {
        m_harness.clear();
        m_writes.clear();
    }

signals:
    void activeSessionChanged();

private:
    QString m_repoRoot;
    QHash<QString, QString> m_harness;
    QList<HarnessWrite> m_writes;
};

// The slice of ch::TerminalFactory a terminal pane drives, with the two-phase
// resolution under this test's control. The pane documents `factory` as the
// seam a test injects through, and a stub is the only way to hold a lookup
// OPEN — the real factory answers (or refuses) before a test can retarget the
// pane underneath the flight, which is exactly the window the pane has to get
// right.
//
// createBridge() deliberately hands back nothing: with no bridge the pane skips
// its WebEngineView entirely, so this stays a pure QML/logic test.
class TerminalFactoryStub : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool workspaceReady READ workspaceReady WRITE setWorkspaceReady
                   NOTIFY workspaceReadyChanged)

public:
    struct Resolution {
        QString devSessionId;
        QString paneName;
        QString terminalPaneId;
        QString workingDir;
        QObject *controller;
    };

    Q_INVOKABLE QObject *create(QObject *)
    {
        // Parented here, so the object has C++ ownership: a QObject* returned
        // to QML with no parent is handed to the JavaScript collector.
        ++m_controllersCreated;
        m_controller = new QObject(this);
        return m_controller;
    }
    Q_INVOKABLE QObject *createBridge(QObject *, QObject *)
    {
        ++m_bridgeRequests;
        return nullptr;
    }
    Q_INVOKABLE bool connected() const { return m_connected; }
    void setConnected(bool connected) { m_connected = connected; }
    bool workspaceReady() const { return m_workspaceReady; }
    void setWorkspaceReady(bool ready)
    {
        if (m_workspaceReady == ready)
            return;
        m_workspaceReady = ready;
        if (!ready)
            m_pending.clear();
        emit workspaceReadyChanged();
    }

    Q_INVOKABLE bool resolveTarget(QObject *controller, const QString &devSessionId,
                                   const QString &paneName, const QString &terminalPaneId,
                                   const QString &workingDir)
    {
        m_pending.insert(controller, m_resolutions.size());
        m_resolutions.append({devSessionId, paneName, terminalPaneId, workingDir, controller});
        return true;
    }

    Q_INVOKABLE bool attach(QObject *, const QString &target, const QString &, int, int)
    {
        m_attached.append(target);
        return true;
    }
    Q_INVOKABLE void detach(QObject *controller) { m_pending.remove(controller); }
    Q_INVOKABLE QString targetFor(QObject *) const { return QString(); }
    Q_INVOKABLE void kill(QObject *) { }

    QObject *controller() const { return m_controller; }
    const QList<Resolution> &resolutions() const { return m_resolutions; }
    const QStringList &attached() const { return m_attached; }
    int controllersCreated() const { return m_controllersCreated; }
    int bridgeRequests() const { return m_bridgeRequests; }

    // Match the factory's latest-request contract: detach/supersession drops
    // delivery altogether, rather than sending an empty stale answer to QML.
    bool completeResolution(qsizetype index, const QString &target)
    {
        QObject *const controller = m_resolutions.at(index).controller;
        if (m_pending.value(controller, -1) != index)
            return false;
        m_pending.remove(controller);
        emit targetResolved(controller, target);
        return true;
    }

signals:
    void workspaceReadyChanged();
    void error(QObject *controller, const QString &message);
    void targetResolved(QObject *controller, const QString &target);
    // The real factory reports this after an attach it discovers CREATED the
    // pane's tmux session instead of re-attaching it: the pane's previous remote
    // session was gone, so the shell on screen is not the one the user left.
    void sessionRecreated(QObject *controller, const QString &tmuxTarget);

private:
    QObject *m_controller = nullptr;
    QList<Resolution> m_resolutions;
    QStringList m_attached;
    QHash<QObject *, qsizetype> m_pending;
    bool m_workspaceReady = true;
    bool m_connected = true;
    int m_controllersCreated = 0;
    int m_bridgeRequests = 0;
};

// Recovery-offer state tests do not load a browser document. Their adapter only
// exposes the host-side restoration notification; browser cases use the real one.
class EditorPageBridgeStub : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    void bufferRestored(QString content, QString baselineContent, QString revision,
                        QString pendingSaveContent, bool savePending);
};

// The slice of ch::EditorController an editor pane binds to. The pane documents
// `controller` as a seam a test may pre-assign, and a stub is what lets a
// crash-recovery offer (SPEC 11.3) be driven without a server holding a
// snapshot: the recovered buffer arrives as `recoveryAvailable`, and what the
// pane pushes BACK into the page arrives here as `contentLoaded`.
class EditorControllerStub : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString fileState READ fileState NOTIFY fileStateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool recoveryEnabled READ recoveryEnabled CONSTANT)
    Q_PROPERTY(quint64 recoveryContextRevision READ recoveryContextRevision CONSTANT)

public:
    QString fileState() const { return m_fileState; }
    bool dirty() const { return m_dirty; }
    bool recoveryEnabled() const { return true; }
    quint64 recoveryContextRevision() const { return 0; }
    void setDirty(bool dirty)
    {
        if (m_dirty == dirty)
            return;
        m_dirty = dirty;
        emit dirtyChanged();
    }

    // ch::EditorController leaves the file in "disconnected" when the SSH
    // transport drops and in "conflict" when a save was refused; both are
    // states the pane has to announce rather than leave in a status line.
    void setFileState(const QString &state)
    {
        if (m_fileState == state)
            return;
        m_fileState = state;
        emit fileStateChanged(m_fileState);
    }

    Q_INVOKABLE void open(const QString &path) { m_opened.append(path); }
    Q_INVOKABLE QObject *createPageBridge()
    {
        auto *bridge = new EditorPageBridgeStub(this);
        bridge->setObjectName(QStringLiteral("editor-stub-%1").arg(++m_bridgeSerial));
        return bridge;
    }
    Q_INVOKABLE void releasePageBridge(QObject *bridge)
    {
        if (bridge && bridge->parent() == this)
            bridge->deleteLater();
    }
    Q_INVOKABLE bool hasPageReloadContent(QObject *) const { return false; }
    Q_INVOKABLE bool preparePageReload(const QString &, quint64) { return false; }
    Q_INVOKABLE void save(const QString &, const QString &) { }
    Q_INVOKABLE void reportContent(const QString &content) { m_reported.append(content); }
    Q_INVOKABLE void reportDirty(bool dirty, const QString &) { setDirty(dirty); }
    Q_INVOKABLE void requestReload() { }
    Q_INVOKABLE void ready() { }
    Q_INVOKABLE void setRecoveryScope(const QString &, const QString &, const QString &) { }

    const QStringList &opened() const { return m_opened; }
    const QStringList &reported() const { return m_reported; }

signals:
    void contentLoaded(const QString &content, const QString &revision);
    void dirtyChanged();
    void fileStateChanged(const QString &state);
    void readOnlyChanged(bool readOnly);
    void saved(const QString &revision);
    void saveConflict(const QString &currentRevision);
    void saveError(const QString &message);
    void recoveryAvailable(const QString &recoveredContent);

private:
    QString m_fileState = QStringLiteral("clean");
    bool m_dirty = false;
    QStringList m_opened;
    QStringList m_reported;
    int m_bridgeSerial = 0;
};
} // namespace

class TstPaneIdentity : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    // The permanent gate: the pane Item and its per-pane C++ object must live
    // through a split of their own region.
    void terminalPaneSurvivesASplit();
    void editorPaneSurvivesASplit();
    // The nested case: splitting the OTHER pane must not take the first one
    // down either (the branch's child set changes, not the root's).
    void bothPanesSurviveASecondSplit();

    // The user-visible consequences, in the pane's real web page.
    void terminalScrollbackSurvivesASplit();
    void unsavedEditorBufferSurvivesASplit();

    // FOCUS (SPEC 4.5). The region must be able to say WHICH pane the user is
    // working in, or every split command lands on the region's first leaf.
    void regionReportsNoFocusUntilAPaneIsUsed();
    void clickingAPaneReportsItsPaneId();
    void focusSurvivesASplit();
    void closingTheFocusedPaneClearsTheFocus();
    void nestedPaneFocusReachesTheRootRegion();
    void terminalPaneReportsFocusThroughItsChrome();
    // The one that decides whether any of the above is worth wiring: a click
    // INSIDE the live web page, not on the pane's chrome.
    void aClickInsideTheLivePageReportsFocusAndStillReachesThePage();

    // THE KEYBOARD MUST BE ABLE TO LEAVE. Both pane types push focus into an
    // embedded Chromium page — xterm's textarea, Monaco's input area — and both
    // pages answer every key themselves, Tab and Escape included. Without an
    // application-defined way out, a keyboard user who enters a pane can never
    // leave it: that is a keyboard trap, and no amount of correct labelling
    // elsewhere makes up for one.
    void theEscapeKeyTakesTheKeyboardOutOfTheTerminalPage();
    // ...and the other half of "which pane am I in": arriving by keyboard has
    // to make a pane the active one, exactly as clicking it does. Otherwise a
    // keyboard user can be typing in one pane while every split and close
    // command lands on another.
    void keyboardFocusEnteringAPaneMakesItTheActivePane();

    // CONTENT (SPEC 4.5). Geometry alone is not a restored Dev Session: the
    // region has to tell the host WHAT each pane has open, or reopening a
    // session brings back the right split with a set of blank panes in it.
    void openingAFileInAPaneIsReportedToTheHost();

    // PANE ACTIONS. Every header action must name its own pane, even when the
    // remembered-pane fallback would otherwise choose the first leaf.
    void paneHeaderRequestsNameTheirOwnPane();
    void terminalCloseKillsOnlyAfterConfirmation();
    void paneHeaderTravelsWithThePaneAcrossASplit();
    void terminalCustomTitleAppearsAndClearsInTheHeader();

    // The header is also the pane's focus indicator, so the region's focus has to
    // reach it or the mark is decoration.
    void theFocusedPaneIsTheOnlyOneMarkedActive();

    // ADDRESS BAR (SPEC 7.5). The one way a user can put anything into a viewer
    // pane. Untested it is invisible: the pane still lays out, still reports
    // focus, and simply never opens what was typed into it.
    void theAddressBarOpensARemotePath();
    void theAddressBarOpensAUrlAsGiven();
    void theAddressBarPercentEncodesADelimiterInAFileName();
    // The host's "show this address in that pane" entry point must let the handler
    // registry choose when the caller names no handler.
    void openPaneTargetWithNoKindLetsTheRegistryDecide();
    void navigationTruncatesForwardHistoryAfterFreshAddress();
    void navigationButtonsAreDisabledAtHistoryEnds();
    void reloadRefreshesNonWebContent();
    void reloadKeepsTheEditorHandlerAlive();
    void editorReloadPreservesDirtyBufferAndRejectsStaleCapture();
    void remoteWebReloadRecoversAfterAReadFailure();
    void navigationDismissesAnOlderEditorClosePrompt();
    void dirtyEditorRejectsNavigation();
    void narrowViewerKeepsItsAddressUsable_data();
    void narrowViewerKeepsItsAddressUsable();
    void supersededAddressProbesCannotNavigate();
    void liveTerminalDiagnosticsRemainVisibleWithoutDisconnecting();
    void homeUsesTheActiveSessionRootAndDisablesWithoutOne();
    // The field's VALUE is a path that changes with every navigation, so
    // without a name of its own a screen reader announces the address and never
    // what the control is FOR.
    void theAddressFieldSaysWhatItIsFor();

    // OUTSIDE THE REPOSITORY ROOT (SPEC 9). A path outside the Dev Session's
    // repository root is allowed and stays openable, but the pane has to SAY
    // so: without a marker, /etc/hosts and a file in another checkout entirely
    // wear exactly the chrome of a file in the project the session is scoped to.
    void theViewerPaneMarksAPathOutsideTheRepositoryRoot();

    // DEFAULT TERMINAL LAYOUT (SPEC 4.4/4.5). A Dev Session opens with two
    // stacked terminal panes, so both have to come up sized and attachable from
    // the FIRST frame — the case the one-shot sizing latch gets wrong most
    // easily, and one a single-pane region can never exercise.
    void twoStackedTerminalPanesComeUpFromTheFirstFrame();

    // SPLIT RATIOS (SPEC 4.5). Restoring stored ratios was already implemented
    // and tested by the sizing latch; REPORTING the ones a drag produces was
    // not, so every drag on a divider inside a region was forgotten when the
    // Dev Session was reopened. The reading has to name the branch it belongs
    // to by index path, which is the part a nested region gets wrong.
    void dragAdjustedRatiosAreReportedForTheRightBranch();
    void reorderedNestedBranchesUpdateRatioPaths();

    // ADDRESS BAR, part two. A name typed without a leading "/" is resolved
    // against the directory the pane is showing, so what the pane opens is NOT
    // what the user typed — and the bar has to end up saying what was actually
    // opened, or it claims the pane is showing a file called "notes.md" in no
    // particular place.
    void theAddressBarShowsWhatARelativeNameResolvedTo();

    // TERMINAL IDENTITY, the asynchronous half. Naming a pane's remote shell is
    // a server round trip, and the pane can be pointed at another Dev Session
    // while that question is still travelling. The answer carries a controller
    // and a target string and nothing else, so an answer to the OLD question
    // must not be able to look like the answer to the new one: that is two
    // panes on one shell, which is the failure the whole resolution path
    // exists to prevent.
    void aTerminalPaneNeverAdoptsTheAnswerToASupersededLookup();

    // TERMINAL IDENTITY, the split half. A pane created by a split has no
    // `terminal_panes` row yet: ch::SessionLayouts publishes the new leaf at
    // once and mints the row behind it, then republishes the tree with the id
    // in it. The new pane must wait for that id and then attach — a pane that
    // never notices the republished id is blank and unusable for ever.
    void aSplitPaneAttachesOnceItsRowIdArrives();

    // CRASH RECOVERY (SPEC 11.3). ch::EditorController snapshots the unsaved
    // buffer while the user types and finds it again on the next open, but
    // finding it is worth nothing unless the user is told and can take it back.
    void theEditorOffersToRestoreARecoveredBuffer();
    void theEditorOffersToRestoreAnEmptyRecoveredBuffer();
    // The other half of the same offer: answering "discard" has to close the
    // dialog and leave the file exactly as it was read from disk, rather than
    // quietly marking it edited.
    void discardingARecoveredBufferLeavesTheEditorShowingTheFile();

    // THE PATH PROBE IS BOUNDED. A path typed with no trailing slash makes the
    // pane ask the server "is this a directory?" and draw its header busy until
    // the answer lands. Nothing else clears that state, so a reply that never
    // arrives — a session that dropped between the request and its answer —
    // left the pane spinning on an address for ever, with no way to ask again.
    void theViewerPaneStopsWaitingForAProbeThatNeverAnswers();

    // ONE ViewerModel serves every viewer pane, and they all listen on the same
    // listing signals. A directory view that matched replies against whatever
    // its URL happened to be at reply time therefore accepted answers to
    // requests it never made — including another pane's failure, painted over
    // a listing that had loaded perfectly well.
    void theDirectoryListingTakesOnlyTheAnswerToItsOwnRequest();

    // The internal-URL table is LRU-bounded, and recency is refreshed only when
    // an address is minted or resolved. A pane still DISPLAYING an image does
    // neither, so its address aged out from under it and the next reload failed
    // on a URL visibly on screen. Retaining is the only signal that carries
    // "still on screen"; the release has to happen on BOTH ways out.
    void theViewsThatHoldAnInternalAddressPinItUntilTheyLetItGo();

    // WebEngine's job interface carries only Chromium's coarse error enum, so a
    // refused internal resource — an image over the inline cap, say — reached
    // the pane as a blank failed page with nothing at all to explain it.
    void aRefusedInternalResourceSaysWhyInsteadOfShowingABlankPage();

    // A LAYOUT RELOAD throws the whole pane cache away, because a reloaded
    // tree can name exactly the same panes and nothing else would notice they
    // now belong to a different load. Every region in the tree then has to
    // rebuild the leaf it was showing: its own record of "I am showing pane X"
    // outlives X, and a region that keeps believing it goes BLANK — with the
    // right pane ids still in the layout and nothing at all on screen.
    void aLayoutReloadRebuildsEveryPaneInTheTree();
    void reconnectRefreshPreservesDirtyEditorAndRetiresOldIntents();
    void editorRecoveryUsesScopedContextAndWarnsWhenUnavailable_data();
    void editorRecoveryUsesScopedContextAndWarnsWhenUnavailable();

    // The editor's drop/conflict banner. It is the only thing that tells a user
    // typing into a buffer that the buffer cannot be saved, and a banner with
    // no geometry is built, flips `visible` correctly, and draws nothing.
    void theEditorSaysWhenItsFileCannotBeSaved();

    // THE DIRECTORY LISTING IS BOUNDED, for the same reason the path probe is:
    // the view says "Listing…" and draws the pane's header busy until a reply
    // settles it, and a session that drops between the request and its answer
    // sends neither.
    void theDirectoryListingStopsWaitingForAnAnswerThatNeverArrives();

    // ...and so is a download. The Download button is disabled while a request
    // this pane started is pending, so a request that never reaches the handler
    // left the pane's one affordance dead for good.
    void theBinaryViewStopsWaitingForADownloadThatNeverStarts();

    // THE MARKDOWN PAGE MUST BE ABLE TO FETCH ITS OWN DOCUMENT. The renderer
    // page is app code loaded from qrc:, the document is served from
    // codeharbor-internal://, and those are two different web origins — so the
    // one thing the whole view exists to do is a cross-origin fetch() out of
    // Chromium. Nothing below the browser can tell whether Chromium will permit
    // it: every unit test over the renderer, the scheme handler and the URL map
    // passed while the shipped viewer showed "Unable to render Markdown: Failed
    // to fetch" on every file. Only the real page, fetching a real
    // internal-scheme document, answers the question.
    void theMarkdownPageRendersADocumentFetchedFromTheInternalScheme();

    // THE MARKDOWN IMAGE PATH IS RESOLVED TWICE, in two languages. The rendered
    // page resolves its own links in TypeScript; the WebChannel image bridge
    // hands QML the raw document-relative string, so QML resolves that one
    // itself. Neither can call the other, so the only thing keeping them in
    // step is a shared table asserted on both sides.
    void theMarkdownImagePathResolverAgreesWithTheRendererBundle_data();
    void theMarkdownImagePathResolverAgreesWithTheRendererBundle();

    // ...and the pins those resolved addresses take. A theme change re-renders
    // the document, which re-requests every image; a view that pinned each
    // answer again grew its held set on every theme change while one document
    // stayed open.
    void theMarkdownViewPinsEachImageAddressExactlyOnce();

    // An "Open as" choice belongs to the ADDRESS it was made about, so any
    // republish of the tree that does not move this pane's address must leave
    // it alone.
    void anUnrelatedRepublishKeepsAnOpenAsChoice();

    // Split proportions must survive a window resize. SplitView stretches only
    // its first fill item, so the region has to rescale the rest itself.
    void splitProportionsSurviveAWindowResize();

    // A terminal pane's sidebar status is computed from the harness stored
    // against its row, and a pane the user simply opened has none. The pane's
    // header must therefore let them say what it runs, and must send the WIRE
    // value for that choice against the right pane id.
    void aTerminalPaneWritesTheHarnessTheUserChose();

    // (16) A pane whose remote session died and was silently replaced by a new,
    // empty shell has to SAY so — once, dismissibly, and to a screen reader too.
    void aTerminalPaneSaysWhenItsRemoteSessionWasSilentlyReplaced();

    void keyboardSplitRatiosPersistAfterLayout_data();
    void keyboardSplitRatiosPersistAfterLayout();
    void keyboardSplitLimitsLeaveTheThirdPaneAlone_data();
    void keyboardSplitLimitsLeaveTheThirdPaneAlone();
    void terminalRetargetDismissesStaleDialogs_data();
    void terminalRetargetDismissesStaleDialogs();
    void terminalHostStateWaitsForTheBrowserApis_data();
    void terminalHostStateWaitsForTheBrowserApis();
    void detachedTerminalLookupCanBeRestarted();
    void terminalLookupWaitsForWorkspaceReadiness();
    void keyboardSplitPublicationCoalescesAndCancels_data();
    void keyboardSplitPublicationCoalescesAndCancels();
    void emptyTerminalRegionDoesNotAllocateUntilSplit();

private:
    // Load one qrc component into the harness window with `props` as its
    // initial properties, and hand back the loaded item. The region types are
    // inert until they have a node, so the properties must be supplied at
    // creation rather than assigned afterwards; a leaf pane is loaded the same
    // way, with the seams (`factory`, `controller`) its own documentation
    // describes.
    QObject *openInShell(const QString &file, const QVariantMap &props);
    QObject *openRegion(const QString &file, const QVariantMap &node, bool terminal);
    void setNode(const QVariantMap &node);
    std::unique_ptr<QObject> installMonacoProbe();
    QString evalJs(QObject *paneItem, const QString &script, int timeoutMs = kProbeTimeoutMs);
    bool waitForJs(QObject *paneItem, const QString &script, const QString &needle, int timeoutMs);
    // Wait until the pane's page has loaded; false means Chromium never got
    // there (a machine property — callers QSKIP rather than fail).
    bool waitForPage(QObject *paneItem, const QString &readyProbe, const QString &needle);
    // The pane's embedded WebEngineView, found the same structural way the
    // harness finds one to run script in. Null means Chromium produced no view
    // at all, which is a machine property — callers QSKIP rather than fail.
    QObject *webViewIn(QObject *paneItem);
    // Click the middle of a pane, the way a user selects it.
    void clickPane(QObject *paneObject);
    // Put `text` in the viewer pane's address field and press Enter on it, the
    // way a user opens something. Driven through the FIELD rather than by calling
    // the pane's function directly, so the field's own Enter wiring is part of
    // what is under test.
    void enterAddress(QObject *paneObject, const QString &text);
    // Accessible.name of an item, read from inside the harness document (it is
    // an attached QML property with no C++ accessor).
    QString accessibleNameOf(QObject *item);
    // Accessible.role of an item, read the same way, as the plain
    // QAccessible::Role number the attached property carries.
    int accessibleRoleOf(QObject *item);

    ch::CodeharbordClient m_client;
    ch::ViewerProfiles m_profiles{&m_client};
    ch::ViewerModel m_viewers{&m_client};
    // The `app` context property. Empty by default, so every OTHER test sees a
    // pane with no Dev Session behind it, exactly as before this existed.
    AppControllerStub m_app;
    ch::EditorFactory m_editorFactory{&m_client};
    // Never connected: attach() refuses, so the pane shows its "not connected"
    // chrome. Identity is about the objects, not about a remote shell — the
    // live gate (src/terminal/tests/tst_livepaneidentity.cpp) covers that.
    ch::SshConnectionPool m_pool;
    ch::TerminalFactory m_terminalFactory{&m_pool};

    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<QObject> m_shell;
    QObject *m_region = nullptr;
};

void TstPaneIdentity::initTestCase()
{
    m_viewers.setProfiles(&m_profiles);
    m_engine = std::make_unique<QQmlEngine>();
    m_engine->rootContext()->setContextProperty(QStringLiteral("viewers"), &m_viewers);
    m_engine->rootContext()->setContextProperty(QStringLiteral("editorFactory"), &m_editorFactory);
    m_engine->rootContext()->setContextProperty(QStringLiteral("terminalFactory"),
                                                &m_terminalFactory);
    m_engine->rootContext()->setContextProperty(QStringLiteral("app"), &m_app);
}

void TstPaneIdentity::cleanup()
{
    m_region = nullptr;
    m_shell.reset();
    m_app.setActiveSessionRepoRoot(QString());
    m_app.clearHarness();
    QTest::qWait(50);
}

std::unique_ptr<QObject> TstPaneIdentity::installMonacoProbe()
{
    QQmlComponent component(m_engine.get());
    component.setData(R"QML(
        import QtQuick
        import QtWebEngine
        QtObject {
            property var previousScripts: []
            function install() {
                const profile = viewers.internalProfile();
                previousScripts = profile.userScripts.collection;
                profile.userScripts.collection = [{
                    name: "chPaneIdentityMonacoApi",
                    sourceCode: "globalThis.MonacoEnvironment = { globalAPI: true };",
                    injectionPoint: WebEngineScript.DocumentCreation,
                    worldId: WebEngineScript.MainWorld,
                    runsOnSubFrames: false
                }];
            }
            function restore() {
                viewers.internalProfile().userScripts.collection = previousScripts;
            }
        }
    )QML", QUrl());
    std::unique_ptr<QObject> probe(component.create());
    if (!probe || !QMetaObject::invokeMethod(probe.get(), "install")) {
        QTest::qFail(qPrintable(component.errorString()), __FILE__, __LINE__);
        return {};
    }
    return probe;
}

QObject *TstPaneIdentity::openInShell(const QString &file, const QVariantMap &props)
{
    QQmlComponent component(m_engine.get());
    component.setData(QByteArray(kShellQml), QUrl(QStringLiteral("qrc:/tst_paneidentity/shell.qml")));
    if (component.isError()) {
        QTest::qFail(qPrintable(component.errorString()), __FILE__, __LINE__);
        return nullptr;
    }

    m_shell.reset(component.createWithInitialProperties(
        {{QStringLiteral("regionFile"), moduleUrl(file).toString()},
         {QStringLiteral("regionProps"), props}}));
    if (!m_shell) {
        QTest::qFail(qPrintable(component.errorString()), __FILE__, __LINE__);
        return nullptr;
    }

    QVariant regionItem;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 5000) {
        QMetaObject::invokeMethod(m_shell.get(), "regionItem", Q_RETURN_ARG(QVariant, regionItem));
        if (asObject(regionItem))
            break;
        QTest::qWait(20);
    }
    m_region = asObject(regionItem);
    if (!m_region)
        QTest::qFail("the component never loaded in the harness window", __FILE__, __LINE__);
    return m_region;
}

QObject *TstPaneIdentity::openRegion(const QString &file, const QVariantMap &node, bool terminal)
{
    QVariantMap props{{QStringLiteral("node"), node}};
    if (terminal) {
        props.insert(QStringLiteral("devSessionId"), QStringLiteral("dev-identity"));
        props.insert(QStringLiteral("workingDir"), QStringLiteral("/tmp"));
    }
    return openInShell(file, props);
}

void TstPaneIdentity::setNode(const QVariantMap &node)
{
    QVERIFY(QMetaObject::invokeMethod(m_shell.get(), "setNode",
                                      Q_ARG(QVariant, QVariant(node))));
}

QString TstPaneIdentity::evalJs(QObject *paneItem, const QString &script, int timeoutMs)
{
    if (!QMetaObject::invokeMethod(m_shell.get(), "evalIn",
                                   Q_ARG(QVariant, QVariant::fromValue(paneItem)),
                                   Q_ARG(QVariant, script)))
        return QStringLiteral("INVOKE_FAILED");
    QElapsedTimer clock;
    clock.start();
    while (!m_shell->property("jsFinished").toBool() && clock.elapsed() < timeoutMs)
        QTest::qWait(50);
    return m_shell->property("jsFinished").toBool() ? m_shell->property("jsResult").toString()
                                                    : QStringLiteral("JS_TIMEOUT");
}

bool TstPaneIdentity::waitForJs(QObject *paneItem, const QString &script, const QString &needle,
                                int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    QString last;
    while (clock.elapsed() < timeoutMs) {
        last = evalJs(paneItem, script, timeoutMs);
        if (last.contains(needle))
            return true;
        QTest::qWait(100);
    }
    qWarning().noquote() << "probe never matched" << needle << "last:" << last.left(400);
    return false;
}

bool TstPaneIdentity::waitForPage(QObject *paneItem, const QString &readyProbe,
                                  const QString &needle)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kPageLoadTimeoutMs) {
        if (evalJs(paneItem, readyProbe, kProbeTimeoutMs).contains(needle))
            return true;
        QTest::qWait(200);
    }
    return false;
}

QObject *TstPaneIdentity::webViewIn(QObject *paneItem)
{
    QVariant view;
    if (!paneItem
        || !QMetaObject::invokeMethod(m_shell.get(), "findView", Q_RETURN_ARG(QVariant, view),
                                      Q_ARG(QVariant, QVariant::fromValue(paneItem))))
        return nullptr;
    return asObject(view);
}

// ---------------------------------------------------------------------------
// (1) Terminal pane: the Item and its TerminalController must survive.
// ---------------------------------------------------------------------------
void TstPaneIdentity::terminalPaneSurvivesASplit()
{
    QVERIFY(openRegion(QStringLiteral("TerminalRegion.qml"), leafNode(QStringLiteral("terminal-1")),
                       /*terminal=*/true));

    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);

    QObject *const original = panes().constFirst();
    QCOMPARE(original->property("paneId").toString(), QStringLiteral("terminal-1"));
    QPointer<QObject> paneGuard(original);

    auto *controller = qobject_cast<ch::TerminalController *>(
        asObject(original->property("controller")));
    QVERIFY2(controller != nullptr, "the pane never minted a controller through terminalFactory");
    QPointer<ch::TerminalController> controllerGuard(controller);
    QObject *const bridge = asObject(original->property("bridge"));
    QPointer<QObject> bridgeGuard(bridge);

    // Exactly what "split this pane" publishes.
    setNode(branchNode(QStringLiteral("horizontal"),
                       QVariantList{leafNode(QStringLiteral("terminal-1")),
                                    leafNode(QStringLiteral("terminal-2"))}));

    QTRY_VERIFY2(panes().size() == 2,
                 qPrintable(QStringLiteral("the split never produced 2 panes: %1")
                                .arg(describePanes(panes()))));
    QTest::qWait(kSettleMs);

    QVERIFY2(!paneGuard.isNull(),
             "the pane the user was working in was DESTROYED by splitting its region");
    QVERIFY2(!controllerGuard.isNull(),
             "the pane's TerminalController was destroyed by the split: its PTY channel was "
             "closed and its scrollback is gone");
    QVERIFY2(!bridgeGuard.isNull(), "the pane's TerminalBridge was destroyed by the split");

    QObject *const survivor = paneWithId(m_region, QStringLiteral("terminal-1"));
    QVERIFY2(survivor == original,
             qPrintable(QStringLiteral("\"terminal-1\" is a DIFFERENT object after the split. "
                                       "before=%1 after=%2")
                            .arg(reinterpret_cast<quintptr>(original), 0, 16)
                            .arg(reinterpret_cast<quintptr>(survivor), 0, 16)));
    QCOMPARE(asObject(survivor->property("controller")), controller);

    // The new pane is a real, distinct pane with its own controller.
    QObject *const fresh = paneWithId(m_region, QStringLiteral("terminal-2"));
    QVERIFY(fresh != nullptr);
    QVERIFY(fresh != original);
    QVERIFY(asObject(fresh->property("controller")) != controller);
}

// ---------------------------------------------------------------------------
// (2) Editor pane: an unsaved buffer lives in the Monaco page inside the pane
// Item, and its baseline/revision guard lives in the EditorController. Losing
// either loses the user's edits.
// ---------------------------------------------------------------------------
void TstPaneIdentity::editorPaneSurvivesASplit()
{
    const QString fileUrl = QStringLiteral("file:///tmp/ch-pane-identity.txt");
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1"), fileUrl), /*terminal=*/false));

    const auto panes = [this] { return collect(m_region, isLeafPane); };
    const auto editors = [this] { return collect(m_region, isEditorPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTRY_VERIFY2(editors().size() == 1,
                 "the .txt pane did not resolve to an EditorPaneView; the identity claim under "
                 "test would be vacuous");
    QTest::qWait(kSettleMs);

    QObject *const originalPane = panes().constFirst();
    QObject *const originalEditor = editors().constFirst();
    QPointer<QObject> paneGuard(originalPane);
    QPointer<QObject> editorGuard(originalEditor);

    auto *controller =
        qobject_cast<ch::EditorController *>(asObject(originalEditor->property("controller")));
    QVERIFY2(controller != nullptr, "the editor pane never minted a controller");
    QPointer<ch::EditorController> controllerGuard(controller);

    setNode(branchNode(QStringLiteral("vertical"),
                       QVariantList{leafNode(QStringLiteral("viewer-1"), fileUrl),
                                    leafNode(QStringLiteral("viewer-2"))}));

    QTRY_VERIFY2(panes().size() == 2,
                 qPrintable(QStringLiteral("the split never produced 2 panes: %1")
                                .arg(describePanes(panes()))));
    QTest::qWait(kSettleMs);

    QVERIFY2(!paneGuard.isNull(), "the viewer pane was DESTROYED by splitting its region");
    QVERIFY2(!editorGuard.isNull(),
             "the EditorPaneView (and the Monaco page holding the unsaved buffer) was destroyed "
             "by the split");
    QVERIFY2(!controllerGuard.isNull(),
             "the pane's EditorController was destroyed by the split: the unsaved buffer and its "
             "revision guard went with it");
    QCOMPARE(paneWithId(m_region, QStringLiteral("viewer-1")), originalPane);

    // Still exactly one editor: the survivor, not a replacement that would
    // re-open the file (and, with a server, meet a recovery prompt).
    QCOMPARE(editors().size(), 1);
    QCOMPARE(editors().constFirst(), originalEditor);
    QCOMPARE(controller->path(), QStringLiteral("/tmp/ch-pane-identity.txt"));
}

// ---------------------------------------------------------------------------
// (3) The nested case. The second split changes a BRANCH's child set rather
// than the root's, so it exercises the other rebuild path.
// ---------------------------------------------------------------------------
void TstPaneIdentity::bothPanesSurviveASecondSplit()
{
    QVERIFY(openRegion(QStringLiteral("TerminalRegion.qml"), leafNode(QStringLiteral("terminal-1")),
                       /*terminal=*/true));

    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);
    QPointer<QObject> first(panes().constFirst());

    setNode(branchNode(QStringLiteral("horizontal"),
                       QVariantList{leafNode(QStringLiteral("terminal-1")),
                                    leafNode(QStringLiteral("terminal-2"))}));
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(kSettleMs);
    QVERIFY2(!first.isNull(), "the first split already destroyed the original pane");
    QPointer<QObject> second(paneWithId(m_region, QStringLiteral("terminal-2")));
    QVERIFY(!second.isNull());

    // Split terminal-2: branch[terminal-1, branch[terminal-2, terminal-3]].
    setNode(branchNode(
        QStringLiteral("horizontal"),
        QVariantList{leafNode(QStringLiteral("terminal-1")),
                     branchNode(QStringLiteral("vertical"),
                                QVariantList{leafNode(QStringLiteral("terminal-2")),
                                             leafNode(QStringLiteral("terminal-3"))})}));
    QTRY_VERIFY2(panes().size() == 3,
                 qPrintable(QStringLiteral("the nested split never produced 3 panes: %1")
                                .arg(describePanes(panes()))));
    QTest::qWait(kSettleMs);

    QVERIFY2(!first.isNull(), "splitting terminal-2 destroyed terminal-1");
    QVERIFY2(!second.isNull(), "splitting terminal-2 destroyed terminal-2 itself");
    QCOMPARE(paneWithId(m_region, QStringLiteral("terminal-1")), first.data());
    QCOMPARE(paneWithId(m_region, QStringLiteral("terminal-2")), second.data());
    QVERIFY(paneWithId(m_region, QStringLiteral("terminal-3")) != nullptr);
}

// ---------------------------------------------------------------------------
// (4) What the user actually loses: the terminal's scrollback. A rebuilt pane
// re-attaches to the same tmux session and looks fine, so the discriminator is
// state that only THIS page holds — output already scrolled off the screen, and
// a sentinel written into the page's own JS context.
// ---------------------------------------------------------------------------
void TstPaneIdentity::terminalScrollbackSurvivesASplit()
{
    QVERIFY(openRegion(QStringLiteral("TerminalRegion.qml"), leafNode(QStringLiteral("terminal-1")),
                       /*terminal=*/true));

    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const original = panes().constFirst();

    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kPageLoadTimeoutMs && !original->property("pageLoaded").toBool())
        QTest::qWait(100);
    if (!original->property("pageLoaded").toBool())
        QSKIP("the terminal page never loaded; WebEngine cannot run under this recipe");
    QVERIFY2(waitForPage(original, QString::fromLatin1(kJsXtermMounted),
                         QStringLiteral("true")),
             "the xterm renderer never mounted");

    auto *controller =
        qobject_cast<ch::TerminalController *>(asObject(original->property("controller")));
    QVERIFY(controller != nullptr);

    // Output the user "already read", pushed through the real controller ->
    // bridge -> xterm path.
    controller->ingestOutput(QByteArrayLiteral("CH_SCROLLBACK_MARKER_alpha\r\n"));
    QVERIFY2(waitForJs(original, QString::fromLatin1(kJsScreen),
                       QStringLiteral("CH_SCROLLBACK_MARKER_alpha"), kProbeTimeoutMs),
             "controller output never reached the xterm screen");

    // A sentinel in the page's JS context: it can only survive if this exact
    // page does.
    QCOMPARE(evalJs(original, QStringLiteral("window.__chPaneSentinel = 'SENTINEL_1'; 'OK'")),
             QStringLiteral("OK"));

    setNode(branchNode(QStringLiteral("horizontal"),
                       QVariantList{leafNode(QStringLiteral("terminal-1")),
                                    leafNode(QStringLiteral("terminal-2"))}));
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(1000);

    QObject *const survivor = paneWithId(m_region, QStringLiteral("terminal-1"));
    QVERIFY2(survivor == original, "the terminal pane object did not survive the split");
    QCOMPARE(evalJs(original, QStringLiteral("String(window.__chPaneSentinel)")),
             QStringLiteral("SENTINEL_1"));
    const QString screen = evalJs(original, QString::fromLatin1(kJsScreen));
    QVERIFY2(screen.contains(QStringLiteral("CH_SCROLLBACK_MARKER_alpha")),
             qPrintable(QStringLiteral("the split wiped the terminal's scrollback; screen now: %1")
                            .arg(screen.left(400))));
}

// ---------------------------------------------------------------------------
// (5) The editor equivalent: text typed into Monaco and NEVER saved anywhere
// exists only in this page. If the split rebuilds the pane it is gone for good.
// ---------------------------------------------------------------------------
void TstPaneIdentity::unsavedEditorBufferSurvivesASplit()
{
    const QString fileUrl = QStringLiteral("file:///tmp/ch-pane-identity.txt");
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1"), fileUrl), /*terminal=*/false));

    const auto editors = [this] { return collect(m_region, isEditorPane); };
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(editors().size() == 1);
    QObject *const editor = editors().constFirst();

    // Monaco mounts on the WebChannel handshake, with no content needed.
    if (!waitForPage(editor, QStringLiteral("String(!!document.querySelector('.monaco-editor'))"),
                     QStringLiteral("true")))
        QSKIP("the Monaco page never mounted; WebEngine cannot run under this recipe");

    QCOMPARE(evalJs(editor, QStringLiteral("window.__chUnsaved = 'UNSAVED_1'; 'OK'")),
             QStringLiteral("OK"));

    // Real unsaved text: typed into Monaco's input area, never saved, never
    // reported to C++.
    const QString type = QStringLiteral(
        "(function(){var t=document.querySelector('.monaco-editor textarea.inputarea');"
        "if(!t) return 'NO_INPUT'; t.focus();"
        "document.execCommand('insertText', false, 'CH_UNSAVED_EDIT_alpha');"
        "return 'TYPED';})()");
    const bool typed = evalJs(editor, type) == QStringLiteral("TYPED")
                       && waitForJs(editor, QString::fromLatin1(kJsMonacoText),
                                    QStringLiteral("CH_UNSAVED_EDIT_alpha"), 5000);
    if (!typed)
        qInfo("Monaco did not accept synthetic typing; the page sentinel still proves the page");

    setNode(branchNode(QStringLiteral("vertical"),
                       QVariantList{leafNode(QStringLiteral("viewer-1"), fileUrl),
                                    leafNode(QStringLiteral("viewer-2"))}));
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(1000);

    QCOMPARE(editors().size(), 1);
    QCOMPARE(editors().constFirst(), editor);
    QCOMPARE(evalJs(editor, QStringLiteral("String(window.__chUnsaved)")),
             QStringLiteral("UNSAVED_1"));
    if (typed) {
        const QString text = evalJs(editor, QString::fromLatin1(kJsMonacoText));
        QVERIFY2(text.contains(QStringLiteral("CH_UNSAVED_EDIT_alpha")),
                 qPrintable(QStringLiteral("the split threw away the unsaved buffer; Monaco now "
                                           "shows: %1")
                                .arg(text.left(400))));
    }
}

// ---------------------------------------------------------------------------
// (6) FOCUS. Splitting is "give me a pane next to the one I am working in", so
// the command needs to know which pane that IS. Only the pane can say: its
// content is usually a WebEngine page, and focus inside a page is Chromium's
// own state which never surfaces as QML activeFocus. Each pane therefore
// reports the click that put focus there, and the ROOT region republishes it as
// `focusedPaneId` — the property the host persists as selectedPane.
//
// Untested, this is invisible: the region still lays out correctly, the split
// still produces the right number of panes, and the only symptom is that the
// new pane appears next to the WRONG one.
// ---------------------------------------------------------------------------
void TstPaneIdentity::clickPane(QObject *paneObject)
{
    auto *item = qobject_cast<QQuickItem *>(paneObject);
    QVERIFY2(item != nullptr, "no such pane to click");
    auto *window = qobject_cast<QQuickWindow *>(m_shell.get());
    QVERIFY2(window != nullptr, "the test shell is not a window");
    // A zero-extent pane would make every click below a false pass: the event
    // would land on whatever else occupies that point.
    QVERIFY2(item->width() > 4 && item->height() > 4,
             qPrintable(QStringLiteral("the pane has no area to click: %1x%2")
                            .arg(item->width())
                            .arg(item->height())));
    const QPointF centre = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
}

void TstPaneIdentity::regionReportsNoFocusUntilAPaneIsUsed()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);

    QVERIFY2(m_region->property("focusedPaneId").toString().isEmpty(),
             "a region nobody has touched claims a focused pane; the host would persist a "
             "selection the user never made");
}

void TstPaneIdentity::clickingAPaneReportsItsPaneId()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       branchNode(QStringLiteral("horizontal"),
                                  QVariantList{leafNode(QStringLiteral("viewer-1")),
                                               leafNode(QStringLiteral("viewer-2"))}),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(kSettleMs);

    clickPane(paneWithId(m_region, QStringLiteral("viewer-2")));
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("viewer-2"));

    // And it FOLLOWS the user, rather than latching onto the first pane ever
    // clicked — a "focus" that only ever moves one way is no better than the
    // first-leaf guess it replaces.
    clickPane(paneWithId(m_region, QStringLiteral("viewer-1")));
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("viewer-1"));
}

void TstPaneIdentity::focusSurvivesASplit()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);

    clickPane(panes().constFirst());
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("viewer-1"));

    setNode(branchNode(QStringLiteral("horizontal"),
                       QVariantList{leafNode(QStringLiteral("viewer-1")),
                                    leafNode(QStringLiteral("viewer-2"))}));
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(kSettleMs);

    // The split the user just asked for must not lose the answer to "which pane
    // did they ask it FROM" — the very next command needs it too.
    QCOMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("viewer-1"));

    // ...and the re-homed pane is still WIRED. A pane that kept its id but lost
    // its focus connection across the re-parent would go quiet, and the region
    // would be stuck reporting a stale pane forever.
    clickPane(paneWithId(m_region, QStringLiteral("viewer-2")));
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("viewer-2"));
    clickPane(paneWithId(m_region, QStringLiteral("viewer-1")));
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("viewer-1"));
}

void TstPaneIdentity::closingTheFocusedPaneClearsTheFocus()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       branchNode(QStringLiteral("horizontal"),
                                  QVariantList{leafNode(QStringLiteral("viewer-1")),
                                               leafNode(QStringLiteral("viewer-2"))}),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(kSettleMs);

    clickPane(paneWithId(m_region, QStringLiteral("viewer-2")));
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("viewer-2"));

    // Exactly what "close this pane" publishes: the survivor, alone.
    setNode(leafNode(QStringLiteral("viewer-1")));
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);

    // Keeping the closed pane's id would aim the next split at a pane that does
    // not exist, and the host would go on persisting it.
    QCOMPARE(m_region->property("focusedPaneId").toString(), QString());
}

// The host persists this report as the leaf's url (SessionLayouts::setPaneUrl),
// which is the whole mechanism behind "reopening a Dev Session shows the files I
// had open". Nothing else in the suite exercises it, and a broken wire is
// invisible: the pane still shows the file, it is only the NEXT open of that
// session that comes up blank.
void TstPaneIdentity::openingAFileInAPaneIsReportedToTheHost()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);

    QSignalSpy reported(m_region, SIGNAL(paneUrlReported(QString, QString)));
    QVERIFY2(reported.isValid(), "the region does not report what its panes have open");

    // Exactly what "open this file in that pane" publishes: the same leaf, now
    // carrying a url.
    const QString fileUrl = QStringLiteral("file:///tmp/ch-pane-report.txt");
    setNode(leafNode(QStringLiteral("viewer-1"), fileUrl));

    QTRY_VERIFY2(reported.size() >= 1,
                 "the region never told the host what the pane opened; the Dev Session would "
                 "reopen with a blank pane");
    const QList<QVariant> arguments = reported.constLast();
    QCOMPARE(arguments.at(0).toString(), QStringLiteral("viewer-1"));
    QCOMPARE(arguments.at(1).toString(), fileUrl);
}

void TstPaneIdentity::nestedPaneFocusReachesTheRootRegion()
{
    QVERIFY(openRegion(
        QStringLiteral("ViewerRegion.qml"),
        branchNode(QStringLiteral("horizontal"),
                   QVariantList{leafNode(QStringLiteral("viewer-1")),
                                branchNode(QStringLiteral("vertical"),
                                           QVariantList{leafNode(QStringLiteral("viewer-2")),
                                                        leafNode(QStringLiteral("viewer-3"))})}),
        /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 3);
    QTest::qWait(kSettleMs);

    // A pane two levels down reports to the ROOT, which is the region the host
    // holds: focus that only reached the intervening region would be invisible.
    clickPane(paneWithId(m_region, QStringLiteral("viewer-3")));
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("viewer-3"));

    clickPane(paneWithId(m_region, QStringLiteral("viewer-2")));
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("viewer-2"));
}

void TstPaneIdentity::terminalPaneReportsFocusThroughItsChrome()
{
    QVERIFY(openRegion(QStringLiteral("TerminalRegion.qml"), leafNode(QStringLiteral("terminal-1")),
                       /*terminal=*/true));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);

    // A terminal pane is layered — renderer, full-pane placeholder, banner — and
    // the click sniffer has to sit above ALL of it, or a pane that has not come
    // up yet (or one showing a drop banner) would be unfocusable.
    clickPane(panes().constFirst());
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("terminal-1"));
}

// ---------------------------------------------------------------------------
// (7) The claim the whole mechanism rests on. "Focus follows the pane you are
// working in" is only true if a click that lands in the LIVE page — the xterm
// screen the user is typing at, not the placeholder chrome around it — is what
// updates the region. If it were not, focus would only move when the user
// clicked pane furniture, which almost never happens, and the tracking would
// be a decoration.
//
// Two halves, and BOTH are required:
//   * the region learns the pane (the sniffer is above the WebEngineView, so it
//     sees the press first), and
//   * the page still gets its mousedown (the sniffer DECLINES the press, so it
//     falls through). A sniffer that only satisfied the first half would report
//     focus perfectly while making the terminal untypeable.
// ---------------------------------------------------------------------------
void TstPaneIdentity::aClickInsideTheLivePageReportsFocusAndStillReachesThePage()
{
    QVERIFY(openRegion(QStringLiteral("TerminalRegion.qml"), leafNode(QStringLiteral("terminal-1")),
                       /*terminal=*/true));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();

    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kPageLoadTimeoutMs && !pane->property("pageLoaded").toBool())
        QTest::qWait(100);
    if (!pane->property("pageLoaded").toBool())
        QSKIP("the terminal page never loaded; WebEngine cannot run under this recipe");
    QVERIFY2(waitForPage(pane, QString::fromLatin1(kJsXtermMounted), QStringLiteral("true")),
             "the xterm renderer never mounted");
    QTest::qWait(kSettleMs);

    // Counted in the page's own capture phase, so it records the press whatever
    // xterm does with it afterwards.
    QCOMPARE(evalJs(pane, QStringLiteral(
                              "(function(){window.__chPresses=0;"
                              "document.addEventListener('mousedown',"
                              "function(){window.__chPresses++;},true);return 'OK';})()")),
             QStringLiteral("OK"));

    // The pane is loaded, so its full-pane placeholder is invisible and this
    // point is over the live page, not over chrome.
    clickPane(pane);

    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("terminal-1"));
    QVERIFY2(waitForJs(pane, QStringLiteral("String(window.__chPresses > 0)"),
                       QStringLiteral("true"), kProbeTimeoutMs),
             "the focus sniffer SWALLOWED the click: the region learned which pane the user is "
             "in, but the terminal page never saw the press");
}

// ---------------------------------------------------------------------------
// (8) THE KEYBOARD TRAP. Focus is pushed into the xterm page on purpose — that
// is what makes a terminal pane typeable — and the page then answers every key
// itself bar one copy shortcut, so Tab, Shift+Tab and Escape all travel to the
// remote shell. A user who reaches this pane with the keyboard and cannot leave
// it with the keyboard has lost the window: no split command, no close command,
// no other pane, no sidebar. Ctrl+Shift+F6 is the way out, and it has to be
// matched BEFORE the page is offered the key or it is no way out at all.
//
// The assertion is deliberately about the WebEngineView's own activeFocus and
// not about anything the pane merely reports: the whole defect was a pane that
// believed its bookkeeping while Chromium held the keyboard.
// ---------------------------------------------------------------------------
void TstPaneIdentity::theEscapeKeyTakesTheKeyboardOutOfTheTerminalPage()
{
    QVERIFY(openRegion(QStringLiteral("TerminalRegion.qml"), leafNode(QStringLiteral("terminal-1")),
                       /*terminal=*/true));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);
    QObject *const pane = panes().constFirst();

    auto *const view = qobject_cast<QQuickItem *>(webViewIn(pane));
    if (!view)
        QSKIP("the terminal pane produced no WebEngineView; Chromium cannot run under this recipe");

    auto *window = qobject_cast<QQuickWindow *>(m_shell.get());
    QVERIFY2(window != nullptr, "the test shell is not a window");

    // The trap, set. Focus is placed on the embedded view directly rather than
    // by clicking, because the click path is already covered above and this
    // test is about getting back OUT of a page that has the keyboard however it
    // came by it.
    view->forceActiveFocus();
    QTRY_VERIFY2(view->hasActiveFocus(),
                 "the embedded page never took the keyboard, so there is no trap to escape");

    QTest::keyClick(window, Qt::Key_F6, Qt::ControlModifier | Qt::ShiftModifier);

    QTRY_VERIFY2(!view->hasActiveFocus(),
                 "Ctrl+Shift+F6 left the keyboard inside the terminal page: the pane is a "
                 "keyboard trap, because the page answers every other key itself and there is "
                 "no other way back to the pane's chrome");
    auto *const paneItem = qobject_cast<QQuickItem *>(pane);
    QVERIFY2(paneItem && paneItem->hasActiveFocus(),
             "the keyboard left the page and landed nowhere, which is a different way of "
             "losing it");
}

// Arriving by keyboard says "I am working here" exactly as a click does. Until
// this, activation came only from the pane's MouseArea, so a keyboard user
// could be typing in one pane while the header's active mark — and every split
// and close command — pointed at whichever pane was last clicked.
void TstPaneIdentity::keyboardFocusEnteringAPaneMakesItTheActivePane()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       branchNode(QStringLiteral("horizontal"),
                                  QVariantList{leafNode(QStringLiteral("viewer-1")),
                                               leafNode(QStringLiteral("viewer-2"))}),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(kSettleMs);

    clickPane(paneWithId(m_region, QStringLiteral("viewer-1")));
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("viewer-1"));

    // No pointer anywhere near the second pane: the keyboard lands in a control
    // inside it, which is all a Tab from the first pane's last control does.
    auto *const field = qobject_cast<QQuickItem *>(
        childNamed(paneWithId(m_region, QStringLiteral("viewer-2")),
                   QStringLiteral("viewerAddressField")));
    QVERIFY2(field != nullptr, "the second viewer pane has no address field to tab into");
    field->forceActiveFocus();
    QTRY_VERIFY(field->hasActiveFocus());

    QTRY_VERIFY2(m_region->property("focusedPaneId").toString() == QLatin1String("viewer-2"),
                 "keyboard focus entered the second pane and the region still names the first: "
                 "a split or close command would act on a pane the user has left");
}

// ---------------------------------------------------------------------------
// PANE ACTIONS. A header action must carry the pane it belongs to, rather than
// asking Main.qml to resolve the remembered pane or the first leaf.
// ---------------------------------------------------------------------------
void TstPaneIdentity::paneHeaderRequestsNameTheirOwnPane()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       branchNode(QStringLiteral("horizontal"),
                                  QVariantList{leafNode(QStringLiteral("viewer-1")),
                                               leafNode(QStringLiteral("viewer-2"))}),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(kSettleMs);
    QCOMPARE(m_region->property("focusedPaneId").toString(), QString());

    QObject *const second = paneWithId(m_region, QStringLiteral("viewer-2"));
    QVERIFY(second);
    QObject *const splitButton = childNamed(second, QStringLiteral("viewerSplitHorizontalButton"));
    QObject *const closeButton = childNamed(second, QStringLiteral("viewerCloseButton"));
    QVERIFY2(splitButton, "the viewer pane has no side-by-side split button");
    QVERIFY2(closeButton, "the viewer pane has no close button");

    QSignalSpy splitRequested(m_region, SIGNAL(splitRequested(QString, QString)));
    QVERIFY(splitRequested.isValid());
    QVERIFY(QMetaObject::invokeMethod(splitButton, "clicked"));
    QTRY_COMPARE(splitRequested.size(), 1);
    QCOMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("viewer-2"));
    const QList<QVariant> splitArgs = splitRequested.constFirst();
    QCOMPARE(splitArgs.at(0).toString(), QStringLiteral("viewer-2"));
    QCOMPARE(splitArgs.at(1).toString(), QStringLiteral("horizontal"));

    // The second pane is still present because this standalone test does not
    // publish a replacement tree. The close request must still name it.
    QSignalSpy closeRequested(m_region, SIGNAL(closePaneRequested(QString)));
    QVERIFY(closeRequested.isValid());
    QVERIFY(QMetaObject::invokeMethod(closeButton, "clicked"));
    QTRY_COMPARE(closeRequested.size(), 1);
    QCOMPARE(closeRequested.constFirst().constFirst().toString(),
             QStringLiteral("viewer-2"));
}

void TstPaneIdentity::terminalCloseKillsOnlyAfterConfirmation()
{
    QVERIFY(openRegion(QStringLiteral("TerminalRegion.qml"),
                       branchNode(QStringLiteral("vertical"),
                                  QVariantList{leafNode(QStringLiteral("terminal-1")),
                                               leafNode(QStringLiteral("terminal-2"))}),
                       /*terminal=*/true));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(kSettleMs);

    QObject *const second = paneWithId(m_region, QStringLiteral("terminal-2"));
    QVERIFY(second);
    QObject *const closeButton = childNamed(second, QStringLiteral("terminalCloseButton"));
    QVERIFY2(closeButton, "the terminal pane has no close button");
    QVERIFY(!childNamed(second, QStringLiteral("terminalKillButton")));
    QSignalSpy killRequested(m_region, SIGNAL(killTerminalRequested(QString)));
    QSignalSpy closeRequested(m_region, SIGNAL(closePaneRequested(QString)));
    QVERIFY(killRequested.isValid());
    QVERIFY(closeRequested.isValid());

    // Declining the destructive close emits neither request.
    QVERIFY(QMetaObject::invokeMethod(closeButton, "clicked"));
    QObject *const dialog = childNamed(second, QStringLiteral("terminalPaneKillDialog"));
    QVERIFY2(dialog, "the terminal close button did not open its confirmation dialog");
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTest::qWait(50);
    QCOMPARE(killRequested.size(), 0);
    QCOMPARE(closeRequested.size(), 0);

    // Acceptance emits kill first, then the layout close, for this pane.
    QVERIFY(QMetaObject::invokeMethod(closeButton, "clicked"));
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
    QTRY_COMPARE(killRequested.size(), 1);
    QTRY_COMPARE(closeRequested.size(), 1);
    QCOMPARE(killRequested.constFirst().constFirst().toString(),
             QStringLiteral("terminal-2"));
    QCOMPARE(closeRequested.constFirst().constFirst().toString(),
             QStringLiteral("terminal-2"));
}

// ---------------------------------------------------------------------------
// (8) The per-pane header. It lives inside the pane, so it is subject to the
// same rule as the pane's web page: the split must RE-HOME it, never rebuild it.
// A rebuilt header would be invisible in a screenshot and would mean the pane
// itself had been rebuilt — the exact failure the rest of this file guards, now
// reachable through a new object.
// ---------------------------------------------------------------------------
void TstPaneIdentity::paneHeaderTravelsWithThePaneAcrossASplit()
{
    QVERIFY(openRegion(QStringLiteral("TerminalRegion.qml"), leafNode(QStringLiteral("terminal-1")),
                       /*terminal=*/true));

    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);

    QObject *const original = panes().constFirst();
    const QList<QObject *> headers = collect(original, isPaneHeader);
    QVERIFY2(headers.size() == 1,
             qPrintable(QStringLiteral("a terminal pane has %1 headers, expected exactly one; "
                                       "without it the pane's name, state and close action are "
                                       "only reachable from the command palette")
                            .arg(headers.size())));
    QObject *const header = headers.constFirst();
    QPointer<QObject> headerGuard(header);

    // The header must actually be SAYING something, or it is a blank strip that
    // satisfies the identity check while telling the user nothing.
    QCOMPARE(header->property("title").toString(), QStringLiteral("terminal-1"));
    QVERIFY2(!header->property("subtitle").toString().isEmpty(),
             "the terminal pane header does not report its connection state");

    setNode(branchNode(QStringLiteral("vertical"),
                       QVariantList{leafNode(QStringLiteral("terminal-1")),
                                    leafNode(QStringLiteral("terminal-2"))}));
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(kSettleMs);

    QVERIFY2(!headerGuard.isNull(), "the split destroyed the pane's header");
    QObject *const survivor = paneWithId(m_region, QStringLiteral("terminal-1"));
    QCOMPARE(survivor, original);
    const QList<QObject *> after = collect(survivor, isPaneHeader);
    QCOMPARE(after.size(), 1);
    QVERIFY2(after.constFirst() == header,
             "\"terminal-1\" came out of the split with a DIFFERENT header object, so the pane "
             "around it was rebuilt too");

    // The new pane has its own header, not a share of the survivor's.
    QObject *const fresh = paneWithId(m_region, QStringLiteral("terminal-2"));
    QVERIFY(fresh != nullptr);
    const QList<QObject *> freshHeaders = collect(fresh, isPaneHeader);
    QCOMPARE(freshHeaders.size(), 1);
    QVERIFY(freshHeaders.constFirst() != header);
    QCOMPARE(freshHeaders.constFirst()->property("title").toString(),
             QStringLiteral("terminal-2"));
}
void TstPaneIdentity::terminalCustomTitleAppearsAndClearsInTheHeader()
{
    const QVariantMap titled =
        leafNode(QStringLiteral("terminal-1"), QString(),
                 QStringLiteral("Build shell"));
    QVERIFY(openRegion(QStringLiteral("TerminalRegion.qml"), titled,
                       /*terminal=*/true));

    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);

    QObject *const pane = panes().constFirst();
    const QList<QObject *> headers = collect(pane, isPaneHeader);
    QCOMPARE(headers.size(), 1);
    QObject *const header = headers.constFirst();
    // The region receives the same trimmed title that SessionLayouts publishes
    // from a persisted leaf; the custom title wins over the generated slot label.
    QCOMPARE(header->property("title").toString(), QStringLiteral("Build shell"));
    QCOMPARE(pane->property("paneId").toString(), QStringLiteral("terminal-1"));

    // Clearing the optional field must reveal the generated label without
    // changing the pane object or its slot identity.
    setNode(leafNode(QStringLiteral("terminal-1")));
    QTRY_COMPARE(header->property("title").toString(),
                 QStringLiteral("terminal-1"));
    QCOMPARE(paneWithId(m_region, QStringLiteral("terminal-1")), pane);
}


// The header's `active` mark is the only thing on screen that answers "where
// will the next split land". It is pushed from the region (applyFocusFlags), so
// a broken push leaves every pane looking equally focused — which is worse than
// no mark, because it is confidently wrong.
void TstPaneIdentity::theFocusedPaneIsTheOnlyOneMarkedActive()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       branchNode(QStringLiteral("horizontal"),
                                  QVariantList{leafNode(QStringLiteral("viewer-1")),
                                               leafNode(QStringLiteral("viewer-2"))}),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(kSettleMs);

    QObject *const first = paneWithId(m_region, QStringLiteral("viewer-1"));
    QObject *const second = paneWithId(m_region, QStringLiteral("viewer-2"));
    QVERIFY(first && second);

    // Nothing touched yet: no pane may claim the mark.
    QVERIFY2(!first->property("paneActive").toBool() && !second->property("paneActive").toBool(),
             "a pane nobody has touched already shows itself as focused");

    clickPane(second);
    QTRY_VERIFY(second->property("paneActive").toBool());
    QVERIFY2(!first->property("paneActive").toBool(),
             "both panes show as focused, so the mark says nothing");

    clickPane(first);
    QTRY_VERIFY(first->property("paneActive").toBool());
    QVERIFY2(!second->property("paneActive").toBool(),
             "the mark did not leave the pane the user moved away from");

    // The mark reaches the header, not just the pane: a flag no header reads is
    // invisible to the user.
    const QList<QObject *> headers = collect(first, isPaneHeader);
    QCOMPARE(headers.size(), 1);
    QVERIFY2(headers.constFirst()->property("active").toBool(),
             "the focused pane's header does not show the focus mark");
}

// ---------------------------------------------------------------------------
// (9) The address bar. Before it there was NO path from a running client to a
// file in a viewer pane at all: the sidebar lists Dev Sessions, and the palette
// only splits and closes panes. So this is the feature, not a convenience.
// ---------------------------------------------------------------------------
void TstPaneIdentity::enterAddress(QObject *paneObject, const QString &text)
{
    QObject *const field = childNamed(paneObject, QStringLiteral("viewerAddressField"));
    QVERIFY2(field != nullptr, "the viewer pane has no address field, so nothing can be opened "
                               "in it");
    auto *window = qobject_cast<QQuickWindow *>(m_shell.get());
    QVERIFY2(window != nullptr, "the test shell is not a window");

    // CLICKED, not focused programmatically. The pane lays a full-size click
    // sniffer over everything it contains (it is how focus is detected inside a
    // WebEngine page) and that sniffer DECLINES the press so it falls through. If
    // it ever stopped declining, the address bar would be unfocusable by mouse —
    // i.e. unusable — while every other assertion here still passed.
    auto *item = qobject_cast<QQuickItem *>(field);
    QVERIFY(item != nullptr);
    QVERIFY2(item->width() > 4 && item->height() > 4,
             qPrintable(QStringLiteral("the address field has no area: %1x%2")
                            .arg(item->width())
                            .arg(item->height())));
    field->setProperty("text", text);
    const QPointF centre = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    QTRY_VERIFY2(field->property("activeFocus").toBool(),
                 "clicking the address field did not give it the keyboard, so nothing can be "
                 "typed into it");
    QTest::keyClick(window, Qt::Key_Return);
}

QString TstPaneIdentity::accessibleNameOf(QObject *item)
{
    QVariant name;
    if (!item
        || !QMetaObject::invokeMethod(m_shell.get(), "accessibleName",
                                      Q_RETURN_ARG(QVariant, name),
                                      Q_ARG(QVariant, QVariant::fromValue(item))))
        return QString();
    return name.toString();
}

int TstPaneIdentity::accessibleRoleOf(QObject *item)
{
    QVariant role;
    if (!item
        || !QMetaObject::invokeMethod(m_shell.get(), "accessibleRole",
                                      Q_RETURN_ARG(QVariant, role),
                                      Q_ARG(QVariant, QVariant::fromValue(item))))
        return -1;
    return role.toInt();
}

void TstPaneIdentity::theAddressBarOpensARemotePath()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);
    QObject *const pane = panes().constFirst();

    // The host is told too, or the Dev Session reopens blank however well the
    // pane itself behaved.
    QSignalSpy reported(m_region, SIGNAL(paneUrlReported(QString, QString)));
    QVERIFY(reported.isValid());

    // A path with no trailing slash is probed with file.listDirectory first (only
    // the server knows whether it is a directory). This ViewerModel has no
    // client, so the probe fails immediately and the path is opened as a file —
    // which is exactly the branch a hand-typed file path takes against a real
    // server too.
    enterAddress(pane, QStringLiteral("/srv/repos/app/README.md"));

    QTRY_COMPARE(pane->property("url").toUrl(),
                 QUrl(QStringLiteral("file:///srv/repos/app/README.md")));
    // file:// inside CodeHarbor always means the REMOTE server, so the pane must
    // have resolved a bare path to that spelling and not to a local one.
    QCOMPARE(pane->property("kind").toString(), QStringLiteral("markdown"));

    QTRY_VERIFY2(reported.size() >= 1,
                 "the pane opened the address but never told the host, so the Dev Session would "
                 "reopen without it");
    const QList<QVariant> arguments = reported.constLast();
    QCOMPARE(arguments.at(0).toString(), QStringLiteral("viewer-1"));
    QCOMPARE(arguments.at(1).toString(), QStringLiteral("file:///srv/repos/app/README.md"));
}

void TstPaneIdentity::theAddressBarOpensAUrlAsGiven()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);
    QObject *const pane = panes().constFirst();

    // An address that already carries a scheme is NOT a remote path and must not
    // be turned into one — "file:///https://example.com/" would be a nonsense
    // remote read, and the sandboxed external profile is what such a page belongs
    // on (SPEC 7.2).
    enterAddress(pane, QStringLiteral("https://example.com/docs"));

    QTRY_COMPARE(pane->property("url").toUrl(),
                 QUrl(QStringLiteral("https://example.com/docs")));
    QCOMPARE(pane->property("kind").toString(), QStringLiteral("web"));
}

void TstPaneIdentity::theAddressBarPercentEncodesADelimiterInAFileName()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);
    QObject *const pane = panes().constFirst();

    // "#" is a URL delimiter, so a remote file whose name contains one only
    // survives the round trip if every path SEGMENT is percent-encoded — which is
    // what makes fileUrlFor() the exact inverse of the remotePath() decoding the
    // views do. encodeURI() would leave the "#" alone and silently turn the rest
    // of the name into a fragment, i.e. read the wrong file.
    enterAddress(pane, QStringLiteral("/tmp/notes#1.txt"));

    QTRY_COMPARE(pane->property("url").toUrl().toString(QUrl::FullyEncoded),
                 QStringLiteral("file:///tmp/notes%231.txt"));
    // ...and it decodes back to the path the user typed, which is what the RPC
    // layer is handed.
    QCOMPARE(pane->property("url").toUrl().toLocalFile(), QStringLiteral("/tmp/notes#1.txt"));
}

// REGRESSION. openPaneTarget() is the host's one "show this address in that
// pane" entry point, and it used to go straight to ViewerPane.openUrlWithKind(),
// which is the "Open as" OVERRIDE and refuses an empty handler. So every caller
// that merely had an address — an agent's `open` command, and any future host
// command — was answered with "That viewer cannot display this target." and the
// pane never navigated. Deciding the handler is the registry's job; an empty kind
// means "you decide", not "no handler".
void TstPaneIdentity::openPaneTargetWithNoKindLetsTheRegistryDecide()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);
    QObject *const pane = panes().constFirst();

    // A refusal arrives as the pane's own message request, so watch for it: an
    // "accepted" return with a toast behind it would be a false pass.
    QSignalSpy refused(pane, SIGNAL(messageRequested(QString)));
    QVERIFY(refused.isValid());

    QVariant accepted;
    QVERIFY(QMetaObject::invokeMethod(
        m_region, "openPaneTarget", Q_RETURN_ARG(QVariant, accepted),
        Q_ARG(QVariant, QStringLiteral("viewer-1")),
        Q_ARG(QVariant, QStringLiteral("file:///srv/repos/app/README.md")),
        Q_ARG(QVariant, QString())));
    QVERIFY2(accepted.toBool(), "the pane refused an address that named no handler");
    QTRY_COMPARE(pane->property("url").toUrl(),
                 QUrl(QStringLiteral("file:///srv/repos/app/README.md")));
    // The REGISTRY chose, which is the whole point: a .md file lands on the
    // rendered Markdown handler without anyone naming it.
    QCOMPARE(pane->property("kind").toString(), QStringLiteral("markdown"));
    QCOMPARE(refused.size(), 0);

    // An explicit kind still overrides it, so the "Open as" path is untouched.
    QVERIFY(QMetaObject::invokeMethod(
        m_region, "openPaneTarget", Q_RETURN_ARG(QVariant, accepted),
        Q_ARG(QVariant, QStringLiteral("viewer-1")),
        Q_ARG(QVariant, QStringLiteral("file:///srv/repos/app/README.md")),
        Q_ARG(QVariant, QStringLiteral("editor"))));
    QVERIFY(accepted.toBool());
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("text"));

    // And an UNKNOWN kind is still refused: that spelling can only come from a
    // caller naming a handler that does not exist.
    QVERIFY(QMetaObject::invokeMethod(
        m_region, "openPaneTarget", Q_RETURN_ARG(QVariant, accepted),
        Q_ARG(QVariant, QStringLiteral("viewer-1")),
        Q_ARG(QVariant, QStringLiteral("file:///srv/repos/app/README.md")),
        Q_ARG(QVariant, QStringLiteral("hologram"))));
    QVERIFY2(!accepted.toBool(), "an unknown handler name was accepted");
    QTRY_COMPARE(refused.size(), 1);
}

void TstPaneIdentity::navigationTruncatesForwardHistoryAfterFreshAddress()
{
    const QString first = QStringLiteral("file:///tmp/first.bin");
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1"), first), /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();
    QTRY_VERIFY(pane->property("navigationReady").toBool());


    const QString second = QStringLiteral("file:///tmp/second.bin");
    const QString third = QStringLiteral("file:///tmp/third.bin");
    const QString replacement = QStringLiteral("file:///tmp/replacement.bin");
    pane->setProperty("url", QUrl(second));
    pane->setProperty("url", QUrl(third));
    QTRY_COMPARE(pane->property("navigationIndex").toInt(), 2);
    QCOMPARE(asList(pane->property("navigationHistory")),
             (QVariantList{first, second, third}));

    QVERIFY(QMetaObject::invokeMethod(pane, "goBack"));
    QTRY_COMPARE(pane->property("url").toUrl(), QUrl(second));
    QCOMPARE(pane->property("navigationIndex").toInt(), 1);

    // A fresh address from the middle of the list discards only the forward
    // branch. Keeping the old third address here would make Forward revisit a
    // page the user deliberately replaced.
    pane->setProperty("url", QUrl(replacement));
    QTRY_COMPARE(pane->property("navigationIndex").toInt(), 2);
    QCOMPARE(asList(pane->property("navigationHistory")),
             (QVariantList{first, second, replacement}));
    QVERIFY(!pane->property("canGoForward").toBool());
}

void TstPaneIdentity::navigationButtonsAreDisabledAtHistoryEnds()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1"),
                                QStringLiteral("file:///tmp/only.bin")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();
    QTRY_VERIFY(pane->property("navigationReady").toBool());

    QObject *const back = childNamed(pane, QStringLiteral("viewerBackButton"));
    QObject *const forward = childNamed(pane, QStringLiteral("viewerForwardButton"));
    QObject *const reload = childNamed(pane, QStringLiteral("viewerReloadButton"));
    QObject *const home = childNamed(pane, QStringLiteral("viewerHomeButton"));
    QVERIFY(back);
    QVERIFY(forward);
    QVERIFY(reload);
    QVERIFY(home);

    // The first address has nowhere to go in either direction. Disabled
    // controls must also leave the Tab chain, not merely ignore a click.
    QTRY_VERIFY(!pane->property("canGoBack").toBool());
    QVERIFY(!pane->property("canGoForward").toBool());
    QVERIFY(!back->property("enabled").toBool());
    QVERIFY(!forward->property("enabled").toBool());
    QCOMPARE(back->property("focusPolicy").toInt() & Qt::TabFocus, 0);
    QCOMPARE(forward->property("focusPolicy").toInt() & Qt::TabFocus, 0);

    pane->setProperty("url", QUrl(QStringLiteral("file:///tmp/next.bin")));
    QTRY_VERIFY(pane->property("canGoBack").toBool());
    QVERIFY(!pane->property("canGoForward").toBool());
    QVERIFY(back->property("enabled").toBool());
    QVERIFY(!forward->property("enabled").toBool());
    QVERIFY((back->property("focusPolicy").toInt() & Qt::TabFocus) != 0);
    QCOMPARE(forward->property("focusPolicy").toInt() & Qt::TabFocus, 0);

    QVERIFY(QMetaObject::invokeMethod(pane, "goBack"));
    QTRY_COMPARE(pane->property("url").toUrl(),
                 QUrl(QStringLiteral("file:///tmp/only.bin")));
    QVERIFY(!back->property("enabled").toBool());
    QVERIFY(forward->property("enabled").toBool());
    QCOMPARE(back->property("focusPolicy").toInt() & Qt::TabFocus, 0);
    QVERIFY((forward->property("focusPolicy").toInt() & Qt::TabFocus) != 0);
}

void TstPaneIdentity::reloadRefreshesNonWebContent()
{
    const QString address = QStringLiteral("file:///tmp/reload.bin");
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1"), address),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();
    QTRY_VERIFY(pane->property("navigationReady").toBool());

    QTRY_VERIFY2(collect(pane, isBinaryView).size() == 1,
                 "the reload test did not resolve to the binary content handler");
    QObject *const original = collect(pane, isBinaryView).constFirst();
    original->setProperty("errorText", QStringLiteral("stale content"));
    QVERIFY(QMetaObject::invokeMethod(pane, "reloadCurrent"));

    // The binary handler has no page-level reload primitive. ViewerPane must
    // still re-fetch its current resource by recreating that handler, not by
    // silently assigning the same URL and leaving stale state on screen.
    // Wait for a DIFFERENT handler object, not merely for one to exist: the old
    // one is still on screen for an event-loop turn, so a check that only
    // counts handlers passes against the stale item and reads its stale state.
    QTRY_VERIFY2(collect(pane, isBinaryView).size() == 1
                     && collect(pane, isBinaryView).constFirst() != original,
                 "the binary handler was never recreated by the reload");
    QObject *const refreshed = collect(pane, isBinaryView).constFirst();
    QCOMPARE(refreshed->property("errorText").toString(), QString());
    QCOMPARE(pane->property("url").toUrl(), QUrl(address));
}
void TstPaneIdentity::reloadKeepsTheEditorHandlerAlive()
{
    const QString address = QStringLiteral("file:///tmp/reload.txt");
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1"), address),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    const auto editors = [this] { return collect(m_region, isEditorPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTRY_VERIFY2(editors().size() == 1,
                 "the reload test did not resolve to an editor handler");
    QObject *const original = editors().constFirst();
    QPointer<QObject> editorGuard(original);
    QObject *const controller = asObject(original->property("controller"));
    QVERIFY(controller);

    QVERIFY(QMetaObject::invokeMethod(panes().constFirst(), "reloadCurrent"));
    QTest::qWait(100);

    QVERIFY2(!editorGuard.isNull(),
             "reloading an editor destroyed the live handler and its unsaved buffer");
    QCOMPARE(editors().size(), 1);
    QCOMPARE(editors().constFirst(), original);
    QCOMPARE(asObject(original->property("controller")), controller);
}

void TstPaneIdentity::editorReloadPreservesDirtyBufferAndRejectsStaleCapture()
{
    ReadFileServer server;
    QVERIFY(server.bind(&m_client));
    const QString firstPath = QStringLiteral("/repo/editor-a.txt");
    server.setFile(firstPath, QString(QChar(0xfeff)) + QStringLiteral("SERVER_BASELINE_A\r\n"));
    server.setFile(QStringLiteral("/repo/editor-b.txt"), QStringLiteral("SERVER_BASELINE_B"));
    std::unique_ptr<QObject> monacoProbe = installMonacoProbe();
    QVERIFY(monacoProbe);
    const auto restoreScripts = qScopeGuard([&monacoProbe] {
        QMetaObject::invokeMethod(monacoProbe.get(), "restore");
    });
    QObject *const pane = openInShell(
        QStringLiteral("ViewerPane.qml"),
        {{QStringLiteral("url"), QUrl(QStringLiteral("file:///repo/editor-a.txt"))},
         {QStringLiteral("acceptedServerId"), QStringLiteral("server-reload")},
         {QStringLiteral("sessionId"), QStringLiteral("dev-reload")},
         {QStringLiteral("paneId"), QStringLiteral("viewer-reload")}});
    QVERIFY(pane);
    QTRY_COMPARE(collect(pane, isEditorPane).size(), 1);
    QObject *const editor = collect(pane, isEditorPane).constFirst();
    auto *const controller =
        qobject_cast<ch::EditorController *>(asObject(editor->property("controller")));
    QVERIFY(controller);
    const QString capture = QStringLiteral(
        "typeof window.captureEditorContent === 'function'"
        " ? window.captureEditorContent() : null");
    QVERIFY2(waitForJs(editor, capture, QStringLiteral("SERVER_BASELINE_A"), kPageLoadTimeoutMs),
             "the real editor did not receive the first file through its document adapter");
    const QString firstCodePoint =
        QStringLiteral("String(window.captureEditorContent().charCodeAt(0))");
    QCOMPARE(evalJs(editor, firstCodePoint), QStringLiteral("65279"));
    const QString typed = evalJs(editor, QStringLiteral(
        "(function(){var e=window.monaco.editor.getEditors()[0];"
        "var m=e.getModel(); e.focus(); var last=m.getLineCount();"
        "e.setPosition({lineNumber:last,column:m.getLineMaxColumn(last)});"
        "e.trigger('keyboard','type',{text:'UNSAVED_RELOAD_TEXT'});"
        "return window.captureEditorContent();})()"));
    QVERIFY2(typed.contains(QStringLiteral("UNSAVED_RELOAD_TEXT")),
             "Monaco did not accept the regression's unsaved edit");
    QVERIFY(typed.contains(QStringLiteral("\r\n")));
    QTRY_VERIFY(controller->dirty());
    QTRY_VERIFY(pane->property("contentDirty").toBool());
    const QString revision = controller->revision();
    const QString oldBridgeId = editor->property("pageBridgeId").toString();
    QPointer<QObject> oldBridge = asObject(editor->property("pageBridge"));
    const int originalReads = server.readCount(firstPath);
    QPointer<QObject> editorGuard(editor);
    auto *const window = qobject_cast<QQuickWindow *>(m_shell.get());
    auto *const reloadButton =
        qobject_cast<QQuickItem *>(childNamed(pane, QStringLiteral("viewerReloadButton")));
    QVERIFY(window && reloadButton);
    QTRY_VERIFY(reloadButton->isVisible() && reloadButton->width() > 4
                && reloadButton->height() > 4);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      reloadButton->mapToScene(QPointF(reloadButton->width() / 2,
                                                       reloadButton->height() / 2)).toPoint());
    QTRY_VERIFY(editorGuard && editor->property("pageBridgeId").toString() != oldBridgeId);
    QCOMPARE(collect(pane, isEditorPane).constFirst(), editor);
    QCOMPARE(asObject(editor->property("controller")), controller);
    QTRY_VERIFY_WITH_TIMEOUT(editor->property("pageReady").toBool(), kPageLoadTimeoutMs);
    QVERIFY2(waitForJs(editor, capture, QStringLiteral("UNSAVED_RELOAD_TEXT"), kPageLoadTimeoutMs),
             "host reload replaced the live dirty buffer with server or recovery bytes");
    QCOMPARE(evalJs(editor, capture), typed);
    QCOMPARE(evalJs(editor, firstCodePoint), QStringLiteral("65279"));
    QVERIFY(controller->dirty());
    QCOMPARE(controller->revision(), revision);
    QCOMPARE(server.readCount(firstPath), originalReads);
    QTRY_VERIFY(oldBridge.isNull());

    // Directory changes retire recovery authority even though the live path,
    // controller and page adapter stay the same. An old capture cannot cross it.
    const QString bridgeBeforeContext = editor->property("pageBridgeId").toString();
    const quint64 contextBeforeCapture = controller->recoveryContextRevision();
    QVERIFY(QMetaObject::invokeMethod(pane, "reloadCurrent"));
    controller->setRecoveryDir(QStringLiteral("/owned-fixture/reload"));
    QVERIFY(controller->recoveryContextRevision() != contextBeforeCapture);
    QTest::qWait(kSettleMs);
    QCOMPARE(editor->property("pageBridgeId").toString(), bridgeBeforeContext);
    QCOMPARE(evalJs(editor, capture), typed);
    QCOMPARE(evalJs(editor, firstCodePoint), QStringLiteral("65279"));
    QVERIFY(controller->dirty());
    QCOMPARE(controller->revision(), revision);

    // A failed replacement document cannot capture its DOM, but the native
    // controller still owns the prepared dirty RAM buffer for this adapter.
    const QUrl validBundle = editor->property("editorBundleUrl").toUrl();
    editor->setProperty("editorBundleUrl",
                        QUrl(QStringLiteral("qrc:/codeharbor/web/editor/missing-retry.html")));
    QTRY_VERIFY_WITH_TIMEOUT(!editor->property("pageError").toString().isEmpty(),
                             kPageLoadTimeoutMs);
    QVERIFY(!editor->property("pageReady").toBool());
    QVERIFY(controller->dirty());
    QCOMPARE(controller->revision(), revision);
    editor->setProperty("editorBundleUrl", validBundle);
    QTRY_VERIFY_WITH_TIMEOUT(editor->property("pageReady").toBool(), kPageLoadTimeoutMs);
    QVERIFY2(waitForJs(editor, capture, QStringLiteral("UNSAVED_RELOAD_TEXT"), kPageLoadTimeoutMs),
             "a failed bundle load stranded the controller-owned dirty reload buffer");
    QCOMPARE(evalJs(editor, capture), typed);
    QCOMPARE(evalJs(editor, firstCodePoint), QStringLiteral("65279"));
    QVERIFY(controller->dirty());
    QCOMPARE(controller->revision(), revision);
    QCOMPARE(server.readCount(firstPath), originalReads);

    // Capture is asynchronous. A direct newer file navigation must invalidate
    // that callback instead of reloading the replacement with the old capture.
    QVERIFY(QMetaObject::invokeMethod(pane, "reloadCurrent"));
    pane->setProperty("url", QUrl(QStringLiteral("file:///repo/editor-b.txt")));
    // A second reload before the new LoadStarted signal must not capture A's
    // still-visible DOM while attributing it to B's freshly registered adapter.
    QVERIFY(!editor->property("pageReady").toBool());
    QVERIFY(QMetaObject::invokeMethod(pane, "reloadCurrent"));
    QVERIFY2(waitForJs(editor, capture, QStringLiteral("SERVER_BASELINE_B"), kPageLoadTimeoutMs),
             "a stale reload capture took over the newer file");
    QTest::qWait(600); // Also allow the retiring page's 500ms report to arrive.
    QCOMPARE(evalJs(editor, capture), QStringLiteral("SERVER_BASELINE_B"));
    QCOMPARE(controller->path(), QStringLiteral("/repo/editor-b.txt"));
    QVERIFY(!controller->dirty());
    QCOMPARE(server.readCount(firstPath), originalReads);
}

void TstPaneIdentity::remoteWebReloadRecoversAfterAReadFailure()
{
    ReadFileServer server;
    QVERIFY(server.bind(&m_client));
    const QString path = QStringLiteral("/repo/retry.html");
    QObject *const handler = openInShell(
        QStringLiteral("ViewerWebView.qml"),
        {{QStringLiteral("url"), QUrl(QStringLiteral("file:///repo/retry.html"))},
         {QStringLiteral("remoteFile"), true}});
    QVERIFY(handler);
    QObject *const browser = webViewIn(handler);
    QVERIFY(browser);
    QTRY_VERIFY_WITH_TIMEOUT(server.readCount(path) > 0, kPageLoadTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!handler->property("errorText").toString().isEmpty(),
                             kPageLoadTimeoutMs);
    QTRY_VERIFY(!handler->property("loading").toBool());
    QVERIFY(!browser->property("visible").toBool());

    server.setFile(path, QStringLiteral("<!doctype html><p>The retry succeeded.</p>"));
    const int previousReads = server.readCount(path);
    QVERIFY(QMetaObject::invokeMethod(handler, "reload"));
    QTRY_VERIFY_WITH_TIMEOUT(server.readCount(path) > previousReads, kPageLoadTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!handler->property("loading").toBool(), kPageLoadTimeoutMs);
    QCOMPARE(handler->property("errorText").toString(), QString());
    QVERIFY(browser->property("visible").toBool());
}

void TstPaneIdentity::navigationDismissesAnOlderEditorClosePrompt()
{
    EditorControllerStub controller;
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1"),
                                QStringLiteral("file:///repo/original.txt")),
                       /*terminal=*/false));
    QTRY_COMPARE(collect(m_region, isLeafPane).size(), 1);
    QTRY_COMPARE(collect(m_region, isEditorPane).size(), 1);
    QObject *const pane = collect(m_region, isLeafPane).constFirst();
    QObject *const editor = collect(m_region, isEditorPane).constFirst();
    editor->setProperty("controller", QVariant::fromValue<QObject *>(&controller));
    QSignalSpy closed(pane, SIGNAL(closeRequested(QString)));
    QVERIFY(closed.isValid());
    controller.setDirty(true);
    QTRY_VERIFY(pane->property("contentDirty").toBool());
    QVERIFY(QMetaObject::invokeMethod(pane, "requestClose"));
    QObject *const dialog = childNamed(pane, QStringLiteral("viewerCloseUnsavedDialog"));
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->property("visible").toBool());
    QCOMPARE(closed.size(), 0);

    // A save already in flight can complete while the dialog is open. A newer
    // host navigation is then allowed, but must retire the old confirmation.
    controller.setDirty(false);
    QTRY_VERIFY(!pane->property("contentDirty").toBool());
    QVariant accepted;
    QVERIFY(QMetaObject::invokeMethod(pane, "openTarget", Q_RETURN_ARG(QVariant, accepted),
                                      Q_ARG(QVariant, QStringLiteral("file:///repo/new.txt")),
                                      Q_ARG(QVariant, QString())));
    QVERIFY(accepted.toBool());
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QCOMPARE(pane->property("url").toUrl(), QUrl(QStringLiteral("file:///repo/new.txt")));
    QCOMPARE(closed.size(), 0);

    QVERIFY(QMetaObject::invokeMethod(pane, "requestClose"));
    QCOMPARE(closed.size(), 1);
}

void TstPaneIdentity::dirtyEditorRejectsNavigation()
{
    EditorControllerStub controller;
    QSignalSpy warnings(m_engine.get(), &QQmlEngine::warnings);
    QVERIFY(warnings.isValid());
    m_viewers.setDefaultKinds({{QStringLiteral("md"), QStringLiteral("text")}});
    const auto restoreDefaults = qScopeGuard([this] { m_viewers.setDefaultKinds({}); });
    m_app.setActiveSessionRepoRoot(QStringLiteral("/srv/repos/app"));
    const QUrl originalUrl(QStringLiteral("file:///tmp/unsaved.md"));
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1"), originalUrl.toString()),
                       /*terminal=*/false));
    QTRY_COMPARE(collect(m_region, isLeafPane).size(), 1);
    QTRY_COMPARE(collect(m_region, isEditorPane).size(), 1);
    QObject *const pane = collect(m_region, isLeafPane).constFirst();
    QObject *const editor = collect(m_region, isEditorPane).constFirst();
    auto *const address = qobject_cast<QQuickItem *>(
        childNamed(pane, QStringLiteral("viewerAddressField")));
    QVERIFY(address);
    // Loader completion precedes the first window layout. Wait for the real
    // address field to be clickable before exercising its mouse/Return path.
    QTRY_VERIFY(address->width() > 4 && address->height() > 4);
    editor->setProperty("controller", QVariant::fromValue<QObject *>(&controller));
    controller.setDirty(true);
    QTRY_VERIFY(pane->property("contentDirty").toBool());
    m_viewers.setDefaultKinds({});
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("text"));
    QCOMPARE(collect(m_region, isEditorPane).constFirst(), editor);
    pane->setProperty("navigationHistory",
                      QVariantList{QStringLiteral("file:///before.bin"),
                                   originalUrl.toString(),
                                   QStringLiteral("file:///after.bin")});
    pane->setProperty("navigationIndex", 1);

    for (const QString &state : {QStringLiteral("modified"), QStringLiteral("saving"),
                                 QStringLiteral("conflict"), QStringLiteral("disconnected"),
                                 QStringLiteral("error"), QStringLiteral("read_only")}) {
        controller.setFileState(state);
        QTRY_VERIFY(pane->property("contentDirty").toBool());
        for (const char *action : {"goBack", "goForward", "goHome"}) {
            QVERIFY(QMetaObject::invokeMethod(pane, action));
            QCOMPARE(pane->property("url").toUrl(), originalUrl);
            QCOMPARE(pane->property("navigationIndex").toInt(), 1);
        }
        QVERIFY(QMetaObject::invokeMethod(pane, "openRemotePath",
                                          Q_ARG(QVariant, QStringLiteral("/next.bin"))));
        QVERIFY(QMetaObject::invokeMethod(pane, "openWebNavigation",
                                          Q_ARG(QVariant, QStringLiteral("https://example.com"))));
        QVariant accepted;
        QVERIFY(QMetaObject::invokeMethod(pane, "openTarget", Q_RETURN_ARG(QVariant, accepted),
                                          Q_ARG(QVariant, QStringLiteral("file:///next.bin")),
                                          Q_ARG(QVariant, QString())));
        QVERIFY(!accepted.toBool());
        QVERIFY(QMetaObject::invokeMethod(pane, "openUrlWithKind",
                                          Q_RETURN_ARG(QVariant, accepted),
                                          Q_ARG(QVariant, QStringLiteral("file:///next.bin")),
                                          Q_ARG(QVariant, QStringLiteral("binary"))));
        QVERIFY(!accepted.toBool());
        enterAddress(pane, QStringLiteral("https://example.com"));
        QCOMPARE(pane->property("url").toUrl(), originalUrl);
        QCOMPARE(pane->property("navigationIndex").toInt(), 1);
        QVERIFY(controller.dirty());
        QCOMPARE(pane->property("forcedKind").toString(), QString());
        QCOMPARE(collect(m_region, isEditorPane).constFirst(), editor);
        QVERIFY(pane->property("contentActive").toBool());
    }

    controller.setDirty(false);
    QTRY_VERIFY(!pane->property("contentDirty").toBool());
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("markdown"));
    QVariant accepted;
    QVERIFY(QMetaObject::invokeMethod(pane, "openTarget", Q_RETURN_ARG(QVariant, accepted),
                                      Q_ARG(QVariant, QStringLiteral("file:///next.bin")),
                                      Q_ARG(QVariant, QString())));
    QVERIFY(accepted.toBool());
    QTRY_COMPARE(pane->property("url").toUrl(), QUrl(QStringLiteral("file:///next.bin")));
    QVERIFY2(warnings.isEmpty(), "dirty-editor replacement emitted QML warnings");
}

void TstPaneIdentity::supersededAddressProbesCannotNavigate()
{
    const QUrl initial(QStringLiteral("file:///tmp/current.bin"));
    const QUrl newer(QStringLiteral("file:///tmp/newer.bin"));
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1"), initial.toString()),
                       /*terminal=*/false));
    QTRY_COMPARE(collect(m_region, isLeafPane).size(), 1);
    QObject *const pane = collect(m_region, isLeafPane).constFirst();
    const QString oldProbe = QStringLiteral("/tmp/old");
    pane->setProperty("probePath", oldProbe);
    QVariant accepted;
    QVERIFY(QMetaObject::invokeMethod(pane, "openTarget", Q_RETURN_ARG(QVariant, accepted),
                                      Q_ARG(QVariant, newer.toString()),
                                      Q_ARG(QVariant, QString())));
    QVERIFY(accepted.toBool());
    QCOMPARE(pane->property("probePath").toString(), QString());
    emit m_viewers.directoryListed(oldProbe, {});
    emit m_viewers.directoryError(oldProbe, QStringLiteral("late failure"));
    QCOMPARE(pane->property("url").toUrl(), newer);

    // Same-URL navigation must retire the probe too: urlChanged cannot do it.
    pane->setProperty("probePath", oldProbe);
    QVERIFY(QMetaObject::invokeMethod(pane, "openTarget", Q_RETURN_ARG(QVariant, accepted),
                                      Q_ARG(QVariant, newer.toString()),
                                      Q_ARG(QVariant, QString())));
    QVERIFY(accepted.toBool());
    QCOMPARE(pane->property("probePath").toString(), QString());
    emit m_viewers.directoryListed(oldProbe, {});
    QCOMPARE(pane->property("url").toUrl(), newer);

    // A direct host URL publication is another newer intent.
    pane->setProperty("probePath", oldProbe);
    pane->setProperty("url", initial);
    QCOMPARE(pane->property("probePath").toString(), QString());
    emit m_viewers.directoryError(oldProbe, QStringLiteral("late failure"));
    QCOMPARE(pane->property("url").toUrl(), initial);
}

void TstPaneIdentity::liveTerminalDiagnosticsRemainVisibleWithoutDisconnecting()
{
    QObject *const pane = openInShell(
        QStringLiteral("TerminalPaneView.qml"),
        {{QStringLiteral("factory"), QVariant::fromValue<QObject *>(nullptr)}});
    QVERIFY(pane);
    QQmlComponent bridgeComponent(m_engine.get());
    bridgeComponent.setData(R"QML(
        import QtQuick
        QtObject {
            property string connectionState: "ready"
            signal geometryChanged()
            function notifyViewVisible(visible) {}
        }
    )QML", QUrl());
    QVERIFY2(bridgeComponent.isReady(), qPrintable(bridgeComponent.errorString()));
    std::unique_ptr<QObject> bridge(bridgeComponent.create());
    QVERIFY(bridge);
    pane->setProperty("bridge", QVariant::fromValue(bridge.get()));
    pane->setProperty("attached", true);
    pane->setProperty("pageLoaded", true);
    pane->setProperty("statusText", QString());
    QTRY_VERIFY(pane->property("live").toBool());

    auto *banner = qobject_cast<QQuickItem *>(
        childNamed(pane, QStringLiteral("terminalStatusBanner")));
    QObject *const label = childNamed(pane, QStringLiteral("paneBannerLabel"));
    QObject *const dismiss = childNamed(pane, QStringLiteral("terminalDiagnosticDismissButton"));
    QObject *const retry = childNamed(pane, QStringLiteral("terminalRetryButton"));
    QVERIFY(banner && label && dismiss && retry);
    QVERIFY(!banner->isVisible());
    const QString warning = QStringLiteral(
        "Terminal input could not be sent completely. Part of it may have reached "
        "the remote program. Inspect the terminal before retrying.");
    pane->setProperty("statusText", warning);
    QTRY_VERIFY(banner->isVisible());
    QCOMPARE(label->property("text").toString(), warning);
    QVERIFY(banner->height() > 22);
    QVERIFY(dismiss->property("visible").toBool());
    QVERIFY(!retry->property("visible").toBool());
    QVERIFY(pane->property("live").toBool());
    QVERIFY(QMetaObject::invokeMethod(dismiss, "clicked"));
    QTRY_VERIFY(!banner->isVisible());
    QVERIFY(pane->property("live").toBool());
    QVERIFY(pane->property("attached").toBool());
}


void TstPaneIdentity::homeUsesTheActiveSessionRootAndDisablesWithoutOne()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const paneWithoutSession = panes().constFirst();
    QTRY_VERIFY(paneWithoutSession->property("navigationReady").toBool());

    QObject *const homeWithoutSession =
        childNamed(paneWithoutSession, QStringLiteral("viewerHomeButton"));
    QVERIFY(homeWithoutSession);
    QVERIFY(!paneWithoutSession->property("canGoHome").toBool());
    QVERIFY(!homeWithoutSession->property("enabled").toBool());
    QCOMPARE(homeWithoutSession->property("focusPolicy").toInt() & Qt::TabFocus, 0);

    // Rebuild the fixture so the new pane starts at the active Dev Session's
    // repository root, exactly the same location Home is required to select.
    cleanup();
    m_app.setActiveSessionRepoRoot(QStringLiteral("/srv/repos/app"));
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();
    QTRY_VERIFY(pane->property("navigationReady").toBool());

    QObject *const home = childNamed(pane, QStringLiteral("viewerHomeButton"));
    QVERIFY(home);
    QTRY_VERIFY(pane->property("canGoHome").toBool());
    QVERIFY(home->property("enabled").toBool());
    QVERIFY((home->property("focusPolicy").toInt() & Qt::TabFocus) != 0);
    QCOMPARE(pane->property("effectiveUrl").toUrl(),
             QUrl(QStringLiteral("file:///srv/repos/app/")));
    // An untouched pane follows a newly selected session's root. Its editable
    // address field must follow too, or it advertises the directory the pane
    // has already left.
    m_app.setActiveSessionRepoRoot(QStringLiteral("/srv/repos/other"));
    QTRY_COMPARE(pane->property("effectiveUrl").toUrl(),
                 QUrl(QStringLiteral("file:///srv/repos/other/")));
    QObject *const address = childNamed(pane, QStringLiteral("viewerAddressField"));
    QVERIFY(address);
    QTRY_COMPARE(address->property("text").toString(),
                 QStringLiteral("/srv/repos/other/"));

    pane->setProperty("url", QUrl(QStringLiteral("file:///tmp/away.bin")));
    QTRY_COMPARE(pane->property("url").toUrl(),
                 QUrl(QStringLiteral("file:///tmp/away.bin")));
    QVERIFY(QMetaObject::invokeMethod(pane, "goHome"));
    QTRY_COMPARE(pane->property("url").toUrl(), QUrl());
    QCOMPARE(pane->property("effectiveUrl").toUrl(),
             QUrl(QStringLiteral("file:///srv/repos/other/")));
}

// The address field's VALUE is the path the pane is showing, and it changes
// with every navigation. A screen reader given only that announces a path and
// never says what the control is — so the field needs a NAME, fixed, and one
// that is not simply the address read out twice.
void TstPaneIdentity::theAddressFieldSaysWhatItIsFor()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1"),
                                QStringLiteral("file:///srv/repos/app/README.md")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);

    QObject *const field =
        childNamed(panes().constFirst(), QStringLiteral("viewerAddressField"));
    QVERIFY2(field != nullptr, "the viewer pane has no address field");

    const QString name = accessibleNameOf(field);
    QVERIFY2(!name.isEmpty(),
             "the address field has no accessible name, so it announces the path it happens to "
             "hold and never says what it is for");
    QVERIFY2(name != field->property("text").toString(),
             "the address field's accessible name is just its current value, which is the same "
             "as having no name at all");
    // The placeholder is not a name: it disappears the moment the field holds
    // anything, which is almost always.
    QVERIFY2(name != field->property("placeholderText").toString(),
             "the address field is named by its placeholder, which is gone whenever the pane "
             "has something open");
}


// ---------------------------------------------------------------------------
// (9b) SPEC 9: "Paths outside the repository root are allowed, but the UI
// should indicate that the file is outside the project." The allowing half has
// always worked. This is the indicating half — the pane asking, and the marker
// following the pane across navigations.
// ---------------------------------------------------------------------------
void TstPaneIdentity::theViewerPaneMarksAPathOutsideTheRepositoryRoot()
{
    m_app.setActiveSessionRepoRoot(QStringLiteral("/srv/repos/app"));
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);
    QObject *const pane = panes().constFirst();

    QObject *const marker = childNamed(pane, QStringLiteral("viewerOutsideRepoMarker"));
    QVERIFY2(marker, "the viewer pane carries no out-of-project marker");

    // The remote client here is never connected, so every real file.resolvePath
    // fails and the pane is left with NO answer. That is the honest unknown
    // case and it must show nothing rather than guess either way.
    QCOMPARE(pane->property("repoRootState").toString(), QString());
    QVERIFY2(!marker->property("visible").toBool(),
             "an undetermined path is marked as outside the project");

    // A file inside the session's repository root. Extensionless on purpose:
    // it resolves to the lightweight binary view instead of spinning up a
    // Monaco page this test has no interest in.
    const QString insidePath = QStringLiteral("/srv/repos/app/notes");
    const QString base = QStringLiteral("/srv/repos/app");
    pane->setProperty("url", QUrl(QStringLiteral("file:///srv/repos/app/notes")));
    QTRY_COMPARE(pane->property("repoCheckPath").toString(), insidePath);
    emit m_viewers.pathResolved(insidePath, base, insidePath, /*insideRepositoryRoot=*/true);
    QCOMPARE(pane->property("repoRootState").toString(), QStringLiteral("inside"));
    QVERIFY2(!marker->property("visible").toBool(),
             "a file inside the repository root is marked as outside it");

    // Navigating to a file in nobody's project. The pane opens it — SPEC 9
    // requires that — and the previous answer is dropped the moment the address
    // changes, so the marker can never describe the file before this one.
    const QString outsidePath = QStringLiteral("/etc/hosts");
    pane->setProperty("url", QUrl(QStringLiteral("file:///etc/hosts")));
    QTRY_COMPARE(pane->property("repoCheckPath").toString(), outsidePath);
    QCOMPARE(pane->property("repoRootState").toString(), QString());
    QVERIFY(!marker->property("visible").toBool());

    emit m_viewers.pathResolved(outsidePath, base, outsidePath, /*insideRepositoryRoot=*/false);
    QCOMPARE(pane->property("repoRootState").toString(), QStringLiteral("outside"));
    QVERIFY2(marker->property("visible").toBool(),
             "an out-of-project file gets exactly the chrome of an in-project one");
    // Colour and three small words reach nobody using a screen reader.
    QVERIFY2(!accessibleNameOf(marker).isEmpty(), "the marker has no accessible name");
    // ...and it has to be on screen, not a zero-width item that reports
    // `visible` while drawing nothing.
    auto *const markerItem = qobject_cast<QQuickItem *>(marker);
    QVERIFY(markerItem);
    QVERIFY2(markerItem->width() > 8 && markerItem->height() > 8,
             qPrintable(QStringLiteral("the marker has no area: %1x%2")
                            .arg(markerItem->width())
                            .arg(markerItem->height())));

    // ONE ViewerModel serves every pane, so a reply about a path this pane is
    // no longer showing must not relabel it.
    emit m_viewers.pathResolved(insidePath, base, insidePath, /*insideRepositoryRoot=*/true);
    QCOMPARE(pane->property("repoRootState").toString(), QStringLiteral("outside"));

    // ...and back in again.
    pane->setProperty("url", QUrl(QStringLiteral("file:///srv/repos/app/notes")));
    QTRY_COMPARE(pane->property("repoCheckPath").toString(), insidePath);
    emit m_viewers.pathResolved(insidePath, base, insidePath, /*insideRepositoryRoot=*/true);
    QCOMPARE(pane->property("repoRootState").toString(), QStringLiteral("inside"));
    QVERIFY(!marker->property("visible").toBool());

    // Keeping the same file while switching repository roots must not accept
    // the old root's late answer, or erase the current marker on its error.
    const QString otherBase = QStringLiteral("/srv/other");
    m_app.setActiveSessionRepoRoot(otherBase);
    QTRY_COMPARE(pane->property("repoCheckBase").toString(), otherBase);
    QCOMPARE(pane->property("repoRootState").toString(), QString());
    emit m_viewers.pathResolved(insidePath, base, insidePath, true);
    QCOMPARE(pane->property("repoRootState").toString(), QString());
    QVERIFY(!marker->property("visible").toBool());
    emit m_viewers.pathResolved(insidePath, otherBase, insidePath, false);
    QCOMPARE(pane->property("repoRootState").toString(), QStringLiteral("outside"));
    QVERIFY(marker->property("visible").toBool());
    emit m_viewers.pathResolveError(insidePath, base, QStringLiteral("late failure"));
    QCOMPARE(pane->property("repoRootState").toString(), QStringLiteral("outside"));
    QVERIFY(marker->property("visible").toBool());

    // A web page is not "outside the repository root": nothing is asked and
    // nothing is shown.
    pane->setProperty("url", QUrl(QStringLiteral("https://example.com/docs")));
    QTRY_COMPARE(pane->property("repoCheckPath").toString(), QString());
    QCOMPARE(pane->property("repoRootState").toString(), QString());
    QVERIFY2(!marker->property("visible").toBool(),
             "a web page is marked as outside the project");

    // A pane showing nothing at all, with no Dev Session behind it either.
    m_app.setActiveSessionRepoRoot(QString());
    pane->setProperty("url", QUrl());
    QTRY_COMPARE(pane->property("repoCheckPath").toString(), QString());
    QCOMPARE(pane->property("repoRootState").toString(), QString());
    QVERIFY(!marker->property("visible").toBool());
}

// ---------------------------------------------------------------------------
// (10) The default terminal layout: two panes, one above the other, present
// from the very first frame. That is the case the one-shot sizing latch gets
// wrong most easily — SplitView stretches only its FIRST fill item, so a latch
// that never fires leaves the second pane at a Loader's implicit size of zero,
// and a zero-height terminal is invisible AND unclickable while the region still
// reports two panes with the right ids.
// ---------------------------------------------------------------------------
void TstPaneIdentity::twoStackedTerminalPanesComeUpFromTheFirstFrame()
{
    // Exactly Main.qml's terminal fallback tree: terminal-1 above terminal-2.
    QVERIFY(openRegion(QStringLiteral("TerminalRegion.qml"),
                       branchNode(QStringLiteral("vertical"),
                                  QVariantList{leafNode(QStringLiteral("terminal-1")),
                                               leafNode(QStringLiteral("terminal-2"))}),
                       /*terminal=*/true));

    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY2(panes().size() == 2,
                 qPrintable(QStringLiteral("the default two-pane layout produced %1 panes: %2")
                                .arg(panes().size())
                                .arg(describePanes(panes()))));
    QTest::qWait(kSettleMs);

    const QList<QObject *> splitViews = collect(m_region, isRegionSplitView);
    QCOMPARE(splitViews.size(), 1);
    QVERIFY2(splitViews.constFirst()->property("ratiosApplied").toBool(),
             "the one-shot sizing latch never fired for a region that started with two panes, so "
             "the second pane has no preferred size");

    auto *top = qobject_cast<QQuickItem *>(paneWithId(m_region, QStringLiteral("terminal-1")));
    auto *bottom = qobject_cast<QQuickItem *>(paneWithId(m_region, QStringLiteral("terminal-2")));
    QVERIFY(top && bottom);

    // Both are real panes with area, not one pane and one sliver.
    for (QQuickItem *pane : {top, bottom}) {
        QVERIFY2(pane->width() > 4 && pane->height() > 4,
                 qPrintable(QStringLiteral("pane \"%1\" came up %2x%3")
                                .arg(pane->property("paneId").toString())
                                .arg(pane->width())
                                .arg(pane->height())));
    }

    // One UP and one DOWN, in the order the tree names them.
    const QPointF topAt = top->mapToItem(nullptr, QPointF(0, 0));
    const QPointF bottomAt = bottom->mapToItem(nullptr, QPointF(0, 0));
    QVERIFY2(topAt.y() + top->height() <= bottomAt.y() + 1,
             qPrintable(QStringLiteral("terminal-1 is not above terminal-2: %1 vs %2")
                            .arg(topAt.y())
                            .arg(bottomAt.y())));

    // Equal shares, modulo the drag handle SplitView takes out of one of them.
    // An unapplied latch fails this by the whole region height, not by a pixel.
    QVERIFY2(qAbs(top->height() - bottom->height()) <= 12,
             qPrintable(QStringLiteral("the two default panes are %1 and %2 high, which is not "
                                       "an even split")
                            .arg(top->height())
                            .arg(bottom->height())));

    // Each pane drives its OWN terminal. Two panes sharing a controller would
    // show the same shell twice and close one PTY when either went away.
    QObject *const topController = asObject(top->property("controller"));
    QObject *const bottomController = asObject(bottom->property("controller"));
    QVERIFY2(topController != nullptr && bottomController != nullptr,
             "a default pane never minted its controller, so it can never attach a shell");
    QVERIFY2(topController != bottomController,
             "both default panes share one TerminalController");
    QCOMPARE(top->property("terminalId").toString(), QStringLiteral("terminal-1"));
    QCOMPARE(bottom->property("terminalId").toString(), QStringLiteral("terminal-2"));

    // And both are reachable: a pane that is present but unclickable cannot be
    // the target of a split or close.
    clickPane(bottom);
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("terminal-2"));
    clickPane(top);
    QTRY_COMPARE(m_region->property("focusedPaneId").toString(), QStringLiteral("terminal-1"));
}

// ---------------------------------------------------------------------------
// (11) Split ratios, the WRITE side.
//
// A region already RESTORES the ratios stored in its node (ratioFor + the
// one-shot sizing latch), so a reopened Dev Session came back with the
// proportions the server held. What was missing is the other half: nothing told
// the host when a DRAG changed them, so ch::SessionLayouts::setRatios() was
// never called from anywhere and every divider the user moved inside a region
// snapped back to the stored fractions on the next reopen.
//
// The reading has to name the branch it belongs to as an index path from the
// root of the region tree — that is setRatios()'s addressing — and a nested
// region is the case that can get it wrong while a single-level one still looks
// perfect. So the tree here is deliberately two levels deep, and the inner
// branch's own report has to arrive on the ROOT region carrying ["1"].
//
// publishRatios() is invoked directly rather than by synthesising a drag: what
// is under test is the arithmetic and the path, and SplitView only sets
// `resizing` for a real pointer grab on a handle.
// ---------------------------------------------------------------------------
void TstPaneIdentity::dragAdjustedRatiosAreReportedForTheRightBranch()
{
    // [ viewer-1 | [ viewer-2 / viewer-3 ] ]
    const QVariantMap tree = branchNode(
        QStringLiteral("horizontal"),
        QVariantList{leafNode(QStringLiteral("viewer-1")),
                     branchNode(QStringLiteral("vertical"),
                                QVariantList{leafNode(QStringLiteral("viewer-2")),
                                             leafNode(QStringLiteral("viewer-3"))})});
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), tree, /*terminal=*/false));

    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY2(panes().size() == 3,
                 qPrintable(QStringLiteral("expected 3 panes, found %1: %2")
                                .arg(panes().size())
                                .arg(describePanes(panes()))));
    QTest::qWait(kSettleMs);

    // Every region in the tree knows where it sits. A leaf region's path is what
    // its PARENT branch would report under, so all five are checked at once.
    const QList<QObject *> regions = collect(m_region, [](QObject *object) {
        return object->metaObject()->indexOfProperty("nodePath") >= 0;
    });
    QStringList paths;
    for (QObject *region : regions)
        paths.append(pathText(asList(region->property("nodePath"))));
    paths.sort();
    // Sorted on both sides: the comparison is about the SET of paths, and
    // QStringList::sort() orders by code unit, so "[0]" sorts before "[]"
    // (']' is 0x5D, '0' is 0x30). Writing the literal in reading order and
    // sorting it too keeps the expectation legible without depending on that.
    QStringList expectedPaths{QStringLiteral("[]"), QStringLiteral("[0]"),
                              QStringLiteral("[1]"), QStringLiteral("[1,0]"),
                              QStringLiteral("[1,1]")};
    expectedPaths.sort();
    QCOMPARE(paths, expectedPaths);

    QSignalSpy reported(m_region, SIGNAL(splitRatiosAdjusted(QVariant, QVariant)));
    QVERIFY(reported.isValid());

    // Two branches, so two SplitViews: the root one and the nested one.
    const QList<QObject *> splits = collect(m_region, isRegionSplitView);
    QCOMPARE(splits.size(), 2);
    for (QObject *split : splits) {
        QVERIFY2(QMetaObject::invokeMethod(split, "publishRatios"),
                 "the region's SplitView has no publishRatios(), so a drag inside a region "
                 "cannot be reported to the host");
    }
    QTRY_COMPARE(reported.size(), 2);

    // Both readings land on the ROOT region (a nested one relays through its
    // owner), each naming its own branch and carrying one fraction per child.
    QStringList reportedPaths;
    for (const QList<QVariant> &emission : reported) {
        const QVariantList path = asList(emission.at(0));
        const QVariantList ratios = asList(emission.at(1));
        reportedPaths.append(pathText(path));

        QCOMPARE(ratios.size(), 2);
        double sum = 0;
        for (const QVariant &ratio : ratios) {
            const double value = ratio.toDouble();
            // ch::SessionLayouts::setRatios() rejects anything not strictly
            // positive, and an even two-way split is what this tree starts at.
            QVERIFY2(value > 0.2 && value < 0.8,
                     qPrintable(QStringLiteral("%1 reported the implausible ratio %2")
                                    .arg(pathText(path))
                                    .arg(value)));
            sum += value;
        }
        QVERIFY2(qAbs(sum - 1.0) < 0.001,
                 qPrintable(QStringLiteral("%1 reported ratios summing to %2, not 1")
                                .arg(pathText(path))
                                .arg(sum)));
    }
    reportedPaths.sort();
    QStringList expectedReported{QStringLiteral("[]"), QStringLiteral("[1]")};
    expectedReported.sort();
    QCOMPARE(reportedPaths, expectedReported);
}
void TstPaneIdentity::reorderedNestedBranchesUpdateRatioPaths()
{
    const QVariantMap nested = branchNode(
        QStringLiteral("vertical"),
        QVariantList{leafNode(QStringLiteral("viewer-2")),
                             leafNode(QStringLiteral("viewer-3"))});
    const QVariantMap initial = branchNode(
        QStringLiteral("horizontal"),
        QVariantList{leafNode(QStringLiteral("viewer-1")), nested});
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), initial, /*terminal=*/false));

    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 3);
    QTest::qWait(kSettleMs);

    // Keep both child counts unchanged while moving the nested branch from
    // index 1 to index 0. Repeater delegates therefore survive and must update
    // their index paths rather than retaining the addresses they were created
    // with.
    const QVariantMap reordered = branchNode(
        QStringLiteral("horizontal"),
        QVariantList{nested, leafNode(QStringLiteral("viewer-1"))});
    setNode(reordered);
    QTRY_VERIFY2(collect(m_region, [](QObject *object) {
                     return object->metaObject()->indexOfProperty("nodePath") >= 0;
                 }).size() == 5,
                 "the reordered tree did not retain all recursive regions");

    const QList<QObject *> regions = collect(m_region, [](QObject *object) {
        return object->metaObject()->indexOfProperty("nodePath") >= 0;
    });
    QStringList paths;
    for (QObject *region : regions)
        paths.append(pathText(asList(region->property("nodePath"))));
    paths.sort();
    QStringList expected{QStringLiteral("[]"), QStringLiteral("[0]"),
                         QStringLiteral("[0,0]"), QStringLiteral("[0,1]"),
                         QStringLiteral("[1]")};
    expected.sort();
    QCOMPARE(paths, expected);
}


// ---------------------------------------------------------------------------
// (11) The address bar after a RELATIVE name.
//
// A name with no leading "/" is resolved against the directory the pane is
// showing, so the pane opens something the user did not literally type. The
// field keeps the keyboard after Enter, so the rule "never overwrite what the
// user is typing" used to leave the fragment sitting there: the bar said
// "notes.md" while the pane showed /srv/repos/app/notes.md, and the next Enter
// would then resolve that fragment against the NEW directory.
// ---------------------------------------------------------------------------
void TstPaneIdentity::theAddressBarShowsWhatARelativeNameResolvedTo()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QTest::qWait(kSettleMs);
    QObject *const pane = panes().constFirst();

    // A trailing slash is already spelled as a directory, so it opens without a
    // probe and becomes the directory a relative name is measured from.
    enterAddress(pane, QStringLiteral("/srv/repos/app/"));
    QTRY_COMPARE(pane->property("url").toUrl(), QUrl(QStringLiteral("file:///srv/repos/app/")));

    enterAddress(pane, QStringLiteral("notes.md"));
    QTRY_COMPARE(pane->property("url").toUrl(),
                 QUrl(QStringLiteral("file:///srv/repos/app/notes.md")));

    QObject *const field = childNamed(pane, QStringLiteral("viewerAddressField"));
    QVERIFY(field);
    QVERIFY2(field->property("activeFocus").toBool(),
             "the address field lost the keyboard, so this no longer exercises the case it "
             "is about");
    QTRY_COMPARE(field->property("text").toString(),
                 QStringLiteral("/srv/repos/app/notes.md"));
}

// ---------------------------------------------------------------------------
// (12) A terminal pane must not adopt the answer to a question it no longer
// has.
//
// The factory suppresses answers cancelled by detach or supersession. QML must
// start the replacement immediately: waiting for the cancelled answer would
// strand the pane, because that signal is deliberately never delivered.
// ---------------------------------------------------------------------------
void TstPaneIdentity::aTerminalPaneNeverAdoptsTheAnswerToASupersededLookup()
{
    TerminalFactoryStub factory;
    QObject *const pane = openInShell(
        QStringLiteral("TerminalPaneView.qml"),
        {{QStringLiteral("factory"), QVariant::fromValue<QObject *>(&factory)},
         {QStringLiteral("paneId"), QStringLiteral("terminal-1")},
         {QStringLiteral("devSessionId"), QStringLiteral("dev-a")},
         {QStringLiteral("terminalPaneId"), QStringLiteral("row-a")},
         {QStringLiteral("workingDir"), QStringLiteral("/srv/a")}});
    QVERIFY(pane);

    QTRY_COMPARE(factory.resolutions().size(), 1);
    QCOMPARE(factory.resolutions().constLast().devSessionId, QStringLiteral("dev-a"));
    QVERIFY2(pane->property("resolving").toBool(),
             "the pane is not waiting on the lookup it just started");
    QVERIFY(factory.controller());

    // The user picks another Dev Session while that lookup is still in flight.
    // The host pushes the pair working-directory first, exactly as Main.qml does.
    pane->setProperty("workingDir", QStringLiteral("/srv/b"));
    pane->setProperty("devSessionId", QStringLiteral("dev-b"));
    pane->setProperty("terminalPaneId", QStringLiteral("row-b"));
    QTRY_COMPARE(factory.resolutions().size(), 3);
    QCOMPARE(factory.resolutions().constLast().devSessionId, QStringLiteral("dev-b"));
    QCOMPARE(factory.resolutions().constLast().terminalPaneId, QStringLiteral("row-b"));
    QCOMPARE(factory.resolutions().constLast().workingDir, QStringLiteral("/srv/b"));
    QVERIFY(pane->property("resolving").toBool());

    // Both the abandoned Dev Session and the intermediate identity can finish
    // late. Neither delivers a signal or attaches a PTY to the wrong shell.
    QSignalSpy answered(&factory, &TerminalFactoryStub::targetResolved);
    QVERIFY(!factory.completeResolution(0, QStringLiteral("ch-dev-a-terminal-1")));
    QVERIFY(!factory.completeResolution(1, QStringLiteral("ch-dev-b-old-row")));
    QCOMPARE(answered.size(), 0);
    QVERIFY(pane->property("tmuxTarget").toString().isEmpty());
    QVERIFY(factory.attached().isEmpty());
    QVERIFY(factory.completeResolution(2, QStringLiteral("ch-dev-b-terminal-1")));
    QTRY_COMPARE(pane->property("tmuxTarget").toString(), QStringLiteral("ch-dev-b-terminal-1"));
    QCOMPARE(factory.attached(), QStringList{QStringLiteral("ch-dev-b-terminal-1")});
}

void TstPaneIdentity::detachedTerminalLookupCanBeRestarted()
{
    TerminalFactoryStub factory;
    QObject *const pane = openInShell(
        QStringLiteral("TerminalPaneView.qml"),
        {{QStringLiteral("factory"), QVariant::fromValue<QObject *>(&factory)},
         {QStringLiteral("paneId"), QStringLiteral("terminal-1")},
         {QStringLiteral("devSessionId"), QStringLiteral("dev-a")},
         {QStringLiteral("terminalPaneId"), QStringLiteral("row-a")}});
    QVERIFY(pane);
    QTRY_COMPARE(factory.resolutions().size(), 1);
    QVERIFY(pane->property("resolving").toBool());
    QVERIFY(QMetaObject::invokeMethod(pane, "detachNow"));
    QVERIFY(!pane->property("resolving").toBool());
    QVERIFY(!factory.completeResolution(0, QStringLiteral("cancelled-target")));
    QVERIFY(factory.attached().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(pane, "reconnectNow"));
    QTRY_COMPARE(factory.resolutions().size(), 2);
    QVERIFY(factory.completeResolution(1, QStringLiteral("current-target")));
    QTRY_COMPARE(factory.attached(), QStringList{QStringLiteral("current-target")});
    QVERIFY(!pane->property("resolving").toBool());
}

void TstPaneIdentity::terminalLookupWaitsForWorkspaceReadiness()
{
    TerminalFactoryStub factory;
    factory.setWorkspaceReady(false);
    QObject *const pane = openInShell(
        QStringLiteral("TerminalPaneView.qml"),
        {{QStringLiteral("factory"), QVariant::fromValue<QObject *>(&factory)},
         {QStringLiteral("paneId"), QStringLiteral("terminal-1")},
         {QStringLiteral("devSessionId"), QStringLiteral("dev-a")},
         {QStringLiteral("terminalPaneId"), QStringLiteral("row-a")}});
    QVERIFY(pane);
    QVERIFY(factory.resolutions().isEmpty());
    QVERIFY(!pane->property("resolving").toBool());
    factory.setWorkspaceReady(true);
    QTRY_COMPARE(factory.resolutions().size(), 1);
    QVERIFY(pane->property("resolving").toBool());

    // The transport can retain the same peer id after reconnect. Retiring its
    // readiness cancels the old answer without leaving a permanent QML wait.
    factory.setWorkspaceReady(false);
    QVERIFY(!pane->property("resolving").toBool());
    QVERIFY(!factory.completeResolution(0, QStringLiteral("old-target")));
    factory.setConnected(false);
    factory.setWorkspaceReady(true);
    QCOMPARE(factory.resolutions().size(), 1);
    factory.setWorkspaceReady(false);
    factory.setConnected(true);
    pane->setProperty("visible", false);
    factory.setWorkspaceReady(true);
    QCOMPARE(factory.resolutions().size(), 1);
    pane->setProperty("visible", true);
    QTRY_COMPARE(factory.resolutions().size(), 2);
    QVERIFY(factory.completeResolution(1, QStringLiteral("current-target")));
    QTRY_COMPARE(factory.attached(), QStringList{QStringLiteral("current-target")});
    QVERIFY(!pane->property("resolving").toBool());
}

// ---------------------------------------------------------------------------
// (12b) The pane a SPLIT creates. It is published with no `terminal_panes` row
// — ch::SessionLayouts mints one behind it and republishes the tree carrying
// the id — so the whole of its identity arrives on a LATER republish than the
// one that put it on screen. A pane that does not pick the id up off that
// second tree never resolves, never attaches and never renders: it is the
// blank, untypeable half of a split.
// ---------------------------------------------------------------------------
namespace {

// A terminal leaf as ch::SessionLayouts publishes one: the slot label plus the
// server row that IS the pane's identity. An empty `rowId` is the leaf a split
// publishes before its mint has landed.
QVariantMap terminalLeafNode(const QString &paneId, const QString &rowId)
{
    QVariantMap leaf = leafNode(paneId);
    leaf.insert(QStringLiteral("terminalPaneId"), rowId);
    return leaf;
}

} // namespace

void TstPaneIdentity::aSplitPaneAttachesOnceItsRowIdArrives()
{
    // The stub replaces the real factory for this test only: the shared one is
    // backed by an unconnected pool, which refuses every attach before the
    // identity path is reached at all.
    TerminalFactoryStub factory;
    QSignalSpy warnings(m_engine.get(), &QQmlEngine::warnings);
    QVERIFY(warnings.isValid());
    m_engine->rootContext()->setContextProperty(QStringLiteral("terminalFactory"), &factory);
    const auto restore = qScopeGuard([this, &warnings] {
        // Retire stub-backed panes before restoring the real factory. Otherwise
        // onFactoryChanged offers their plain QObject controllers to the real
        // createBridge(TerminalController*), raising a QML TypeError.
        cleanup();
        m_engine->rootContext()->setContextProperty(QStringLiteral("terminalFactory"),
                                                    &m_terminalFactory);
        QVERIFY2(warnings.isEmpty(), "terminal factory fixture lifecycle raised QML warnings");
    });

    // The Dev Session default: two stacked panes, both already bound to rows.
    QVERIFY(openRegion(
        QStringLiteral("TerminalRegion.qml"),
        branchNode(QStringLiteral("vertical"),
                   QVariantList{terminalLeafNode(QStringLiteral("terminal-1"),
                                                 QStringLiteral("row-1")),
                                terminalLeafNode(QStringLiteral("terminal-2"),
                                                 QStringLiteral("row-2"))}),
        /*terminal=*/true));

    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_COMPARE(panes().size(), 2);
    QTRY_COMPARE(factory.resolutions().size(), 2);

    // The user splits terminal-2. Its leaf becomes a branch holding itself and
    // a brand new leaf whose row is still being minted.
    setNode(branchNode(
        QStringLiteral("vertical"),
        QVariantList{terminalLeafNode(QStringLiteral("terminal-1"), QStringLiteral("row-1")),
                     branchNode(QStringLiteral("horizontal"),
                                QVariantList{terminalLeafNode(QStringLiteral("terminal-2"),
                                                              QStringLiteral("row-2")),
                                             terminalLeafNode(QStringLiteral("terminal-3"),
                                                              QString())})}));
    QTRY_VERIFY2(panes().size() == 3,
                 qPrintable(QStringLiteral("the split never produced 3 panes: %1")
                                .arg(describePanes(panes()))));
    QObject *const fresh = paneWithId(m_region, QStringLiteral("terminal-3"));
    QVERIFY(fresh);
    QTest::qWait(kSettleMs);
    // Waiting, deliberately: resolving by the recyclable slot label instead is
    // how a new pane used to be handed a closed pane's still-running shell.
    QCOMPARE(factory.resolutions().size(), 2);

    // The mint lands and the tree is republished with the id on the new leaf.
    setNode(branchNode(
        QStringLiteral("vertical"),
        QVariantList{terminalLeafNode(QStringLiteral("terminal-1"), QStringLiteral("row-1")),
                     branchNode(QStringLiteral("horizontal"),
                                QVariantList{terminalLeafNode(QStringLiteral("terminal-2"),
                                                              QStringLiteral("row-2")),
                                             terminalLeafNode(QStringLiteral("terminal-3"),
                                                              QStringLiteral("row-3"))})}));

    QTRY_VERIFY2(fresh->property("terminalPaneId").toString() == QStringLiteral("row-3"),
                 qPrintable(QStringLiteral("the republished row id never reached the new pane; "
                                           "it still holds \"%1\"")
                                .arg(fresh->property("terminalPaneId").toString())));
    QTRY_VERIFY2(factory.resolutions().size() == 3,
                 "the pane the split created never asked the server for its shell, so it stays "
                 "blank and cannot be typed into");
    QCOMPARE(factory.resolutions().constLast().terminalPaneId, QStringLiteral("row-3"));

    // ...and the answer brings the PTY up.
    QObject *const controller = asObject(fresh->property("controller"));
    QVERIFY(controller);
    emit factory.targetResolved(controller, QStringLiteral("ch-dev-identity-terminal-3"));
    QTRY_COMPARE(fresh->property("tmuxTarget").toString(),
                 QStringLiteral("ch-dev-identity-terminal-3"));
    QVERIFY2(factory.attached().contains(QStringLiteral("ch-dev-identity-terminal-3")),
             "the split pane resolved its shell and then never attached a PTY to it");
}

namespace {

// The editor pane, driven by a stub controller and with NO page: the offer and
// the restore are decisions the QML pane makes, and loading Monaco would turn
// this into a Chromium test for no extra coverage. The pane's navigate() reads
// an empty bundle url as "go nowhere", which is what keeps the view inert.
QVariantMap recoveryPaneProps(QObject *controller)
{
    return {{QStringLiteral("controller"), QVariant::fromValue(controller)},
            {QStringLiteral("editorBundleUrl"), QString()},
            {QStringLiteral("recoveryServerId"), QStringLiteral("server-recovery")},
            {QStringLiteral("recoverySessionId"), QStringLiteral("dev-recovery")},
            {QStringLiteral("recoveryPaneId"), QStringLiteral("viewer-1")},
            {QStringLiteral("fileUrl"), QStringLiteral("file:///srv/repos/app/notes.md")}};
}

} // namespace

// ---------------------------------------------------------------------------
// (13) SPEC 11.3: unsaved work survives a crash, and the user is offered it.
//
// ch::EditorController already does the finding — it snapshots the buffer while
// the user types and compares the snapshot with the file on the next open — and
// announces the difference as `recoveryAvailable`. Nothing listened, so the
// recovered buffer was read back off the server and dropped, once per file
// open, and the user was never told their work had survived.
// ---------------------------------------------------------------------------
void TstPaneIdentity::theEditorOffersToRestoreARecoveredBuffer()
{
    EditorControllerStub controller;
    QSignalSpy delivered(&controller, &EditorControllerStub::contentLoaded);
    QVERIFY(delivered.isValid());

    QObject *const pane =
        openInShell(QStringLiteral("EditorPaneView.qml"), recoveryPaneProps(&controller));
    QVERIFY(pane);
    QTRY_COMPARE(controller.opened(), QStringList{QStringLiteral("/srv/repos/app/notes.md")});

    QObject *const dialog = childNamed(pane, QStringLiteral("editorRecoveryDialog"));
    QVERIFY2(dialog, "the editor pane never offers a recovered buffer, so unsaved work found on "
                     "the server is read back and thrown away");
    QVERIFY(!dialog->property("visible").toBool());

    // The snapshot is found before the page has taken the file...
    emit controller.recoveryAvailable(QStringLiteral("recovered line\n"));
    QTest::qWait(80);
    QVERIFY2(!dialog->property("visible").toBool(),
             "the offer went up before the editor page held the file: restoring then pushes the "
             "recovered text at a page that cannot take it, losing the very work being offered");

    // ...and the load completes, which is when the offer can safely be acted on.
    emit controller.contentLoaded(QStringLiteral("what is on disk\n"), QStringLiteral("rev-7"));
    QTRY_VERIFY2(dialog->property("visible").toBool(),
                 "unsaved work was recovered and the user was never told");
    QCOMPARE(delivered.size(), 1);

    QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
    QTRY_COMPARE(delivered.size(), 2);
    // The recovered text goes into the page...
    QCOMPARE(delivered.constLast().at(0).toString(), QStringLiteral("recovered line\n"));
    // ...guarded by the revision the page already holds. An empty guard is a
    // create-only write on the server, i.e. a save that would be refused.
    QCOMPARE(delivered.constLast().at(1).toString(), QStringLiteral("rev-7"));
    // ...and the host is told the buffer no longer matches the server, so an
    // external change cannot silently reload over the restored work (SPEC 8.7)
    // and the snapshot survives until the user saves.
    QCOMPARE(controller.reported(), QStringList{QStringLiteral("recovered line\n")});
    QTRY_VERIFY(!dialog->property("visible").toBool());
}
void TstPaneIdentity::theEditorOffersToRestoreAnEmptyRecoveredBuffer()
{
    EditorControllerStub controller;
    QSignalSpy delivered(&controller, &EditorControllerStub::contentLoaded);
    QVERIFY(delivered.isValid());

    QObject *const pane =
        openInShell(QStringLiteral("EditorPaneView.qml"), recoveryPaneProps(&controller));
    QVERIFY(pane);
    QObject *const dialog = childNamed(pane, QStringLiteral("editorRecoveryDialog"));
    QVERIFY(dialog);

    // Empty recovered content and an empty revision are both valid: the user
    // may have deleted every character from a new file. Neither value may be
    // used as the "nothing is pending" sentinel.
    emit controller.recoveryAvailable(QString());
    QTest::qWait(80);
    QVERIFY(!dialog->property("visible").toBool());

    emit controller.contentLoaded(QStringLiteral("what is on disk\n"), QString());
    QTRY_VERIFY2(dialog->property("visible").toBool(),
                 "an empty recovered buffer was treated as no recovery");
    QCOMPARE(delivered.size(), 1);

    QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
    QTRY_COMPARE(delivered.size(), 2);
    QCOMPARE(delivered.constLast().at(0).toString(), QString());
    QCOMPARE(delivered.constLast().at(1).toString(), QString());
    QCOMPARE(controller.reported(), QStringList{QString()});
}


void TstPaneIdentity::discardingARecoveredBufferLeavesTheEditorShowingTheFile()
{
    EditorControllerStub controller;
    QSignalSpy delivered(&controller, &EditorControllerStub::contentLoaded);
    QVERIFY(delivered.isValid());

    QObject *const pane =
        openInShell(QStringLiteral("EditorPaneView.qml"), recoveryPaneProps(&controller));
    QVERIFY(pane);

    QObject *const dialog = childNamed(pane, QStringLiteral("editorRecoveryDialog"));
    QVERIFY(dialog);
    emit controller.contentLoaded(QStringLiteral("what is on disk\n"), QStringLiteral("rev-7"));
    emit controller.recoveryAvailable(QStringLiteral("recovered line\n"));
    QTRY_VERIFY(dialog->property("visible").toBool());
    QCOMPARE(delivered.size(), 1);

    // Exactly what the Discard button does.
    QVERIFY(QMetaObject::invokeMethod(dialog, "discarded"));
    QTRY_VERIFY2(!dialog->property("visible").toBool(),
                 "answering the recovery offer left it on screen");
    QCOMPARE(delivered.size(), 1);
    QVERIFY2(controller.reported().isEmpty(),
             "discarding the recovered buffer still marked the file dirty");
}

// ---------------------------------------------------------------------------
void TstPaneIdentity::theViewerPaneStopsWaitingForAProbeThatNeverAnswers()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();

    QObject *const watchdog = childNamed(pane, QStringLiteral("viewerProbeWatchdog"));
    QVERIFY2(watchdog,
             "nothing bounds the path probe: a reply that never arrives leaves the pane "
             "busy on an address for ever");
    QVERIFY2(!watchdog->property("running").toBool(),
             "the probe watchdog is armed with no probe outstanding");

    // Twenty seconds is the shipped budget; what is under test is what happens
    // when it runs out, not how long it is.
    watchdog->setProperty("interval", 60);

    const QList<QObject *> headers = collect(pane, isPaneHeader);
    QCOMPARE(headers.size(), 1);
    QObject *const header = headers.constFirst();

    // A probe the server never answers. Written straight into the pane's own
    // published record of what is outstanding rather than typed into the
    // address bar: this fixture's client has no transport, so a real
    // listDirectory fails before the test could observe the wait at all — and
    // it is the WAIT, not the request, that was unbounded.
    pane->setProperty("probePath", QStringLiteral("/srv/repos/app/thing"));
    QVERIFY2(watchdog->property("running").toBool(),
             "an outstanding probe does not arm the watchdog");
    QVERIFY2(header->property("busy").toBool(),
             "the pane does not show the probe as in flight");

    QTRY_VERIFY2(pane->property("probePath").toString().isEmpty(),
                 "the pane sat out its own watchdog and stayed busy");
    QVERIFY2(!watchdog->property("running").toBool(), "the watchdog re-armed itself after firing");
    QVERIFY2(!header->property("busy").toBool(), "the pane is still drawn busy after giving up");
    // Giving up OPENS the path, as a file — the same answer a directoryError
    // produces. A pane that silently forgot the question would be no better
    // than one that waited for ever.
    QCOMPARE(pane->property("url").toUrl(), QUrl(QStringLiteral("file:///srv/repos/app/thing")));

    // A probe that settles normally disarms the watchdog instead of firing
    // later over whatever the pane has moved on to.
    pane->setProperty("probePath", QStringLiteral("/srv/repos/app/other"));
    QVERIFY(watchdog->property("running").toBool());
    pane->setProperty("probePath", QString());
    QVERIFY2(!watchdog->property("running").toBool(), "a settled probe leaves its watchdog armed");
}

// ---------------------------------------------------------------------------
void TstPaneIdentity::theDirectoryListingTakesOnlyTheAnswerToItsOwnRequest()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();

    const QString path = QStringLiteral("/srv/repos/app/");
    pane->setProperty("url", QUrl(QStringLiteral("file:///srv/repos/app/")));
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("directory"));

    QList<QObject *> views;
    QTRY_VERIFY((views = collect(pane, isDirectoryView)).size() == 1);
    QObject *const view = views.constFirst();

    QVERIFY2(view->metaObject()->indexOfProperty("requestedPath") >= 0,
             "the directory view keeps no record of what it asked for, so it matches replies "
             "against whatever its URL happens to be when one arrives");

    // This fixture's client has no transport, so the view's own listDirectory
    // fails immediately. That leaves a SETTLED view: it asked, it was answered,
    // and it has nothing outstanding.
    QTRY_VERIFY(!view->property("errorText").toString().isEmpty());
    QVERIFY(!view->property("loading").toBool());
    QCOMPARE(view->property("requestedPath").toString(), QString());
    const QString settled = view->property("errorText").toString();

    const QVariantList entries{
        QVariantMap{{QStringLiteral("name"), QStringLiteral("README.md")},
                    {QStringLiteral("kind"), QStringLiteral("file")}}};

    // Another pane listing the same directory. ONE ViewerModel serves them all,
    // so its reply arrives here too — and it answers nothing this view asked,
    // so it must not replace what is on screen.
    emit m_viewers.directoryListed(path, entries);
    QCOMPARE(view->property("entries").toList().size(), 0);

    // Nor may somebody else's failure be painted over a listing this view is
    // perfectly happy with.
    emit m_viewers.directoryError(path, QStringLiteral("permission denied"));
    QCOMPARE(view->property("errorText").toString(), settled);

    // The other direction: a reply the view IS waiting for is taken, and one
    // for a different directory is not. `requestedPath` stands in for a request
    // this transport-less fixture cannot leave outstanding on its own.
    view->setProperty("requestedPath", path);
    view->setProperty("loading", true);
    emit m_viewers.directoryListed(QStringLiteral("/somewhere/else/"), entries);
    QCOMPARE(view->property("entries").toList().size(), 0);
    QVERIFY2(view->property("loading").toBool(),
             "a listing of another directory settled this view");

    emit m_viewers.directoryListed(path, entries);
    QCOMPARE(view->property("entries").toList().size(), 1);
    QVERIFY2(!view->property("loading").toBool(), "the view's own answer did not settle it");
    QCOMPARE(view->property("requestedPath").toString(), QString());
}

// ---------------------------------------------------------------------------
void TstPaneIdentity::theViewsThatHoldAnInternalAddressPinItUntilTheyLetItGo()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();

    ch::InternalUrlMap &map = ch::InternalUrlMap::shared();
    const int pinnedBefore = map.retainedCount();

    // --- the image view, which pins for as long as it is displaying ---------
    pane->setProperty("url", QUrl(QStringLiteral("file:///srv/repos/app/logo.png")));
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("image"));

    QList<QObject *> views;
    QTRY_VERIFY((views = collect(pane, isPinningView)).size() == 1);
    QObject *const image = views.constFirst();

    QTRY_VERIFY(!image->property("internalUrl").toUrl().toString().isEmpty());
    const QString firstAddress = image->property("internalUrl").toUrl().toString();
    QVERIFY2(image->property("retained").toBool(),
             "the image view does not pin the address it is showing, so it ages out from "
             "under a pane that is still displaying it");
    QCOMPARE(map.retainedCount(), pinnedBefore + 1);

    // Navigating to another image releases the old address and pins the new
    // one: exactly one release per retain, so the count does not drift.
    pane->setProperty("url", QUrl(QStringLiteral("file:///srv/repos/app/other.png")));
    QTRY_VERIFY(image->property("internalUrl").toUrl().toString() != firstAddress);
    QVERIFY(image->property("retained").toBool());
    QCOMPARE(map.retainedCount(), pinnedBefore + 1);

    // Swapping the handler out destroys the view. Without a release on
    // destruction every image ever opened stays pinned for the life of the
    // process, which is the bound the table exists to enforce.
    pane->setProperty("url", QUrl(QStringLiteral("https://example.com/")));
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("web"));
    QTRY_COMPARE(map.retainedCount(), pinnedBefore);

    // --- the download view, the third holder of an internal address ---------
    pane->setProperty("url", QUrl(QStringLiteral("file:///srv/repos/app/archive.tar.gz")));
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("binary"));

    QList<QObject *> buttons;
    QTRY_VERIFY((buttons = collect(pane, isDownloadButton)).size() == 1);
    QObject *const download = buttons.constFirst();

    QCOMPARE(map.retainedCount(), pinnedBefore);
    QVERIFY(QMetaObject::invokeMethod(download, "clicked"));

    QList<QObject *> binaries;
    QTRY_VERIFY((binaries = collect(pane, [](QObject *object) {
                     return object->metaObject()->indexOfProperty("pendingDownloadUrl") >= 0;
                 })).size() == 1);
    QObject *const binary = binaries.constFirst();
    QVERIFY2(binary->property("retained").toBool(),
             "the address a download is fetching is not pinned, so a busy session can evict "
             "it between the click and the profile's callback");
    QCOMPARE(map.retainedCount(), pinnedBefore + 1);

    // Closing the view with a download still pending must not leak the pin.
    pane->setProperty("url", QUrl(QStringLiteral("https://example.com/")));
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("web"));
    QTRY_COMPARE(map.retainedCount(), pinnedBefore);
}

// ---------------------------------------------------------------------------
void TstPaneIdentity::aRefusedInternalResourceSaysWhyInsteadOfShowingABlankPage()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();

    pane->setProperty("url", QUrl(QStringLiteral("file:///srv/repos/app/manual.pdf")));
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("pdf"));

    QList<QObject *> views;
    QTRY_VERIFY((views = collect(pane, isPinningView)).size() == 1);
    QObject *const view = views.constFirst();
    QTRY_VERIFY(!view->property("internalUrl").toUrl().toString().isEmpty());
    const QUrl shown = view->property("internalUrl").toUrl();

    QObject *const status = childNamed(view, QStringLiteral("pdfStatus"));
    QVERIFY2(status, "the PDF view has nowhere to say a resource was refused");
    QVERIFY(!status->property("visible").toBool());

    // A refusal about something else entirely. The model is shared, so it
    // arrives here too and must be ignored — the same rule the pane applies to
    // its own replies.
    emit m_viewers.internalResourceError(
        QUrl(QStringLiteral("codeharbor-internal://file/not-this-one")),
        QStringLiteral("someone else's problem"));
    QVERIFY2(view->property("errorText").toString().isEmpty(),
             "a refusal about another pane's resource was shown here");
    QVERIFY(!status->property("visible").toBool());

    const QString why =
        QStringLiteral("this file is 42 MB, over the 8 MB limit for an inline read");
    emit m_viewers.internalResourceError(shown, why);
    QCOMPARE(view->property("errorText").toString(), why);
    QTRY_VERIFY2(status->property("visible").toBool(),
                 "the refusal never reached the screen");
    QVERIFY2(status->property("text").toString().contains(why),
             qPrintable(status->property("text").toString()));
}

// ---------------------------------------------------------------------------
// An explicit layout RELOAD (SPEC 4.5) advances SessionLayouts.paneGeneration
// as well as the request epoch. A reconnect refresh deliberately does not, so
// unchanged documents retain their live buffers.
//
// Dropping them is only half of it. Each region in the recursive tree keeps its
// own note of which pane it is showing, and that note survives the pane. The
// host binds node and paneGeneration to the same facade, so an explicit reload
// of an IDENTICAL tree moves its document generation — no node change
// re-runs the leaves — and every region went on believing a destroyed pane was
// on screen. The layout looked right in every property and the column was
// empty.
void TstPaneIdentity::aLayoutReloadRebuildsEveryPaneInTheTree()
{
    QVERIFY(openRegion(
        QStringLiteral("ViewerRegion.qml"),
        branchNode(QStringLiteral("horizontal"),
                   QVariantList{leafNode(QStringLiteral("viewer-1")),
                                branchNode(QStringLiteral("vertical"),
                                           QVariantList{leafNode(QStringLiteral("viewer-2")),
                                                        leafNode(QStringLiteral("viewer-3"))})}),
        /*terminal=*/false));

    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 3);
    QTest::qWait(kSettleMs);

    const QList<QObject *> live = panes();
    QList<QPointer<QObject>> before;
    before.reserve(live.size());
    for (QObject *pane : live)
        before.append(QPointer<QObject>(pane));

    // An explicit reload advances both authority and document generations
    // while retaining the same stored tree.
    m_region->setProperty("layoutGeneration", 1.0);
    m_region->setProperty("paneGeneration", 1.0);
    QTest::qWait(kSettleMs);

    const QList<QObject *> after = panes();
    QVERIFY2(after.size() == 3,
             qPrintable(QStringLiteral("the reload left %1 pane(s) in a 3-leaf tree; the region "
                                       "is showing nothing where a pane should be: %2")
                            .arg(after.size())
                            .arg(describePanes(after))));

    // Every one of them is a NEW object: the old ones belonged to the previous
    // load and were destroyed with the cache.
    for (const QPointer<QObject> &old : before) {
        QVERIFY2(old.isNull(),
                 "a pane from the previous layout load survived into the reloaded tree");
    }

    QStringList ids;
    for (QObject *pane : after)
        ids.append(pane->property("paneId").toString());
    ids.sort();
    QCOMPARE(ids, (QStringList{QStringLiteral("viewer-1"), QStringLiteral("viewer-2"),
                               QStringLiteral("viewer-3")}));

    // And they are real panes, not zero-extent placeholders left behind by a
    // sizing pass that never re-ran. QTRY on the first one: the rebuilt panes
    // are re-parented before the region's next layout pass gives them their
    // share, so a bare read here measures the gap rather than the result.
    auto *const firstItem = qobject_cast<QQuickItem *>(after.constFirst());
    QVERIFY(firstItem);
    QTRY_VERIFY2(firstItem->width() > 4 && firstItem->height() > 4,
                 qPrintable(QStringLiteral("pane \"%1\" came back %2x%3 after the reload")
                                .arg(after.constFirst()->property("paneId").toString())
                                .arg(firstItem->width())
                                .arg(firstItem->height())));
    for (QObject *pane : after) {
        auto *item = qobject_cast<QQuickItem *>(pane);
        QVERIFY(item);
        QVERIFY2(item->width() > 4 && item->height() > 4,
                 qPrintable(QStringLiteral("pane \"%1\" came back %2x%3 after the reload")
                                .arg(pane->property("paneId").toString())
                                .arg(item->width())
                                .arg(item->height())));
    }
}

void TstPaneIdentity::reconnectRefreshPreservesDirtyEditorAndRetiresOldIntents()
{
    ReadFileServer server;
    QVERIFY(server.bind(&m_client));
    const QString path = QStringLiteral("/repo/reconnect.txt");
    const QString address = QStringLiteral("file:///repo/reconnect.txt");
    server.setFile(path, QStringLiteral("RECONNECT_SERVER_BASELINE"));
    server.setLayout(QStringLiteral("viewer"),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("split")},
                    {QStringLiteral("orientation"), QStringLiteral("horizontal")},
                    {QStringLiteral("children"),
                     QJsonArray{
                         QJsonObject{{QStringLiteral("type"), QStringLiteral("leaf")},
                                     {QStringLiteral("paneId"), QStringLiteral("viewer-1")},
                                     {QStringLiteral("url"), address}},
                         QJsonObject{{QStringLiteral("type"), QStringLiteral("leaf")},
                                     {QStringLiteral("paneId"), QStringLiteral("viewer-2")},
                                     {QStringLiteral("url"), QStringLiteral("file:///repo/other.bin")}}}},
                    {QStringLiteral("ratios"), QJsonArray{0.5, 0.5}}});
    server.setLayout(QStringLiteral("terminal"),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("leaf")},
                    {QStringLiteral("paneId"), QString()}});
    QTemporaryDir settings;
    QVERIFY(settings.isValid());
    ch::UiStateStore uiState(settings.filePath(QStringLiteral("ui.ini")));
    ch::WorkspaceDb workspace(&m_client);
    ch::SessionLayouts layouts(&workspace, &uiState);
    const auto retireShell = qScopeGuard([this] { cleanup(); });
    layouts.setServerId(QStringLiteral("server-reconnect"));
    QSignalSpy loaded(&layouts, &ch::SessionLayouts::loaded);
    layouts.load(QStringLiteral("dev-reconnect"));
    QTRY_COMPARE(loaded.size(), 1);
    std::unique_ptr<QObject> monacoProbe = installMonacoProbe();
    QVERIFY(monacoProbe);
    const auto restoreScripts = qScopeGuard([&monacoProbe] {
        QMetaObject::invokeMethod(monacoProbe.get(), "restore");
    });
    m_region = openInShell(
        QStringLiteral("ViewerRegion.qml"),
        {{QStringLiteral("node"), layouts.viewerTree()},
         {QStringLiteral("serverId"), layouts.serverId()},
         {QStringLiteral("acceptedServerId"), layouts.serverId()},
         {QStringLiteral("sessionId"), layouts.devSessionId()},
         {QStringLiteral("layoutGeneration"), double(layouts.generation())},
         {QStringLiteral("paneGeneration"), double(layouts.paneGeneration())},
         {QStringLiteral("hostStampsWrites"), true}});
    QVERIFY(m_region);
    QVERIFY(QMetaObject::invokeMethod(m_shell.get(), "bindViewerLayouts",
                                      Q_ARG(QVariant, QVariant::fromValue<QObject *>(&layouts))));
    QTRY_COMPARE(collect(m_region, isEditorPane).size(), 1);
    QObject *const editor = collect(m_region, isEditorPane).constFirst();
    QObject *const pane = paneWithId(m_region, QStringLiteral("viewer-1"));
    QVERIFY(pane);
    auto *const controller =
        qobject_cast<ch::EditorController *>(asObject(editor->property("controller")));
    QVERIFY(controller);
    const QString capture = QStringLiteral(
        "typeof window.captureEditorContent === 'function'"
        " ? window.captureEditorContent() : null");
    QVERIFY(waitForJs(editor, capture, QStringLiteral("RECONNECT_SERVER_BASELINE"),
                      kPageLoadTimeoutMs));
    const QString text = evalJs(editor, QStringLiteral(
        "(function(){var e=window.monaco.editor.getEditors()[0];"
        "var m=e.getModel(); var last=m.getLineCount();"
        "e.setPosition({lineNumber:last,column:m.getLineMaxColumn(last)});"
        "e.trigger('keyboard','type',{text:'DIRTY_RECONNECT_RAM'});"
        "window.__reconnectSurvivor='CURRENT_DOCUMENT';"
        "return window.captureEditorContent();})()"));
    QVERIFY(text.contains(QStringLiteral("DIRTY_RECONNECT_RAM")));
    QTRY_VERIFY(controller->dirty());
    controller->setRecoveryDir(QStringLiteral("/owned-fixture/reconnect"));
    QVERIFY(controller->recoveryEnabled());
    controller->reportContent(text);
    QTRY_VERIFY(!server.fileWrites().isEmpty());
    const QJsonObject scopeRecord = QJsonDocument::fromJson(
        server.fileWrites().constLast().value(QStringLiteral("content")).toString().toUtf8()).object();
    QCOMPARE(scopeRecord.value(QStringLiteral("serverId")).toString(), layouts.serverId());
    QCOMPARE(scopeRecord.value(QStringLiteral("devSessionId")).toString(), layouts.devSessionId());
    QCOMPARE(scopeRecord.value(QStringLiteral("paneId")).toString(), QStringLiteral("viewer-1"));
    QCOMPARE(scopeRecord.value(QStringLiteral("path")).toString(), path);
    // Losing current wire authority retires recovery scope, not the live
    // document. Reaccepting this server must likewise preserve its dirty RAM.
    m_region->setProperty("acceptedServerId", QString());
    QCOMPARE(editor->property("recoveryServerId").toString(), QString());
    QVERIFY(!controller->recoveryEnabled());
    QVERIFY(editor->property("recoveryUnavailable").toBool());
    QCOMPARE(collect(m_region, isEditorPane).constFirst(), editor);
    QCOMPARE(evalJs(editor, capture), text);
    QVERIFY(controller->dirty());
    m_region->setProperty("acceptedServerId", layouts.serverId());
    QCOMPARE(editor->property("recoveryServerId").toString(), layouts.serverId());
    QVERIFY(controller->recoveryEnabled());
    QVERIFY(!editor->property("recoveryUnavailable").toBool());
    QCOMPARE(collect(m_region, isEditorPane).constFirst(), editor);
    QCOMPARE(evalJs(editor, capture), text);
    QVERIFY(controller->dirty());
    const QString revision = controller->revision();
    const int fileReads = server.readCount(path);
    const int layoutReads = server.layoutReadCount();
    const quint64 oldGeneration = layouts.generation();
    const quint64 documentGeneration = layouts.paneGeneration();
    QPointer<QObject> preservedEditor(editor);
    QPointer<QObject> preservedPane(pane);
    const QList<QObject *> handles = collect(m_region, [](QObject *object) {
        return object->metaObject()->indexOfProperty("keyStep") >= 0;
    });
    QCOMPARE(handles.size(), 1);
    QObject *const handle = handles.constFirst();
    QSignalSpy ratios(m_region,
                      SIGNAL(splitRatiosAdjustedForSession(QString,double,QVariant,QVariant)));
    QVERIFY(ratios.isValid());
    pane->setProperty("probePath", QStringLiteral("/repo/retired-probe"));
    m_region->setProperty("pendingOpenTargets",
        QVariantMap{{QStringLiteral("queued-pane"),
                     QVariantMap{{QStringLiteral("url"), QStringLiteral("file:///retired.txt")},
                                 {QStringLiteral("kind"), QStringLiteral("text")}}}});
    QCOMPARE(asMap(m_region->property("pendingOpenTargets")).size(), 1);
    QVERIFY(QMetaObject::invokeMethod(handle, "moveBy", Q_ARG(QVariant, 16)));
    QVERIFY(QMetaObject::invokeMethod(editor, "reload")); // Async capture still belongs to old epoch.
    layouts.refresh();
    QCOMPARE(layouts.paneGeneration(), documentGeneration);
    QVERIFY(layouts.generation() > oldGeneration);
    QTRY_COMPARE(loaded.size(), 2);
    QCOMPARE(server.layoutReadCount(), layoutReads + 2);
    QTest::qWait(kSettleMs);
    QCOMPARE(ratios.size(), 0);
    QVERIFY(!handle->property("resizePending").toBool());
    QCOMPARE(pane->property("probePath").toString(), QString());
    QVERIFY(pane->property("url").toUrl() == QUrl(address));
    QVERIFY(asMap(m_region->property("pendingOpenTargets")).isEmpty());
    QCOMPARE(preservedEditor.data(), collect(m_region, isEditorPane).constFirst());
    QCOMPARE(preservedPane.data(), paneWithId(m_region, QStringLiteral("viewer-1")));
    QCOMPARE(asObject(editor->property("controller")), controller);
    QCOMPARE(evalJs(editor, capture), text);
    QCOMPARE(evalJs(editor, QStringLiteral("window.__reconnectSurvivor")),
             QStringLiteral("CURRENT_DOCUMENT"));
    QVERIFY(controller->dirty());
    QCOMPARE(controller->revision(), revision);
    QCOMPARE(server.readCount(path), fileReads);
    QCOMPARE(server.layoutWriteCount(), 0);

    // An old captured callback stays fenced, while current pane reports have
    // advanced stamps and can persist against the accepted fresh layout.
    layouts.setPaneUrlForSession(QStringLiteral("dev-reconnect"), oldGeneration,
                                QStringLiteral("viewer"), QStringLiteral("viewer-1"),
                                QStringLiteral("file:///retired.txt"));
    QCOMPARE(server.layoutWriteCount(), 0);
    QVERIFY(QMetaObject::invokeMethod(handle, "moveBy", Q_ARG(QVariant, -16)));
    QTRY_COMPARE(ratios.size(), 1);
    QCOMPARE(ratios.constFirst().at(1).toDouble(), double(layouts.generation()));
    QTRY_COMPARE(server.layoutWriteCount(), 1);
    QCOMPARE(evalJs(editor, capture), text);

    // A deliberate same-session load is still a document reset, not refresh.
    layouts.load(QStringLiteral("dev-reconnect"));
    QTRY_COMPARE(loaded.size(), 3);
    QTRY_VERIFY(preservedEditor.isNull());
    QTRY_VERIFY(preservedPane.isNull());
    QTRY_COMPARE(collect(m_region, isEditorPane).size(), 1);
    QObject *const replacement = collect(m_region, isEditorPane).constFirst();
    QVERIFY(waitForJs(replacement, capture, QStringLiteral("RECONNECT_SERVER_BASELINE"),
                      kPageLoadTimeoutMs));
    auto *const replacementController =
        qobject_cast<ch::EditorController *>(asObject(replacement->property("controller")));
    QVERIFY(replacementController);
    QVERIFY(!replacementController->dirty());
    QVERIFY(server.readCount(path) > fileReads);
}

void TstPaneIdentity::editorRecoveryUsesScopedContextAndWarnsWhenUnavailable_data()
{
    QTest::addColumn<QByteArray>("missingProperty");
    QTest::addColumn<QString>("restoredIdentity");
    QTest::newRow("unaccepted-server") << QByteArray("recoveryServerId")
                                      << QStringLiteral("accepted-server");
    QTest::newRow("missing-session") << QByteArray("recoverySessionId")
                                    << QStringLiteral("session-a");
    QTest::newRow("missing-pane") << QByteArray("recoveryPaneId")
                                 << QStringLiteral("viewer-1");
}

void TstPaneIdentity::editorRecoveryUsesScopedContextAndWarnsWhenUnavailable()
{
    QFETCH(QByteArray, missingProperty);
    QFETCH(QString, restoredIdentity);
    ReadFileServer server;
    QVERIFY(server.bind(&m_client));
    const auto retireShell = qScopeGuard([this] { cleanup(); });
    server.setFile(QStringLiteral("/repo/scope-a.txt"), QStringLiteral("SAVED_A"));
    server.setFile(QStringLiteral("/repo/scope-b.txt"), QStringLiteral("SAVED_B"));
    QObject *const editor = openInShell(
        QStringLiteral("EditorPaneView.qml"),
        {{QStringLiteral("editorBundleUrl"), QString()},
         {QStringLiteral("recoveryServerId"), QStringLiteral("accepted-server")},
         {QStringLiteral("recoverySessionId"), QStringLiteral("session-a")},
         {QStringLiteral("recoveryPaneId"), QStringLiteral("viewer-1")},
         {QStringLiteral("fileUrl"), QUrl(QStringLiteral("file:///repo/scope-a.txt"))}});
    QVERIFY(editor);
    auto *const controller =
        qobject_cast<ch::EditorController *>(asObject(editor->property("controller")));
    QVERIFY(controller);
    controller->setRecoveryDir(QStringLiteral("/owned-fixture/recovery"));
    QTRY_COMPARE(controller->fileState(), QStringLiteral("clean"));
    QVERIFY(controller->recoveryEnabled());
    controller->reportContent(QStringLiteral("DIRTY_A"));
    QTRY_VERIFY(!server.fileWrites().isEmpty());
    const QJsonObject firstWrite = server.fileWrites().constLast();
    const QString firstTarget = firstWrite.value(QStringLiteral("path")).toString();
    QVERIFY(!firstTarget.isEmpty());
    const QJsonObject firstRecord = QJsonDocument::fromJson(
        firstWrite.value(QStringLiteral("content")).toString().toUtf8()).object();
    QCOMPARE(firstRecord.value(QStringLiteral("serverId")).toString(),
             QStringLiteral("accepted-server"));
    QCOMPARE(firstRecord.value(QStringLiteral("devSessionId")).toString(),
             QStringLiteral("session-a"));
    QCOMPARE(firstRecord.value(QStringLiteral("paneId")).toString(), QStringLiteral("viewer-1"));
    QCOMPARE(firstRecord.value(QStringLiteral("path")).toString(), QStringLiteral("/repo/scope-a.txt"));
    QCOMPARE(firstRecord.value(QStringLiteral("content")).toString(), QStringLiteral("DIRTY_A"));

    QObject *const banner = childNamed(editor, QStringLiteral("editorStateBanner"));
    QVERIFY(banner);
    editor->setProperty(missingProperty.constData(), QString());
    QTRY_VERIFY(!controller->recoveryEnabled());
    QVERIFY(controller->dirty());
    QTRY_VERIFY(editor->property("recoveryUnavailable").toBool());
    QTRY_VERIFY(banner->property("visible").toBool());
    QCOMPARE(asObject(editor->property("controller")), controller);
    const qsizetype writesBeforeUnaccepted = server.fileWrites().size();
    controller->reportContent(QStringLiteral("DIRTY_WITHOUT_AUTHORITY"));
    QTest::qWait(80);
    QCOMPARE(server.fileWrites().size(), writesBeforeUnaccepted);

    editor->setProperty(missingProperty.constData(), restoredIdentity);
    QTRY_VERIFY(controller->recoveryEnabled());
    QVERIFY(controller->dirty());
    QTRY_VERIFY(!editor->property("recoveryUnavailable").toBool());
    QTRY_VERIFY(!banner->property("visible").toBool());
    controller->reportContent(QStringLiteral("DIRTY_A_REACCEPTED"));
    QTRY_VERIFY(server.fileWrites().size() > writesBeforeUnaccepted);
    QCOMPARE(server.fileWrites().constLast().value(QStringLiteral("path")).toString(), firstTarget);
    const QString preservedFirstRecord = server.fileContent(firstTarget);

    // A different complete scope cannot adopt this still-loaded A buffer.
    const qsizetype writesBeforeSession = server.fileWrites().size();
    editor->setProperty("recoverySessionId", QStringLiteral("session-b"));
    QVERIFY(!controller->recoveryEnabled());
    QVERIFY(controller->dirty());
    QVERIFY(editor->property("recoveryUnavailable").toBool());
    controller->reportContent(QStringLiteral("STALE_SESSION_A_BUFFER"));
    QTest::qWait(80);
    QCOMPARE(server.fileWrites().size(), writesBeforeSession);
    QCOMPARE(server.fileContent(firstTarget), preservedFirstRecord);

    // Only an explicit new document open binds B's complete scope. It can use
    // the same file/pane labels without touching the earlier A snapshot.
    QVERIFY(QMetaObject::invokeMethod(editor, "start"));
    QTRY_COMPARE(controller->fileState(), QStringLiteral("clean"));
    QVERIFY(controller->recoveryEnabled());
    controller->reportContent(QStringLiteral("DIRTY_SESSION_B"));
    QTRY_VERIFY(server.fileWrites().size() > writesBeforeSession);
    const QJsonObject sessionWrite = server.fileWrites().constLast();
    const QString sessionTarget = sessionWrite.value(QStringLiteral("path")).toString();
    QVERIFY(sessionTarget != firstTarget);
    const QJsonObject sessionRecord = QJsonDocument::fromJson(
        sessionWrite.value(QStringLiteral("content")).toString().toUtf8()).object();
    QCOMPARE(sessionRecord.value(QStringLiteral("devSessionId")).toString(),
             QStringLiteral("session-b"));
    QCOMPARE(server.fileContent(firstTarget), preservedFirstRecord);
    const QString preservedSessionRecord = server.fileContent(sessionTarget);

    // File identity comes from the actual opened path, not a host-supplied hash.
    const qsizetype writesBeforeFile = server.fileWrites().size();
    editor->setProperty("fileUrl", QUrl(QStringLiteral("file:///repo/scope-b.txt")));
    QTRY_COMPARE(controller->fileState(), QStringLiteral("clean"));
    controller->reportContent(QStringLiteral("DIRTY_FILE_B"));
    QTRY_VERIFY(server.fileWrites().size() > writesBeforeFile);
    const QJsonObject fileWrite = server.fileWrites().constLast();
    const QString fileTarget = fileWrite.value(QStringLiteral("path")).toString();
    QVERIFY(fileTarget != firstTarget && fileTarget != sessionTarget);
    const QJsonObject fileRecord = QJsonDocument::fromJson(
        fileWrite.value(QStringLiteral("content")).toString().toUtf8()).object();
    QCOMPARE(fileRecord.value(QStringLiteral("path")).toString(), QStringLiteral("/repo/scope-b.txt"));
    QCOMPARE(server.fileContent(firstTarget), preservedFirstRecord);
    QCOMPARE(server.fileContent(sessionTarget), preservedSessionRecord);
}

// ---------------------------------------------------------------------------
// The editor's state banner (SPEC 8.4/8.6). "conflict" and "disconnected" are
// the two states in which the text on screen cannot be saved, and the editor
// page prints only a state word in its status line — far too quiet for either.
//
// The banner has to be DRAWN, not merely built: a Rectangle with no anchors and
// no height is 0x0 in the pane's top-left corner, and every property assertion
// about it passes while the user is shown nothing at all.
void TstPaneIdentity::theEditorSaysWhenItsFileCannotBeSaved()
{
    EditorControllerStub controller;
    QObject *const pane =
        openInShell(QStringLiteral("EditorPaneView.qml"), recoveryPaneProps(&controller));
    QVERIFY(pane);

    // openInShell() returns the instant the Loader has an item, which is BEFORE
    // the harness Window's first layout pass: at that moment the Loader has not
    // been given the window's size, so the pane root is 0x0 and every child
    // anchored to it is zero-width. Measuring there says nothing about this
    // pane — so wait for the pane itself to have area first, and say so
    // separately, or a harness that never lays out reads as a broken banner.
    auto *const paneItem = qobject_cast<QQuickItem *>(pane);
    QVERIFY(paneItem);
    QTest::qWait(kSettleMs);
    QTRY_VERIFY2(paneItem->width() > 100 && paneItem->height() > 100,
                 qPrintable(QStringLiteral("the harness never laid the pane out: %1x%2")
                                .arg(paneItem->width())
                                .arg(paneItem->height())));

    QObject *const banner = childNamed(pane, QStringLiteral("editorStateBanner"));
    QVERIFY2(banner, "the editor pane has nowhere to say that its file cannot be saved");
    auto *const bannerItem = qobject_cast<QQuickItem *>(banner);
    QVERIFY(bannerItem);

    // A file that is loading normally says nothing.
    QVERIFY(!banner->property("visible").toBool());

    controller.setFileState(QStringLiteral("disconnected"));
    QTRY_VERIFY2(banner->property("visible").toBool(),
                 "the connection dropped and the editor never said so");
    // Full width of the pane, not merely "some" width: the banner is anchored
    // left and right, so anything less means it is not anchored to the pane at
    // all. QTRY because a geometry change can be one event-loop turn behind the
    // property change that revealed it; the assertion itself is not weakened.
    QTRY_VERIFY2(qFuzzyCompare(bannerItem->width(), paneItem->width())
                     && bannerItem->height() > 8,
                 qPrintable(QStringLiteral("the banner is %1x%2 inside a %3-wide pane, so "
                                           "nothing is drawn however correctly `visible` behaves")
                                .arg(bannerItem->width())
                                .arg(bannerItem->height())
                                .arg(paneItem->width())));

    QObject *const label = childNamed(pane, QStringLiteral("editorStateBannerLabel"));
    QVERIFY(label);
    QVERIFY2(label->property("text").toString().contains(QStringLiteral("connection")),
             qPrintable(label->property("text").toString()));

    // A refused save is the other one, and it is the more dangerous of the two:
    // the user's edits are at risk rather than merely suspended.
    controller.setFileState(QStringLiteral("conflict"));
    QTRY_VERIFY(pane->property("conflicted").toBool());
    QVERIFY(banner->property("visible").toBool());
    QVERIFY2(label->property("text").toString().contains(QStringLiteral("changed on the server")),
             qPrintable(label->property("text").toString()));

    controller.setFileState(QStringLiteral("clean"));
    QTRY_VERIFY(!banner->property("visible").toBool());
}

// ---------------------------------------------------------------------------
// The directory listing is bounded, exactly like the viewer pane's path probe.
void TstPaneIdentity::theDirectoryListingStopsWaitingForAnAnswerThatNeverArrives()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();

    pane->setProperty("url", QUrl(QStringLiteral("file:///srv/repos/app/")));
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("directory"));

    QList<QObject *> views;
    QTRY_VERIFY((views = collect(pane, isDirectoryView)).size() == 1);
    QObject *const view = views.constFirst();

    QObject *const watchdog = childNamed(view, QStringLiteral("directoryListingWatchdog"));
    QVERIFY2(watchdog,
             "nothing bounds the directory listing: a reply that never arrives leaves the view "
             "saying \"Listing…\" and the pane's header busy for ever");

    // This fixture's client has no transport, so the view's own listDirectory
    // already failed. That leaves a SETTLED view with nothing outstanding.
    QTRY_VERIFY(!view->property("errorText").toString().isEmpty());
    QVERIFY2(!watchdog->property("running").toBool(),
             "the listing watchdog is armed with no listing outstanding");

    // Twenty seconds is the shipped budget; what is under test is what happens
    // when it runs out, not how long it is.
    watchdog->setProperty("interval", 60);

    const QList<QObject *> headers = collect(pane, isPaneHeader);
    QCOMPARE(headers.size(), 1);
    QObject *const header = headers.constFirst();

    // A listing the server never answers. Written straight into the view's own
    // published record of what is outstanding, for the reason the probe test
    // gives: this fixture cannot leave a real request in flight.
    view->setProperty("errorText", QString());
    view->setProperty("loading", true);
    view->setProperty("requestedPath", QStringLiteral("/srv/repos/app/"));
    QVERIFY2(watchdog->property("running").toBool(),
             "an outstanding listing does not arm the watchdog");
    QVERIFY2(header->property("busy").toBool(),
             "the pane does not show the listing as in flight");

    QTRY_VERIFY2(view->property("requestedPath").toString().isEmpty(),
                 "the view sat out its own watchdog and stayed loading");
    QVERIFY2(!view->property("loading").toBool(), "the view is still loading after giving up");
    QVERIFY2(!view->property("errorText").toString().isEmpty(),
             "the view gave up silently, so an empty listing and a lost reply look identical");
    QVERIFY2(!header->property("busy").toBool(), "the pane is still drawn busy after giving up");
    QVERIFY2(!watchdog->property("running").toBool(), "the watchdog re-armed itself after firing");

    // A listing that settles normally disarms the watchdog instead of firing
    // later over whatever the view has moved on to.
    view->setProperty("requestedPath", QStringLiteral("/srv/repos/app/sub/"));
    QVERIFY(watchdog->property("running").toBool());
    view->setProperty("requestedPath", QString());
    QVERIFY2(!watchdog->property("running").toBool(),
             "a settled listing leaves its watchdog armed");
}

// ---------------------------------------------------------------------------
// A download is bounded for the same reason. The Download button is the binary
// view's only affordance and it is disabled while a request is pending, so a
// request the handler never answers took the pane out of service permanently.
void TstPaneIdentity::theBinaryViewStopsWaitingForADownloadThatNeverStarts()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"), leafNode(QStringLiteral("viewer-1")),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();

    pane->setProperty("url", QUrl(QStringLiteral("file:///srv/repos/app/archive.tar.gz")));
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("binary"));

    QList<QObject *> views;
    QTRY_VERIFY((views = collect(pane, isBinaryView)).size() == 1);
    QObject *const view = views.constFirst();

    QObject *const watchdog = childNamed(view, QStringLiteral("binaryDownloadWatchdog"));
    QVERIFY2(watchdog,
             "nothing bounds a pending download: a request the handler never answers leaves the "
             "Download button disabled for the life of the pane");
    QVERIFY(!watchdog->property("running").toBool());
    watchdog->setProperty("interval", 60);

    QList<QObject *> buttons;
    QTRY_VERIFY((buttons = collect(pane, isDownloadButton)).size() == 1);
    QObject *const button = buttons.constFirst();
    QVERIFY(button->property("enabled").toBool());

    // A request nothing ever answers. Driven through the view's own published
    // record rather than the button, so the outcome is the watchdog's and not a
    // refusal this transport-less fixture would produce on its own.
    view->setProperty("downloadRequested", true);
    QVERIFY2(watchdog->property("running").toBool(),
             "a pending download does not arm the watchdog");
    QVERIFY2(!button->property("enabled").toBool(),
             "the Download button is still live while a request is pending");

    QTRY_VERIFY2(!view->property("downloadRequested").toBool(),
                 "the view sat out its own watchdog and stayed pending");
    QVERIFY2(!view->property("retained").toBool(), "giving up left the internal address pinned");
    QVERIFY2(!view->property("errorText").toString().isEmpty(),
             "the view gave up silently, so the user is shown a button that simply works again");
    QVERIFY2(button->property("enabled").toBool(),
             "the Download button never came back, so the pane cannot be retried");
    QVERIFY2(!watchdog->property("running").toBool(), "the watchdog re-armed itself after firing");
}

// ---------------------------------------------------------------------------
// The Markdown image path is resolved in TWO places, in two languages.
//
// The rendered page (src/web/markdown/src/renderer.ts) resolves its own link
// targets with isRelativeResource() / pathDirectory() / resolveRemotePath().
// The image bridge deliberately does not use the answer: it hands QML the RAW
// document-relative string, and ViewerMarkdownView resolves it against its own
// copy of the document path, because the address it produces is fed to a remote
// file read. QML cannot call into the bundle — it is JavaScript in a separate
// Chromium world reachable only through the frozen WebChannel contract.
//
// So the two implementations must agree, and nothing but a shared table can
// make them. This is that table on the QML side; renderer.test.ts pins the same
// cases on the other side, and a comment at each end points at the other.
void TstPaneIdentity::theMarkdownImagePathResolverAgreesWithTheRendererBundle_data()
{
    QTest::addColumn<QString>("documentPath");
    QTest::addColumn<QString>("reference");
    QTest::addColumn<QString>("expected");

    const QString doc = QStringLiteral("/srv/repos/app/docs/guide.md");

    // The ordinary cases: a sibling, an explicit "./", and a climb.
    QTest::newRow("sibling") << doc << QStringLiteral("diagram.png")
                             << QStringLiteral("/srv/repos/app/docs/diagram.png");
    QTest::newRow("dot-sibling") << doc << QStringLiteral("./diagram.png")
                                 << QStringLiteral("/srv/repos/app/docs/diagram.png");
    QTest::newRow("parent") << doc << QStringLiteral("../img/logo.png")
                            << QStringLiteral("/srv/repos/app/img/logo.png");
    QTest::newRow("bare-parent") << doc << QStringLiteral("..")
                                 << QStringLiteral("/srv/repos/app");
    QTest::newRow("interior-dots") << doc << QStringLiteral("a/../b/./c.png")
                                   << QStringLiteral("/srv/repos/app/docs/b/c.png");

    // ".." is CLAMPED at the server root on both sides. It does not escape and
    // it does not produce a relative fragment; it simply stops climbing.
    QTest::newRow("clamped-at-root")
        << doc << QStringLiteral("../../../../../../../etc/passwd")
        << QStringLiteral("/etc/passwd");

    // An absolute server path is taken as it stands.
    QTest::newRow("absolute") << doc << QStringLiteral("/srv/other/a.png")
                              << QStringLiteral("/srv/other/a.png");

    // Query and fragment are cut before resolving, on both sides.
    QTest::newRow("query") << doc << QStringLiteral("img.png?v=2")
                           << QStringLiteral("/srv/repos/app/docs/img.png");
    QTest::newRow("fragment") << doc << QStringLiteral("img.png#frag")
                              << QStringLiteral("/srv/repos/app/docs/img.png");
    QTest::newRow("document-hash")
        << QStringLiteral("/srv/docs#archive/guide.md") << QStringLiteral("img.png")
        << QStringLiteral("/srv/docs#archive/img.png");
    QTest::newRow("document-question")
        << QStringLiteral("/srv/docs?archive/guide.md") << QStringLiteral("img.png")
        << QStringLiteral("/srv/docs?archive/img.png");
    QTest::newRow("encoded-space") << doc << QStringLiteral("img%20one.png")
                                   << QStringLiteral("/srv/repos/app/docs/img one.png");
    QTest::newRow("encoded-hash") << doc << QStringLiteral("img%23one.png")
                                  << QStringLiteral("/srv/repos/app/docs/img#one.png");
    QTest::newRow("encoded-question") << doc << QStringLiteral("img%3Fone.png")
                                      << QStringLiteral("/srv/repos/app/docs/img?one.png");
    QTest::newRow("encoded-percent") << doc << QStringLiteral("img%25one.png")
                                     << QStringLiteral("/srv/repos/app/docs/img%one.png");
    QTest::newRow("decode-once") << doc << QStringLiteral("img%2520one.png")
                                 << QStringLiteral("/srv/repos/app/docs/img%20one.png");
    QTest::newRow("encoded-parent") << doc << QStringLiteral("%2e%2e/img.png")
                                    << QStringLiteral("/srv/repos/app/img.png");
    QTest::newRow("malformed-escape") << doc << QStringLiteral("img%2Xone.png")
                                      << QStringLiteral("/srv/repos/app/docs/img%2Xone.png");

    // A backslash is an ORDINARY CHARACTER in a POSIX remote path — not a
    // separator — on both sides, so a name containing one stays one segment.
    QTest::newRow("backslash") << doc << QStringLiteral("sub\\odd name.png")
                               << QStringLiteral("/srv/repos/app/docs/sub\\odd name.png");

    // Everything that is NOT a document-relative reference is refused. A
    // protocol-relative "//host/x" is the dangerous one: it reaches an external
    // host, and the page's own URL policy refuses it for exactly that reason.
    QTest::newRow("protocol-relative") << doc << QStringLiteral("//host/x.png") << QString();
    QTest::newRow("https") << doc << QStringLiteral("https://host/x.png") << QString();
    QTest::newRow("data") << doc << QStringLiteral("data:image/png;base64,AAAA") << QString();
    QTest::newRow("empty") << doc << QString() << QString();

    // A "#" or "?" prefix is a same-document reference, not a resource. Refused
    // by isRelativeResource() on the page — and it MUST be refused here too,
    // because everything from the first "?" or "#" is cut before resolving, so
    // "#anchor" would otherwise resolve to the document's own DIRECTORY and
    // this view would mint a file read for it.
    QTest::newRow("hash-only") << doc << QStringLiteral("#anchor") << QString();
    QTest::newRow("query-only") << doc << QStringLiteral("?v=2") << QString();

    // The two DELIBERATE DIVERGENCES from resolveRemotePath(), documented at
    // both ends. Both go the same way: this side refuses where the page's side
    // answers, because this answer is fed to a remote file READ while the
    // page's is used as a link target.
    //
    // 1. No document to resolve against. resolveRemotePath("", "a.png") answers
    //    "/a.png" (renderer.test.ts pins that); this answers "". The view can
    //    reach the state — its url is empty until one arrives — and the page
    //    cannot, because its document path is the ?path= query the view wrote.
    QTest::newRow("no-document") << QString() << QStringLiteral("a.png") << QString();

    // 2. A reference that climbs to the filesystem root. resolveRemotePath()
    //    answers "/", which is a fine LINK target; "/" is not a file.
    QTest::newRow("resolves-to-root")
        << QStringLiteral("/a.md") << QStringLiteral("../..") << QString();
}

void TstPaneIdentity::theMarkdownImagePathResolverAgreesWithTheRendererBundle()
{
    QFETCH(QString, documentPath);
    QFETCH(QString, reference);
    QFETCH(QString, expected);

    // An empty bundle url keeps the view's own WebEngineView inert: navigate()
    // reads it as "go nowhere", so this stays a test of the resolver.
    QObject *const view = openInShell(
        QStringLiteral("ViewerMarkdownView.qml"),
        {{QStringLiteral("markdownBundleUrl"), QString()},
         {QStringLiteral("url"),
          documentPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(documentPath)}});
    QVERIFY(view);

    QVariant resolved;
    QVERIFY2(QMetaObject::invokeMethod(view, "resolveImagePath",
                                       Q_RETURN_ARG(QVariant, resolved),
                                       Q_ARG(QVariant, reference)),
             "ViewerMarkdownView has no resolveImagePath()");
    QCOMPARE(resolved.toString(), expected);
}

// ---------------------------------------------------------------------------
// The renderer page fetching its own document, end to end.
//
// Everything below the browser was already covered and all of it passed while
// the feature was completely broken in the shipped app: the URL map minted the
// address, the scheme handler served the bytes, and the TypeScript renderer
// turned Markdown into sanitised HTML. What none of them could see is that
// Chromium refused the request before it became a request at all, because the
// custom scheme was not registered as reachable by the Fetch API. That refusal
// exists only inside the browser, so only the real page can prove it is gone.
//
// The assertion is deliberately on the DOCUMENT TEXT rather than on the absence
// of an error: a page that renders nothing and says nothing is just as broken
// as one that shows a banner, and only "the user's words are on the screen"
// rules both out.
// ---------------------------------------------------------------------------
void TstPaneIdentity::theMarkdownPageRendersADocumentFetchedFromTheInternalScheme()
{
    const QString path = QStringLiteral("/srv/repos/app/docs/guide.md");
    ReadFileServer server;
    QVERIFY2(server.bind(&m_client),
             "the local RPC transport pair could not be created");
    server.setFile(path, QStringLiteral("# Harbor Notes\n\nThe body of the document.\n\n"
                                        "[Project site](https://example.com/harbor)\n"));

    // The REAL bundle url, unlike every other Markdown test in this file: those
    // blank it precisely so no page loads, and that is why they never saw this.
    QObject *const view =
        openInShell(QStringLiteral("ViewerMarkdownView.qml"),
                    {{QStringLiteral("url"), QStringLiteral("file://") + path}});
    QVERIFY(view);

    QElapsedTimer clock;
    clock.start();
    while (!view->property("pageReady").toBool() && clock.elapsed() < kPageLoadTimeoutMs)
        QTest::qWait(100);
    // Whether Chromium can start at all is a property of the box, so it QSKIPs
    // here exactly as the terminal and editor page tests above do. What the
    // page then does with a document it CAN load is our code, and fails.
    if (!view->property("pageReady").toBool())
        QSKIP("the Markdown page never loaded; WebEngine cannot run under this recipe");

    const bool rendered = waitForJs(view, QString::fromLatin1(kJsMarkdownBody),
                                    QStringLiteral("Harbor Notes"), kPageLoadTimeoutMs);
    const QString status = evalJs(view, QString::fromLatin1(kJsMarkdownStatus));
    QVERIFY2(rendered,
             qPrintable(QStringLiteral("the page never rendered the document it was "
                                       "pointed at; it reported: \"%1\"")
                            .arg(status)));
    QVERIFY2(status.isEmpty(),
             qPrintable(QStringLiteral("the page rendered the document and still showed "
                                       "an error: \"%1\"")
                            .arg(status)));
    QVERIFY2(evalJs(view, QString::fromLatin1(kJsMarkdownBody))
                 .contains(QStringLiteral("The body of the document.")),
             "only part of the document survived the fetch");

    // QML's own failure channel must agree with the page: a view that rendered
    // the file while ViewerPane's banner said the read had failed would be two
    // contradictory answers on one screen.
    QCOMPARE(view->property("errorText").toString(), QString());

    // Exercise the actual Chromium context-menu request, not a QML helper or
    // a synthetic request. The menu must describe the rendered document and
    // its useful actions must still affect the real browser selection.
    auto *const window = qobject_cast<QQuickWindow *>(m_shell.get());
    auto *const browser = qobject_cast<QQuickItem *>(webViewIn(view));
    QVERIFY(window && browser);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    const auto contextPoint = [this, view, browser](const QString &selector,
                                                  bool selectText) {
        const QString script = QStringLiteral(
            "(function(){var e=document.querySelector(%1);"
            "var range=document.createRange();range.selectNodeContents(e);"
            "if(%2){window.getSelection().removeAllRanges();"
            "window.getSelection().addRange(range);}"
            "var r=range.getBoundingClientRect();return JSON.stringify("
            "{x:r.left+r.width/2,y:r.top+r.height/2});})()")
                                   .arg(QStringLiteral("\"") + selector + QStringLiteral("\""),
                                        selectText ? QStringLiteral("true")
                                                   : QStringLiteral("false"));
        const QJsonObject rect = QJsonDocument::fromJson(evalJs(view, script).toUtf8()).object();
        return browser->mapToScene(QPointF(rect.value(QStringLiteral("x")).toDouble(),
                                          rect.value(QStringLiteral("y")).toDouble())).toPoint();
    };
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, contextPoint("h1", true));
    QObject *const menu = childNamed(view, QStringLiteral("markdownDocumentMenu"));
    QVERIFY(menu);
    QTRY_VERIFY(menu->property("visible").toBool());
    // The four supported operations are Copy, Copy link address, Copy image,
    // and Select all: no generated-page Save Page or View Source defaults.
    QCOMPARE(menu->property("count").toInt(), 4);
    auto *const copy = qobject_cast<QQuickItem *>(childNamed(view, QStringLiteral("markdownCopy")));
    auto *const copyLink = qobject_cast<QQuickItem *>(
        childNamed(view, QStringLiteral("markdownCopyLink")));
    auto *const selectAll = qobject_cast<QQuickItem *>(
        childNamed(view, QStringLiteral("markdownSelectAll")));
    QVERIFY(copy && copyLink && selectAll);
    QVERIFY(copy->isEnabled());
    QVERIFY(!copyLink->isVisible());
    QGuiApplication::clipboard()->clear();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      copy->mapToScene(QPointF(copy->width() / 2, copy->height() / 2)).toPoint());
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Harbor Notes"));
    QTRY_VERIFY(!menu->property("visible").toBool());

    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, contextPoint("a", false));
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(copyLink->isVisible());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      copyLink->mapToScene(
                          QPointF(copyLink->width() / 2, copyLink->height() / 2)).toPoint());
    QTRY_COMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("https://example.com/harbor"));
    QTRY_VERIFY(!menu->property("visible").toBool());

    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, contextPoint("h1", false));
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(selectAll->isEnabled());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      selectAll->mapToScene(
                          QPointF(selectAll->width() / 2, selectAll->height() / 2)).toPoint());
    QVERIFY(evalJs(view, QStringLiteral("window.getSelection().toString()"))
                .contains(QStringLiteral("The body of the document.")));
}

// ---------------------------------------------------------------------------
// One pin per image address per document.
//
// applyTheme() re-renders the whole document on purpose, so a live theme change
// never opens a second unsanitised insertion path — and the re-render asks the
// bridge for EVERY image again. A view that pinned each answer took one more
// pin, and held one more entry, per image per theme change, for as long as the
// document stayed open. The reference counts balanced, so nothing leaked at
// teardown and nothing ever showed it.
void TstPaneIdentity::theMarkdownViewPinsEachImageAddressExactlyOnce()
{
    ch::InternalUrlMap &map = ch::InternalUrlMap::shared();
    const int pinnedBefore = map.retainedCount();

    QObject *const view = openInShell(
        QStringLiteral("ViewerMarkdownView.qml"),
        {{QStringLiteral("markdownBundleUrl"), QString()},
         {QStringLiteral("url"), QStringLiteral("file:///srv/repos/app/docs/guide.md")}});
    QVERIFY(view);

    // The document itself is pinned by retarget().
    QTRY_COMPARE(map.retainedCount(), pinnedBefore + 1);

    QObject *const bridge = childNamed(view, QStringLiteral("markdownImageBridge"));
    QVERIFY2(bridge, "the markdown view exposes no image bridge to its page");

    const auto resolve = [bridge](const QString &reference) {
        QVariant answer;
        QMetaObject::invokeMethod(bridge, "resolveImage", Q_RETURN_ARG(QVariant, answer),
                                  Q_ARG(QVariant, reference));
        return answer.toString();
    };

    const QString first = resolve(QStringLiteral("a.png"));
    QVERIFY2(first.startsWith(QStringLiteral("codeharbor-internal://file/")),
             qPrintable(first));
    const QString second = resolve(QStringLiteral("b.png"));
    QVERIFY(second.startsWith(QStringLiteral("codeharbor-internal://file/")));
    QVERIFY(second != first);
    QCOMPARE(map.retainedCount(), pinnedBefore + 3);

    // Exactly what a theme change does: the page re-renders and asks again for
    // every image it holds.
    for (int pass = 0; pass < 5; ++pass) {
        QCOMPARE(resolve(QStringLiteral("a.png")), first);
        QCOMPARE(resolve(QStringLiteral("b.png")), second);
    }
    QVERIFY2(map.retainedCount() == pinnedBefore + 3,
             qPrintable(QStringLiteral("re-rendering took %1 extra pin(s); the view's held set "
                                       "grows with every theme change")
                            .arg(map.retainedCount() - pinnedBefore - 3)));

    // A refused reference is reported as "" — the page's documented "no image"
    // value — and pins nothing.
    QCOMPARE(resolve(QStringLiteral("https://host/x.png")), QString());
    QCOMPARE(resolve(QStringLiteral("#anchor")), QString());
    QCOMPARE(map.retainedCount(), pinnedBefore + 3);

    // Every pin is given back when the VIEW goes away — so take away the view,
    // not the window it happens to be sitting in. The window is torn down by
    // cleanup() like every other test's, which keeps this test's exit from
    // being the one shape of teardown no other test in this file performs.
    QVERIFY(QMetaObject::invokeMethod(m_shell.get(), "unloadRegion"));
    m_region = nullptr;
    QTRY_COMPARE(map.retainedCount(), pinnedBefore);
}

// ---------------------------------------------------------------------------
// (17) "Open as" is a choice about an ADDRESS, not about a moment.
//
// A pane's handler override used to be wiped by every republish of the region
// tree — and the tree is republished for reasons that have nothing to do with
// this pane: a sibling being split, a divider being dragged, the layout being
// reloaded. The user opened a .png in the editor, split another pane, and the
// .png silently went back to being an image.
// ---------------------------------------------------------------------------
void TstPaneIdentity::anUnrelatedRepublishKeepsAnOpenAsChoice()
{
    const QString address = QStringLiteral("file:///srv/repos/app/logo.png");
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       leafNode(QStringLiteral("viewer-1"), address),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 1);
    QObject *const pane = panes().constFirst();

    // The registry's own answer for a .png, which is what the override has to
    // be visibly different from.
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("image"));

    QVariant accepted;
    QVERIFY(QMetaObject::invokeMethod(
        m_region, "openPaneTarget", Q_RETURN_ARG(QVariant, accepted),
        Q_ARG(QVariant, QStringLiteral("viewer-1")), Q_ARG(QVariant, address),
        Q_ARG(QVariant, QStringLiteral("binary"))));
    QVERIFY(accepted.toBool());
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("binary"));

    // A split of a DIFFERENT pane. viewer-1's leaf keeps the very same address,
    // so nothing about what it is showing has changed.
    setNode(branchNode(QStringLiteral("horizontal"),
                       QVariantList{leafNode(QStringLiteral("viewer-1"), address),
                                    leafNode(QStringLiteral("viewer-2"))}));
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(kSettleMs);

    QCOMPARE(paneWithId(m_region, QStringLiteral("viewer-1")), pane);
    QCOMPARE(pane->property("forcedKind").toString(), QStringLiteral("binary"));
    QVERIFY2(pane->property("kind").toString() == QStringLiteral("binary"),
             "splitting another pane threw away this pane's \"Open as\" choice");

    // Pointing the pane at a DIFFERENT address is a real navigation and must
    // clear the override, so the new file gets the registry's handler.
    setNode(branchNode(
        QStringLiteral("horizontal"),
        QVariantList{leafNode(QStringLiteral("viewer-1"),
                              QStringLiteral("file:///srv/repos/app/other.png")),
                     leafNode(QStringLiteral("viewer-2"))}));
    QTRY_COMPARE(pane->property("forcedKind").toString(), QString());
    QTRY_COMPARE(pane->property("kind").toString(), QStringLiteral("image"));
}

// ---------------------------------------------------------------------------
// (18) Split proportions across a window resize.
//
// SplitView stretches exactly ONE of its fill items, so every pixel a window
// resize adds or removes used to land on the first pane in the region: a 50/50
// split became lopsided the first time the window was widened, and the stored
// ratios were only seen again after the Dev Session was reopened.
// ---------------------------------------------------------------------------
void TstPaneIdentity::splitProportionsSurviveAWindowResize()
{
    QVERIFY(openRegion(QStringLiteral("ViewerRegion.qml"),
                       branchNode(QStringLiteral("horizontal"),
                                  QVariantList{leafNode(QStringLiteral("viewer-1")),
                                               leafNode(QStringLiteral("viewer-2"))}),
                       /*terminal=*/false));
    const auto panes = [this] { return collect(m_region, isLeafPane); };
    QTRY_VERIFY(panes().size() == 2);
    QTest::qWait(kSettleMs);

    auto *const left = qobject_cast<QQuickItem *>(paneWithId(m_region, QStringLiteral("viewer-1")));
    auto *const right = qobject_cast<QQuickItem *>(paneWithId(m_region, QStringLiteral("viewer-2")));
    QVERIFY(left && right);
    QVERIFY2(qAbs(left->width() - right->width()) <= 12,
             qPrintable(QStringLiteral("the split did not start out even: %1 vs %2")
                            .arg(left->width())
                            .arg(right->width())));

    const qreal startWidth = m_shell->property("width").toReal();
    QVERIFY(startWidth > 0);
    const qreal rightBefore = right->width();
    m_shell->setProperty("width", startWidth + 400);
    QTRY_VERIFY(left->width() + right->width() > startWidth);
    QTest::qWait(kSettleMs);

    // The second pane must actually receive a share of the 400 pixels. Before
    // the region rescaled its children itself, SplitView gave every one of
    // them to the first fill item and this width did not move at all.
    QVERIFY2(right->width() >= rightBefore + 120,
             qPrintable(QStringLiteral("widening the window by 400 moved the second pane from "
                                       "%1 to %2; the extra space went to the first pane")
                            .arg(rightBefore)
                            .arg(right->width())));
    QVERIFY2(qAbs(left->width() - right->width()) <= 40,
             qPrintable(QStringLiteral("after widening, the panes are %1 and %2 wide, which is "
                                       "no longer the even split they started at")
                            .arg(left->width())
                            .arg(right->width())));

    // ...and back again, so shrinking is not a one-way trip either.
    m_shell->setProperty("width", startWidth);
    QTest::qWait(kSettleMs);
    QVERIFY2(qAbs(left->width() - right->width()) <= 40,
             qPrintable(QStringLiteral("narrowing the window again left the panes at %1 and %2")
                            .arg(left->width())
                            .arg(right->width())));
}

// ---------------------------------------------------------------------------
// (19) Saying what a terminal pane runs.
//
// The sidebar's per-session status (SPEC 6.6) is derived from the harness held
// against each `terminal_panes` row. A pane driven by an agent hook announces
// its harness on the wire; a pane the user opened themselves and then started
// something in cannot, and a row with no harness at all is deliberately kept
// quiet. So the pane header carries the only affordance that can ever set one,
// and what it must get right is small and exact: the WIRE value of the choice
// the user made, against the id of the row this pane owns — not its recyclable
// slot label, and not the human label off the button.
// ---------------------------------------------------------------------------
void TstPaneIdentity::aTerminalPaneWritesTheHarnessTheUserChose()
{
    // The pane already has a harness, so the dialog has something to be opened
    // ON: a control that always offered the same default would look identical
    // in every other assertion here.
    m_app.seedTerminalPaneHarness(QStringLiteral("row-a"), QStringLiteral("claude-code"));

    TerminalFactoryStub factory;
    QObject *const pane = openInShell(
        QStringLiteral("TerminalPaneView.qml"),
        {{QStringLiteral("factory"), QVariant::fromValue<QObject *>(&factory)},
         {QStringLiteral("paneId"), QStringLiteral("terminal-1")},
         {QStringLiteral("devSessionId"), QStringLiteral("dev-a")},
         {QStringLiteral("terminalPaneId"), QStringLiteral("row-a")},
         {QStringLiteral("workingDir"), QStringLiteral("/srv/a")}});
    QVERIFY(pane);

    QObject *const button = childNamed(pane, QStringLiteral("terminalHarnessButton"));
    QVERIFY2(button, "the terminal pane header has no control for what the pane runs, so its "
                     "sidebar status can never be anything but Idle");
    QVERIFY2(button->property("enabled").toBool(),
             "the control is disabled on a pane that HAS a server row to write to");
    // Glyph-only, so the words are the whole of what a screen reader and the
    // tooltip have to go on.
    QVERIFY2(!button->property("text").toString().isEmpty(),
             "the header control is a bare glyph with no words: it announces nothing");

    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QObject *const dialog = childNamed(pane, QStringLiteral("terminalPaneHarnessDialog"));
    QVERIFY2(dialog, "the header control opened no dialog");
    QTRY_VERIFY(dialog->property("visible").toBool());

    QObject *const generic =
        childNamed(dialog, QStringLiteral("terminalPaneHarnessOption_generic"));
    QObject *const claude =
        childNamed(dialog, QStringLiteral("terminalPaneHarnessOption_claude-code"));
    QObject *const shell =
        childNamed(dialog, QStringLiteral("terminalPaneHarnessOption_shell"));
    QVERIFY2(generic && claude && shell,
             "the dialog does not offer the harness vocabulary the wire accepts");
    // "Plain shell" is a CHOICE, not the absence of one, and it is the option
    // "generic" has to be told apart from.
    QCOMPARE(accessibleNameOf(shell), QStringLiteral("Plain shell"));
    QVERIFY2(!accessibleNameOf(generic).isEmpty(),
             "a radio button with no accessible name is unreachable by anything but the mouse");
    QVERIFY2(claude->property("checked").toBool(),
             "the dialog did not open on the harness the pane already has, so accepting it "
             "unchanged would silently change the pane");
    QVERIFY(!generic->property("checked").toBool());

    // Dismissing writes nothing at all: this is a stored choice, not a preview.
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTest::qWait(50);
    QVERIFY2(m_app.harnessWrites().isEmpty(),
             "a dismissed dialog still wrote the pane's harness");

    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(generic, "clicked"));
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));

    QTRY_COMPARE(m_app.harnessWrites().size(), 1);
    const AppControllerStub::HarnessWrite &write = m_app.harnessWrites().constFirst();
    // The ROW id, never "terminal-1": the slot label is recycled when a pane is
    // closed, so writing against it would eventually hand another pane's shell
    // this harness.
    QCOMPARE(write.terminalPaneId, QStringLiteral("row-a"));
    // ...and the wire value, not the words on the button.
    QCOMPARE(write.harness, QStringLiteral("generic"));
}

// ---------------------------------------------------------------------------
// (20) A pane whose remote session died and was silently replaced.
//
// The attach command is `tmux new-session -A`: it attaches if the session is
// there and CREATES it if it is not, and it never says which it did. So a pane
// whose remote session had died handed the user a brand new empty shell — their
// agent, its work and its scrollback gone — and said nothing at all. That
// silence is the whole bug: they could not even tell whether anything was lost.
//
// ch::TerminalFactory finds it out from the host's own session list and reports
// it as sessionRecreated. What this covers is the other half: the pane has to
// SAY it, in words that mean something to a person, without trapping them behind
// chrome they cannot get rid of, and out loud for a screen reader.
// ---------------------------------------------------------------------------
void TstPaneIdentity::aTerminalPaneSaysWhenItsRemoteSessionWasSilentlyReplaced()
{
    TerminalFactoryStub factory;
    QObject *const pane = openInShell(
        QStringLiteral("TerminalPaneView.qml"),
        {{QStringLiteral("factory"), QVariant::fromValue<QObject *>(&factory)},
         {QStringLiteral("paneId"), QStringLiteral("terminal-1")},
         {QStringLiteral("devSessionId"), QStringLiteral("dev-a")},
         {QStringLiteral("terminalPaneId"), QStringLiteral("row-a")},
         {QStringLiteral("workingDir"), QStringLiteral("/srv/a")}});
    QVERIFY(pane);
    QTRY_VERIFY(factory.controller());

    QObject *const notice = childNamed(pane, QStringLiteral("terminalSessionReplacedNotice"));
    QVERIFY2(notice, "the pane has nothing that can report a replaced remote session, so it "
                     "hands the user an empty shell and says nothing");
    // Silent until there is something to say: this is the state every working
    // pane is in.
    QVERIFY(!notice->property("visible").toBool());

    emit factory.sessionRecreated(factory.controller(), QStringLiteral("ch_dev-a_row-a"));
    QTRY_VERIFY2(notice->property("visible").toBool(),
                 "the factory reported that this pane's remote session had been replaced and the "
                 "pane showed nothing");

    QObject *const label = childNamed(notice, QStringLiteral("terminalSessionReplacedLabel"));
    QVERIFY(label);
    const QString said = label->property("text").toString();
    // Plain language about what happened to their work, and NOTHING they cannot
    // act on: no tmux, no session name, no row id.
    QVERIFY2(!said.isEmpty(), "the notice is blank");
    QVERIFY2(!said.contains(QStringLiteral("tmux")), qPrintable(said));
    QVERIFY2(!said.contains(QStringLiteral("ch_dev-a_row-a")), qPrintable(said));
    QVERIFY2(!said.contains(QStringLiteral("row-a")), qPrintable(said));

    // Announced, not merely painted: a notice a screen reader user never hears
    // is, for them, exactly the silence this exists to end.
    QCOMPARE(accessibleRoleOf(notice), int(QAccessible::AlertMessage));
    QCOMPARE(accessibleNameOf(notice), said);

    // ...and it goes away when they have read it. A notice that could not be
    // dismissed would sit permanently over the terminal it is talking about.
    QObject *const dismiss =
        childNamed(notice, QStringLiteral("terminalSessionReplacedDismissButton"));
    QVERIFY2(dismiss, "the notice cannot be dismissed, so it covers the terminal for good");
    QVERIFY2(!accessibleNameOf(dismiss).isEmpty(),
             "the dismiss control announces nothing, so it is reachable by mouse only");
    QVERIFY(QMetaObject::invokeMethod(dismiss, "clicked"));
    QTRY_VERIFY(!notice->property("visible").toBool());
    QVERIFY(!pane->property("sessionReplaced").toBool());

    // Retargeting the pane at another terminal must not carry the verdict over:
    // it was never true of the terminal the pane is now showing.
    emit factory.sessionRecreated(factory.controller(), QStringLiteral("ch_dev-a_row-a"));
    QTRY_VERIFY(notice->property("visible").toBool());
    pane->setProperty("terminalPaneId", QStringLiteral("row-b"));
    QTRY_VERIFY2(!notice->property("visible").toBool(),
                 "the notice followed the pane onto a different terminal, where it is a report "
                 "about a session that terminal never had");
}

void TstPaneIdentity::narrowViewerKeepsItsAddressUsable_data()
{
    QTest::addColumn<int>("paneWidth");
    QTest::addColumn<bool>("outside");
    for (int width : {120, 220, 960}) {
        for (bool outside : {false, true}) {
            const QByteArray name = QByteArray::number(width) + (outside ? "-outside" : "-inside");
            QTest::newRow(name.constData()) << width << outside;
        }
    }
}

void TstPaneIdentity::narrowViewerKeepsItsAddressUsable()
{
    QFETCH(int, paneWidth);
    QFETCH(bool, outside);
    QSignalSpy warnings(m_engine.get(), &QQmlEngine::warnings);
    QVERIFY(warnings.isValid());
    m_app.setActiveSessionRepoRoot(QStringLiteral("/srv/repos/app"));
    QObject *const pane = openInShell(
        QStringLiteral("ViewerPane.qml"),
        {{QStringLiteral("url"), QUrl(QStringLiteral("file:///tmp/original.bin"))}});
    QVERIFY(pane);
    auto *const window = qobject_cast<QQuickWindow *>(m_shell.get());
    auto *const paneItem = qobject_cast<QQuickItem *>(pane);
    auto *const address = qobject_cast<QQuickItem *>(
        childNamed(pane, QStringLiteral("viewerAddressField")));
    auto *const navigation = qobject_cast<QQuickItem *>(
        childNamed(pane, QStringLiteral("viewerNavigationControls")));
    auto *const marker = qobject_cast<QQuickItem *>(
        childNamed(pane, QStringLiteral("viewerOutsideRepoMarker")));
    QVERIFY(window && paneItem && address && navigation && marker);
    window->resize(paneWidth, 640);
    QTRY_COMPARE(qRound(paneItem->width()), paneWidth);
    pane->setProperty("repoRootState", outside ? QStringLiteral("outside")
                                              : QStringLiteral("inside"));
    QTRY_COMPARE(marker->isVisible(), outside);
    QTRY_VERIFY(address->width() > 4 && address->height() > 4);
    const QRectF fieldRect(address->mapToItem(paneItem, QPointF()), address->size());
    const QRectF navigationRect(navigation->mapToItem(paneItem, QPointF()), navigation->size());
    const QRectF paneRect(0, 0, paneItem->width(), paneItem->height());
    QVERIFY(paneRect.contains(fieldRect));
    QVERIFY(paneRect.contains(navigationRect));
    QVERIFY(!fieldRect.intersects(navigationRect));
    if (outside) {
        const QRectF markerRect(marker->mapToItem(paneItem, QPointF()), marker->size());
        QVERIFY(paneRect.contains(markerRect));
        QVERIFY(!fieldRect.intersects(markerRect));
    }
    enterAddress(pane, QStringLiteral("file:///tmp/next.bin"));
    QTRY_COMPARE(pane->property("url").toUrl(), QUrl(QStringLiteral("file:///tmp/next.bin")));
    QVERIFY(address->hasActiveFocus());
    QVERIFY2(warnings.isEmpty(), "responsive viewer emitted QML warnings");
}

void TstPaneIdentity::keyboardSplitRatiosPersistAfterLayout_data()
{
    QTest::addColumn<bool>("horizontal");
    QTest::addColumn<bool>("terminal");
    QTest::newRow("terminal-horizontal") << true << true;
    QTest::newRow("terminal-vertical") << false << true;
    QTest::newRow("viewer-horizontal") << true << false;
    QTest::newRow("viewer-vertical") << false << false;
}

void TstPaneIdentity::keyboardSplitRatiosPersistAfterLayout()
{
    QFETCH(bool, horizontal);
    QFETCH(bool, terminal);
    const QString firstId = terminal ? QStringLiteral("terminal-1") : QStringLiteral("viewer-1");
    const QString secondId = terminal ? QStringLiteral("terminal-2") : QStringLiteral("viewer-2");
    QVariantMap tree = branchNode(
        horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical"),
        {leafNode(firstId), leafNode(secondId)});
    QVERIFY(openRegion(terminal ? QStringLiteral("TerminalRegion.qml")
                                : QStringLiteral("ViewerRegion.qml"), tree, terminal));
    QTRY_VERIFY(paneWithId(m_region, secondId));
    QTest::qWait(kSettleMs);
    auto *const first = qobject_cast<QQuickItem *>(paneWithId(m_region, firstId));
    auto *const second = qobject_cast<QQuickItem *>(paneWithId(m_region, secondId));
    auto *const window = qobject_cast<QQuickWindow *>(m_shell.get());
    QVERIFY(first && second && window);
    const auto extent = [horizontal](QQuickItem *item) {
        return horizontal ? item->width() : item->height();
    };
    const QList<QObject *> handles = collect(m_region, [](QObject *object) {
        return object->metaObject()->indexOfProperty("keyStep") >= 0;
    });
    QCOMPARE(handles.size(), 1);
    auto *const handle = qobject_cast<QQuickItem *>(handles.constFirst());
    QVERIFY(handle);
    const qreal before = extent(first);
    QSignalSpy reported(m_region, SIGNAL(splitRatiosAdjusted(QVariant, QVariant)));
    QVERIFY(reported.isValid());
    handle->forceActiveFocus();
    QTRY_VERIFY(handle->hasActiveFocus());
    QTest::keyClick(window, horizontal ? Qt::Key_Right : Qt::Key_Down);
    QTRY_COMPARE(reported.size(), 1);
    QTRY_VERIFY(extent(first) > before + 10);
    const qreal changed = extent(first);
    const QVariantList ratios = asList(reported.constFirst().at(1));
    QCOMPARE(ratios.size(), 2);
    QVERIFY(qAbs(ratios.at(0).toDouble()
                 - extent(first) / (extent(first) + extent(second))) < 0.001);

    // Simulate the host restoring its previous tree, then the newly persisted
    // ratio payload. A report of the pre-key geometry fails this round trip.
    setNode(tree);
    QTRY_VERIFY(qAbs(extent(first) - before) <= 6);
    tree.insert(QStringLiteral("ratios"), ratios);
    setNode(tree);
    QTRY_VERIFY(qAbs(extent(first) - changed) <= 6);
}

void TstPaneIdentity::keyboardSplitLimitsLeaveTheThirdPaneAlone_data()
{
    QTest::addColumn<bool>("horizontal");
    QTest::newRow("horizontal") << true;
    QTest::newRow("vertical") << false;
}

void TstPaneIdentity::keyboardSplitLimitsLeaveTheThirdPaneAlone()
{
    QFETCH(bool, horizontal);
    QQmlComponent component(m_engine.get());
    component.setData(R"QML(
import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import CodeHarbor
Window {
    id: splitFixture
    width: 712; height: 712; visible: true
    property bool splitHorizontal: true
    SplitView {
        anchors.fill: parent
        orientation: splitFixture.splitHorizontal ? Qt.Horizontal : Qt.Vertical
        handle: AppSplitHandle {}
        Item {
            objectName: "unrelatedFill"
            SplitView.fillWidth: true; SplitView.fillHeight: true
        }
        Item {
            objectName: "before"
            SplitView.preferredWidth: 150; SplitView.preferredHeight: 150
        }
        Item {
            objectName: "after"
            SplitView.preferredWidth: 250; SplitView.preferredHeight: 250
            SplitView.minimumWidth: 200; SplitView.minimumHeight: 200
        }
    }
}
)QML", QUrl(QStringLiteral("qrc:/tst_paneidentity/split-limits.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));
    m_shell.reset(component.createWithInitialProperties({{QStringLiteral("splitHorizontal"), horizontal}}));
    QVERIFY2(m_shell, qPrintable(component.errorString()));
    auto *const window = qobject_cast<QQuickWindow *>(m_shell.get());
    auto *const fill = qobject_cast<QQuickItem *>(childNamed(window, QStringLiteral("unrelatedFill")));
    auto *const before = qobject_cast<QQuickItem *>(childNamed(window, QStringLiteral("before")));
    auto *const after = qobject_cast<QQuickItem *>(childNamed(window, QStringLiteral("after")));
    QVERIFY(window && fill && before && after);
    const auto extent = [horizontal](QQuickItem *item) {
        return horizontal ? item->width() : item->height();
    };
    QTRY_COMPARE(qRound(extent(after)), 250);
    QTRY_COMPARE(qRound(extent(before)), 150);
    const qreal fillBefore = extent(fill);
    const QList<QObject *> handles = collect(window, [](QObject *object) {
        return object->metaObject()->indexOfProperty("keyStep") >= 0;
    });
    QCOMPARE(handles.size(), 2);
    QQuickItem *handle = nullptr;
    for (QObject *candidate : handles) {
        auto *const item = qobject_cast<QQuickItem *>(candidate);
        QVERIFY(item);
        if (!handle || (horizontal ? item->x() > handle->x() : item->y() > handle->y()))
            handle = item;
    }
    QVERIFY(handle);
    handle->forceActiveFocus();
    QTRY_VERIFY(handle->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_PageDown);
    QTRY_COMPARE(qRound(extent(after)), 200);
    QCOMPARE(qRound(extent(before)), 200);
    QCOMPARE(extent(fill), fillBefore);
    QTest::keyClick(window, Qt::Key_PageDown);
    QTest::qWait(50);
    QCOMPARE(qRound(extent(before)), 200);
    QCOMPARE(extent(fill), fillBefore);
    QTest::keyClick(window, horizontal ? Qt::Key_Left : Qt::Key_Up);
    QTRY_COMPARE(qRound(extent(after)), 216);
    QCOMPARE(qRound(extent(before)), 184);
    QCOMPARE(extent(fill), fillBefore);
}

void TstPaneIdentity::terminalRetargetDismissesStaleDialogs_data()
{
    QTest::addColumn<QString>("openMethod");
    QTest::addColumn<QString>("dialogName");
    QTest::newRow("rename") << QStringLiteral("beginRename")
                            << QStringLiteral("terminalPaneRenameDialog");
    QTest::newRow("harness") << QStringLiteral("beginSetHarness")
                             << QStringLiteral("terminalPaneHarnessDialog");
    QTest::newRow("kill") << QStringLiteral("beginClose")
                          << QStringLiteral("terminalPaneKillDialog");
}

void TstPaneIdentity::terminalRetargetDismissesStaleDialogs()
{
    QFETCH(QString, openMethod);
    QFETCH(QString, dialogName);
    TerminalFactoryStub factory;
    QObject *const pane = openInShell(
        QStringLiteral("TerminalPaneView.qml"),
        {{QStringLiteral("factory"), QVariant::fromValue<QObject *>(&factory)},
         {QStringLiteral("paneId"), QStringLiteral("terminal-1")},
         {QStringLiteral("devSessionId"), QStringLiteral("dev-a")},
         {QStringLiteral("terminalPaneId"), QStringLiteral("row-a")}});
    QVERIFY(pane);
    QSignalSpy killed(pane, SIGNAL(killRequested(QString)));
    QSignalSpy closed(pane, SIGNAL(closeRequested(QString)));
    QSignalSpy renamed(pane, SIGNAL(titleChangedRequested(QString, QString)));
    QVERIFY(killed.isValid() && closed.isValid() && renamed.isValid());
    QVERIFY(QMetaObject::invokeMethod(pane, qPrintable(openMethod)));
    QObject *const dialog = childNamed(pane, dialogName);
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(pane->setProperty("terminalPaneId", QStringLiteral("row-b")));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QCOMPARE(killed.size(), 0);
    QCOMPARE(closed.size(), 0);
    QCOMPARE(renamed.size(), 0);
    QVERIFY(m_app.harnessWrites().isEmpty());

    // The replacement still has working controls, but requires fresh consent.
    QVERIFY(QMetaObject::invokeMethod(pane, qPrintable(openMethod)));
    QTRY_VERIFY(dialog->property("visible").toBool());
    if (openMethod == QStringLiteral("beginRename")) {
        QObject *const field = childNamed(dialog, QStringLiteral("terminalPaneRenameField"));
        QVERIFY(field);
        QVERIFY(field->setProperty("text", QStringLiteral("Replacement")));
    }
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
    if (openMethod == QStringLiteral("beginRename")) {
        QCOMPARE(renamed.size(), 1);
        QCOMPARE(renamed.constFirst().at(1).toString(), QStringLiteral("Replacement"));
    } else if (openMethod == QStringLiteral("beginSetHarness")) {
        QCOMPARE(m_app.harnessWrites().size(), 1);
        QCOMPARE(m_app.harnessWrites().constFirst().terminalPaneId, QStringLiteral("row-b"));
    } else {
        QCOMPARE(killed.size(), 1);
        QCOMPARE(closed.size(), 1);
    }
}

void TstPaneIdentity::terminalHostStateWaitsForTheBrowserApis_data()
{
    QTest::addColumn<bool>("cancelFocus");
    QTest::newRow("restore-focus") << false;
    QTest::newRow("user-moved-focus") << true;
}

void TstPaneIdentity::terminalHostStateWaitsForTheBrowserApis()
{
    QFETCH(bool, cancelFocus);
    QQmlComponent settingsComponent(m_engine.get());
    settingsComponent.setData(
        "import QtQml\nQtObject { property real terminalFontSize: 17;"
        " property real terminalPixelRatio: 1.25 }",
        QUrl(QStringLiteral("qrc:/tst_paneidentity/terminal-settings.qml")));
    std::unique_ptr<QObject> settings(settingsComponent.create());
    QVERIFY2(settings, qPrintable(settingsComponent.errorString()));
    const QUrl page(QStringLiteral("data:text/html;base64,")
                    + QString::fromLatin1(QByteArray("<!doctype html><title>Delayed APIs</title>")
                                             .toBase64()));
    QObject *const pane = openInShell(
        QStringLiteral("TerminalPaneView.qml"),
        {{QStringLiteral("terminalBundleUrl"), page},
         {QStringLiteral("settingsObject"), QVariant::fromValue(settings.get())},
         {QStringLiteral("paneId"), QStringLiteral("terminal-1")},
         {QStringLiteral("paneActive"), true}});
    QVERIFY(pane);
    QTRY_VERIFY_WITH_TIMEOUT(pane->property("pageLoaded").toBool(), kPageLoadTimeoutMs);
    QCOMPARE(evalJs(pane, QStringLiteral("document.title")), QStringLiteral("Delayed APIs"));
    QVERIFY(QMetaObject::invokeMethod(pane, "acceptFocus"));
    QTest::qWait(150);
    QVERIFY(pane->property("focusPending").toBool());
    QVERIFY(!pane->property("rendererReady").toBool());
    QVERIFY(settings->setProperty("terminalFontSize", 21.0));
    QVERIFY(settings->setProperty("terminalPixelRatio", 1.5));
    if (cancelFocus)
        QVERIFY(pane->setProperty("paneActive", false));
    QCOMPARE(evalJs(pane, QStringLiteral(R"JS(
window.__focusCalls = 0;
window.applyTheme = roles => { window.__theme = roles; };
window.codeharborSetTerminalPreferences = (font, ratio) => {
    window.__preferences = [font, ratio];
};
window.codeharborFocusTerminal = () => { window.__focusCalls++; };
'installed'
)JS")), QStringLiteral("installed"));
    QTRY_VERIFY(pane->property("rendererReady").toBool());
    QVERIFY(waitForJs(pane, QStringLiteral("JSON.stringify(window.__preferences)"),
                      QStringLiteral("[21,1.5]"), kProbeTimeoutMs));
    QCOMPARE(evalJs(pane, QStringLiteral(
                 "String(!!window.__theme && typeof window.__theme.accent === 'string'"
                 " && typeof window.__theme.surfaceSunken === 'string')")),
             QStringLiteral("true"));
    QCOMPARE(evalJs(pane, QStringLiteral("String(window.__focusCalls)")),
             cancelFocus ? QStringLiteral("0") : QStringLiteral("1"));
    QVERIFY(!pane->property("focusPending").toBool());
}

void TstPaneIdentity::keyboardSplitPublicationCoalescesAndCancels_data()
{
    QTest::addColumn<bool>("terminal");
    QTest::newRow("terminal") << true;
    QTest::newRow("viewer") << false;
}

void TstPaneIdentity::keyboardSplitPublicationCoalescesAndCancels()
{
    QFETCH(bool, terminal);
    const QString firstId = terminal ? QStringLiteral("terminal-1") : QStringLiteral("viewer-1");
    const QString secondId = terminal ? QStringLiteral("terminal-2") : QStringLiteral("viewer-2");
    QVariantMap tree = branchNode(QStringLiteral("horizontal"),
                                 {leafNode(firstId), leafNode(secondId)});
    QVERIFY(openRegion(terminal ? QStringLiteral("TerminalRegion.qml")
                                : QStringLiteral("ViewerRegion.qml"), tree, terminal));
    QVERIFY(m_region->setProperty(terminal ? "devSessionId" : "sessionId",
                                  QStringLiteral("dev-identity")));
    QVERIFY(m_region->setProperty("hostStampsWrites", true));
    QTRY_VERIFY(paneWithId(m_region, secondId));
    QTest::qWait(kSettleMs);
    const QList<QObject *> handles = collect(m_region, [](QObject *object) {
        return object->metaObject()->indexOfProperty("keyStep") >= 0;
    });
    QCOMPARE(handles.size(), 1);
    QObject *const handle = handles.constFirst();
    auto *const first = qobject_cast<QQuickItem *>(paneWithId(m_region, firstId));
    auto *const second = qobject_cast<QQuickItem *>(paneWithId(m_region, secondId));
    QVERIFY(first && second);
    const qreal before = first->width();
    QSignalSpy reported(m_region,
                        SIGNAL(splitRatiosAdjustedForSession(QString,double,QVariant,QVariant)));
    QVERIFY(reported.isValid());

    // Deliberately deliver both steps before the event loop can polish.
    QVERIFY(QMetaObject::invokeMethod(handle, "moveBy", Q_ARG(QVariant, 16)));
    QVERIFY(QMetaObject::invokeMethod(handle, "moveBy", Q_ARG(QVariant, 16)));
    QCOMPARE(reported.size(), 0);
    QTRY_COMPARE(reported.size(), 1);
    QTRY_VERIFY(qAbs(first->width() - before - 32) < 0.5);
    QCOMPARE(reported.constFirst().at(0).toString(), QStringLiteral("dev-identity"));
    QCOMPARE(reported.constFirst().at(1).toDouble(), 0.0);
    QVERIFY(asList(reported.constFirst().at(2)).isEmpty());
    const QVariantList ratios = asList(reported.constFirst().at(3));
    QCOMPARE(ratios.size(), 2);
    QVERIFY(qAbs(ratios.at(0).toDouble()
                 - first->width() / (first->width() + second->width())) < 0.001);

    // A republished tree cancels the pending key, rather than filing the new
    // tree's geometry under the key's older stamp/path.
    reported.clear();
    QVERIFY(QMetaObject::invokeMethod(handle, "moveBy", Q_ARG(QVariant, 16)));
    tree.insert(QStringLiteral("ratios"), QVariantList{0.4, 0.6});
    setNode(tree);
    QTest::qWait(kSettleMs);
    QCOMPARE(reported.size(), 0);
    QVERIFY(!handle->property("resizePending").toBool());
    QVERIFY(QMetaObject::invokeMethod(handle, "moveBy", Q_ARG(QVariant, 16)));
    QVERIFY(m_region->setProperty("layoutGeneration", 1.0));
    QTest::qWait(kSettleMs);
    QCOMPARE(reported.size(), 0);
    QVERIFY(!handle->property("resizePending").toBool());
}

void TstPaneIdentity::emptyTerminalRegionDoesNotAllocateUntilSplit()
{
    TerminalFactoryStub factory;
    m_engine->rootContext()->setContextProperty(QStringLiteral("terminalFactory"), &factory);
    const auto restore = qScopeGuard([this] {
        cleanup();
        m_engine->rootContext()->setContextProperty(QStringLiteral("terminalFactory"),
                                                    &m_terminalFactory);
    });
    QVERIFY(openRegion(QStringLiteral("TerminalRegion.qml"),
                       {{QStringLiteral("type"), QStringLiteral("leaf")},
                        {QStringLiteral("paneId"), QString()}}, true));
    QTRY_COMPARE(collect(m_region, isLeafPane).size(), 1);
    QObject *const empty = collect(m_region, isLeafPane).constFirst();
    QVERIFY(empty->property("placeholder").toBool());
    QCOMPARE(m_region->property("devSessionId").toString(), QStringLiteral("dev-identity"));
    QCOMPARE(factory.controllersCreated(), 0);
    QCOMPARE(factory.bridgeRequests(), 0);
    QVERIFY(factory.resolutions().isEmpty());
    QVERIFY(!asObject(empty->property("controller")));
    QVERIFY(!asObject(empty->property("bridge")));
    QVERIFY(!webViewIn(empty));
    QVERIFY(!empty->property("comingUp").toBool());
    const QList<QObject *> headers = collect(empty, isPaneHeader);
    QCOMPARE(headers.size(), 1);
    QVERIFY(!headers.constFirst()->property("busy").toBool());
    QObject *const title = childNamed(empty, QStringLiteral("paneTitle"));
    QObject *const reason = childNamed(empty, QStringLiteral("paneReason"));
    QVERIFY(title && reason);
    QCOMPARE(title->property("text").toString(), QStringLiteral("No terminals open"));
    QVERIFY(reason->property("text").toString().contains(QStringLiteral("empty")));
    const QList<QObject *> watchdogs = collect(empty, [](QObject *object) {
        return object->property("interval").toInt() == 20000;
    });
    QCOMPARE(watchdogs.size(), 1);
    QVERIFY(!watchdogs.constFirst()->property("running").toBool());
    QObject *const connect = childNamed(empty, QStringLiteral("terminalConnectButton"));
    QObject *const rename = childNamed(empty, QStringLiteral("terminalRenameButton"));
    QObject *const close = childNamed(empty, QStringLiteral("terminalCloseButton"));
    QObject *const split = childNamed(empty, QStringLiteral("terminalSplitHorizontalButton"));
    QVERIFY(connect && rename && close && split);
    QVERIFY(!connect->property("visible").toBool());
    QVERIFY(!rename->property("enabled").toBool());
    QVERIFY(!close->property("enabled").toBool());
    QVERIFY(split->property("enabled").toBool());
    QSignalSpy requested(m_region, SIGNAL(splitRequested(QString,QString)));
    QVERIFY(requested.isValid());
    QVERIFY(QMetaObject::invokeMethod(split, "clicked"));
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.constFirst().at(0).toString(), QString());
    QCOMPARE(requested.constFirst().at(1).toString(), QStringLiteral("horizontal"));
    QCOMPARE(factory.controllersCreated(), 0);
    QVERIFY(factory.resolutions().isEmpty());

    // The ordinary server leaf replaces the empty sentinel through the host's
    // split path, restoring normal identity resolution and explicit retry.
    setNode(terminalLeafNode(QStringLiteral("terminal-1"), QStringLiteral("row-a")));
    QTRY_COMPARE(factory.controllersCreated(), 1);
    QCOMPARE(factory.bridgeRequests(), 1);
    QTRY_COMPARE(factory.resolutions().size(), 1);
    QObject *const pane = paneWithId(m_region, QStringLiteral("terminal-1"));
    QVERIFY(pane);
    QVERIFY(!pane->property("placeholder").toBool());
    QCOMPARE(factory.resolutions().constFirst().terminalPaneId, QStringLiteral("row-a"));
    QVERIFY(factory.completeResolution(0, QStringLiteral("real-terminal")));
    QTRY_VERIFY(pane->property("attached").toBool());
    QCOMPARE(factory.attached(), QStringList{QStringLiteral("real-terminal")});
    QVERIFY(QMetaObject::invokeMethod(pane, "detachNow"));
    QObject *const realConnect = childNamed(pane, QStringLiteral("terminalConnectButton"));
    QVERIFY(realConnect);
    QTRY_VERIFY(realConnect->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(realConnect, "clicked"));
    QTRY_COMPARE(factory.attached().size(), 2);
    QCOMPARE(factory.attached().constLast(), QStringLiteral("real-terminal"));
}

// QTEST_MAIN cannot be used: registerUrlScheme() and QtWebEngineQuick::initialize()
// must run BEFORE the QGuiApplication is constructed, exactly as in main.cpp.
int main(int argc, char *argv[])
{
    QStandardPaths::setTestModeEnabled(true);

    ch::ViewerProfiles::registerUrlScheme();
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setOrganizationName(QStringLiteral("CodeHarbor"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    TstPaneIdentity testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_paneidentity.moc"
