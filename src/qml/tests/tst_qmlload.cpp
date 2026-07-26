// Permanent regression gate for the application QML tree (SPEC 4.1/4.5).
//
// The unit suite can be entirely green while the shipped application refuses to
// start, because nothing else in the build ever instantiates the real QML tree.
// This test does exactly that: it mirrors src/app/main.cpp's setup order, loads
// the real qrc Main.qml through a QQmlApplicationEngine and fails on ANY QML
// error or warning. It additionally drives the recursive viewer/terminal split
// trees with a real 3-node tree, so both the "a component may not instantiate
// its own type" recursion fix and the stray-default-node pane it replaced stay
// permanently defended.
//
// Runs headless: the ctest registration pins QT_QPA_PLATFORM=offscreen and the
// software Quick backend (see CMakeLists.txt).

#include "AgentStatusMonitor.h"
#include "AppController.h"
#include "CodeharbordClient.h"
#include "EditorFactory.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"

#include <QtTest>

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQuickControls2/QQuickStyle>
#include <QtWebEngineQuick/QtWebEngineQuick>

#include <memory>
#include <utility>

namespace {

// Resource prefix produced by qt_add_qml_module(URI CodeHarbor) — the very URL
// the shipped binary resolves loadFromModule("CodeHarbor", "Main") to.
constexpr auto kModuleRoot = "qrc:/qt/qml/CodeHarbor/";

QUrl moduleUrl(const QString &file)
{
    return QUrl(QLatin1String(kModuleRoot) + file);
}

// ---------------------------------------------------------------------------
// Warning capture
//
// Two independent nets, because neither alone is complete:
//   * QQmlEngine::warnings  — every QML binding/type/runtime warning the engine
//     raises, including ones produced long after load() returned.
//   * a message handler that fails the gate on EVERY warning-or-worse message
//     the process logs, minus a closed allowlist of known-environmental noise.
//
// The handler deliberately does NOT filter on the CodeHarbor qrc prefix. It used
// to, and that hole is precisely why the Qt 6.9 WebEngine profile deprecation
//
//   <Unknown File>: QML WebEngineProfile: Please use WebEngineProfilePrototype
//   for profile creation from 6.9, ...
//
// sailed past a green suite: qmlWarning() raised from a C++-constructed object
// has no QML file to blame, so the text never mentions our module path and the
// prefix filter never saw it. The polarity is therefore inverted — anything
// logged at warning level while OUR tree is being built is our problem until
// proven otherwise: fail by default, allowlist by exception.
// ---------------------------------------------------------------------------

// Messages this box unavoidably emits under the headless recipe pinned by
// CMakeLists.txt (QT_QPA_PLATFORM=offscreen, QT_QUICK_BACKEND=software,
// QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox --disable-dev-shm-usage).
// Each entry is matched as a SUBSTRING, and each is here because the PLATFORM
// produces it, never CodeHarbor:
//
//   * "QRhiGles2: Failed to create temporary context" /
//     "QRhiGles2: Failed to create context"
//         Qt RHI probes for an OpenGL context at QQuickWindow setup. The
//         offscreen QPA plugin has no GL, so the probe fails and Quick falls
//         back to the software renderer we asked for. Emitted before any
//         CodeHarbor object exists.
//   * "does not support createPlatformVulkanInstance" /
//     "QVulkanInstance: Failed to initialize Vulkan"
//         The same probe, one rung down: after GL fails, Qt tries Vulkan. The
//         offscreen plugin implements no Vulkan entry point either.
//   * "Unable to detect GPU vendor"
//         Tail of the same probe — with neither GL nor Vulkan there is no
//         driver string to read.
//   * "GPU process isn't usable" / "Failed to create GLES3 context" /
//     "Passthrough is not supported" / "Fontconfig error"
//         Chromium's own GPU/font bring-up under --disable-gpu in a container.
//         Emitted from Chromium threads, entirely outside our control.
//
// NOTHING may be added here that a CodeHarbor change could ever produce. An
// entry that could mask our own defect belongs nowhere near this array.
constexpr const char *kEnvironmentalNoise[] = {
    "QRhiGles2: Failed to create temporary context",
    "QRhiGles2: Failed to create context",
    "does not support createPlatformVulkanInstance",
    "QVulkanInstance: Failed to initialize Vulkan",
    "Unable to detect GPU vendor",
    "GPU process isn't usable",
    "Failed to create GLES3 context",
    "Passthrough is not supported",
    "Fontconfig error",
};

bool isEnvironmentalNoise(const QString &msg)
{
    for (const char *known : kEnvironmentalNoise) {
        if (msg.contains(QLatin1String(known)))
            return true;
    }
    return false;
}

QMutex g_logMutex;
QStringList g_loggedQmlWarnings;
QtMessageHandler g_previousHandler = nullptr;

void qmlWarningMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    // QtDebugMsg/QtInfoMsg are chatter by design. QtFatalMsg never returns, so
    // warning and critical are the levels a gate can actually act on.
    if ((type == QtWarningMsg || type == QtCriticalMsg) && !isEnvironmentalNoise(msg)) {
        QMutexLocker locker(&g_logMutex);
        g_loggedQmlWarnings.append(msg);
    }
    if (g_previousHandler)
        g_previousHandler(type, context, msg);
}

