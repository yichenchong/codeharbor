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
#include "SshConnectionPool.h"
#include "TerminalController.h"
#include "TerminalFactory.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"

#include <QtTest>

#include <QElapsedTimer>
#include <QGuiApplication>
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

    // Accessible.name is an attached QML property with no reachable C++
    // accessor, so the reading has to be taken from inside the document.
    function accessibleName(item) { return item ? String(item.Accessible.name) : "" }
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

// The `app` context property, cut down to the one thing a viewer pane reads out
// of it: the ACTIVE Dev Session's repository root. That root is what "outside
// the project" (SPEC 9) is measured against, so without it a pane has nothing
// to be outside OF and asks nothing. Mirrors AppController's property name and
// its NOTIFY-on-session-change shape.
class RepoRootApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString activeSessionRepoRoot READ activeSessionRepoRoot
                       NOTIFY activeSessionChanged)

public:
    QString activeSessionRepoRoot() const { return m_repoRoot; }

    void setActiveSessionRepoRoot(const QString &root)
    {
        if (m_repoRoot == root)
            return;
        m_repoRoot = root;
        emit activeSessionChanged();
    }

signals:
    void activeSessionChanged();

private:
    QString m_repoRoot;
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

public:
    struct Resolution {
        QString devSessionId;
        QString paneName;
        QString terminalPaneId;
        QString workingDir;
    };

    Q_INVOKABLE QObject *create(QObject *)
    {
        // Parented here, so the object has C++ ownership: a QObject* returned
        // to QML with no parent is handed to the JavaScript collector.
        m_controller = new QObject(this);
        return m_controller;
    }
    Q_INVOKABLE QObject *createBridge(QObject *, QObject *) { return nullptr; }
    Q_INVOKABLE bool connected() const { return true; }

    Q_INVOKABLE bool resolveTarget(QObject *, const QString &devSessionId,
                                   const QString &paneName, const QString &terminalPaneId,
                                   const QString &workingDir)
    {
        m_resolutions.append({devSessionId, paneName, terminalPaneId, workingDir});
        return true;
    }

    Q_INVOKABLE bool attach(QObject *, const QString &target, const QString &, int, int)
    {
        m_attached.append(target);
        return true;
    }
    Q_INVOKABLE void detach(QObject *) { }
    Q_INVOKABLE QString targetFor(QObject *) const { return QString(); }
    Q_INVOKABLE void kill(QObject *) { }

    QObject *controller() const { return m_controller; }
    const QList<Resolution> &resolutions() const { return m_resolutions; }
    const QStringList &attached() const { return m_attached; }

signals:
    void error(QObject *controller, const QString &message);
    void targetResolved(QObject *controller, const QString &target);

private:
    QObject *m_controller = nullptr;
    QList<Resolution> m_resolutions;
    QStringList m_attached;
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

public:
    QString fileState() const { return m_fileState; }

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
    Q_INVOKABLE void save(const QString &, const QString &) { }
    Q_INVOKABLE void reportContent(const QString &content) { m_reported.append(content); }
    Q_INVOKABLE void requestReload() { }
    Q_INVOKABLE void ready() { }
    Q_INVOKABLE void setRecoveryId(const QString &id) { m_recoveryId = id; }

    const QStringList &opened() const { return m_opened; }
    const QStringList &reported() const { return m_reported; }
    QString recoveryId() const { return m_recoveryId; }

signals:
    void contentLoaded(const QString &content, const QString &revision);
    void fileStateChanged(const QString &state);
    void readOnlyChanged(bool readOnly);
    void saved(const QString &revision);
    void saveConflict(const QString &currentRevision);
    void saveError(const QString &message);
    void recoveryAvailable(const QString &recoveredContent);

private:
    QString m_fileState = QStringLiteral("clean");
    QStringList m_opened;
    QStringList m_reported;
    QString m_recoveryId;
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

    // CONTENT (SPEC 4.5). Geometry alone is not a restored Dev Session: the
    // region has to tell the host WHAT each pane has open, or reopening a
    // session brings back the right split with a set of blank panes in it.
    void openingAFileInAPaneIsReportedToTheHost();

    // PANE ACTIONS. Every header action must name its own pane, even when the
    // remembered-pane fallback would otherwise choose the first leaf.
    void paneHeaderRequestsNameTheirOwnPane();
    void terminalKillRequiresConfirmation();
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
    void navigationTruncatesForwardHistoryAfterFreshAddress();
    void navigationButtonsAreDisabledAtHistoryEnds();
    void reloadRefreshesNonWebContent();
    void reloadKeepsTheEditorHandlerAlive();
    void homeUsesTheActiveSessionRootAndDisablesWithoutOne();

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
    // Accessible.name of an item, read from inside the harness document (it is
    // an attached QML property with no C++ accessor).
    QString accessibleNameOf(QObject *item);

    ch::CodeharbordClient m_client;
    ch::ViewerProfiles m_profiles{&m_client};
    ch::ViewerModel m_viewers{&m_client};
    // The `app` context property. Empty by default, so every OTHER test sees a
    // pane with no Dev Session behind it, exactly as before this existed.
    RepoRootApp m_app;
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
    QTest::qWait(50);
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

