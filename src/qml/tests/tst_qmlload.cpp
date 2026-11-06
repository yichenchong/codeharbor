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
#include "EditorController.h"
#include "ViewerModel.h"
#include "ViewerProfiles.h"
#include "ViewerCommandService.h"
#include "RpcTypes.h"
#include "SessionLayouts.h"
#include "SessionBootstrap.h"
#include "UiStateStore.h"

#include <QtTest>

#include <QColor>
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
#include <QJsonDocument>
#include <QJsonArray>
#include <QJSValue>
#include <QJsonObject>
#include <QQmlError>
#include <QQmlExpression>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
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

#include <memory>
#include <utility>

namespace {

// Resource prefix produced by qt_add_qml_module(URI CodeHarbor) — the very URL
// the shipped binary resolves loadFromModule("CodeHarbor", "Main") to.
constexpr auto kModuleRoot = "qrc:/qt/qml/CodeHarbor/";

QString checkedTemporaryRoot(const QTemporaryDir &directory)
{
    if (!directory.isValid() || directory.path().isEmpty())
        qFatal("Cannot allocate private QML settings/profile root: %s", qPrintable(directory.errorString()));
    return directory.path();
}

QObject *qmlObject(const QVariant &value)
{
    if (QObject *object = value.value<QObject *>())
        return object;
    return value.value<QJSValue>().toQObject();
}

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
    // macOS's offscreen Qt platform may lazily populate its system font
    // aliases while the first QML/WebEngine graph is created. This is host
    // setup noise, not a warning produced by the CodeHarbor QML tree.
    "Populating font family aliases took",
    // Chromium's Windows headless backend reports this informational startup
    // line at warning level even though it explicitly says it is not a warning.
    "VizNullHypothesis is disabled (not a warning)",
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

// The one descendant carrying `name`, over BOTH child lists for the same reason
// collectPanesInto walks both. Null when there is not exactly one, so an
// ambiguous name fails the test instead of picking arbitrarily.
void findNamedInto(QObject *root, const QString &name, QSet<const QObject *> &visited,
                   QList<QObject *> &out)
{
    if (!root || visited.contains(root))
        return;
    visited.insert(root);
    if (root->objectName() == name)
        out.append(root);
    const auto objectChildren = root->children();
    for (QObject *child : objectChildren)
        findNamedInto(child, name, visited, out);
    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        const auto itemChildren = item->childItems();
        for (QQuickItem *child : itemChildren)
            findNamedInto(child, name, visited, out);
    }
}