QStringList takeLoggedQmlWarnings()
{
    QMutexLocker locker(&g_logMutex);
    return std::exchange(g_loggedQmlWarnings, QStringList());
}

// ---------------------------------------------------------------------------
// Pane identification
//
// Deliberately structural rather than name-based: a leaf pane is any object that
// exposes `paneId` but not `node`. ViewerPane / TerminalPaneView match;
// ViewerRegion / TerminalRegion (which carry `node`) do not. This survives the
// generated "<Type>_QMLTYPE_n" metaobject names.
// ---------------------------------------------------------------------------

bool isLeafPane(const QObject *object)
{
    const QMetaObject *mo = object->metaObject();
    return mo->indexOfProperty("paneId") >= 0 && mo->indexOfProperty("node") < 0;
}

struct PaneInfo {
    QString className;
    QString paneId;
};

// Walks BOTH the QObject child list and the QQuickItem visual child list: QML
// parents loader/repeater content visually, and Qt deliberately skips QObject
// re-parenting in some of those paths, so neither list alone sees the tree.
void collectPanesInto(QObject *root, QSet<const QObject *> &visited, QList<PaneInfo> &out)
{
    if (!root || visited.contains(root))
        return;
    visited.insert(root);

    if (isLeafPane(root))
        out.append({QString::fromLatin1(root->metaObject()->className()), root->property("paneId").toString()});

    const auto objectChildren = root->children();
    for (QObject *child : objectChildren)
        collectPanesInto(child, visited, out);

    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        const auto itemChildren = item->childItems();
        for (QQuickItem *child : itemChildren)
            collectPanesInto(child, visited, out);
    }
}

void collectPanes(QObject *root, QList<PaneInfo> &out)
{
    QSet<const QObject *> visited;
    collectPanesInto(root, visited, out);
}

// Full tree dump, printed whenever a pane assertion fails so the failure is
// diagnosable without a rebuild.
void dumpTree(QObject *root, QSet<const QObject *> &visited, int depth, QStringList &out)
{
    if (!root || visited.contains(root))
        return;
    visited.insert(root);
    out.append(QString(depth * 2, QLatin1Char(' ')) + QString::fromLatin1(root->metaObject()->className())
               + (isLeafPane(root) ? QStringLiteral(" [pane paneId=\"%1\"]").arg(root->property("paneId").toString())
                                   : QString()));
    const auto objectChildren = root->children();
    for (QObject *child : objectChildren)
        dumpTree(child, visited, depth + 1, out);
    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        const auto itemChildren = item->childItems();
        for (QQuickItem *child : itemChildren)
            dumpTree(child, visited, depth + 1, out);
    }
}

QString treeReport(QObject *root)
{
    QSet<const QObject *> visited;
    QStringList lines;
    dumpTree(root, visited, 1, lines);
    return QStringLiteral("object tree:\n") + lines.join(QLatin1Char('\n'));
}

