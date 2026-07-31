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
#include "SshConnectionPool.h"
#include "TerminalController.h"
#include "TerminalFactory.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"

#include <QtTest>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
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

// The index path a region reports its ratios under, printed so a failure names
// it: [] is the root branch, ["1"] its second child.
QString pathText(const QVariantList &path)
{
    QStringList parts;
    for (const QVariant &step : path)
        parts.append(step.toString());
    return QLatin1Char('[') + parts.join(QLatin1Char(',')) + QLatin1Char(']');
}

QVariantMap leafNode(const QString &paneId, const QString &url = QString())
{
    QVariantMap leaf{{QStringLiteral("paneId"), paneId},
                     {QStringLiteral("children"), QVariantList{}}};
    if (!url.isEmpty())
        leaf.insert(QStringLiteral("url"), url);
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

    // CONTENT (SPEC 4.5). Geometry alone is not a restored Dev Session: the
    // region has to tell the host WHAT each pane has open, or reopening a
    // session brings back the right split with a set of blank panes in it.
    void openingAFileInAPaneIsReportedToTheHost();

    // HEADERS (SPEC 4.3/4.4). Both regions now put an AppPaneHeader INSIDE every
    // pane, which is the one place a header could break the invariant this file
    // exists to defend: a header the region owned would have to be rebuilt to
    // follow a pane across a split, and rebuilding is what destroys the pane.
    void paneHeaderTravelsWithThePaneAcrossASplit();
    // The header is also the pane's focus indicator, so the region's focus has to
    // reach it or the mark is decoration.
    void theFocusedPaneIsTheOnlyOneMarkedActive();

    // ADDRESS BAR (SPEC 7.5). The one way a user can put anything into a viewer
    // pane. Untested it is invisible: the pane still lays out, still reports
    // focus, and simply never opens what was typed into it.
    void theAddressBarOpensARemotePath();
    void theAddressBarOpensAUrlAsGiven();
    void theAddressBarPercentEncodesADelimiterInAFileName();

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

private:
    QObject *openRegion(const QString &file, const QVariantMap &node, bool terminal);
    void setNode(const QVariantMap &node);
    QString evalJs(QObject *paneItem, const QString &script, int timeoutMs = kProbeTimeoutMs);
    bool waitForJs(QObject *paneItem, const QString &script, const QString &needle, int timeoutMs);
    // Wait until the pane's page has loaded; false means Chromium never got
    // there (a machine property — callers QSKIP rather than fail).
    bool waitForPage(QObject *paneItem, const QString &readyProbe, const QString &needle);
    // Click the middle of a pane, the way a user selects it.
    void clickPane(QObject *paneObject);
    // Put `text` in the viewer pane's address field and press Enter on it, the
    // way a user opens something. Driven through the FIELD rather than by calling
    // the pane's function directly, so the field's own Enter wiring is part of
    // what is under test.
    void enterAddress(QObject *paneObject, const QString &text);

    ch::CodeharbordClient m_client;
    ch::ViewerProfiles m_profiles{&m_client};
    ch::ViewerModel m_viewers{&m_client};
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
}

void TstPaneIdentity::cleanup()
{
    m_region = nullptr;
    m_shell.reset();
    QTest::qWait(50);
}

QObject *TstPaneIdentity::openRegion(const QString &file, const QVariantMap &node, bool terminal)
{
    QQmlComponent component(m_engine.get());
    component.setData(QByteArray(kShellQml), QUrl(QStringLiteral("qrc:/tst_paneidentity/shell.qml")));
    if (component.isError()) {
        QTest::qFail(qPrintable(component.errorString()), __FILE__, __LINE__);
        return nullptr;
    }

    QVariantMap props{{QStringLiteral("node"), node}};
    if (terminal) {
        props.insert(QStringLiteral("devSessionId"), QStringLiteral("dev-identity"));
        props.insert(QStringLiteral("workingDir"), QStringLiteral("/tmp"));
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
        QTest::qFail("the region component never loaded", __FILE__, __LINE__);
    return m_region;
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
    // ED15: the per-pane recovery key must be THIS pane's id, never the empty
    // string a pane starts with before its layout id settles (two panes keyed by
    // "" would share one crash-recovery snapshot). QTRY: the id can arrive a beat
    // after the controller is minted.
    QTRY_COMPARE(controller->recoveryId(), QStringLiteral("viewer-1"));

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
    QVERIFY2(waitForPage(original, QString::fromLatin1(kJsScreen), QStringLiteral("")),
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
    QVERIFY2(waitForPage(pane, QString::fromLatin1(kJsScreen), QStringLiteral("")),
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