QObject *findNamed(QObject *root, const QString &name)
{
    QSet<const QObject *> visited;
    QList<QObject *> found;
    findNamedInto(root, name, visited, found);
    return found.size() == 1 ? found.constFirst() : nullptr;
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

// Controls only publication of accepted peer identity. Main's consumers,
// AppController's remaining state and the actual editor graph stay real.
class AcceptedIdentityController : public ch::AppController
{
    Q_OBJECT
    Q_PROPERTY(QString acceptedServerId READ acceptedIdentity NOTIFY acceptedIdentityChanged)

public:
    using ch::AppController::AppController;
    QString acceptedIdentity() const { return m_acceptedIdentity; }
    void setAcceptedIdentity(const QString &identity)
    {
        if (m_acceptedIdentity == identity)
            return;
        m_acceptedIdentity = identity;
        emit acceptedIdentityChanged();
    }

signals:
    void acceptedIdentityChanged();

private:
    QString m_acceptedIdentity;
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
    QTemporaryDir m_stateDir;
    const QString m_stateRoot = checkedTemporaryRoot(m_stateDir);
    ch::CodeharbordClient m_client;
    ch::AgentStatusMonitor m_monitor;
    ch::SessionBootstrap m_bootstrap;
    ch::AppController m_controller;
    ch::ViewerProfiles m_profiles;
    ch::ViewerModel m_viewers;
    ch::EditorFactory m_editorFactory;
    // The agent-facing viewer control seam. Injected exactly as main.cpp injects
    // it, so `app.viewerCommands` is non-null and Main.qml's Connections block is
    // live — without it the block is disabled and every agent command in this
    // file would be a silent no-op that still passed.
    ch::ViewerCommandService m_viewerCommands;

public:
    QStringList engineWarnings;
    QQmlApplicationEngine engine;

    // The agent-facing control seam, so a case can announce a command exactly as
    // codeharbord's notification does.
    ch::ViewerCommandService &viewerCommands() { return m_viewerCommands; }
    ch::CodeharbordClient &client() { return m_client; }
    ch::AppController &controller() { return m_controller; }
    ch::SessionBootstrap &bootstrap() { return m_bootstrap; }

    ShellFixture()
        : m_bootstrap(nullptr, &m_client, &m_monitor)
        , m_controller(&m_client, nullptr, m_stateRoot + QStringLiteral("/ui.ini"))
        , m_profiles(&m_client, m_stateRoot + QStringLiteral("/webengine"))
        , m_viewers(&m_client)
        , m_editorFactory(&m_client)
        , m_viewerCommands(&m_client)
    {
        m_controller.setAgentMonitor(&m_monitor);
        m_controller.setEditorFactory(&m_editorFactory);
        m_viewers.setProfiles(&m_profiles);

        m_controller.setViewerCommands(&m_viewerCommands);
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

    // 5. The Theme singleton must actually RESOLVE. A mis-registered QML
    //    singleton is silent, not loud: see the test body.
    void themeSingletonResolves();

    // 6. The region headers and pane headers are only BUTTONS: the work behind
    //    them belongs to the host, which has to be listening. Nothing else in
    //    the suite notices when it is not.
    void regionRequestsReachTheHost();

    // AGENT VIEWER CONTROL (SPEC 4.3). An AI agent in a terminal pane reaches the
    // viewer region through ch::ViewerCommandService -> Main.qml. Three things are
    // worth defending here, all of them properties of the real Main.qml:
    //
    //   * every command is ANSWERED — an arm that returns without responding
    //     blocks the agent's turn until the daemon times it out;
    //   * a command for another Dev Session is refused rather than applied to the
    //     one on screen;
    //   * a remote path becomes an absolute file:// URL. openPaneTarget() hands
    //     its argument to WebEngine verbatim, so a bare "README.md" left alone is
    //     a relative QML URL and the pane shows nothing.
    void everyAgentCommandIsAnswered();
    void anAgentCommandForAnotherSessionIsRefused();
    void agentPathsBecomeAbsoluteRemoteFileUrls();
    void theAgentPaneInventoryReadsTheLayoutTree();
    void agentCommandsFillAnEmptiedViewerRegion_data();
    void agentCommandsFillAnEmptiedViewerRegion();
    void workspaceExitRequiresExplicitDiscard();
    void windowCloseKeepsDirtyEditorsAlive();
    void authoritativeArchiveRetainsEditorsBeforeSessionNotification_data();
    void authoritativeArchiveRetainsEditorsBeforeSessionNotification();
    void workspaceDiscardRejectsAcceptedPeerAndRetirementChanges_data();
    void workspaceDiscardRejectsAcceptedPeerAndRetirementChanges();
    void connectionPromptsReplaceCoveringSheetsAndRetireOnStateChange();
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

// Every colour and metric in the module comes from the `Theme` singleton, and a
// broken singleton registration is the one QML defect this file's warning nets
// cannot see. `import CodeHarbor` still succeeds without the
// QT_QML_SINGLETON_TYPE source property (or without `pragma Singleton`), and
// `Theme.surface` then evaluates to undefined — which a `color` property reads
// back as an INVALID colour, i.e. black, with nothing logged. The whole window
// would come up in the Basic style's light defaults and every other test here
// would stay green, so the value is asserted rather than merely the resolution.
void TstQmlLoad::themeSingletonResolves()
{
    ShellFixture fixture;

    QQmlComponent component(&fixture.engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import CodeHarbor\n"
                                        "QtObject {\n"
                                        "    readonly property color surface: Theme.surface\n"
                                        "    readonly property int headerHeight: Theme.headerHeight\n"
                                        "    readonly property string monoFamily: Theme.monoFamily\n"
                                        "}\n"),
                      moduleUrl(QStringLiteral("ThemeProbe.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));

    const std::unique_ptr<QObject> probe(component.create(fixture.engine.rootContext()));
    QVERIFY2(probe != nullptr, qPrintable(component.errorString()));

    const QColor surface = probe->property("surface").value<QColor>();
    QVERIFY2(surface.isValid(),
             "Theme.surface did not resolve to a colour: the QML singleton registration in "
             "src/qml/CMakeLists.txt and the `pragma Singleton` in src/qml/Theme.qml disagree");
    QCOMPARE(surface, QColor(QStringLiteral("#1e1e2e")));

    // A metric and a string role too, so the check covers more than one property
    // type reading back a plausible-looking default.
    QVERIFY2(probe->property("headerHeight").toInt() > 0, "Theme.headerHeight resolved to 0");
    QVERIFY2(!probe->property("monoFamily").toString().isEmpty(),
             "Theme.monoFamily resolved to an empty string");

    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
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
    // "always at least one pane" guarantee is Main.qml's job. Prove Main actually
    // discharges it — and with the layout the application now defaults to: ONE
    // viewer pane, and TWO terminal panes (one above the other), which is what a
    // terminal region is for.
    QList<PaneInfo> panes;
    collectPanes(root, panes);
    const QStringList expectedPanes{QStringLiteral("terminal-1"), QStringLiteral("terminal-2"),
                                    QStringLiteral("viewer-1")};
    QVERIFY2(paneIds(panes) == expectedPanes,
             qPrintable(QStringLiteral("Main.qml did not supply the SPEC 4.5 default panes; found: %1\n%2")
                            .arg(describePanes(panes), treeReport(root))));

    // ...and the terminal default is a VERTICAL split, i.e. the two panes are
    // stacked, not side by side. The pane list above cannot tell those apart.
    const QVariantMap terminalFallback = root->property("terminalFallbackNode").toMap();
    QCOMPARE(terminalFallback.value(QStringLiteral("orientation")).toString(),
             QStringLiteral("vertical"));
    const QVariantList fallbackChildren =
            terminalFallback.value(QStringLiteral("children")).toList();
    QCOMPARE(fallbackChildren.size(), 2);
    QCOMPARE(fallbackChildren.at(0).toMap().value(QStringLiteral("paneId")).toString(),
             QStringLiteral("terminal-1"));
    QCOMPARE(fallbackChildren.at(1).toMap().value(QStringLiteral("paneId")).toString(),
             QStringLiteral("terminal-2"));

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

// Every pane header owns its split, close and terminal-kill requests. The
// region relays them to Main.qml, which owns ch::SessionLayouts and the terminal
// cache. Driven by emitting the region's own signals (invoking a signal through
// the meta-object emits it), which is exactly what the pane header relay does.
//
// With no Dev Session loaded — this fixture has no transport, so there is none —
// each command's FIRST act is to refuse and say why in the shell's toast. That
// refusal is the observable proof that the request arrived: an unhandled signal
// leaves the toast untouched.
void TstQmlLoad::regionRequestsReachTheHost()
{
    ShellFixture fixture;
    fixture.engine.load(moduleUrl(QStringLiteral("Main.qml")));
    QVERIFY(!fixture.engine.rootObjects().isEmpty());
    QObject *root = fixture.engine.rootObjects().constFirst();
    settle();

    QObject *viewerRegion = findNamed(root, QStringLiteral("viewerRegion"));
    QObject *terminalRegion = findNamed(root, QStringLiteral("terminalRegion"));
    QVERIFY2(viewerRegion && terminalRegion, "Main.qml no longer names its two regions");
    QObject *toast = findNamed(root, QStringLiteral("shellErrorLabel"));
    QVERIFY2(toast, "the shell has no error toast to report a refusal in");

    struct Case {
        QObject *region;
        const char *signalName;
        QString paneId;
        QString orientation;
        bool hasOrientation;
        const char *expected;
        const char *what;
    };
    const Case cases[] = {
        {viewerRegion, "splitRequested", QStringLiteral("viewer-2"),
         QStringLiteral("horizontal"), true,
         "Select a Dev Session before splitting a pane.", "viewer split"},
        {viewerRegion, "closePaneRequested", QStringLiteral("viewer-2"),
         QString(), false,
         "Select a Dev Session before closing a pane.", "viewer close"},
        {terminalRegion, "splitRequested", QStringLiteral("terminal-2"),
         QStringLiteral("vertical"), true,
         "Select a Dev Session before splitting a pane.", "terminal split"},
        {terminalRegion, "closePaneRequested", QStringLiteral("terminal-2"),
         QString(), false,
         "Select a Dev Session before closing a pane.", "terminal close"},
        {terminalRegion, "killTerminalRequested", QStringLiteral("terminal-2"),
         QString(), false,
         "No active Dev Session.", "terminal kill"},
    };

    for (const Case &testCase : cases) {
        toast->setProperty("text", QString());
        const bool emitted = testCase.hasOrientation
                ? QMetaObject::invokeMethod(testCase.region, testCase.signalName,
                                            Q_ARG(QString, testCase.paneId),
                                            Q_ARG(QString, testCase.orientation))
                : QMetaObject::invokeMethod(testCase.region, testCase.signalName,
                                            Q_ARG(QString, testCase.paneId));
        QVERIFY2(emitted, qPrintable(QStringLiteral("no %1 signal to emit for the %2 request")
                                         .arg(QLatin1String(testCase.signalName),
                                              QLatin1String(testCase.what))));
        settle(80);
        QVERIFY2(toast->property("text").toString() == QLatin1String(testCase.expected),
                 qPrintable(QStringLiteral("the %1 request produced \"%2\"; the host is not "
                                           "handling it (expected \"%3\")")
                                .arg(QLatin1String(testCase.what),
                                     toast->property("text").toString(),
                                     QLatin1String(testCase.expected))));
    }

    // Stamped requests must preserve the pane id alongside the session and
    // layout generation. This catches a handler that still resolves a target
    // through targetPaneId() or silently drops the new argument.
    QSignalSpy splitStamped(
        viewerRegion, SIGNAL(splitRequestedForSession(QString, double, QString, QString)));
    QVERIFY2(splitStamped.isValid(), "viewer split stamp signal is missing");
    QVERIFY(QMetaObject::invokeMethod(
        viewerRegion, "splitRequestedForSession",
        Q_ARG(QString, QStringLiteral("dev-session-7")),
        Q_ARG(double, 42.0),
        Q_ARG(QString, QStringLiteral("viewer-2")),
        Q_ARG(QString, QStringLiteral("vertical"))));
    QCOMPARE(splitStamped.size(), 1);
    const QList<QVariant> splitStamp = splitStamped.constFirst();
    QCOMPARE(splitStamp.at(0).toString(), QStringLiteral("dev-session-7"));
    QCOMPARE(splitStamp.at(1).toDouble(), 42.0);
    QCOMPARE(splitStamp.at(2).toString(), QStringLiteral("viewer-2"));
    QCOMPARE(splitStamp.at(3).toString(), QStringLiteral("vertical"));

    QSignalSpy killStamped(
        terminalRegion, SIGNAL(killTerminalRequestedForSession(QString, double, QString)));
    QVERIFY2(killStamped.isValid(), "terminal kill stamp signal is missing");
    QVERIFY(QMetaObject::invokeMethod(
        terminalRegion, "killTerminalRequestedForSession",
        Q_ARG(QString, QStringLiteral("dev-session-8")),
        Q_ARG(double, 43.0),
        Q_ARG(QString, QStringLiteral("terminal-2"))));
    QCOMPARE(killStamped.size(), 1);
    const QList<QVariant> killStamp = killStamped.constFirst();
    QCOMPARE(killStamp.at(0).toString(), QStringLiteral("dev-session-8"));
    QCOMPARE(killStamp.at(1).toDouble(), 43.0);
    QCOMPARE(killStamp.at(2).toString(), QStringLiteral("terminal-2"));

    // The ratio report has no visible effect without a loaded layout (setRatios
    // is deliberately quiet), so what is asserted here is that the host's handler
    // ACCEPTS it: a handler with the wrong parameter list raises a TypeError on
    // emit, which the warning nets below turn into a failure.
    for (QObject *region : {viewerRegion, terminalRegion}) {
        QVERIFY(QMetaObject::invokeMethod(
            region, "splitRatiosAdjusted",
            Q_ARG(QVariant, QVariant(QVariantList{QStringLiteral("0")})),
            Q_ARG(QVariant, QVariant(QVariantList{0.25, 0.75}))));
    }
    settle(80);

    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

// ---------------------------------------------------------------------------
// Agent viewer control (SPEC 4.3)
// ---------------------------------------------------------------------------

namespace {

// In-process stand-in for the RPC transport, so an agent command can be driven
// the way production drives it — a `viewer.command` notification off the wire —
// and the ANSWER read back off that same wire. Unbuffered keeps dispatch
// synchronous, so no case needs to guess at a delay.
class AgentTransport : public QIODevice
{
public:
    explicit AgentTransport(QObject *parent = nullptr) : QIODevice(parent)
    {
        open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        return m_incoming.size() + QIODevice::bytesAvailable();
    }

    void deliver(const QByteArray &frame)
    {
        m_incoming.append(frame);
        emit readyRead();
    }

    QByteArray takeSent()
    {
        const QByteArray sent = m_sent;
        m_sent.clear();
        return sent;
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 n = qMin<qint64>(maxSize, m_incoming.size());
        if (n > 0) {
            memcpy(data, m_incoming.constData(), static_cast<size_t>(n));
            m_incoming.remove(0, n);
        }
        return n;
    }

    qint64 writeData(const char *data, qint64 len) override
    {
        m_sent.append(data, len);
        return len;
    }

private:
    QByteArray m_incoming;
    QByteArray m_sent;
};

// Cross the same accepted-identity/readiness boundary as production. Neither
// selected IDs nor a manually forced layout-ready flag prove a peer's identity.
bool acceptServer(ch::AppController &app, ch::CodeharbordClient &client,
                  ch::SessionBootstrap &bootstrap, AgentTransport &transport,
                  ch::SessionLayouts *layouts, const QString &serverId)
{
    app.setConnection(nullptr, &bootstrap, nullptr, layouts);
    client.setTransport(&transport);
    bootstrap.stateChanged(ch::SessionBootstrap::State::Wired);
    bootstrap.wired();
    int answered = 0;
    for (const QByteArray &line : transport.takeSent().split('\n')) {
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        if (request.value(QStringLiteral("method")) != QStringLiteral("server.info"))
            continue;
        const QJsonObject info{
            {QStringLiteral("name"), QStringLiteral("codeharbord")},
            {QStringLiteral("version"), QStringLiteral(CODEHARBOR_VERSION)},
            {QStringLiteral("schemaVersion"), ch::AppController::kMinimumServerSchemaVersion},
            {QStringLiteral("serverId"), serverId},
            {QStringLiteral("controlSocket"), QStringLiteral("/run/codeharbor/control-fixture.sock")},
            {QStringLiteral("agentSocket"), QStringLiteral("/run/codeharbor/agent-fixture.sock")},
            {QStringLiteral("recoveryDir"), QString()}};
        transport.deliver(QJsonDocument(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), request.value(QStringLiteral("id"))},
            {QStringLiteral("result"), info}}).toJson(QJsonDocument::Compact) + '\n');
        ++answered;
    }
    return answered == 1 && app.serverId() == serverId
        && app.acceptedServerId() == serverId && (!layouts || layouts->workspaceReady());
}

bool acceptFixtureServer(ShellFixture &fixture, AgentTransport &transport,
                         ch::SessionLayouts *layouts, const QString &serverId)
{
    return acceptServer(fixture.controller(), fixture.client(), fixture.bootstrap(),
                        transport, layouts, serverId);
}

QJsonArray fixtureWorkspace(const QString &serverId, const QString &sessionId, bool archived = false)
{
    const QJsonObject session{
        {QStringLiteral("id"), sessionId}, {QStringLiteral("serverId"), serverId},
        {QStringLiteral("groupId"), QStringLiteral("group")},
        {QStringLiteral("name"), sessionId}, {QStringLiteral("repositoryRoot"), QString()},
        {QStringLiteral("defaultWorkingDirectory"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("taskDescription"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("position"), 0}, {QStringLiteral("archived"), archived},
        {QStringLiteral("pinned"), false}, {QStringLiteral("viewerPanes"), QJsonArray{}},
        {QStringLiteral("terminalPanes"), QJsonArray{}}, {QStringLiteral("layouts"), QJsonObject{}}};
    return QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("group")}, {QStringLiteral("serverId"), serverId},
        {QStringLiteral("name"), QStringLiteral("Group")}, {QStringLiteral("position"), 0},
        {QStringLiteral("collapsed"), false}, {QStringLiteral("sessions"), QJsonArray{session}}}};
}

// What Main.qml answered one command with.
struct AgentAnswer {
    bool answered = false;
    bool ok = false;
    QString code;
    QVariantMap data;
};

// Announce one command exactly as codeharbord does and read back the answer.
//
// Driven through the REAL notification path, not by emitting the service's signal
// by hand: only the notification registers the command as in flight, and without
// that registration a duplicate or missing answer would be indistinguishable from
// a correct one.
AgentAnswer askViewer(AgentTransport &transport, const QString &devSessionId,
                      const QString &op, const QVariantMap &args)
{
    static int counter = 0;
    const QString commandId = QStringLiteral("qml-vc-%1").arg(++counter);
    transport.takeSent();

    QJsonObject params;
    params.insert(QStringLiteral("commandId"), commandId);
    params.insert(QStringLiteral("devSessionId"), devSessionId);
    params.insert(QStringLiteral("terminalId"), QStringLiteral("term-1"));
    params.insert(QStringLiteral("op"), op);
    params.insert(QStringLiteral("args"), QJsonObject::fromVariantMap(args));
    QJsonObject frame;
    frame.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    frame.insert(QStringLiteral("method"),
                 QString::fromLatin1(ch::rpc::kNotificationViewerCommand));
    frame.insert(QStringLiteral("params"), params);
    transport.deliver(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
    settle(80);

    AgentAnswer answer;
    for (const QByteArray &line : transport.takeSent().split('\n')) {
        if (line.trimmed().isEmpty())
            continue;
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        if (request.value(QStringLiteral("method")).toString()
            != QString::fromLatin1(ch::rpc::kMethodViewerCommandResult)) {
            continue;
        }
        const QJsonObject result = request.value(QStringLiteral("params")).toObject();
        if (result.value(QStringLiteral("commandId")).toString() != commandId)
            continue;
        // A SECOND answer for the same command would mean a QML arm fell through;
        // the service drops it, but this catches it here where the arm lives.
        Q_ASSERT(!answer.answered);
        answer.answered = true;
        answer.ok = result.value(QStringLiteral("ok")).toBool();
        answer.code = result.value(QStringLiteral("error")).toObject()
                          .value(QStringLiteral("code")).toString();
        answer.data = result.value(QStringLiteral("data")).toObject().toVariantMap();
    }
    return answer;
}

} // namespace

void TstQmlLoad::agentCommandsFillAnEmptiedViewerRegion_data()
{
    QTest::addColumn<QString>("operation");
    QTest::addColumn<bool>("newPane");
    QTest::newRow("open") << QStringLiteral("open") << false;
    QTest::newRow("open-new-pane") << QStringLiteral("open") << true;
    QTest::newRow("split") << QStringLiteral("split") << false;
}

void TstQmlLoad::agentCommandsFillAnEmptiedViewerRegion()
{
    QFETCH(QString, operation);
    QFETCH(bool, newPane);
    ShellFixture fixture;
    AgentTransport transport;
    auto &controller = fixture.controller();
    auto *layouts = new ch::SessionLayouts(controller.workspaceDb(), controller.uiState(),
                                           &controller);
    QVERIFY(acceptFixtureServer(fixture, transport, layouts, QStringLiteral("placeholder-test")));
    controller.refresh();
    const auto deliverResult = [&transport](const QJsonValue &id, const QJsonValue &result) {
        transport.deliver(QJsonDocument(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id}, {QStringLiteral("result"), result}})
                              .toJson(QJsonDocument::Compact) + '\n');
    };
    const QJsonArray groups = fixtureWorkspace(controller.serverId(), QStringLiteral("session"));
    for (const QByteArray &line : transport.takeSent().split('\n')) {
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        if (request.value(QStringLiteral("method")) == QStringLiteral("workspace.list"))
            deliverResult(request.value(QStringLiteral("id")), groups);
    }
    controller.activateSession(QStringLiteral("session"));
    QCOMPARE(controller.activeSessionId(), QStringLiteral("session"));
    const QJsonObject placeholder{{QStringLiteral("type"), QStringLiteral("leaf")},
                                  {QStringLiteral("paneId"), QString()}};
    for (const QByteArray &line : transport.takeSent().split('\n')) {
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        if (request.value(QStringLiteral("method")) == QStringLiteral("workspace.getLayout"))
            deliverResult(request.value(QStringLiteral("id")),
                          QJsonObject{{QStringLiteral("id"), QStringLiteral("layout")},
                                      {QStringLiteral("tree"), placeholder}});
    }
    QVERIFY(layouts->viewerTree().isValid());
    QCOMPARE(layouts->viewerTree().toMap(), placeholder.toVariantMap());
    fixture.engine.load(moduleUrl(QStringLiteral("Main.qml")));
    QVERIFY(!fixture.engine.rootObjects().isEmpty());
    settle();
    QVariantMap args{{QStringLiteral("url"), QStringLiteral("/target.bin")},
                     {QStringLiteral("kind"), QStringLiteral("binary")},
                     {QStringLiteral("orientation"), QStringLiteral("horizontal")},
                     {QStringLiteral("newPane"), newPane}};
    QVariantMap missing = args;
    missing.insert(QStringLiteral("pane"), QStringLiteral("viewer-missing"));
    const AgentAnswer refused = askViewer(transport, QStringLiteral("session"), operation, missing);
    QVERIFY(refused.answered);
    QVERIFY(!refused.ok);
    QCOMPARE(refused.code, QStringLiteral("unknown_pane"));
    QCOMPARE(layouts->viewerTree().toMap(), placeholder.toVariantMap());

    const AgentAnswer answer = askViewer(transport, QStringLiteral("session"), operation, args);
    QVERIFY(answer.answered);
    QVERIFY2(answer.ok, qPrintable(answer.code));
    const QString created = answer.data.value(QStringLiteral("paneId")).toString();
    QVERIFY(!created.isEmpty());
    const QVariantMap tree = layouts->viewerTree().toMap();
    QCOMPARE(tree.value(QStringLiteral("paneId")).toString(), created);
    QVERIFY(tree.value(QStringLiteral("children")).toList().isEmpty());
    if (operation == QLatin1String("open"))
        QCOMPARE(tree.value(QStringLiteral("url")).toString(), QStringLiteral("file:///target.bin"));
    const AgentAnswer inventory = askViewer(transport, QStringLiteral("session"),
                                             QStringLiteral("list"), {});
    QVERIFY(inventory.ok);
    const QVariantList panes = inventory.data.value(QStringLiteral("panes")).toList();
    QCOMPARE(panes.size(), 1);
    QCOMPARE(panes.constFirst().toMap().value(QStringLiteral("paneId")).toString(), created);
    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

void TstQmlLoad::authoritativeArchiveRetainsEditorsBeforeSessionNotification_data()
{
    QTest::addColumn<bool>("closeWithoutActiveSession");
    QTest::newRow("palette-close-without-active-session") << true;
    QTest::newRow("retention-across-new-owner") << false;
}

void TstQmlLoad::authoritativeArchiveRetainsEditorsBeforeSessionNotification()
{
    QFETCH(bool, closeWithoutActiveSession);
    ShellFixture fixture;
    AgentTransport transport;
    auto &app = fixture.controller();
    auto *layouts = new ch::SessionLayouts(app.workspaceDb(), app.uiState(), &app);
    QVERIFY(acceptFixtureServer(fixture, transport, layouts, QStringLiteral("archive-retirement")));
    const auto deliverResult = [&transport](const QJsonValue &id, const QJsonValue &result) {
        transport.deliver(QJsonDocument(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id}, {QStringLiteral("result"), result}})
                              .toJson(QJsonDocument::Compact) + '\n');
    };
    const auto publishWorkspace = [&](const QString &sessionId, bool archived) {
        app.refresh();
        bool answered = false;
        const QJsonArray groups = fixtureWorkspace(app.serverId(), sessionId, archived);
        for (const QByteArray &line : transport.takeSent().split('\n')) {
            const QJsonObject request = QJsonDocument::fromJson(line).object();
            if (request.value(QStringLiteral("method")).toString() == QStringLiteral("workspace.list")) {
                answered = true;
                deliverResult(request.value(QStringLiteral("id")), groups);
            }
        }
        return answered;
    };
    const QJsonObject placeholder{{QStringLiteral("type"), QStringLiteral("leaf")},
                                  {QStringLiteral("paneId"), QString()}};
    const auto publishLayouts = [&](const QString &viewerUrl) {
        int answered = 0;
        for (const QByteArray &line : transport.takeSent().split('\n')) {
            const QJsonObject request = QJsonDocument::fromJson(line).object();
            if (request.value(QStringLiteral("method")).toString() != QStringLiteral("workspace.getLayout"))
                continue;
            const bool viewer = request.value(QStringLiteral("params")).toObject()
                .value(QStringLiteral("region")).toString() == QStringLiteral("viewer");
            const QJsonObject tree = viewer ? QJsonObject{
                {QStringLiteral("type"), QStringLiteral("leaf")},
                {QStringLiteral("paneId"), QStringLiteral("viewer-1")},
                {QStringLiteral("url"), viewerUrl}, {QStringLiteral("children"), QJsonArray{}}}
                : placeholder;
            deliverResult(request.value(QStringLiteral("id")),
                          QJsonObject{{QStringLiteral("id"), QStringLiteral("layout")},
                                      {QStringLiteral("tree"), tree}});
            ++answered;
        }
        return answered;
    };
    QVERIFY(publishWorkspace(QStringLiteral("session-old"), false));
    app.activateSession(QStringLiteral("session-old"));
    QCOMPARE(app.activeSessionId(), QStringLiteral("session-old"));
    QCOMPARE(publishLayouts(QStringLiteral("file:///old/retained.txt")), 2);
    fixture.engine.load(moduleUrl(QStringLiteral("Main.qml")));
    QVERIFY(!fixture.engine.rootObjects().isEmpty());
    QObject *root = fixture.engine.rootObjects().constFirst();
    QObject *region = findNamed(root, QStringLiteral("viewerRegion"));
    QVERIFY(region);
    fixture.engine.rootContext()->setContextProperty(QStringLiteral("guardWindow"), root);
    fixture.engine.rootContext()->setContextProperty(QStringLiteral("guardRegion"), region);
    const auto evaluate = [&](const QString &script) {
        QQmlExpression expression(fixture.engine.rootContext(), root, script);
        const QVariant value = expression.evaluate();
        if (expression.hasError())
            fixture.engineWarnings.append(expression.error().toString());
        return value;
    };
    settle();
    QObject *pane = nullptr;
    QTRY_VERIFY((pane = qmlObject(evaluate(QStringLiteral("guardRegion.paneCache['viewer-1']")))));
    QObject *editor = nullptr;
    ch::EditorController *controller = nullptr;
    QTRY_VERIFY((editor = qmlObject(pane->property("documentObject"))));
    QTRY_VERIFY((controller = qobject_cast<ch::EditorController *>(
        qmlObject(pane->property("documentController")))));
    auto *paneItem = qobject_cast<QQuickItem *>(pane);
    QVERIFY(paneItem);
    QTRY_VERIFY(paneItem->width() > 0 && paneItem->height() > 0);
    // Complete a real editable read, watch and permission response before
    // editing. An error/no-transport buffer is not an accepted native document.
    int fileReads = 0;
    for (const QByteArray &line : transport.takeSent().split('\n')) {
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        if (request.value(QStringLiteral("method")) != QStringLiteral("file.readFile"))
            continue;
        deliverResult(request.value(QStringLiteral("id")), QJsonObject{
            {QStringLiteral("path"), QStringLiteral("/old/retained.txt")},
            {QStringLiteral("encoding"), QStringLiteral("utf-8")},
            {QStringLiteral("content"), QStringLiteral("original native document")},
            {QStringLiteral("revision"), QStringLiteral("fixture-revision-1")},
            {QStringLiteral("truncated"), false}});
        ++fileReads;
    }
    QCOMPARE(fileReads, 1);
    int fileWatches = 0;
    int fileStats = 0;
    for (const QByteArray &line : transport.takeSent().split('\n')) {
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        const QString method = request.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("file.watch")) {
            deliverResult(request.value(QStringLiteral("id")), QJsonObject{
                {QStringLiteral("subscriptionId"), QStringLiteral("fixture-watch-1")}});
            ++fileWatches;
        } else if (method == QStringLiteral("file.stat")) {
            deliverResult(request.value(QStringLiteral("id")), QJsonObject{
                {QStringLiteral("path"), QStringLiteral("/old/retained.txt")},
                {QStringLiteral("kind"), QStringLiteral("file")},
                {QStringLiteral("size"), 24}, {QStringLiteral("mode"), 0644},
                {QStringLiteral("writable"), true},
                {QStringLiteral("revision"), QStringLiteral("fixture-revision-1")}});
            ++fileStats;
        }
    }
    QCOMPARE(fileWatches, 1);
    QCOMPARE(fileStats, 1);
    QCOMPARE(controller->fileState(), QStringLiteral("clean"));
    QVERIFY(!controller->readOnly());
    QCOMPARE(controller->revision(), QStringLiteral("fixture-revision-1"));
    controller->reportDirty(true, controller->revision());
    QVERIFY(controller->dirty());
    QVERIFY(pane->property("contentDirty").toBool());
    QPointer<QObject> retainedPane(pane);
    QPointer<QObject> retainedEditor(editor);
    QPointer<ch::EditorController> retainedController(controller);
    evaluate(QStringLiteral(
        "guardWindow.requestWorkspaceAction(function() { guardWindow.title = 'obsolete-workspace-action'; })"));
    QVERIFY(qmlObject(root->property("workspaceDialog")));
    QStringList retirementOrder;
    QObject retirementObserver;
    QObject::connect(layouts, &ch::SessionLayouts::viewerTreeChanged, &retirementObserver, [&] {
        if (layouts->viewerTree().isNull())
            retirementOrder.append(QStringLiteral("tree-cleared"));
    });
    QObject::connect(&app, &ch::AppController::activeSessionChanged, &retirementObserver, [&] {
        if (app.activeSessionId().isEmpty())
            retirementOrder.append(QStringLiteral("session-notified"));
    });
    QVERIFY(publishWorkspace(QStringLiteral("session-old"), true));
    QVERIFY(app.activeSessionId().isEmpty());
    QVERIFY(retirementOrder.indexOf(QStringLiteral("tree-cleared")) >= 0);
    QVERIFY(retirementOrder.indexOf(QStringLiteral("tree-cleared"))
            < retirementOrder.indexOf(QStringLiteral("session-notified")));
    QTRY_VERIFY(region->property("contextRetired").toBool());
    QVERIFY(!retainedPane.isNull() && !retainedEditor.isNull() && !retainedController.isNull());
    QVERIFY(editor->property("serverContextRetired").toBool());
    QCOMPARE(pane->property("url").toUrl(), QUrl(QStringLiteral("file:///old/retained.txt")));
    QTRY_VERIFY(paneItem->height() > 0);
    // Native detachment preserves a locally editable document, not a read-only
    // replacement. Only exact old-watch cleanup may cross the server boundary.
    int detachedUnwatches = 0;
    for (const QByteArray &line : transport.takeSent().split('\n')) {
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        const QString method = request.value(QStringLiteral("method")).toString();
        if (!method.startsWith(QStringLiteral("file.")))
            continue;
        QCOMPARE(method, QStringLiteral("file.unwatch"));
        QCOMPARE(request.value(QStringLiteral("params")).toObject()
                     .value(QStringLiteral("subscriptionId")).toString(),
                 QStringLiteral("fixture-watch-1"));
        deliverResult(request.value(QStringLiteral("id")), QJsonObject{});
        ++detachedUnwatches;
    }
    QCOMPARE(detachedUnwatches, 1);
    QVERIFY(!controller->readOnly());
    controller->reportDirty(false, controller->revision());
    QVERIFY(!controller->dirty());
    controller->reportContent(QStringLiteral("local edit after authoritative retirement"));
    QVERIFY(controller->dirty());
    QSignalSpy detachedSaveErrors(controller, &ch::EditorController::saveError);
    controller->save(QStringLiteral("local edit after authoritative retirement"), controller->revision());
    QCOMPARE(detachedSaveErrors.size(), 1);
    QVERIFY(transport.takeSent().isEmpty());
    // The old modal describes the retired workspace, not the current empty
    // selection. Its stale acceptance must not destroy the retained RAM.
    evaluate(QStringLiteral(
        "(function() { var dialog = guardWindow.workspaceDialog;"
        "dialog.standardButton(dialog.standardButtons & ~dialog.defaultButton).clicked(); })()"));
    QVERIFY(root->property("title").toString() != QStringLiteral("obsolete-workspace-action"));
    QVERIFY(!retainedPane.isNull() && !retainedController.isNull());
    QTRY_VERIFY(!qmlObject(root->property("workspaceDialog")));

    if (closeWithoutActiveSession) {
        QVERIFY(app.activeSessionId().isEmpty());
        transport.takeSent();
        QSignalSpy remoteLayoutChange(layouts, &ch::SessionLayouts::viewerTreeChanged);
        evaluate(QStringLiteral(
            "(function() { var command = guardWindow.paletteCommands.find("
            "item => item.id === 'pane.close.viewer');"
            "if (!command) throw new Error('missing viewer close command');"
            "command.invoke(); })()"));
        QObject *closeDialog = findNamed(root, QStringLiteral("viewerCloseUnsavedDialog"));
        QVERIFY(closeDialog && closeDialog->property("visible").toBool());
        QVERIFY(app.activeSessionId().isEmpty());
        QVERIFY(!retainedPane.isNull() && !retainedController.isNull());
        QVERIFY(QMetaObject::invokeMethod(closeDialog, "accept"));
        QTRY_VERIFY(retainedPane.isNull());
        QTRY_VERIFY(retainedEditor.isNull());
        QTRY_VERIFY(retainedController.isNull());
        QTRY_VERIFY(!region->property("contextRetired").toBool());
        QVERIFY(app.activeSessionId().isEmpty());
        QVERIFY(layouts->viewerTree().isNull());
        QCOMPARE(remoteLayoutChange.size(), 0);
        for (const QByteArray &line : transport.takeSent().split('\n')) {
            const QString method = QJsonDocument::fromJson(line).object()
                .value(QStringLiteral("method")).toString();
            QVERIFY(method != QStringLiteral("workspace.setLayout"));
            QVERIFY(method != QStringLiteral("workspace.deleteViewerPane"));
        }
        QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
        return;
    }

    QVERIFY(publishWorkspace(QStringLiteral("session-new"), false));
    app.activateSession(QStringLiteral("session-new"));
    QCOMPARE(publishLayouts(QStringLiteral("file:///new/current.bin")), 2);
    QCOMPARE(app.activeSessionId(), QStringLiteral("session-new"));
    const QVariant currentAuthority = layouts->viewerTree();
    app.uiState()->setSelectedPane(QStringLiteral("session-new"), QStringLiteral("new-owner-choice"));
    QVERIFY(QMetaObject::invokeMethod(region, "noteFocus",
                                      Q_ARG(QVariant, QStringLiteral("viewer-1"))));
    QVERIFY(QMetaObject::invokeMethod(root, "armFocusRestore"));
    QVERIFY(QMetaObject::invokeMethod(root, "tryRestoreFocus"));
    QCOMPARE(app.uiState()->selectedPane(QStringLiteral("session-new")),
             QStringLiteral("new-owner-choice"));
    const AgentAnswer inventory = askViewer(transport, QStringLiteral("session-new"),
                                             QStringLiteral("list"), {});
    QVERIFY(inventory.ok);
    QCOMPARE(inventory.data.value(QStringLiteral("focusedPaneId")).toString(), QString());
    const QVariantList panes = inventory.data.value(QStringLiteral("panes")).toList();
    QCOMPARE(panes.size(), 1);
    QCOMPARE(panes.constFirst().toMap().value(QStringLiteral("url")).toString(),
             QStringLiteral("file:///new/current.bin"));
    for (const QString &op : {QStringLiteral("focus"), QStringLiteral("open"),
                              QStringLiteral("reload"), QStringLiteral("close"),
                              QStringLiteral("split")}) {
        const AgentAnswer answer = askViewer(transport, QStringLiteral("session-new"), op,
            {{QStringLiteral("pane"), QStringLiteral("viewer-1")},
             {QStringLiteral("url"), QStringLiteral("/new/requested.bin")},
             {QStringLiteral("orientation"), QStringLiteral("horizontal")}});
        QVERIFY(answer.answered);
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, QStringLiteral("unsaved_changes"));
        QCOMPARE(layouts->viewerTree(), currentAuthority);
        QCOMPARE(qmlObject(evaluate(QStringLiteral("guardRegion.paneCache['viewer-1']"))), pane);
    }
    transport.takeSent();
    QSignalSpy saveErrors(controller, &ch::EditorController::saveError);
    controller->save(QStringLiteral("retired text"), controller->revision());
    QCOMPARE(saveErrors.size(), 1);
    QVERIFY(transport.takeSent().isEmpty());
    QSignalSpy remoteLayoutChange(layouts, &ch::SessionLayouts::viewerTreeChanged);
    QVERIFY(QMetaObject::invokeMethod(pane, "requestClose"));
    QObject *closeDialog = findNamed(root, QStringLiteral("viewerCloseUnsavedDialog"));
    QVERIFY(closeDialog && closeDialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(closeDialog, "accept"));
    QTRY_VERIFY(retainedPane.isNull());
    QTRY_VERIFY(retainedEditor.isNull());
    QTRY_VERIFY(retainedController.isNull());
    QTRY_VERIFY(!region->property("contextRetired").toBool());
    QCOMPARE(layouts->viewerTree(), currentAuthority);
    QCOMPARE(remoteLayoutChange.size(), 0);
    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