QStringList paneIds(const QList<PaneInfo> &panes)
{
    QStringList ids;
    ids.reserve(panes.size());
    for (const PaneInfo &pane : panes)
        ids.append(pane.paneId);
    ids.sort();
    return ids;
}

QString describePanes(const QList<PaneInfo> &panes)
{
    QStringList parts;
    for (const PaneInfo &pane : panes)
        parts.append(QStringLiteral("%1(paneId=\"%2\")").arg(pane.className, pane.paneId));
    return parts.isEmpty() ? QStringLiteral("<none>") : parts.join(QStringLiteral(", "));
}

} // namespace

// Watches every leaf pane from the moment it enters the tree and records EVERY
// paneId it ever holds. This is what defends the transient-default-child bug
// class: when a child region is instantiated before its `node` is supplied, its
// pane is first bound against the region's DEFAULT node ("viewer-1" /
// "terminal-1") and only re-bound to the real id a moment later. The finished
// tree looks perfect, yet a terminal controller was spun up for a pane nobody
// asked for. Only the id HISTORY distinguishes the two implementations.
//
// QEvent::ChildAdded is useless here: QML parents loader/repeater content with
// QQml_setParent_noEvent() and QQuickItem::setParentItem(), neither of which
// sends a child event. QQuickItem::childrenChanged() IS emitted on every visual
// add/remove, so the recorder latches onto the root before completion and walks
// itself down the tree from inside those (directly connected, therefore
// synchronous) signals — meeting every item as it appears.
class PaneCreationRecorder : public QObject
{
    Q_OBJECT

public:
    // `object` must come from QQmlComponent::beginCreate() so the recorder is in
    // place before completeCreate() instantiates the Loader/Repeater content.
    void attach(QObject *object)
    {
        if (!object || m_attached.contains(object))
            return;
        m_attached.insert(object);
        connect(object, &QObject::destroyed, this, [this](QObject *dead) { m_attached.remove(dead); });

        if (isLeafPane(object)) {
            m_created.append({QString::fromLatin1(object->metaObject()->className()),
                              object->property("paneId").toString()});
            // paneId is a binding; it is still empty at this point and settles
            // during/after completion. Follow its notify signal so every value
            // it ever takes is recorded, transient ones included.
            const QMetaObject *mo = object->metaObject();
            const QMetaProperty property = mo->property(mo->indexOfProperty("paneId"));
            if (property.hasNotifySignal()) {
                connect(object, property.notifySignal(), this,
                        metaObject()->method(metaObject()->indexOfSlot("recordPaneId()")));
            }
        }

        const auto objectChildren = object->children();
        for (QObject *child : objectChildren)
            attach(child);

        if (auto *item = qobject_cast<QQuickItem *>(object)) {
            connect(item, &QQuickItem::childrenChanged, this, [this, item] { rescan(item); });
            const auto itemChildren = item->childItems();
            for (QQuickItem *child : itemChildren)
                attach(child);
        }
    }

    // Every pane object ever seen, described by its birth-time identity.
    const QList<PaneInfo> &created() const { return m_created; }

    // Every non-empty paneId any pane ever carried, sorted and de-duplicated.
    QStringList observedPaneIds() const
    {
        QStringList ids(m_observedIds.cbegin(), m_observedIds.cend());
        ids.sort();
        return ids;
    }

private slots:
    void recordPaneId()
    {
        if (QObject *pane = sender()) {
            const QString id = pane->property("paneId").toString();
            if (!id.isEmpty())
                m_observedIds.insert(id);
        }
    }

private:
    void rescan(QQuickItem *item)
    {
        const auto objectChildren = item->children();
        for (QObject *child : objectChildren)
            attach(child);
        const auto itemChildren = item->childItems();
        for (QQuickItem *child : itemChildren)
            attach(child);
    }

    // Identity only; entries are dropped on destroyed() so a recycled address is
    // recorded again rather than silently masking a stray pane.
    QSet<QObject *> m_attached;
    QList<PaneInfo> m_created;
    QSet<QString> m_observedIds;
};