void TstPaneIdentity::terminalKillRequiresConfirmation()
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
    QObject *const killButton = childNamed(second, QStringLiteral("terminalKillButton"));
    QVERIFY2(killButton, "the terminal pane has no kill button");
    QSignalSpy killRequested(m_region, SIGNAL(killTerminalRequested(QString)));
    QVERIFY(killRequested.isValid());

    // Declining the dialog emits no request.
    QVERIFY(QMetaObject::invokeMethod(killButton, "clicked"));
    QObject *const dialog = childNamed(second, QStringLiteral("terminalPaneKillDialog"));
    QVERIFY2(dialog, "the terminal kill button did not open its confirmation dialog");
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTest::qWait(50);
    QCOMPARE(killRequested.size(), 0);

    // Accepting the same dialog emits exactly the named pane id.
    QVERIFY(QMetaObject::invokeMethod(killButton, "clicked"));
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
    QTRY_COMPARE(killRequested.size(), 1);
    QCOMPARE(killRequested.constFirst().constFirst().toString(),
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
    pane->setProperty("url", QUrl(QStringLiteral("file:///srv/repos/app/notes")));
    QTRY_COMPARE(pane->property("repoCheckPath").toString(), insidePath);
    emit m_viewers.pathResolved(insidePath, insidePath, /*insideRepositoryRoot=*/true);
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

    emit m_viewers.pathResolved(outsidePath, outsidePath, /*insideRepositoryRoot=*/false);
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
    emit m_viewers.pathResolved(insidePath, insidePath, /*insideRepositoryRoot=*/true);
    QCOMPARE(pane->property("repoRootState").toString(), QStringLiteral("outside"));

    // ...and back in again.
    pane->setProperty("url", QUrl(QStringLiteral("file:///srv/repos/app/notes")));
    QTRY_COMPARE(pane->property("repoCheckPath").toString(), insidePath);
    emit m_viewers.pathResolved(insidePath, insidePath, /*insideRepositoryRoot=*/true);
    QCOMPARE(pane->property("repoRootState").toString(), QStringLiteral("inside"));
    QVERIFY(!marker->property("visible").toBool());

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
// Naming a pane's remote shell is a server round trip. A user who switches Dev
// Session while it is travelling leaves an answer in flight that describes the
// PREVIOUS session's shell, and ch::TerminalFactory delivers it as nothing but
// (controller, target) — there is no slot in the reply to check. Adopting it
// attaches this pane to the other session's tmux session, which is two panes on
// one shell.
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
    QTest::qWait(80);
    QVERIFY2(factory.resolutions().size() == 1,
             "a second lookup was started beside the one still in flight: their answers carry "
             "nothing but a controller and a target, so neither could then be told apart");

    // ...and the answer to the ABANDONED question lands.
    emit factory.targetResolved(factory.controller(), QStringLiteral("ch-dev-a-terminal-1"));
    QTest::qWait(80);
    QVERIFY2(pane->property("tmuxTarget").toString().isEmpty(),
             qPrintable(QStringLiteral("the pane adopted \"%1\", the shell belonging to the Dev "
                                       "Session it has already left")
                            .arg(pane->property("tmuxTarget").toString())));
    QVERIFY2(factory.attached().isEmpty(), "the pane attached a PTY to the wrong shell");

    // Discarding it is only half the job: nothing else would ever ask again, so
    // the pane has to put its current question instead of stranding itself.
    QTRY_COMPARE(factory.resolutions().size(), 2);
    QCOMPARE(factory.resolutions().constLast().devSessionId, QStringLiteral("dev-b"));
    QCOMPARE(factory.resolutions().constLast().terminalPaneId, QStringLiteral("row-b"));

    emit factory.targetResolved(factory.controller(), QStringLiteral("ch-dev-b-terminal-1"));
    QTRY_COMPARE(pane->property("tmuxTarget").toString(), QStringLiteral("ch-dev-b-terminal-1"));
    QCOMPARE(factory.attached(), QStringList{QStringLiteral("ch-dev-b-terminal-1")});
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
// A layout RELOAD (SPEC 4.5). ch::SessionLayouts republishes a tree and bumps
// its load generation; the region drops every cached pane, because a reloaded
// tree can name the very same pane ids and a stale pane would otherwise be
// handed to a load it does not belong to.
//
// Dropping them is only half of it. Each region in the recursive tree keeps its
// own note of which pane it is showing, and that note survives the pane. The
// host binds `node` and `layoutGeneration` to the same object, so a reload that
// republishes an IDENTICAL tree moves only the generation — no `node` change
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

    // Exactly what a reload of the same layout does: the generation moves and
    // the tree does not.
    m_region->setProperty("layoutGeneration", 1.0);
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
          documentPath.isEmpty() ? QString()
                                 : QStringLiteral("file://") + documentPath}});
    QVERIFY(view);

    QVariant resolved;
    QVERIFY2(QMetaObject::invokeMethod(view, "resolveImagePath",
                                       Q_RETURN_ARG(QVariant, resolved),
                                       Q_ARG(QVariant, reference)),
             "ViewerMarkdownView has no resolveImagePath()");
    QCOMPARE(resolved.toString(), expected);
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

    // Every pin is given back when the view goes away.
    m_shell.reset();
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