void TstQmlLoad::workspaceDiscardRejectsAcceptedPeerAndRetirementChanges_data()
{
    QTest::addColumn<QString>("nextAcceptedIdentity");
    QTest::addColumn<bool>("retireBeforeAcceptance");
    QTest::newRow("accepted-peer-replaced") << QStringLiteral("server-B") << true;
    QTest::newRow("accepted-peer-cleared-retired") << QString() << true;
    QTest::newRow("accepted-peer-cleared-reconnect-gap") << QString() << false;
    QTest::newRow("same-peer-document-retired") << QStringLiteral("server-A") << true;
}

void TstQmlLoad::workspaceDiscardRejectsAcceptedPeerAndRetirementChanges()
{
    QFETCH(QString, nextAcceptedIdentity);
    QFETCH(bool, retireBeforeAcceptance);
    QTemporaryDir directory;
    const QString privateRoot = checkedTemporaryRoot(directory);
    AgentTransport transport;
    ch::CodeharbordClient client;
    ch::AgentStatusMonitor monitor;
    ch::SessionBootstrap bootstrap(nullptr, &client, &monitor);
    AcceptedIdentityController app(&client, nullptr, privateRoot + QStringLiteral("/ui.ini"));
    app.setAgentMonitor(&monitor);
    ch::ViewerProfiles profiles(&client, privateRoot + QStringLiteral("/webengine"));
    ch::ViewerModel viewers(&client);
    viewers.setProfiles(&profiles);
    ch::EditorFactory factory(&client);
    ch::ViewerCommandService commands(&client);
    app.setViewerCommands(&commands);
    app.setEditorFactory(&factory);
    QVERIFY(acceptServer(app, client, bootstrap, transport, nullptr, QStringLiteral("server-A")));
    QCOMPARE(app.acceptedServerId(), QStringLiteral("server-A"));
    // Only the later accepted-property publication is controlled. The initial
    // peer and native file I/O cross real AppController/server.info boundaries.
    app.setAcceptedIdentity(app.acceptedServerId());
    const auto deliverResult = [&transport](const QJsonValue &id, const QJsonValue &result) {
        transport.deliver(QJsonDocument(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id}, {QStringLiteral("result"), result}})
                              .toJson(QJsonDocument::Compact) + '\n');
    };
    int workspaceLists = 0;
    for (const QByteArray &line : transport.takeSent().split('\n')) {
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        if (request.value(QStringLiteral("method")) != QStringLiteral("workspace.list"))
            continue;
        deliverResult(request.value(QStringLiteral("id")), QJsonArray{});
        ++workspaceLists;
    }
    QCOMPARE(workspaceLists, 1);
    QStringList engineWarnings;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &app);
    engine.rootContext()->setContextProperty(QStringLiteral("viewers"), &viewers);
    engine.rootContext()->setContextProperty(QStringLiteral("agentMonitor"), &monitor);
    engine.rootContext()->setContextProperty(QStringLiteral("editorFactory"), &factory);
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&engineWarnings](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            engineWarnings.append(warning.toString());
    });
    engine.load(moduleUrl(QStringLiteral("Main.qml")));
    QVERIFY(!engine.rootObjects().isEmpty());
    QObject *root = engine.rootObjects().constFirst();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QObject *region = findNamed(root, QStringLiteral("viewerRegion"));
    QVERIFY(window && region);
    engine.rootContext()->setContextProperty(QStringLiteral("guardWindow"), root);
    engine.rootContext()->setContextProperty(QStringLiteral("guardRegion"), region);
    const auto evaluate = [&](const QString &script) {
        QQmlExpression expression(engine.rootContext(), root, script);
        const QVariant result = expression.evaluate();
        if (expression.hasError())
            engineWarnings.append(expression.error().toString());
        return result;
    };
    region->setProperty("node", QVariantMap{
        {QStringLiteral("paneId"), QStringLiteral("viewer-1")},
        {QStringLiteral("url"), QStringLiteral("file:///old/retained.txt")},
        {QStringLiteral("children"), QVariantList{}}});
    QObject *pane = nullptr;
    QTRY_VERIFY((pane = qmlObject(evaluate(
        QStringLiteral("guardRegion.paneCache['viewer-1']")))));
    ch::EditorController *controller = nullptr;
    QTRY_VERIFY((controller = qobject_cast<ch::EditorController *>(
        qmlObject(pane->property("documentController")))));
    auto *paneItem = qobject_cast<QQuickItem *>(pane);
    QVERIFY(paneItem);
    QTRY_VERIFY(paneItem->width() > 0 && paneItem->height() > 0);
    int fileReads = 0;
    for (const QByteArray &line : transport.takeSent().split('\n')) {
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        if (request.value(QStringLiteral("method")) != QStringLiteral("file.readFile"))
            continue;
        deliverResult(request.value(QStringLiteral("id")), QJsonObject{
            {QStringLiteral("path"), QStringLiteral("/old/retained.txt")},
            {QStringLiteral("encoding"), QStringLiteral("utf-8")},
            {QStringLiteral("content"), QStringLiteral("accepted native baseline")},
            {QStringLiteral("revision"), QStringLiteral("guard-revision-1")},
            {QStringLiteral("truncated"), false}});
        ++fileReads;
    }
    QCOMPARE(fileReads, 1);
    int fileWatches = 0;
    int fileStats = 0;
    for (const QByteArray &line : transport.takeSent().split('\n')) {
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        const QString method = request.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("file.watch")) {
            deliverResult(request.value(QStringLiteral("id")), QJsonObject{
                {QStringLiteral("subscriptionId"), QStringLiteral("guard-watch-1")}});
            ++fileWatches;
        } else if (method == QStringLiteral("file.stat")) {
            deliverResult(request.value(QStringLiteral("id")), QJsonObject{
                {QStringLiteral("path"), QStringLiteral("/old/retained.txt")},
                {QStringLiteral("kind"), QStringLiteral("file")},
                {QStringLiteral("size"), 24}, {QStringLiteral("mode"), 0644},
                {QStringLiteral("writable"), true},
                {QStringLiteral("revision"), QStringLiteral("guard-revision-1")}});
            ++fileStats;
        }
    }
    QCOMPARE(fileWatches, 1);
    QCOMPARE(fileStats, 1);
    QCOMPARE(controller->fileState(), QStringLiteral("clean"));
    QCOMPARE(controller->revision(), QStringLiteral("guard-revision-1"));
    QVERIFY(!controller->readOnly());
    controller->reportDirty(true, controller->revision());
    QVERIFY(controller->dirty());
    QVERIFY(pane->property("contentDirty").toBool());
    QVERIFY(!pane->property("contextRetired").toBool());
    QPointer<QObject> retainedPane(pane);
    QPointer<ch::EditorController> retainedController(controller);
    evaluate(QStringLiteral(
        "guardWindow.requestWorkspaceAction(function() { guardWindow.title = 'obsolete-peer-action'; })"));
    QObject *dialog = qmlObject(root->property("workspaceDialog"));
    QVERIFY(dialog && dialog->property("visible").toBool());
    QCOMPARE(dialog->property("requestedAcceptedServerId").toString(), QStringLiteral("server-A"));
    QCOMPARE(dialog->property("requestedGeneration").toDouble(), 0.0);
    app.setAcceptedIdentity(nextAcceptedIdentity);
    if (retireBeforeAcceptance && !region->property("contextRetired").toBool())
        QVERIFY(QMetaObject::invokeMethod(region, "retainRetiredContext"));
    QCOMPARE(region->property("contextRetired").toBool(), retireBeforeAcceptance);
    QCOMPARE(app.serverId(), QStringLiteral("server-A"));
    QVERIFY(app.activeSessionId().isEmpty());
    QVERIFY(app.layouts() == nullptr); // Same generation and selected server throughout.
    QCOMPARE(pane->property("documentServerId").toString(), QStringLiteral("server-A"));
    evaluate(QStringLiteral(
        "(function() { var dialog = guardWindow.workspaceDialog;"
        "dialog.standardButton(dialog.standardButtons & ~dialog.defaultButton).clicked(); })()"));
    QVERIFY(root->property("title").toString() != QStringLiteral("obsolete-peer-action"));
    QVERIFY(!retainedPane.isNull() && !retainedController.isNull());
    QTRY_VERIFY(!qmlObject(root->property("workspaceDialog")));
    QCOMPARE(qmlObject(evaluate(QStringLiteral("guardRegion.paneCache['viewer-1']"))), pane);

    // A fresh question about THESE already-retired objects remains valid
    // offline. Empty accepted identity is explicit state, not missing proof.
    controller->reportDirty(true, controller->revision()); // A local edit, never server I/O.
    QVERIFY(controller->dirty());
    if (!region->property("contextRetired").toBool())
        QVERIFY(QMetaObject::invokeMethod(region, "retainRetiredContext"));
    app.setAcceptedIdentity(QString());
    int detachedUnwatches = 0;
    for (const QByteArray &line : transport.takeSent().split('\n')) {
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        const QString method = request.value(QStringLiteral("method")).toString();
        if (method.isEmpty())
            continue;
        QCOMPARE(method, QStringLiteral("file.unwatch"));
        QCOMPARE(request.value(QStringLiteral("params")).toObject()
                     .value(QStringLiteral("subscriptionId")).toString(),
                 QStringLiteral("guard-watch-1"));
        deliverResult(request.value(QStringLiteral("id")), QJsonObject{});
        ++detachedUnwatches;
    }
    QCOMPARE(detachedUnwatches, 1);
    QVERIFY(!controller->readOnly());
    controller->reportContent(QStringLiteral("local edit before fresh offline consent"));
    QVERIFY(controller->dirty());
    QSignalSpy saveErrors(controller, &ch::EditorController::saveError);
    controller->save(QStringLiteral("local edit before fresh offline consent"), controller->revision());
    QCOMPARE(saveErrors.size(), 1);
    QVERIFY(transport.takeSent().isEmpty());
    client.setTransport(nullptr);
    QVERIFY(!client.transport());
    region->setProperty("node", QVariant());
    QVERIFY(!window->close());
    dialog = qmlObject(root->property("workspaceDialog"));
    QVERIFY(dialog && dialog->property("visible").toBool());
    QVERIFY(dialog->property("requestedAcceptedServerId").isValid());
    QCOMPARE(dialog->property("requestedAcceptedServerId").toString(), QString());
    evaluate(QStringLiteral(
        "(function() { var dialog = guardWindow.workspaceDialog;"
        "dialog.standardButton(dialog.standardButtons & ~dialog.defaultButton).clicked(); })()"));
    QVERIFY(!window->isVisible());
    QTRY_VERIFY(retainedPane.isNull());
    QTRY_VERIFY(retainedController.isNull());
    engineWarnings.append(takeLoggedQmlWarnings());
    QVERIFY2(engineWarnings.isEmpty(), qPrintable(engineWarnings.join(QLatin1Char('\n'))));
}