namespace {

// ---------------------------------------------------------------------------
// Engine fixture — the exact context main.cpp installs.
// ---------------------------------------------------------------------------

class ShellFixture
{
private:
    // Declared FIRST so they are destroyed LAST: the engine (declared below)
    // must go down first, otherwise the QML tree unbinds against dangling
    // context objects and floods the log with "property of null" TypeErrors.
    ch::CodeharbordClient m_client;
    ch::AgentStatusMonitor m_monitor;
    ch::AppController m_controller;
    ch::ViewerProfiles m_profiles;
    ch::ViewerModel m_viewers;
    ch::EditorFactory m_editorFactory;

public:
    QStringList engineWarnings;
    QQmlApplicationEngine engine;

    ShellFixture()
        : m_controller(&m_client)
        , m_profiles(&m_client)
        , m_viewers(&m_client)
        , m_editorFactory(&m_client)
    {
        m_controller.setAgentMonitor(&m_monitor);
        m_viewers.setProfiles(&m_profiles);

        engine.rootContext()->setContextProperty(QStringLiteral("app"), &m_controller);
        engine.rootContext()->setContextProperty(QStringLiteral("viewers"), &m_viewers);
        engine.rootContext()->setContextProperty(QStringLiteral("agentMonitor"), &m_monitor);
        engine.rootContext()->setContextProperty(QStringLiteral("editorFactory"), &m_editorFactory);

        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [this](const QList<QQmlError> &warnings) {
                             for (const QQmlError &error : warnings)
                                 engineWarnings.append(error.toString());
                         });
    }

    // Build the two WebEngine security contexts through the very accessors QML
    // binds to (`WebEngineView.profile: viewers.internalProfile()`).
    //
    // Loading Main.qml is NOT enough to reach them, and that is the hole this
    // exists to close. Every WebEngineView in the tree lives behind an inactive
    // Loader — TerminalPaneView's is a `sourceComponent: Component {}` that only
    // instantiates once a terminal controller exists, EditorPaneView's only once
    // a file is opened — so a bare shell load never evaluates a single
    // `viewers.*Profile()` binding and the profiles are never constructed at
    // all. The Qt 6.9 "use WebEngineProfilePrototype" deprecation is emitted
    // from QQuickWebEngineProfile's CONSTRUCTOR, so the gate that was tightened
    // to catch exactly that class of warning could not see it: the shipped
    // binary logged it on every launch while this test stayed green.
    //
    // Returns false if either accessor yields nothing, so a null profile cannot
    // turn the assertion that follows into a no-op.
    bool buildViewerProfiles()
    {
        return m_viewers.internalProfile() != nullptr
               && m_viewers.externalProfile() != nullptr;
    }

    // Every warning seen so far, from both nets, de-duplicated in order.
    //
    // NON-DESTRUCTIVE, and that is load-bearing rather than tidy. The log net's
    // queue is drained by takeLoggedQmlWarnings(), so what was taken has to be
    // latched here or the second caller sees an empty list. QVERIFY2 evaluates
    // its condition and its message as ordinary function arguments: the
    // condition `allWarnings().isEmpty()` drained the queue, and the report
    // built from the message argument then had nothing left to name. The gate
    // duly failed on the Qt 6.9 WebEngine deprecation and printed
    // "0 QML warning(s):" — caught the regression, told the maintainer nothing.
    QStringList allWarnings()
    {
        for (const QString &logged : takeLoggedQmlWarnings()) {
            if (!m_logged.contains(logged))
                m_logged.append(logged);
        }
        QStringList combined = engineWarnings;
        for (const QString &logged : m_logged) {
            if (!combined.contains(logged))
                combined.append(logged);
        }
        return combined;
    }

    QString warningReport()
    {
        const QStringList warnings = allWarnings();
        QString report = QStringLiteral("%1 QML warning(s):").arg(warnings.size());
        for (const QString &warning : warnings)
            report += QStringLiteral("\n  * ") + warning;
        return report;
    }

private:
    // Everything drained out of the log net so far, so allWarnings() can be
    // asked twice and answer the same both times.
    QStringList m_logged;
};