void TstQmlLoad::everyAgentCommandIsAnswered()
{
    ShellFixture fixture;
    AgentTransport transport;
    fixture.client().setTransport(&transport);
    fixture.engine.load(moduleUrl(QStringLiteral("Main.qml")));
    QVERIFY(!fixture.engine.rootObjects().isEmpty());
    settle();

    ch::ViewerCommandService &service = fixture.viewerCommands();
    // Every op, and both the well-formed and the malformed argument shapes. The
    // contract under test is not that any of them SUCCEEDS — this fixture has no
    // server, so none can — but that each produces exactly one answer. An arm
    // that returns without answering blocks the agent's turn until the daemon
    // times it out, and nothing else in the suite would notice.
    const QVector<QPair<QString, QVariantMap>> cases = {
        {QStringLiteral("list"), {}},
        {QStringLiteral("open"), {{QStringLiteral("url"), QStringLiteral("README.md")}}},
        {QStringLiteral("open"), {{QStringLiteral("url"), QStringLiteral("/srv/a.md")}}},
        {QStringLiteral("open"), {}}, // no url at all
        {QStringLiteral("open"), {{QStringLiteral("url"), QStringLiteral("/srv/a.md")},
                                  {QStringLiteral("newPane"), true}}},
        {QStringLiteral("split"), {{QStringLiteral("orientation"), QStringLiteral("vertical")}}},
        {QStringLiteral("split"), {{QStringLiteral("orientation"), QStringLiteral("sideways")}}},
        {QStringLiteral("split"), {}},
        {QStringLiteral("close"), {}},
        {QStringLiteral("focus"), {{QStringLiteral("pane"), QStringLiteral("viewer-1")}}},
        {QStringLiteral("reload"), {}},
        {QStringLiteral("reload"), {{QStringLiteral("pane"), QStringLiteral("viewer-404")}}},
    };
    for (const auto &testCase : cases) {
        // activeSessionId is empty in this fixture, so a command naming the empty
        // session passes the session gate and reaches the real handler.
        const AgentAnswer answer = askViewer(transport, QString(), testCase.first,
                                             testCase.second);
        QVERIFY2(answer.answered,
                 qPrintable(QStringLiteral("the '%1' command was never answered; an agent "
                                           "would stall until the server timed it out")
                                .arg(testCase.first)));
        QVERIFY2(!answer.code.isEmpty() || answer.ok,
                 qPrintable(QStringLiteral("the '%1' refusal carried no code")
                                .arg(testCase.first)));
    }
    // Nothing left announced-but-unanswered.
    QCOMPARE(service.inFlightCount(), 0);

    // The two argument faults are refused by NAME, not merely refused: an agent
    // branches on these codes.
    QCOMPARE(askViewer(transport, QString(), QStringLiteral("open"), {}).code,
             QStringLiteral("bad_request"));
    QCOMPARE(askViewer(transport, QString(), QStringLiteral("split"),
                       {{QStringLiteral("orientation"), QStringLiteral("sideways")}})
                 .code,
             QStringLiteral("bad_request"));

    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

void TstQmlLoad::anAgentCommandForAnotherSessionIsRefused()
{
    // The QML tree only ever holds the ACTIVE Dev Session, and SessionLayouts
    // drops writes stamped for any other, so a command from a pane of a different
    // session must be refused BY NAME rather than applied to whatever is on
    // screen. The gate runs first, before the no-server check, which is what makes
    // the answer this specific code rather than "failed".
    ShellFixture fixture;
    AgentTransport transport;
    fixture.client().setTransport(&transport);
    fixture.engine.load(moduleUrl(QStringLiteral("Main.qml")));
    QVERIFY(!fixture.engine.rootObjects().isEmpty());
    settle();

    const AgentAnswer answer = askViewer(transport, QStringLiteral("some-other-session"),
                                         QStringLiteral("split"),
                                         {{QStringLiteral("orientation"),
                                           QStringLiteral("horizontal")}});
    QVERIFY2(answer.answered, "a foreign-session command was swallowed instead of refused");
    QVERIFY(!answer.ok);
    QCOMPARE(answer.code, QStringLiteral("not_active_session"));
    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

void TstQmlLoad::agentPathsBecomeAbsoluteRemoteFileUrls()
{
    // The highest-risk piece of the executor. An agent names files the way a
    // shell does; openPaneTarget() hands its argument to WebEngine verbatim. A
    // relative string left alone resolves against the QML module, i.e. a pane
    // showing nothing, with no error anywhere.
    ShellFixture fixture;
    fixture.engine.load(moduleUrl(QStringLiteral("Main.qml")));
    QVERIFY(!fixture.engine.rootObjects().isEmpty());
    QObject *root = fixture.engine.rootObjects().constFirst();
    settle();

    const auto address = [root](const QString &raw) {
        QVariant out;
        const bool called = QMetaObject::invokeMethod(
            root, "viewerAddressFor", Q_RETURN_ARG(QVariant, out), Q_ARG(QVariant, raw));
        return called ? out.toString() : QStringLiteral("<not callable>");
    };

    // No Dev Session, so a RELATIVE path has no root to resolve against and is
    // refused rather than guessed at.
    QCOMPARE(address(QStringLiteral("README.md")), QString());

    // Absolute remote paths, web addresses and file URLs need no session.
    QCOMPARE(address(QStringLiteral("/srv/project/README.md")),
             QStringLiteral("file:///srv/project/README.md"));
    // A directory keeps its trailing slash: that is what the handler registry
    // reads to pick the listing view instead of a file view.
    QCOMPARE(address(QStringLiteral("/srv/project/src/")),
             QStringLiteral("file:///srv/project/src/"));
    // Every segment is percent-encoded individually, so a space or a '#' in a
    // file name cannot be read as a URL delimiter and silently address a
    // different file.
    QCOMPARE(address(QStringLiteral("/srv/a b/c#d.md")),
             QStringLiteral("file:///srv/a%20b/c%23d.md"));
    QCOMPARE(address(QStringLiteral("https://example.com/x?y=1#z")),
             QStringLiteral("https://example.com/x?y=1#z"));
    QCOMPARE(address(QStringLiteral("file:///srv/already.md")),
             QStringLiteral("file:///srv/already.md"));
    QCOMPARE(address(QStringLiteral("  /srv/padded.md  ")),
             QStringLiteral("file:///srv/padded.md"));

    // Refused: nothing to open, and the internal scheme, which SPEC 7.4 makes an
    // implementation detail of the privileged handlers rather than an address
    // anyone may hand in.
    QCOMPARE(address(QString()), QString());
    QCOMPARE(address(QStringLiteral("   ")), QString());
    QCOMPARE(address(QStringLiteral("codeharbor-internal://file/abc")), QString());
    QCOMPARE(address(QStringLiteral("javascript:alert(1)")), QString());
    QCOMPARE(address(QStringLiteral("data:text/html,<b>x</b>")), QString());

    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

void TstQmlLoad::theAgentPaneInventoryReadsTheLayoutTree()
{
    // `list` is how an agent learns which panes exist before naming one, so the
    // walk must find every leaf, skip the empty placeholder a fully closed region
    // leaves behind, and carry each leaf's url and title.
    ShellFixture fixture;
    fixture.engine.load(moduleUrl(QStringLiteral("Main.qml")));
    QVERIFY(!fixture.engine.rootObjects().isEmpty());
    QObject *root = fixture.engine.rootObjects().constFirst();
    settle();

    const QVariantMap leafA{{QStringLiteral("paneId"), QStringLiteral("viewer-1")},
                            {QStringLiteral("url"), QStringLiteral("file:///srv/a.md")},
                            {QStringLiteral("customTitle"), QStringLiteral("Notes")}};
    const QVariantMap leafB{{QStringLiteral("paneId"), QStringLiteral("viewer-2")}};
    const QVariantMap placeholder{{QStringLiteral("paneId"), QString()}};
    const QVariantMap tree{
        {QStringLiteral("orientation"), QStringLiteral("horizontal")},
        {QStringLiteral("children"), QVariantList{leafA, leafB, placeholder}},
    };

    QVariant out;
    QVERIFY(QMetaObject::invokeMethod(root, "viewerInventory", Q_RETURN_ARG(QVariant, out),
                                      Q_ARG(QVariant, QVariant(tree)),
                                      Q_ARG(QVariant, QVariant(QVariantList{}))));
    const QVariantList panes = out.toList();
    QCOMPARE(panes.size(), 2);
    QCOMPARE(panes.at(0).toMap().value(QStringLiteral("paneId")).toString(),
             QStringLiteral("viewer-1"));
    QCOMPARE(panes.at(0).toMap().value(QStringLiteral("url")).toString(),
             QStringLiteral("file:///srv/a.md"));
    QCOMPARE(panes.at(0).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Notes"));
    QCOMPARE(panes.at(1).toMap().value(QStringLiteral("paneId")).toString(),
             QStringLiteral("viewer-2"));
    // A leaf with no url/title reports empty strings rather than `undefined`,
    // which would reach the agent as a missing JSON field.
    QCOMPARE(panes.at(1).toMap().value(QStringLiteral("url")).toString(), QString());

    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

void TstQmlLoad::workspaceExitRequiresExplicitDiscard()
{
    ShellFixture fixture;
    fixture.engine.load(moduleUrl(QStringLiteral("Main.qml")));
    QVERIFY(!fixture.engine.rootObjects().isEmpty());
    QObject *root = fixture.engine.rootObjects().constFirst();
    settle();
    QObject *region = findNamed(root, QStringLiteral("viewerRegion"));
    QVERIFY(region);
    fixture.engine.rootContext()->setContextProperty(QStringLiteral("guardWindow"), root);
    fixture.engine.rootContext()->setContextProperty(QStringLiteral("guardRegion"), region);
    const auto run = [&](const QString &script) {
        QQmlExpression expression(fixture.engine.rootContext(), root,
                                 QStringLiteral("(function() { %1 })()").arg(script));
        expression.evaluate();
        return expression.hasError() ? expression.error().toString() : QString();
    };
    const auto activeDialog = [root] { return qmlObject(root->property("workspaceDialog")); };
    // The shell guard owns consent, not the editor bridge. Its fixture exposes
    // document identity and an observable local retirement, while real panes
    // remain in the same cache and participate in the actual reset.
    const QString seed = QStringLiteral(
        "guardRegion.paneCache['dirty-test'] = Qt.createQmlObject("
        "'import QtQuick; Item { objectName: \"dirtyShellEditor\"; "
        "property bool contentDirty: true; "
        "property bool contextRetired: false; "
        "property QtObject documentObject: QtObject {}; "
        "property var documentController: null; "
        "function retireServerContext() { contextRetired = true; contentDirty = false; } }', guardRegion);");
    const QString discard = QStringLiteral(
        "var dialog = guardWindow.workspaceDialog;"
        "dialog.standardButton(dialog.standardButtons & ~dialog.defaultButton).clicked();");
    QCOMPARE(run(seed + QStringLiteral(
        "guardWindow.requestWorkspaceAction(function() { guardWindow.title = 'target-B'; });")),
        QString());
    QObject *dirty = findNamed(root, QStringLiteral("dirtyShellEditor"));
    QVERIFY(dirty && activeDialog());
    QVERIFY(activeDialog()->property("visible").toBool());
    QVERIFY(root->property("title").toString() != QStringLiteral("target-B"));
    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window);
    settle(80);
    QTest::keyClick(window, Qt::Key_Return); // Cancel remains the safe default.
    QTRY_VERIFY(!activeDialog());
    QVERIFY(root->property("title").toString() != QStringLiteral("target-B"));
    QCOMPARE(findNamed(root, QStringLiteral("dirtyShellEditor")), dirty);
    const QStringList exits{
        QStringLiteral("guardWindow.requestSessionActivation('other-session');"),
        QStringLiteral("guardWindow.requestServerConnect('other-profile');"),
        QStringLiteral("guardWindow.requestServerDisconnect();"),
        QStringLiteral("guardWindow.requestServerUpgrade('other-profile');")};
    for (const QString &exit : exits) {
        QCOMPARE(run(exit), QString());
        QVERIFY(activeDialog() && activeDialog()->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(activeDialog(), "reject"));
        QTRY_VERIFY(!activeDialog());
        QCOMPARE(findNamed(root, QStringLiteral("dirtyShellEditor")), dirty);
    }
    QObject *settings = findNamed(root, QStringLiteral("settingsWindow"));
    QVERIFY(settings);
    for (const char *request : {"connectRequested", "disconnectRequested", "upgradeRequested"}) {
        const bool called = QByteArray(request) == "disconnectRequested"
            ? QMetaObject::invokeMethod(settings, request)
            : QMetaObject::invokeMethod(settings, request,
                                       Q_ARG(QString, QStringLiteral("other-profile")));
        QVERIFY(called);
        QVERIFY(activeDialog() && activeDialog()->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(activeDialog(), "reject"));
        QTRY_VERIFY(!activeDialog());
        QCOMPARE(findNamed(root, QStringLiteral("dirtyShellEditor")), dirty);
    }
    // A replacement document in the same pane/context is NOT covered by the
    // earlier question, even if session/generation have not changed.
    QCOMPARE(run(QStringLiteral(
        "guardWindow.requestWorkspaceAction(function() { guardWindow.title = 'stale-document'; });"
        "guardRegion.paneCache['dirty-test'].documentObject = "
        "Qt.createQmlObject('import QtQml; QtObject {}', guardRegion);") + discard), QString());
    QTRY_VERIFY(!activeDialog());
    QVERIFY(root->property("title").toString() != QStringLiteral("stale-document"));
    QCOMPARE(findNamed(root, QStringLiteral("dirtyShellEditor")), dirty);
    QVERIFY(dirty->property("contentDirty").toBool());
    QCOMPARE(run(QStringLiteral(
        "guardWindow.requestWorkspaceAction(function() { guardWindow.title = 'unasked-buffer'; });"
        "guardRegion.paneCache['extra-dirty'] = Qt.createQmlObject("
        "'import QtQuick; Item { objectName: \"extraDirtyEditor\"; "
        "property bool contentDirty: true }', guardRegion);") + discard), QString());
    QTRY_VERIFY(!activeDialog());
    QVERIFY(root->property("title").toString() != QStringLiteral("unasked-buffer"));
    QVERIFY(findNamed(root, QStringLiteral("extraDirtyEditor")));
    QCOMPARE(findNamed(root, QStringLiteral("dirtyShellEditor")), dirty);
    QCOMPARE(run(QStringLiteral(
        "guardRegion.paneCache['extra-dirty'].destroy();"
        "delete guardRegion.paneCache['extra-dirty'];")), QString());
    QCOMPARE(run(QStringLiteral(
        "guardWindow.requestWorkspaceAction(function() { guardWindow.title = 'stale-context'; });"
        "guardWindow.workspaceDialog.requestedSessionId = 'retired-session';") + discard), QString());
    QTRY_VERIFY(!activeDialog());
    QVERIFY(root->property("title").toString() != QStringLiteral("stale-context"));
    QCOMPARE(findNamed(root, QStringLiteral("dirtyShellEditor")), dirty);
    // The old modal can emit a delayed discard in the same turn a fresh modal
    // opens. It must neither invoke the new action nor consume its consent.
    QCOMPARE(run(QStringLiteral(
        "guardWindow.requestWorkspaceAction(function() { guardWindow.title = 'obsolete-action'; });"
        "var oldDialog = guardWindow.workspaceDialog;"
        "oldDialog.reject();"
        "guardWindow.requestWorkspaceAction(function() { guardWindow.title = 'target-B'; });"
        "oldDialog.discarded();"
        "guardWindow.requestWorkspaceAction(function() { guardWindow.title = 'target-C'; });")),
        QString());
    QVERIFY(activeDialog() && activeDialog()->property("visible").toBool());
    QVERIFY(root->property("title").toString() != QStringLiteral("obsolete-action"));
    QPointer<QObject> discarded(dirty);
    QCOMPARE(run(discard), QString());
    QCOMPARE(root->property("title").toString(), QStringLiteral("target-B"));
    QTRY_VERIFY(!activeDialog());
    QTRY_VERIFY(discarded.isNull());
    QCOMPARE(run(QStringLiteral(
        "guardWindow.requestWorkspaceAction(function() { guardWindow.title = 'clean-target'; });")),
        QString());
    QCOMPARE(root->property("title").toString(), QStringLiteral("clean-target"));
    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

void TstQmlLoad::windowCloseKeepsDirtyEditorsAlive()
{
    ShellFixture fixture;
    fixture.engine.load(moduleUrl(QStringLiteral("Main.qml")));
    QVERIFY(!fixture.engine.rootObjects().isEmpty());
    QObject *root = fixture.engine.rootObjects().constFirst();
    settle();
    auto *window = qobject_cast<QQuickWindow *>(root);
    QObject *region = findNamed(root, QStringLiteral("viewerRegion"));
    QVERIFY(window && region);
    fixture.engine.rootContext()->setContextProperty(QStringLiteral("guardWindow"), root);
    fixture.engine.rootContext()->setContextProperty(QStringLiteral("guardRegion"), region);
    QQmlExpression inject(fixture.engine.rootContext(), root, QStringLiteral(
        "guardRegion.paneCache['dirty-test'] = Qt.createQmlObject("
        "'import QtQuick; Item { objectName: \"dirtyShellEditor\"; "
        "property bool contentDirty: true; "
        "property bool contextRetired: false; "
        "function retireServerContext() { contextRetired = true; contentDirty = false; } }', guardRegion)"));
    inject.evaluate();
    QVERIFY2(!inject.hasError(), qPrintable(inject.error().toString()));
    QVERIFY(!window->close());
    QVERIFY(window->isVisible());
    QObject *dialog = qmlObject(root->property("workspaceDialog"));
    QVERIFY(dialog && dialog->property("visible").toBool());
    QVERIFY(findNamed(root, QStringLiteral("dirtyShellEditor")));
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTRY_VERIFY(!qmlObject(root->property("workspaceDialog")));
    QVERIFY(window->isVisible());
    QVERIFY(!window->close());
    QQmlExpression discard(fixture.engine.rootContext(), root, QStringLiteral(
        "(function() { var dialog = guardWindow.workspaceDialog;"
        "dialog.standardButton(dialog.standardButtons & ~dialog.defaultButton).clicked(); })()"));
    discard.evaluate();
    QVERIFY2(!discard.hasError(), qPrintable(discard.error().toString()));
    QVERIFY(!window->isVisible());
    QTRY_VERIFY(!qmlObject(root->property("workspaceDialog")));
    QVERIFY2(fixture.allWarnings().isEmpty(), qPrintable(fixture.warningReport()));
}

void TstQmlLoad::connectionPromptsReplaceCoveringSheetsAndRetireOnStateChange()
{
    ShellFixture fixture;
    fixture.engine.load(moduleUrl(QStringLiteral("Main.qml")));
    QVERIFY(!fixture.engine.rootObjects().isEmpty());
    QObject *root = fixture.engine.rootObjects().constFirst();
    settle();
    QObject *sheet = findNamed(root, QStringLiteral("connectSheet"));
    QObject *settings = findNamed(root, QStringLiteral("settingsWindow"));
    QObject *log = findNamed(root, QStringLiteral("logSheet"));
    QVERIFY(sheet && settings && log);
    settings->setProperty("selectedGroup", QStringLiteral("appearance"));
    QVERIFY(QMetaObject::invokeMethod(sheet, "settingsRequested"));
    QCOMPARE(settings->property("selectedGroup").toString(), QStringLiteral("server"));
    QVERIFY(settings->property("shown").toBool());
    log->setProperty("shown", true);
    emit fixture.controller().hostKeyPrompt(QStringLiteral("ssh.example"), QStringLiteral("ed25519"),
                                           QStringLiteral("SHA256:fixture"));
    settle(80);
    QVERIFY(!settings->property("shown").toBool());
    QVERIFY(!log->property("shown").toBool());
    QVERIFY(sheet->property("shown").toBool());
    QObject *host = findNamed(root, QStringLiteral("hostKeyHost"));
    QVERIFY(host);
    QVERIFY(host->property("text").toString().contains(QStringLiteral("ssh.example")));
    QVERIFY(sheet->property("promptActive").toBool());
    // This real controller is disconnected; its notification retires the UI
    // question exactly as a cancelled backend attempt does.
    emit fixture.controller().connectionStateChanged();
    QVERIFY(!sheet->property("promptActive").toBool());

    settings->setProperty("shown", true);
    log->setProperty("shown", true);
    emit fixture.controller().credentialPrompt(QStringLiteral("user"), QStringLiteral("ssh.example"),
                                               QStringLiteral("Password"), QStringLiteral("password"));
    settle(80);
    QVERIFY(!settings->property("shown").toBool());
    QVERIFY(!log->property("shown").toBool());
    QObject *field = findNamed(root, QStringLiteral("credentialField"));
    QVERIFY(field);
    field->setProperty("text", QStringLiteral("one-attempt-only"));
    QVERIFY(sheet->property("promptActive").toBool());
    emit fixture.controller().connectionStateChanged();
    QVERIFY(!sheet->property("promptActive").toBool());
    QCOMPARE(field->property("text").toString(), QString());
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