// Create a region component with `node` supplied up front, with `recorder`
// latched on before the tree completes. Mirrors createWithInitialProperties().
QObject *createRegion(QQmlComponent &component, QQmlContext *context, const QVariantMap &node,
                      PaneCreationRecorder &recorder)
{
    QObject *root = component.beginCreate(context);
    if (!root)
        return nullptr;
    component.setInitialProperties(root, {{QStringLiteral("node"), node}});
    recorder.attach(root);
    component.completeCreate();
    return root;
}

// Give deferred bindings, Component.onCompleted work, incubators and the async
// (transport-less) AppController::refresh() error path a chance to run and, if
// broken, to log.
void settle(int ms = 600)
{
    QTest::qWait(ms);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

} // namespace

class TstQmlLoad : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // 1. The gate: the real application tree must instantiate cleanly.
    void loadsRealApplicationTreeWithoutWarnings();

    // 2. The recursion fix: a branch node must nest into real child panes, and
    //    no pane may ever be built from the region's placeholder default.
    void regionRecursesIntoChildPanes_data();
    void regionRecursesIntoChildPanes();

    // 3. A leaf node must still produce exactly one pane (no recursion regress).
    void regionLeafNodeProducesSinglePane();

    // 4. The contract that makes (2) possible: no node -> no instantiation.
    void regionWithoutNodeCreatesNothing();
};

void TstQmlLoad::init()
{
    takeLoggedQmlWarnings();
    // QTest::qExec() installs its own message handler, replacing whatever main()
    // set up, so the log net must be (re)installed per test and chained to
    // QTest's handler so QWARN output is still produced.
    g_previousHandler = qInstallMessageHandler(qmlWarningMessageHandler);
}

void TstQmlLoad::cleanup()
{
    qInstallMessageHandler(g_previousHandler);
    g_previousHandler = nullptr;
}

void TstQmlLoad::loadsRealApplicationTreeWithoutWarnings()
{
    ShellFixture fixture;

    bool objectCreatedFired = false;
    bool objectCreatedNull = false;
    QObject::connect(&fixture.engine, &QQmlApplicationEngine::objectCreated, &fixture.engine,
                     [&](QObject *object, const QUrl &) {
                         objectCreatedFired = true;
                         if (!object)
                             objectCreatedNull = true;
                     });
    QSignalSpy creationFailed(&fixture.engine, &QQmlApplicationEngine::objectCreationFailed);

    fixture.engine.load(moduleUrl(QStringLiteral("Main.qml")));

    QVERIFY2(objectCreatedFired, "QQmlApplicationEngine::objectCreated never fired for Main.qml");
    QVERIFY2(!objectCreatedNull,
             qPrintable(QStringLiteral("Main.qml failed to instantiate (objectCreated(nullptr)).\n")
                        + fixture.warningReport()));
    QVERIFY2(creationFailed.isEmpty(),
             qPrintable(QStringLiteral("objectCreationFailed was emitted.\n") + fixture.warningReport()));
    QVERIFY2(!fixture.engine.rootObjects().isEmpty(),
             qPrintable(QStringLiteral("engine.rootObjects() is empty.\n") + fixture.warningReport()));

    // The whole point: the shipped shell must reach a live window.
    QObject *root = fixture.engine.rootObjects().constFirst();
    QVERIFY(root != nullptr);
    QVERIFY2(root->property("visible").toBool(), "root ApplicationWindow is not visible");

    settle();

    // SPEC 4.5: the recursive region types start out empty on purpose, so the
    // "always at least one pane" guarantee is now Main.qml's job. Prove Main
    // actually discharges it — one viewer pane and one terminal pane, carrying
    // the documented default ids.
    QList<PaneInfo> panes;
    collectPanes(root, panes);
    QVERIFY2(paneIds(panes)
                     == (QStringList{QStringLiteral("terminal-1"), QStringLiteral("viewer-1")}),
             qPrintable(QStringLiteral("Main.qml did not supply the SPEC 4.5 default panes; found: %1\n%2")
                            .arg(describePanes(panes), treeReport(root))));

    // Bring up the two WebEngine security contexts the shipped panes bind to.
    // They are part of the shell coming up, so a warning raised while building
    // them is a warning the user sees on every launch — and until this call
    // existed the gate never reached them at all (see buildViewerProfiles()).
    QVERIFY2(fixture.buildViewerProfiles(),
             "ViewerModel did not hand back both WebEngine profiles");
    settle(200);

    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

void TstQmlLoad::regionRecursesIntoChildPanes_data()
{
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("orientation");
    QTest::addColumn<QString>("firstId");
    QTest::addColumn<QString>("secondId");
    QTest::addColumn<QString>("defaultId");

    QTest::newRow("viewer") << QStringLiteral("ViewerRegion.qml") << QStringLiteral("horizontal")
                            << QStringLiteral("viewer-left") << QStringLiteral("viewer-right")
                            << QStringLiteral("viewer-1");
    QTest::newRow("terminal") << QStringLiteral("TerminalRegion.qml") << QStringLiteral("vertical")
                              << QStringLiteral("terminal-top") << QStringLiteral("terminal-bottom")
                              << QStringLiteral("terminal-1");
}

void TstQmlLoad::regionRecursesIntoChildPanes()
{
    QFETCH(QString, file);
    QFETCH(QString, orientation);
    QFETCH(QString, firstId);
    QFETCH(QString, secondId);
    QFETCH(QString, defaultId);

    ShellFixture fixture;
    PaneCreationRecorder recorder;

    QQmlComponent component(&fixture.engine, moduleUrl(file));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));

    // One branch with two leaf children (SPEC 4.5): the 3-node tree that a
    // self-instantiating component could never build.
    const QVariantMap first{{QStringLiteral("paneId"), firstId},
                            {QStringLiteral("url"), QString()},
                            {QStringLiteral("children"), QVariantList{}}};
    const QVariantMap second{{QStringLiteral("paneId"), secondId},
                             {QStringLiteral("url"), QString()},
                             {QStringLiteral("children"), QVariantList{}}};
    const QVariantMap branch{{QStringLiteral("orientation"), orientation},
                             {QStringLiteral("children"), QVariantList{first, second}}};

    std::unique_ptr<QObject> region(
        createRegion(component, fixture.engine.rootContext(), branch, recorder));
    QVERIFY2(region != nullptr, qPrintable(component.errorString()));

    // Branch children come up through a url-sourced Loader, so the leaves can
    // land a turn or two after completeCreate().
    const auto livePanes = [&region] {
        QList<PaneInfo> panes;
        collectPanes(region.get(), panes);
        return panes;
    };
    QTRY_VERIFY2(livePanes().size() == 2,
                 qPrintable(QStringLiteral("expected 2 nested panes, found %1: %2\n%3\n%4")
                                .arg(livePanes().size())
                                .arg(describePanes(livePanes()), fixture.warningReport(),
                                     treeReport(region.get()))));

    QStringList expectedIds{firstId, secondId};
    expectedIds.sort();
    QCOMPARE(paneIds(livePanes()), expectedIds);

    // Let anything late — or anything transient — show itself before counting.
    settle(300);
    const QList<PaneInfo> panes = livePanes();
    QCOMPARE(paneIds(panes), expectedIds);

    // Exactly two pane OBJECTS may ever exist: none built and dropped.
    QVERIFY2(recorder.created().size() == 2,
             qPrintable(QStringLiteral("%1 pane object(s) were created for a 2-leaf tree; born as: %2")
                            .arg(recorder.created().size())
                            .arg(describePanes(recorder.created()))));

    // ZERO panes may ever carry the region's documented DEFAULT id. The
    // recursive type only instantiates once a node is assigned, so supplying an
    // explicit 2-leaf tree must never produce a placeholder pane — not even for
    // the microsecond it would take to re-bind it.
    QVERIFY2(!recorder.observedPaneIds().contains(defaultId),
             qPrintable(QStringLiteral("a pane was bound to the DEFAULT id \"%1\". "
                                       "paneIds ever seen: [%2]")
                            .arg(defaultId, recorder.observedPaneIds().join(QStringLiteral(", ")))));
    QVERIFY2(recorder.observedPaneIds() == expectedIds,
             qPrintable(QStringLiteral("unexpected pane identities. ever seen: [%1], expected: [%2]")
                            .arg(recorder.observedPaneIds().join(QStringLiteral(", ")),
                                 expectedIds.join(QStringLiteral(", ")))));

    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

void TstQmlLoad::regionWithoutNodeCreatesNothing()
{
    ShellFixture fixture;

    // The recursive region types default to a null node and MUST stay inert
    // until one is assigned; that is what keeps a recursive child from building
    // a placeholder pane (and, for terminals, a placeholder controller) before
    // its real node arrives. The SPEC 4.5 "always one pane" default is Main's.
    for (const auto *file : {"ViewerRegion.qml", "TerminalRegion.qml"}) {
        PaneCreationRecorder recorder;
        QQmlComponent component(&fixture.engine, moduleUrl(QLatin1String(file)));
        QVERIFY2(!component.isError(), qPrintable(component.errorString()));

        QObject *raw = component.beginCreate(fixture.engine.rootContext());
        QVERIFY2(raw != nullptr, qPrintable(component.errorString()));
        std::unique_ptr<QObject> region(raw);
        recorder.attach(raw);
        component.completeCreate();
        settle(200);

        QList<PaneInfo> panes;
        collectPanes(region.get(), panes);
        QVERIFY2(panes.isEmpty(),
                 qPrintable(QStringLiteral("%1 instantiated %2 pane(s) with no node: %3")
                                .arg(QLatin1String(file))
                                .arg(panes.size())
                                .arg(describePanes(panes))));
        QVERIFY2(recorder.created().isEmpty(),
                 qPrintable(QStringLiteral("%1 transiently built %2 pane(s) with no node: %3")
                                .arg(QLatin1String(file))
                                .arg(recorder.created().size())
                                .arg(describePanes(recorder.created()))));
    }

    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

void TstQmlLoad::regionLeafNodeProducesSinglePane()
{
    ShellFixture fixture;

    struct Case {
        const char *file;
        const char *paneId;
    };
    const Case cases[] = {{"ViewerRegion.qml", "viewer-solo"}, {"TerminalRegion.qml", "terminal-solo"}};

    for (const Case &testCase : cases) {
        QQmlComponent component(&fixture.engine, moduleUrl(QLatin1String(testCase.file)));
        QVERIFY2(!component.isError(), qPrintable(component.errorString()));

        const QVariantMap leaf{{QStringLiteral("paneId"), QLatin1String(testCase.paneId)},
                               {QStringLiteral("children"), QVariantList{}}};
        std::unique_ptr<QObject> region(
            component.createWithInitialProperties({{QStringLiteral("node"), leaf}}));
        QVERIFY2(region != nullptr, qPrintable(component.errorString()));
        settle(100);

        QList<PaneInfo> panes;
        collectPanes(region.get(), panes);
        QVERIFY2(panes.size() == 1,
                 qPrintable(QStringLiteral("%1: expected exactly 1 pane, found %2: %3")
                                .arg(QLatin1String(testCase.file))
                                .arg(panes.size())
                                .arg(describePanes(panes))));
        QCOMPARE(panes.constFirst().paneId, QString(QLatin1String(testCase.paneId)));
    }

    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

// QTEST_MAIN cannot be used: registerUrlScheme() and QtWebEngineQuick::initialize()
// must run BEFORE the QGuiApplication is constructed, exactly as in main.cpp.
int main(int argc, char *argv[])
{
    // Keep UiStateStore's QSettings out of the developer's real config.
    QStandardPaths::setTestModeEnabled(true);

    ch::ViewerProfiles::registerUrlScheme();
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setOrganizationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setApplicationVersion(QStringLiteral(CODEHARBOR_VERSION));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    g_previousHandler = qInstallMessageHandler(qmlWarningMessageHandler);

    TstQmlLoad testCase;
    const int result = QTest::qExec(&testCase, argc, argv);

    qInstallMessageHandler(g_previousHandler);
    return result;
}

#include "tst_qmlload.moc"
