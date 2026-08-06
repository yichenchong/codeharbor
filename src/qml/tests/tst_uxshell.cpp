// Cold-start usability gate for the shell surfaces a first-time user actually
// lands on, plus the frame grabber that produced the review screenshots.
//
// Two modes, one binary:
//
//   tst_uxshell            run the assertions (this is what ctest does)
//   tst_uxshell -shots     render every surface into PNGs and exit
//
// The shot mode exists because "is this usable?" is a question about pixels, not
// properties: it grabs the REAL components through QQuickWindow::grabWindow()
// under the headless recipe and writes them to
// <build>/src/qml/tests/ux-shots/$CH_UX_PHASE, so a before/after pair can be
// compared directly.
//
// The assertions cover the surfaces nothing else gates: ConnectSheet's status
// vocabulary (it has to agree with the states ch::AppController actually
// publishes), its dismissible error banner, the two "nothing here yet" panes — a
// terminal that cannot attach and a viewer with no file — which must explain
// themselves instead of showing a raw pane id, and the window's own title bar.
//
// The title bar is here because the window is FRAMELESS on the platforms with
// custom chrome (see Main.qml), so that bar is the only maximise and the only
// close the pointer can reach. It is driven against a StubWindow rather than the
// offscreen QQuickView, whose platform plugin does not model window states.
//
// Runs headless; the ctest registration pins the offscreen QPA, the software
// Quick backend and Chromium's no-GPU flags (see CMakeLists.txt).

#include "SessionsModel.h"

#include <QtTest>

#include <QAbstractItemModel>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QGuiApplication>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QPointF>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlIncubationController>
#include <QQuickItem>
#include <QQuickView>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QWindow>
#include <QtQuickControls2/QQuickStyle>
#include <QtWebEngineQuick/QtWebEngineQuick>

#include <memory>

namespace {

constexpr auto kModuleRoot = "qrc:/qt/qml/CodeHarbor/";

QUrl moduleUrl(const QString &file)
{
    return QUrl(QLatin1String(kModuleRoot) + file);
}

// Depth-first over BOTH child lists: QML parents delegate/loader content into
// the visual tree without always re-parenting the QObject.
QObject *findByName(QObject *root, const QString &name, QSet<const QObject *> *seen = nullptr)
{
    QSet<const QObject *> local;
    QSet<const QObject *> &visited = seen ? *seen : local;
    if (!root || visited.contains(root))
        return nullptr;
    visited.insert(root);
    if (root->objectName() == name)
        return root;
    const auto objectChildren = root->children();
    for (QObject *child : objectChildren) {
        if (QObject *found = findByName(child, name, &visited))
            return found;
    }
    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        const auto itemChildren = item->childItems();
        for (QQuickItem *child : itemChildren) {
            if (QObject *found = findByName(child, name, &visited))
                return found;
        }
    }
    return nullptr;
}

QString textOf(QObject *object)
{
    return object ? object->property("text").toString() : QString();
}

// One synthetic terminal whose (connection, agent) pair aggregates to exactly
// the requested sidebar row state (SPEC 4.2 precedence).
QVector<ch::TerminalStatus> terminalsFor(ch::SessionRowState state)
{
    ch::TerminalStatus terminal;
    terminal.id = ch::TerminalId{QStringLiteral("t1")};
    terminal.connection = ch::TerminalState::Ready;
    switch (state) {
    case ch::SessionRowState::Error:
        terminal.connection = ch::TerminalState::Error;
        break;
    case ch::SessionRowState::WaitingForInput:
        terminal.agent = ch::AgentState::WaitingInput;
        break;
    case ch::SessionRowState::Running:
        terminal.agent = ch::AgentState::Running;
        break;
    case ch::SessionRowState::FinishedUnseen:
        terminal.agent = ch::AgentState::IdleUnseen;
        break;
    case ch::SessionRowState::Idle:
        terminal.agent = ch::AgentState::Idle;
        break;
    case ch::SessionRowState::Disconnected:
        terminal.connection = ch::TerminalState::Disconnected;
        break;
    }
    return {terminal};
}

ch::GroupRow makeGroup(const QString &id, const QString &name,
                       const QVector<QPair<QString, ch::SessionRowState>> &sessions,
                       bool collapsed = false)
{
    ch::GroupRow group;
    group.group.id = ch::GroupId{id};
    group.group.serverId = ch::ServerId{QStringLiteral("srv")};
    group.group.name = name;
    group.group.collapsed = collapsed;

    int position = 0;
    for (const auto &entry : sessions) {
        ch::SessionRow row;
        row.session.id = ch::DevSessionId{entry.first};
        row.session.serverId = ch::ServerId{QStringLiteral("srv")};
        row.session.groupId = ch::GroupId{id};
        row.session.name = entry.first;
        row.session.repositoryRoot = QStringLiteral("/srv/") + entry.first.toLower();
        row.session.position = position++;
        row.subtitle = QStringLiteral("~/src/") + entry.first.toLower();
        row.terminals = terminalsFor(entry.second);
        group.sessions.append(row);
    }
    return group;
}

} // namespace

// The real controller exposes client-local filter state through UiStateStore.
// Keep the shell fixture's surface identical so a model preconfigured to show
// archived sessions is not reset by SessionsSidebar's completion sync.
class StubUiState : public QObject
{
    Q_OBJECT

public:
    explicit StubUiState(ch::SessionsModel *model, QObject *parent = nullptr)
        : QObject(parent), m_model(model)
    {
    }

    Q_INVOKABLE bool pinnedOnly() const
    {
        return m_model && m_model->pinnedOnly();
    }
    Q_INVOKABLE void setPinnedOnly(bool value)
    {
        if (m_model)
            m_model->setPinnedOnly(value);
    }
    Q_INVOKABLE bool showArchived() const
    {
        return m_model && m_model->showArchived();
    }
    Q_INVOKABLE void setShowArchived(bool value)
    {
        if (m_model)
            m_model->setShowArchived(value);
    }

private:
    ch::SessionsModel *m_model = nullptr;
};

// The slice of ch::AppController the sidebar reads. Mirrors tst_sidebar's stub
// and adds the connection surface the status footer binds to.
class ShotApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *sessionsModel READ sessionsModel CONSTANT)
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionStateChanged)
    Q_PROPERTY(QString activeSessionId READ activeSessionId NOTIFY activeSessionChanged)
    Q_PROPERTY(QObject *uiState READ uiState CONSTANT)

public:
    explicit ShotApp(QAbstractItemModel *model, QObject *parent = nullptr)
        : QObject(parent),
          m_model(model),
          m_uiState(qobject_cast<ch::SessionsModel *>(model), this)
    {
    }

    QAbstractItemModel *sessionsModel() const { return m_model; }
    QObject *uiState() const { return const_cast<StubUiState *>(&m_uiState); }

    QString connectionState() const { return m_connectionState; }
    void setConnectionState(const QString &state)
    {
        m_connectionState = state;
        emit connectionStateChanged();
    }

    QString activeSessionId() const { return m_activeSessionId; }
    void setActiveSessionId(const QString &id)
    {
        m_activeSessionId = id;
        emit activeSessionChanged();
    }

    Q_INVOKABLE void refresh() {}
    Q_INVOKABLE void createGroup(QString) {}
    Q_INVOKABLE void renameGroup(QString, QString) {}
    Q_INVOKABLE void setGroupCollapsed(QString, bool) {}
    Q_INVOKABLE void deleteGroup(QString id)
    {
        m_lastDeleteGroupId = id;
        m_deleteGroupCalls++;
    }
    Q_INVOKABLE void reorderGroups(QStringList) {}
    Q_INVOKABLE void createSession(QString, QString, QString) {}
    Q_INVOKABLE void renameSession(QString, QString) {}
    Q_INVOKABLE void duplicateSession(QString) {}
    Q_INVOKABLE void moveSession(QString, QString, int) {}
    Q_INVOKABLE void deleteSession(QString id)
    {
        m_lastDeleteSessionId = id;
        m_deleteSessionCalls++;
    }
    Q_INVOKABLE int sessionCountForGroup(QString id) const
    {
        return m_sessionCounts.value(id, 0);
    }
    void setAuthoritativeSessionCount(const QString &id, int count)
    {
        m_sessionCounts.insert(id, count);
    }
    QString lastDeleteGroupId() const { return m_lastDeleteGroupId; }
    int deleteGroupCalls() const { return m_deleteGroupCalls; }
    QString lastDeleteSessionId() const { return m_lastDeleteSessionId; }
    int deleteSessionCalls() const { return m_deleteSessionCalls; }
    Q_INVOKABLE void archiveSession(QString id)
    {
        m_lastArchiveId = id;
        m_archiveCalls++;
    }
    Q_INVOKABLE void unarchiveSession(QString id)
    {
        m_lastArchiveId = id;
        m_unarchiveCalls++;
    }
    QString lastArchiveId() const { return m_lastArchiveId; }
    int archiveCalls() const { return m_archiveCalls; }
    int unarchiveCalls() const { return m_unarchiveCalls; }
signals:
    void connectionStateChanged();
    void activeSessionChanged();

private:
    QAbstractItemModel *m_model = nullptr;
    StubUiState m_uiState;
    QString m_connectionState = QStringLiteral("disconnected");
    QHash<QString, int> m_sessionCounts;
    QString m_lastDeleteGroupId;
    int m_deleteGroupCalls = 0;
    QString m_lastDeleteSessionId;
    int m_deleteSessionCalls = 0;
    QString m_lastArchiveId;
    int m_archiveCalls = 0;
    int m_unarchiveCalls = 0;
    QString m_activeSessionId;
};

// A stand-in for the window AppTitleBar drives. The bar's whole job is to do
// what the window manager used to do, so what a test can observe is the STATE it
// asks the window for: `visibility` (QWindow::Visibility values, which is what
// QML's Window.Maximized / Window.Windowed are) and close().
//
// A real window would be the more faithful host, but the offscreen platform
// plugin the suite runs under does not model maximise/minimise, so a click would
// leave `visibility` untouched and the assertion would pass or fail on the
// platform plugin rather than on the bar.
class StubWindow : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int visibility READ visibility WRITE setVisibility NOTIFY visibilityChanged)

public:
    int visibility() const { return m_visibility; }
    void setVisibility(int visibility)
    {
        if (visibility == m_visibility)
            return;
        m_visibility = visibility;
        emit visibilityChanged();
    }

    Q_INVOKABLE bool startSystemMove()
    {
        ++m_moveCalls;
        m_moveVisibility = m_visibility;
        return true;
    }

    Q_INVOKABLE void close() { ++m_closeCalls; }

    int closeCalls() const { return m_closeCalls; }
    int moveCalls() const { return m_moveCalls; }
    int moveVisibility() const { return m_moveVisibility; }

signals:
    void visibilityChanged();

private:
    int m_visibility = int(QWindow::Windowed);
    int m_closeCalls = 0;
    int m_moveCalls = 0;
    int m_moveVisibility = int(QWindow::Windowed);
};

class StubNativeHelper : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE void registerMaximizeButton(QObject *window, QObject *button)
    {
        ++m_registerCalls;
        m_window = window;
        m_button = button;
    }

    int registerCalls() const { return m_registerCalls; }
    QObject *window() const { return m_window; }
    QObject *button() const { return m_button; }

private:
    int m_registerCalls = 0;
    QObject *m_window = nullptr;
    QObject *m_button = nullptr;
};

namespace {

// A shown QQuickView over one component, with the QML warning net attached.
class Surface
{
public:
    QQuickView view;
    QStringList warnings;

    Surface(const QUrl &url, const QSize &size, QObject *app = nullptr)
    {
        catchWarnings();
        if (app)
            view.rootContext()->setContextProperty(QStringLiteral("app"), app);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(size);
        view.setSource(url);
    }

    // The same, over an INLINE document. Some shipped components need a host:
    // AppTitleBar drives the window it is shown in, so a test has to hand it a
    // window it can observe rather than the offscreen QQuickView itself, whose
    // maximise/minimise states the offscreen platform plugin does not model.
    // `contextObject` is exposed to the document under `contextName`.
    Surface(const QByteArray &document, const QSize &size, const QString &contextName,
            QObject *contextObject)
    {
        catchWarnings();
        if (contextObject)
            view.rootContext()->setContextProperty(contextName, contextObject);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(size);

        // Parented to the engine: QQuickView keeps the raw pointer for status().
        auto *component = new QQmlComponent(view.engine(), view.engine());
        const QUrl url(QStringLiteral("qrc:/qt/qml/CodeHarbor/UxHarness.qml"));
        component->setData(document, url);
        m_componentError = component->errorString();
        if (QObject *root = component->create(view.rootContext()))
            view.setContent(url, component, root);
    }

    QQuickItem *root() const { return view.rootObject(); }

    QString componentError() const { return m_componentError; }

    bool expose()
    {
        view.show();
        return QTest::qWaitForWindowExposed(&view);
    }

    QObject *child(const QString &name) const { return findByName(view.rootObject(), name); }

    QString warningReport() const
    {
        return QStringLiteral("%1 QML warning(s):\n  * ").arg(warnings.size())
                + warnings.join(QStringLiteral("\n  * "));
    }

private:
    void catchWarnings()
    {
        QObject::connect(view.engine(), &QQmlEngine::warnings, view.engine(),
                         [this](const QList<QQmlError> &list) {
                             for (const QQmlError &error : list)
                                 warnings.append(error.toString());
                         });
    }

    QString m_componentError;
};

void settle(int ms = 120)
{
    QTest::qWait(ms);
    QCoreApplication::processEvents();
}

// ---------------------------------------------------------------------------
// Fixtures, shared by the shot mode and the assertions so a screenshot always
// shows the same state a test made a claim about.
// ---------------------------------------------------------------------------

QVariantList twoProfiles()
{
    return {QVariantMap{{QStringLiteral("id"), QStringLiteral("id-a")},
                        {QStringLiteral("name"), QStringLiteral("Workstation")},
                        {QStringLiteral("host"), QStringLiteral("10.0.0.4")},
                        {QStringLiteral("port"), 22},
                        {QStringLiteral("user"), QStringLiteral("yichen")},
                        {QStringLiteral("nodePath"), QStringLiteral("/usr/bin/node")},
                        {QStringLiteral("repoRoot"), QStringLiteral("/srv/codeharbor")}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("id-b")},
                        {QStringLiteral("name"), QStringLiteral("Fixture box")},
                        {QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                        {QStringLiteral("port"), 2222},
                        {QStringLiteral("user"), QStringLiteral("yichen")},
                        {QStringLiteral("nodePath"),
                         QStringLiteral("/home/yichen/.local/bin/node")},
                        {QStringLiteral("repoRoot"),
                         QStringLiteral("/home/yichen/projects/codeharbor")}}};
}

QVector<ch::GroupRow> populatedGroups()
{
    // One row per interesting aggregate state, so the badge encoding is visible.
    using S = ch::SessionRowState;
    return {makeGroup(QStringLiteral("g1"), QStringLiteral("Client work"),
                      {{QStringLiteral("codeharbor"), S::Running},
                       {QStringLiteral("bridge-rpc"), S::WaitingForInput},
                       {QStringLiteral("docs"), S::Idle}}),
            makeGroup(QStringLiteral("g2"), QStringLiteral("Experiments"),
                      {{QStringLiteral("wasm-spike"), S::Error},
                       {QStringLiteral("old-branch"), S::Disconnected}}),
            makeGroup(QStringLiteral("g3"), QStringLiteral("Archived"),
                      {{QStringLiteral("2024-audit"), S::FinishedUnseen}}, /*collapsed=*/true)};
}

// The palette needs a host document (its visible part is a Popup parented to the
// window overlay), exactly like Main.qml gives it.
QByteArray paletteHarness()
{
    return R"QML(
import QtQuick
import CodeHarbor

Item {
    id: harness
    width: 900
    height: 560

    Rectangle { anchors.fill: parent; color: Theme.surface }

    CommandPalette {
        id: palette
        objectName: "palette"
        commands: [
            { id: "server.connect", title: "Connect to Server\u2026", shortcut: "Ctrl+Shift+O",
              invoke: function () {} },
            { id: "server.disconnect", title: "Disconnect from Server", invoke: function () {} },
            { id: "session.refresh", title: "Refresh Workspace", shortcut: "Ctrl+Shift+R",
              invoke: function () {} },
            { id: "viewer.split.h", title: "Split Viewer Pane Horizontally",
              invoke: function () {} },
            { id: "viewer.split.v", title: "Split Viewer Pane Vertically",
              invoke: function () {} },
            { id: "terminal.split.h", title: "Split Terminal Pane Horizontally",
              invoke: function () {} },
            { id: "agent.markSeen", title: "Mark Agent Output Seen", invoke: function () {} }
        ]
    }

    function openPalette() { palette.open(); }
}
)QML";
}

// The title bar drives a window, so it gets one: `stubWindow` is the C++
// StubWindow, exposed as a context property by Surface's inline-document
// constructor. accessibleName() exists because Accessible.name is an attached
// QML property with no reachable C++ accessor — the assertion has to be made
// from inside the document.
QByteArray titleBarHarness()
{
    return R"QML(
import QtQuick
import CodeHarbor

Item {
    id: harness
    width: 640
    height: 120

    Rectangle { anchors.fill: parent; color: Theme.surface }

    function accessibleName(item) { return item ? String(item.Accessible.name) : ""; }

    AppTitleBar {
        id: bar
        objectName: "titleBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        // Rectangle does not adopt its own implicitHeight, exactly as Main.qml
        // has to spell out too.
        height: bar.implicitHeight
        win: stubWindow
        title: "CodeHarbor"
        sessionLabel: "codeharbor"
    }
}
)QML";
}

// One of the three full-window overlay sheets, laid over a background that
// counts the clicks it receives. `extra` carries whatever the sheet needs to be
// live (LogView and SettingsWindow gate themselves on `shown`).
QByteArray overlayHarness(const QByteArray &component, const QByteArray &extra)
{
    QByteArray qml = R"QML(
import QtQuick
import CodeHarbor

Item {
    id: harness
    width: 900
    height: 560
    property int behindClicks: 0
    property alias sheetVisible: sheet.visible

    // Stands in for the three regions the sheet covers in Main.qml.
    MouseArea {
        anchors.fill: parent
        onClicked: harness.behindClicks++
    }

    @SHEET@ {
        id: sheet
        objectName: "sheet"
        anchors.fill: parent
        z: 10
        visible: false
        @EXTRA@
    }
}
)QML";
    qml.replace("@SHEET@", component);
    qml.replace("@EXTRA@", extra);
    return qml;
}

// Accessible.name of an item in the document above.
QString accessibleName(QObject *harness, QObject *item)
{
    QVariant name;
    if (!harness || !item
        || !QMetaObject::invokeMethod(harness, "accessibleName", Q_RETURN_ARG(QVariant, name),
                                      Q_ARG(QVariant, QVariant::fromValue(item))))
        return QString();
    return name.toString();
}

// ---------------------------------------------------------------------------
// Shot mode
// ---------------------------------------------------------------------------

QString shotDir()
{
    QString base = qEnvironmentVariable("CH_UX_SHOTS");
    if (base.isEmpty())
        base = QCoreApplication::applicationDirPath() + QStringLiteral("/ux-shots");
    const QString phase = qEnvironmentVariable("CH_UX_PHASE", QStringLiteral("shots"));
    const QString dir = base + QLatin1Char('/') + phase;
    QDir().mkpath(dir);
    return dir;
}

bool grab(QQuickView &view, const QString &dir, const QString &name)
{
    settle(250);
    const QImage image = view.grabWindow();
    if (image.isNull()) {
        qWarning("grabWindow() returned a null image for %s", qPrintable(name));
        return false;
    }
    const QString path = dir + QLatin1Char('/') + name + QStringLiteral(".png");
    if (!image.save(path)) {
        qWarning("could not write %s", qPrintable(path));
        return false;
    }
    qInfo("wrote %s (%dx%d)", qPrintable(path), image.width(), image.height());
    return true;
}

int renderShots()
{
    const QString dir = shotDir();
    bool ok = true;

    { // Cold start: a fresh config, nothing stored.
        Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(1000, 620));
        ok &= surface.expose();
        ok &= grab(surface.view, dir, QStringLiteral("connect-cold"));
    }
    { // Saved servers, and the last connect failed.
        Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(1000, 620));
        ok &= surface.expose();
        surface.root()->setProperty("profiles", twoProfiles());
        surface.root()->setProperty("activeId", QStringLiteral("id-b"));
        surface.root()->setProperty("connectionState", QStringLiteral("failed"));
        surface.root()->setProperty(
                "errorText",
                QStringLiteral("ssh: connect to host 127.0.0.1 port 2222: Connection refused"));
        ok &= grab(surface.view, dir, QStringLiteral("connect-failed"));
    }
    { // Reconnecting, so the busy affordance is on screen.
        Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(1000, 620));
        ok &= surface.expose();
        surface.root()->setProperty("profiles", twoProfiles());
        surface.root()->setProperty("activeId", QStringLiteral("id-b"));
        surface.root()->setProperty("connectionState", QStringLiteral("reconnecting"));
        ok &= grab(surface.view, dir, QStringLiteral("connect-reconnecting"));
    }
    { // First-use host key decision.
        Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(1000, 620));
        ok &= surface.expose();
        surface.root()->setProperty("profiles", twoProfiles());
        surface.root()->setProperty("connectionState", QStringLiteral("hostkey"));
        surface.root()->setProperty(
                "pendingHostKey",
                QVariantMap{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                            {QStringLiteral("keyType"), QStringLiteral("ssh-ed25519")},
                            {QStringLiteral("fingerprint"),
                             QStringLiteral("SHA256:6dGRJ0mCkQeAxQ0nQ0mm2b3xk0e3iF0jnq0oO3pP1qQ")}});
        ok &= grab(surface.view, dir, QStringLiteral("connect-hostkey"));
    }
    { // The window's own title bar, windowed and maximised.
        StubWindow stub;
        Surface surface(titleBarHarness(), QSize(900, 120), QStringLiteral("stubWindow"), &stub);
        ok &= surface.expose();
        ok &= grab(surface.view, dir, QStringLiteral("titlebar-windowed"));
        stub.setVisibility(int(QWindow::Maximized));
        ok &= grab(surface.view, dir, QStringLiteral("titlebar-maximised"));
    }
    { // Sidebar with nothing in it and no server.
        ch::SessionsModel model;
        ShotApp app(&model);
        Surface surface(moduleUrl(QStringLiteral("SessionsSidebar.qml")), QSize(300, 620), &app);
        ok &= surface.expose();
        ok &= grab(surface.view, dir, QStringLiteral("sidebar-cold"));
    }
    { // Sidebar with real content, connected, one session active.
        ch::SessionsModel model;
        model.setGroups(populatedGroups());
        ShotApp app(&model);
        app.setConnectionState(QStringLiteral("connected"));
        app.setActiveSessionId(QStringLiteral("bridge-rpc"));
        Surface surface(moduleUrl(QStringLiteral("SessionsSidebar.qml")), QSize(300, 620), &app);
        ok &= surface.expose();
        ok &= grab(surface.view, dir, QStringLiteral("sidebar-live"));
    }
    { // Sidebar while the link is down: every row is stale.
        ch::SessionsModel model;
        model.setGroups(populatedGroups());
        ShotApp app(&model);
        app.setConnectionState(QStringLiteral("reconnecting"));
        Surface surface(moduleUrl(QStringLiteral("SessionsSidebar.qml")), QSize(300, 620), &app);
        ok &= surface.expose();
        ok &= grab(surface.view, dir, QStringLiteral("sidebar-reconnecting"));
    }
    { // Command palette, open.
        QQuickView view;
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(900, 560);
        auto *component = new QQmlComponent(view.engine(), view.engine());
        component->setData(paletteHarness(), QUrl(QStringLiteral("qrc:/chtest/ShotHarness.qml")));
        QObject *root = component->create(view.rootContext());
        if (!root) {
            qWarning("palette harness failed: %s", qPrintable(component->errorString()));
            ok = false;
        } else {
            view.setContent(QUrl(QStringLiteral("qrc:/chtest/ShotHarness.qml")), component, root);
            view.show();
            ok &= QTest::qWaitForWindowExposed(&view);
            view.requestActivate();
            (void)QTest::qWaitForWindowActive(&view);
            QMetaObject::invokeMethod(root, "openPalette");
            ok &= grab(view, dir, QStringLiteral("palette-open"));
        }
    }
    { // A terminal pane with no terminal service behind it.
        Surface surface(moduleUrl(QStringLiteral("TerminalPaneView.qml")), QSize(560, 320));
        ok &= surface.expose();
        surface.root()->setProperty("paneId", QStringLiteral("terminal-1"));
        ok &= grab(surface.view, dir, QStringLiteral("terminal-inert"));
    }
    { // A viewer pane with no file in it.
        Surface surface(moduleUrl(QStringLiteral("ViewerPane.qml")), QSize(560, 320));
        ok &= surface.expose();
        surface.root()->setProperty("paneId", QStringLiteral("viewer-1"));
        ok &= grab(surface.view, dir, QStringLiteral("viewer-empty"));
    }

    qInfo("shots written to %s", qPrintable(dir));
    return ok ? 0 : 1;
}

} // namespace

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------

class TstUxShell : public QObject
{
    Q_OBJECT
private slots:
    // ConnectSheet: the status chip has to speak the vocabulary AppController
    // actually publishes, or a failed/reconnecting link reads as "idle".
    void sheetStatusMatchesTheStatesTheAppPublishes_data();
    void sheetStatusMatchesTheStatesTheAppPublishes();
    void everyPublishedStateHasASheetRowAndAGlyph();

    // The sheet's own chrome must be unreachable — by pointer AND by keyboard —
    // while a host-key or credential prompt is waiting on the user.
    void sheetChromeIsDisabledWhileAPromptIsUp();

    // The sidebar footer is the only connection status a user sees once the
    // sheet is closed, so it needs a word for every state the app publishes.
    void sidebarFooterNamesEveryConnectionState_data();
    void sidebarFooterNamesEveryConnectionState();
    void sidebarArchiveButtonCallsArchiveAndUnarchive();
    void sidebarDeleteActionsAreConfirmedAndNamed();
    void sidebarArchiveFilterToggleIsNamedAndChangesModel();
    void sidebarArchiveEmptyStateExplainsArchivedSessions();
    // The sidebar row badge is the only place a Dev Session's aggregate state
    // is shown, and nothing asserted how it is drawn: a swapped case in
    // SessionRow.stateColor() would paint Error green and no test would care.
    void sidebarRowBadgeSeparatesEveryRowState();

    // A long RPC error must be readable and the banner must be dismissible,
    // then come back for the next failure.
    void sheetErrorIsReadableAndDismissible();

    // Cold start: the sheet has to say what this dialog is for, not just show
    // an empty list.
    void sheetColdStartExplainsItself();
    void sheetRejectsPartiallyNumericPorts();
    void sheetModelRefreshKeepsUnsavedEdits();

    // A pane with nothing behind it explains itself instead of showing an id.
    void inertTerminalPaneExplainsItself();
    void emptyViewerPaneExplainsItself();

    // The window is frameless on the platforms with custom chrome, so the title
    // bar IS the window controls: if its maximise button does not change the
    // window state, or its close button is invisible to a screen reader, the
    // window has become unusable in a way no other test would notice.
    void titleBarMaximiseButtonTogglesTheWindowState();
    void titleBarButtonsAreNamedAndCloseIsWired();
    void titleBarDragUsesTheSystemMoveOperation();

    // The one control in the sheet that WRITES to the server: it has to be
    // wired to a profile and, like Connect, unreachable behind a prompt.
    void sheetUpdateServerButtonNamesItsProfile();

    // The three full-window sheets sit ON TOP of the workspace. A Rectangle
    // accepts no input of its own, so without a shield every click that missed
    // one of their controls fell straight through to the pane behind them.
    void overlaySheetsSwallowClicksToTheWorkspace_data();
    void overlaySheetsSwallowClicksToTheWorkspace();

    // The Settings server pane lays its fields out in a two-column grid; one
    // over-wide entry pushes the whole second column off the pane.
    void settingsServerFieldsStayInsideThePane();

    // The sheet's Save gate has to be the STORE's rule. Anything looser reports
    // a save that ServerProfiles silently drops on the floor.
    void sheetRejectsAHostOrUserThatIsNotASingleWord();
};

// The states ch::AppController publishes, read out of the SOURCE rather than
// listed here by hand.
//
// The hand-written list this replaces was stale by convention: it silently
// passed while the newest state ("provisioning") had no wording in either
// surface, which is the exact defect these tests exist to catch. A scan of the
// one function that publishes them cannot go stale. Same mechanism as
// src/models/tests/tst_models.cpp, which reads the agent-state words out of the
// TypeScript that defines them.
//
// Fails loudly rather than returning an empty set: a test that quietly checks
// nothing is worse than no test.
static QStringList publishedConnectionStates()
{
    const QString path = QStringLiteral(CH_REPO_ROOT "/src/app/AppController.cpp");
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qFatal("cannot open %s: %s", qUtf8Printable(path),
               qUtf8Printable(source.errorString()));
    }
    const QString text = QString::fromUtf8(source.readAll());

    // Escaped rather than a raw string literal: moc's own preprocessor cannot
    // parse a raw string used as a macro argument.
    static const QRegularExpression re(
            QStringLiteral("setConnectionState\\(QStringLiteral\\(\"([a-z]+)\"\\)"));
    QStringList states;
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QString state = it.next().captured(1);
        if (!states.contains(state))
            states << state;
    }
    if (states.isEmpty())
        qFatal("no setConnectionState literals found in %s", qUtf8Printable(path));
    states.sort();
    return states;
}

// Expected status-chip rendering per connection state. A colour is a design
// decision, so it cannot be scanned out of the source the way the state NAMES
// can — but the two halves are joined here rather than listed twice: the data
// function walks publishedConnectionStates() and looks each one up in this map,
// so a state added to ch::AppController with no entry here fails the test with a
// message telling you to add one, instead of quietly never being exercised.
struct ChipExpectation {
    const char *colour = nullptr;
    bool busy = false;
};

static QHash<QString, ChipExpectation> chipExpectations()
{
    return {
        {QStringLiteral("disconnected"), {"#6c7086", false}},
        {QStringLiteral("connecting"), {"#f9e2af", true}},
        {QStringLiteral("hostkey"), {"#f9e2af", true}},
        {QStringLiteral("credential"), {"#f9e2af", true}},
        // Installing the remote service on first connect: in progress, so it
        // reads like connecting rather than like a fault.
        {QStringLiteral("provisioning"), {"#f9e2af", true}},
        {QStringLiteral("connected"), {"#a6e3a1", false}},
        {QStringLiteral("reconnecting"), {"#fab387", true}},
        {QStringLiteral("failed"), {"#f38ba8", false}},
    };
}

void TstUxShell::sheetStatusMatchesTheStatesTheAppPublishes_data()
{
    QTest::addColumn<QString>("state");
    QTest::addColumn<QString>("colour");
    QTest::addColumn<bool>("busy");

    const QHash<QString, ChipExpectation> expected = chipExpectations();
    for (const QString &state : publishedConnectionStates()) {
        const ChipExpectation e = expected.value(state);
        // An unknown state yields an EMPTY expected colour, which the test body
        // turns into a clear failure naming the state.
        QTest::newRow(qUtf8Printable(state))
                << state
                << QString::fromUtf8(e.colour ? e.colour : "")
                << e.busy;
    }
}

void TstUxShell::sheetStatusMatchesTheStatesTheAppPublishes()
{
    QFETCH(QString, state);
    QFETCH(QString, colour);
    QFETCH(bool, busy);

    QVERIFY2(!colour.isEmpty(),
             qPrintable(QStringLiteral("ch::AppController publishes the connection state "
                                       "\"%1\" but chipExpectations() in this file has no "
                                       "entry for it, so nothing checks how it is drawn")
                                .arg(state)));

    Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(900, 560));
    QVERIFY(surface.expose());
    QQuickItem *root = surface.root();
    QVERIFY(root);

    root->setProperty("connectionState", state);
    settle(40);

    QObject *dot = surface.child(QStringLiteral("stateDot"));
    QVERIFY2(dot, "the connection status chip has no stateDot");
    QCOMPARE(dot->property("color").value<QColor>(), QColor(colour));

    QObject *indicator = surface.child(QStringLiteral("connectingIndicator"));
    QVERIFY(indicator);
    QCOMPARE(indicator->property("running").toBool(), busy);

    // Whatever the colour says, the state is also spelled out in words.
    QVERIFY2(!textOf(surface.child(QStringLiteral("stateLabel"))).isEmpty(),
             "the connection state is conveyed by colour alone");

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

// The table above cannot be derived (a colour is a design decision, not
// something a scan can read), so this is what stops it going stale: every state
// the application actually publishes must have a row there, and the sheet must
// give it a glyph of its own rather than falling through to the "nothing is
// running" en dash. Both halves matter — a state missing from the table is
// untested, and a state the sheet does not recognise is drawn as if the
// application were idle.
void TstUxShell::everyPublishedStateHasASheetRowAndAGlyph()
{
    const QStringList published = publishedConnectionStates();
    QVERIFY(!published.isEmpty());

    Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(900, 560));
    QVERIFY(surface.expose());
    QQuickItem *root = surface.root();
    QVERIFY(root);

    // "disconnected" IS the fall-through, so it is the one state allowed to
    // render as the idle dash; every other one needs its own glyph.
    root->setProperty("connectionState", QStringLiteral("disconnected"));
    settle(40);
    QObject *dot = surface.child(QStringLiteral("stateDot"));
    QVERIFY2(dot, "the connection status chip has no stateDot");
    const QColor idleColour = dot->property("color").value<QColor>();

    // The glyph the dot carries. The test's name and the comment above have
    // always promised this reading, and nothing took it: a state with no case
    // in ConnectSheet.stateGlyph() falls through to the "nothing is running"
    // en dash, which is the same thing "disconnected" draws.
    auto *dotItem = qobject_cast<QQuickItem *>(dot);
    QVERIFY(dotItem);
    const auto glyphOfDot = [dotItem]() -> QString {
        const auto children = dotItem->childItems();
        return children.size() == 1 ? children.constFirst()->property("text").toString()
                                    : QString();
    };
    const QString idleGlyph = glyphOfDot();
    QVERIFY2(!idleGlyph.isEmpty(), "the status chip draws no glyph inside its dot");

    for (const QString &state : published) {
        if (state == QStringLiteral("disconnected"))
            continue;
        root->setProperty("connectionState", state);
        settle(40);
        QVERIFY2(dot->property("color").value<QColor>() != idleColour,
                 qPrintable(QStringLiteral("the sheet paints \"%1\" exactly as it paints "
                                           "\"disconnected\", so it has no case for it")
                                    .arg(state)));
        QVERIFY2(glyphOfDot() != idleGlyph,
                 qPrintable(QStringLiteral("the sheet gives \"%1\" the same glyph it gives "
                                           "\"disconnected\", so it has no case for it")
                                    .arg(state)));
    }

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

// The two prompt panels cover the sheet and swallow clicks, but a disabled-free
// sheet underneath is still TAB-REACHABLE: a keyboard user could reach Close and
// dismiss the whole sheet while ch::AppController sat waiting for the host-key
// answer that only this sheet can give, leaving an attempt stuck forever with
// nothing on screen to answer it.
void TstUxShell::sheetChromeIsDisabledWhileAPromptIsUp()
{
    Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(900, 560));
    QVERIFY(surface.expose());
    QQuickItem *root = surface.root();
    QVERIFY(root);
    root->setProperty("profiles", twoProfiles());
    settle(40);

    auto *closeButton = qobject_cast<QQuickItem *>(surface.child(QStringLiteral("closeButton")));
    auto *connectButton =
            qobject_cast<QQuickItem *>(surface.child(QStringLiteral("connectButton")));
    auto *upgradeButton =
            qobject_cast<QQuickItem *>(surface.child(QStringLiteral("upgradeButton")));
    auto *profileList = qobject_cast<QQuickItem *>(surface.child(QStringLiteral("profileList")));
    QVERIFY(closeButton && connectButton && upgradeButton && profileList);
    QVERIFY(closeButton->isEnabled());

    root->setProperty("pendingHostKey",
                      QVariantMap{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                  {QStringLiteral("keyType"), QStringLiteral("ssh-ed25519")},
                                  {QStringLiteral("fingerprint"), QStringLiteral("SHA256:abc")}});
    settle(40);

    QVERIFY2(!closeButton->isEnabled(),
             "Close is still reachable behind the host-key prompt: dismissing the sheet there "
             "abandons a connect attempt that is waiting for the answer");
    QVERIFY2(!connectButton->isEnabled(), "Connect is still reachable behind the host-key prompt");
    QVERIFY2(!upgradeButton->isEnabled(),
             "Update server is still reachable behind the host-key prompt: it tears the "
             "parked attempt down and reinstalls the service under it");
    QVERIFY2(!profileList->isEnabled(), "the profile list is still usable behind the prompt");

    // ...while the panel that must be answered stays live.
    auto *reject =
            qobject_cast<QQuickItem *>(surface.child(QStringLiteral("hostKeyRejectButton")));
    QVERIFY(reject);
    QVERIFY2(reject->isEnabled(), "the host-key prompt disabled its own answer buttons");

    // Answering it hands the sheet back.
    root->setProperty("pendingHostKey", QVariant());
    settle(40);
    QVERIFY2(closeButton->isEnabled(), "the sheet stayed disabled after the prompt was answered");

    // Same rule for the credential prompt.
    root->setProperty("pendingCredential",
                      QVariantMap{{QStringLiteral("user"), QStringLiteral("yichen")},
                                  {QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                  {QStringLiteral("prompt"), QStringLiteral("Password:")},
                                  {QStringLiteral("kind"), QStringLiteral("password")}});
    settle(40);
    QVERIFY2(!closeButton->isEnabled(), "Close is still reachable behind the credential prompt");

    root->setProperty("pendingCredential", QVariant());
    settle(40);
    QVERIFY(closeButton->isEnabled());

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

// The button is the only way most people will ever update the service on their
// server, and it must carry the profile the sheet is showing: an upgrade
// request with an empty id installs onto whatever profile happens to be active,
// which is not necessarily the one on screen.
void TstUxShell::sheetUpdateServerButtonNamesItsProfile()
{
    Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(900, 560));
    QVERIFY(surface.expose());
    QQuickItem *root = surface.root();
    QVERIFY(root);

    auto *upgradeButton =
            qobject_cast<QQuickItem *>(surface.child(QStringLiteral("upgradeButton")));
    QVERIFY(upgradeButton);

    // Nothing selected: there is no server to install onto, so the button is
    // dead rather than firing a request with an empty id.
    root->setProperty("profiles", QVariantList());
    settle(40);
    QVERIFY2(!upgradeButton->isEnabled(),
             "Update server is live with no profile to update");

    root->setProperty("profiles", twoProfiles());
    root->setProperty("activeId", QStringLiteral("id-b"));
    settle(40);
    QVERIFY(upgradeButton->isEnabled());

    QSignalSpy requested(root, SIGNAL(upgradeRequested(QString)));
    QMetaObject::invokeMethod(upgradeButton, "clicked");
    settle(40);
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.at(0).at(0).toString(), QStringLiteral("id-b"));

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

void TstUxShell::sidebarFooterNamesEveryConnectionState_data()
{
    QTest::addColumn<QString>("state");

    // Derived from the source, never listed here: a state added to
    // AppController with no wording in the footer must fail this test rather
    // than wait for somebody to remember to add a row.
    for (const QString &state : publishedConnectionStates())
        QTest::newRow(qUtf8Printable(state)) << state;
}

// Once the connection sheet is closed the sidebar footer is the ONLY thing on
// screen saying whether the server is reachable, so a state it has no word for
// silently reads as "Not connected" — which is a lie for "credential" (the app
// is waiting for the user's password) and for "connecting".
void TstUxShell::sidebarFooterNamesEveryConnectionState()
{
    QFETCH(QString, state);

    ch::SessionsModel model;
    model.setGroups(populatedGroups());
    ShotApp app(&model);
    Surface surface(moduleUrl(QStringLiteral("SessionsSidebar.qml")), QSize(300, 620), &app);
    QVERIFY(surface.expose());
    QVERIFY(surface.root());

    // The wording for "there is no server", i.e. the fall-through answer no
    // other state may share.
    app.setConnectionState(QStringLiteral("disconnected"));
    settle(40);
    const QString idleWords = textOf(surface.child(QStringLiteral("linkStatusLabel")));
    QVERIFY2(!idleWords.isEmpty(), "the sidebar footer says nothing at all");

    app.setConnectionState(state);
    settle(40);
    const QString words = textOf(surface.child(QStringLiteral("linkStatusLabel")));
    QVERIFY2(!words.isEmpty(), qPrintable(QStringLiteral("no footer wording for \"%1\"").arg(state)));
    if (state != QStringLiteral("disconnected")) {
        QVERIFY2(words != idleWords,
                 qPrintable(QStringLiteral("the footer describes \"%1\" as \"%2\", the same thing "
                                           "it says when there is no server at all")
                                    .arg(state, words)));
    }

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}
void TstUxShell::sidebarArchiveButtonCallsArchiveAndUnarchive()
{
    {
        ch::SessionsModel model;
        model.setGroups({makeGroup(QStringLiteral("g"), QStringLiteral("G"),
                                   {{QStringLiteral("s"), ch::SessionRowState::Idle}})});
        ShotApp app(&model);
        app.setConnectionState(QStringLiteral("connected"));
        Surface surface(moduleUrl(QStringLiteral("SessionsSidebar.qml")),
                        QSize(320, 620), &app);
        QVERIFY(surface.expose());
        settle(40);

        QObject *archive = surface.child(QStringLiteral("archiveButton:s"));
        QVERIFY2(archive, "the row has no archive action");
        QObject *marker = surface.child(QStringLiteral("archivedMarker:s"));
        QVERIFY2(marker, "the row has no archived marker at all, so the empty reading "
                         "below would pass for the wrong reason");
        QCOMPARE(textOf(marker), QString());
        QMetaObject::invokeMethod(archive, "clicked");
        QCOMPARE(app.archiveCalls(), 1);
        QCOMPARE(app.lastArchiveId(), QStringLiteral("s"));
        QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
    }

    ch::SessionsModel model;
    auto archivedGroups = makeGroup(
        QStringLiteral("g"), QStringLiteral("G"),
        {{QStringLiteral("s"), ch::SessionRowState::Idle}});
    archivedGroups.sessions[0].session.archived = true;
    model.setGroups({archivedGroups});
    model.setShowArchived(true);
    ShotApp app(&model);
    app.setConnectionState(QStringLiteral("connected"));
    Surface surface(moduleUrl(QStringLiteral("SessionsSidebar.qml")),
                    QSize(320, 620), &app);
    QVERIFY(surface.expose());
    settle(40);
    // The remembered filter state is read before the sidebar synchronises the
    // model; construction must not silently turn it back off.
    QVERIFY(model.showArchived());
    QObject *unarchive = surface.child(QStringLiteral("archiveButton:s"));
    QVERIFY2(unarchive, "the archived row has no unarchive action");
    QObject *marker = surface.child(QStringLiteral("archivedMarker:s"));
    QVERIFY2(marker, "the archived row has no archived marker");
    QCOMPARE(textOf(marker), QStringLiteral("\u25a3"));
    QMetaObject::invokeMethod(unarchive, "clicked");
    QCOMPARE(app.unarchiveCalls(), 1);
    QCOMPARE(app.lastArchiveId(), QStringLiteral("s"));
    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}
void TstUxShell::sidebarDeleteActionsAreConfirmedAndNamed()
{
    ch::SessionsModel model;
    auto group = makeGroup(
        QStringLiteral("g"), QStringLiteral("Client work"),
        {{QStringLiteral("s-visible"), ch::SessionRowState::Idle},
         {QStringLiteral("s-archived-a"), ch::SessionRowState::Idle},
         {QStringLiteral("s-archived-b"), ch::SessionRowState::Idle}});
    // Only the first row is visible under the normal archive filter. The
    // authoritative count still has to include the two hidden rows because a
    // group deletion destroys all three.
    group.sessions[1].session.archived = true;
    group.sessions[2].session.archived = true;
    model.setGroups({group});

    ShotApp app(&model);
    app.setAuthoritativeSessionCount(QStringLiteral("g"), 3);
    app.setConnectionState(QStringLiteral("connected"));
    Surface surface(moduleUrl(QStringLiteral("SessionsSidebar.qml")),
                    QSize(360, 620), &app);
    QVERIFY(surface.expose());
    settle(40);

    QObject *sessionDelete =
        surface.child(QStringLiteral("deleteButton:s-visible"));
    QVERIFY2(sessionDelete, "the Dev Session row has no delete button");
    QCOMPARE(sessionDelete->property("actionText").toString(),
             QStringLiteral("Delete s-visible"));
    QVERIFY((sessionDelete->property("focusPolicy").toInt() & int(Qt::TabFocus)) != 0);
    QVERIFY(sessionDelete->property("width").toReal() >= 24);
    QVERIFY(sessionDelete->property("height").toReal() >= 24);

    QMetaObject::invokeMethod(sessionDelete, "clicked");
    settle(40);
    QObject *sessionDialog =
        surface.child(QStringLiteral("deleteSessionDialog:s-visible"));
    QObject *sessionMessage =
        surface.child(QStringLiteral("deleteSessionMessage:s-visible"));
    QVERIFY2(sessionDialog && sessionDialog->property("visible").toBool(),
             "deleting a Dev Session did not open its confirmation");
    QVERIFY(sessionMessage);
    QVERIFY(sessionMessage->property("text").toString().contains(
        QStringLiteral("s-visible")));
    QMetaObject::invokeMethod(sessionDialog, "reject");
    settle(40);
    QCOMPARE(app.deleteSessionCalls(), 0);

    // Accepting is the only path that reaches the host mutation.
    QMetaObject::invokeMethod(sessionDelete, "clicked");
    settle(20);
    QMetaObject::invokeMethod(sessionDialog, "accept");
    settle(20);
    QCOMPARE(app.deleteSessionCalls(), 1);
    QCOMPARE(app.lastDeleteSessionId(), QStringLiteral("s-visible"));

    QObject *groupDelete = surface.child(QStringLiteral("deleteGroupButton:g"));
    QVERIFY2(groupDelete, "the group header has no delete button");
    QCOMPARE(groupDelete->property("actionText").toString(),
             QStringLiteral("Delete group \"Client work\""));
    QVERIFY((groupDelete->property("focusPolicy").toInt() & int(Qt::TabFocus)) != 0);
    QVERIFY(groupDelete->property("width").toReal() >= 24);
    QVERIFY(groupDelete->property("height").toReal() >= 24);

    QMetaObject::invokeMethod(groupDelete, "clicked");
    settle(40);
    QObject *groupDialog = surface.child(QStringLiteral("deleteGroupDialog:g"));
    QObject *groupMessage = surface.child(QStringLiteral("deleteGroupMessage:g"));
    QVERIFY2(groupDialog && groupDialog->property("visible").toBool(),
             "deleting a group did not open its confirmation");
    QVERIFY(groupMessage);
    const QString confirmation = groupMessage->property("text").toString();
    QVERIFY(confirmation.contains(QStringLiteral("Client work")));
    QVERIFY(confirmation.contains(QStringLiteral("3 sessions")));
    QMetaObject::invokeMethod(groupDialog, "reject");
    settle(40);
    QCOMPARE(app.deleteGroupCalls(), 0);

    QMetaObject::invokeMethod(groupDelete, "clicked");
    settle(20);
    QMetaObject::invokeMethod(groupDialog, "accept");
    settle(20);
    QCOMPARE(app.deleteGroupCalls(), 1);
    QCOMPARE(app.lastDeleteGroupId(), QStringLiteral("g"));

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

void TstUxShell::sidebarArchiveFilterToggleIsNamedAndChangesModel()
{
    ch::SessionsModel model;
    model.setGroups({makeGroup(QStringLiteral("g"), QStringLiteral("G"),
                               {{QStringLiteral("s"), ch::SessionRowState::Idle}})});
    ShotApp app(&model);
    app.setConnectionState(QStringLiteral("connected"));
    Surface surface(moduleUrl(QStringLiteral("SessionsSidebar.qml")),
                    QSize(320, 620), &app);
    QVERIFY(surface.expose());
    settle(40);

    QObject *toggle = surface.child(QStringLiteral("archiveFilterButton"));
    QVERIFY2(toggle, "the sidebar has no show-archived toggle");
    QCOMPARE(toggle->property("actionText").toString(),
             QStringLiteral("Show archived sessions"));
    QMetaObject::invokeMethod(toggle, "clicked");
    settle(40);
    QVERIFY(model.showArchived());
    QCOMPARE(toggle->property("actionText").toString(),
             QStringLiteral("Hide archived sessions"));
    QVERIFY(surface.warnings.isEmpty());
}

void TstUxShell::sidebarArchiveEmptyStateExplainsArchivedSessions()
{
    ch::SessionsModel model;
    auto archivedGroups = makeGroup(
        QStringLiteral("g"), QStringLiteral("G"),
        {{QStringLiteral("s"), ch::SessionRowState::Idle}});
    archivedGroups.sessions[0].session.archived = true;
    model.setGroups({archivedGroups});
    ShotApp app(&model);
    app.setConnectionState(QStringLiteral("connected"));
    Surface surface(moduleUrl(QStringLiteral("SessionsSidebar.qml")),
                    QSize(320, 620), &app);
    QVERIFY(surface.expose());
    settle(40);

    QCOMPARE(textOf(surface.child(QStringLiteral("sidebarEmptyTitle"))),
             QStringLiteral("All your sessions are archived"));
    QCOMPARE(textOf(surface.child(QStringLiteral("sidebarEmptyHint"))),
             QStringLiteral("Show archived sessions to see them here."));
    QVERIFY(surface.warnings.isEmpty());
}

// SPEC 4.2 gives six aggregate row states in a fixed precedence — Error,
// Waiting for input, Running, Finished with unseen output, Idle, Disconnected —
// and the sidebar row is where a user reads them. Two things have to hold, and
// neither was defended anywhere: each state must be drawn as ITSELF (a swapped
// case in SessionRow's colour table is invisible to every other test), and each
// must be told apart THREE ways, because a six-shade dot is exactly the
// encoding a colour-blind reader cannot use and a greyscale screenshot loses.
//
// The colours are spelled out rather than read from Theme for the same reason
// chipExpectations() above spells its own out: which role a state is painted in
// is a design decision, and pinning it here is the point.
void TstUxShell::sidebarRowBadgeSeparatesEveryRowState()
{
    using S = ch::SessionRowState;
    const QVector<QPair<QString, S>> sessions{{QStringLiteral("err"), S::Error},
                                              {QStringLiteral("ask"), S::WaitingForInput},
                                              {QStringLiteral("run"), S::Running},
                                              {QStringLiteral("done"), S::FinishedUnseen},
                                              {QStringLiteral("idle"), S::Idle},
                                              {QStringLiteral("gone"), S::Disconnected}};
    // Theme.danger / warning / success / accent / textDim / textFaint, in the
    // precedence order above.
    const QStringList expected{QStringLiteral("#f38ba8"), QStringLiteral("#f9e2af"),
                               QStringLiteral("#a6e3a1"), QStringLiteral("#89b4fa"),
                               QStringLiteral("#6c7086"), QStringLiteral("#45475a")};

    ch::SessionsModel model;
    model.setGroups({makeGroup(QStringLiteral("g"), QStringLiteral("G"), sessions)});
    ShotApp app(&model);
    app.setConnectionState(QStringLiteral("connected"));
    Surface surface(moduleUrl(QStringLiteral("SessionsSidebar.qml")), QSize(340, 620), &app);
    QVERIFY(surface.expose());
    settle(60);

    QSet<QString> colours;
    QSet<QString> glyphs;
    QSet<QString> words;
    QObject *anyRow = nullptr;
    for (int i = 0; i < sessions.size(); ++i) {
        const QString id = sessions.at(i).first;
        QObject *row = surface.child(QStringLiteral("sessionRow:") + id);
        QVERIFY2(row, qPrintable(QStringLiteral("no sidebar row for \"%1\"").arg(id)));
        anyRow = row;

        QObject *dot = surface.child(QStringLiteral("statusDot:") + id);
        QVERIFY2(dot, qPrintable(QStringLiteral("the \"%1\" row has no status dot").arg(id)));
        const QColor drawn = dot->property("color").value<QColor>();
        QCOMPARE(drawn, QColor(expected.at(i)));
        QVERIFY2(drawn.isValid(),
                 qPrintable(QStringLiteral("row state %1 has no rendered colour").arg(i)));
        colours.insert(drawn.name());

        QObject *glyph = surface.child(QStringLiteral("statusGlyph:") + id);
        QVERIFY2(glyph, qPrintable(QStringLiteral("the \"%1\" row has no status glyph").arg(id)));
        QVERIFY2(!textOf(glyph).isEmpty(),
                 qPrintable(QStringLiteral("row state %1 is carried by colour alone").arg(i)));
        glyphs.insert(textOf(glyph));
        QVariant expectedGlyph;
        QVERIFY(QMetaObject::invokeMethod(anyRow, "stateGlyph",
                                          Q_RETURN_ARG(QVariant, expectedGlyph),
                                          Q_ARG(QVariant, QVariant(i))));
        QVERIFY(!expectedGlyph.toString().isEmpty());
        // COMPARED, not merged into the set: when the drawn glyph and the row's
        // own table disagreed, both landed in `glyphs` and the size check at the
        // end started asserting something nobody meant.
        QCOMPARE(textOf(glyph), expectedGlyph.toString());

        QVariant expectedColour;
        QVERIFY(QMetaObject::invokeMethod(anyRow, "stateColor",
                                          Q_RETURN_ARG(QVariant, expectedColour),
                                          Q_ARG(QVariant, QVariant(i))));
        QCOMPARE(expectedColour.value<QColor>(), drawn);
    }
    QVERIFY(anyRow);

    // The words the row's tooltip and its accessible description are built
    // from. Asked of the row itself, so an unreachable state cannot hide.
    for (int i = 0; i < sessions.size(); ++i) {
        QVariant said;
        QVERIFY(QMetaObject::invokeMethod(anyRow, "stateWords", Q_RETURN_ARG(QVariant, said),
                                          Q_ARG(QVariant, QVariant(i))));
        QVERIFY2(!said.toString().isEmpty(),
                 qPrintable(QStringLiteral("row state %1 has no wording").arg(i)));
        words.insert(said.toString());
    }

    QCOMPARE(colours.size(), sessions.size());
    QCOMPARE(glyphs.size(), sessions.size());
    QCOMPARE(words.size(), sessions.size());

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

void TstUxShell::sheetErrorIsReadableAndDismissible()
{
    Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(900, 560));
    QVERIFY(surface.expose());
    QQuickItem *root = surface.root();
    QVERIFY(root);

    QQuickItem *banner = qobject_cast<QQuickItem *>(surface.child(QStringLiteral("errorBanner")));
    QVERIFY(banner);
    QVERIFY(!banner->isVisible());

    const QString message = QStringLiteral(
            "ssh: connect to host build-07.internal.example.com port 22022: No route to host "
            "(check that the machine is up and that port 22022 is reachable from here)");
    root->setProperty("errorText", message);
    settle(40);
    QVERIFY(banner->isVisible());

    QObject *label = surface.child(QStringLiteral("errorLabel"));
    QVERIFY(label);
    QCOMPARE(textOf(label), message);
    // Legible means the whole sentence is reachable: a single elided line of a
    // 160-character ssh error tells the user nothing.
    QVERIFY2(label->property("wrapMode").toInt() != 0,
             "the error is squeezed onto one elided line");
    auto *labelItem = qobject_cast<QQuickItem *>(label);
    QVERIFY(labelItem);
    QVERIFY2(labelItem->height() + 8 <= banner->height(),
             qPrintable(QStringLiteral("banner %1px cannot show a %2px error")
                                .arg(banner->height())
                                .arg(labelItem->height())));

    QObject *dismiss = surface.child(QStringLiteral("errorDismissButton"));
    QVERIFY2(dismiss, "the error banner cannot be dismissed");
    QMetaObject::invokeMethod(dismiss, "clicked");
    settle(40);
    QVERIFY2(!banner->isVisible(), "dismissing the error left the banner up");

    // The next failure must not be swallowed by the previous dismissal.
    root->setProperty("errorText", QStringLiteral("permission denied (publickey)"));
    settle(40);
    QVERIFY2(banner->isVisible(), "a new error stayed hidden after an earlier dismissal");

    // Clearing it still hides it, which is the contract tst_serverprofiles pins.
    root->setProperty("errorText", QString());
    settle(40);
    QVERIFY(!banner->isVisible());

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

void TstUxShell::sheetColdStartExplainsItself()
{
    Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(900, 560));
    QVERIFY(surface.expose());
    QVERIFY(surface.root());

    QQuickItem *intro = qobject_cast<QQuickItem *>(surface.child(QStringLiteral("coldStartIntro")));
    QVERIFY2(intro, "nothing tells a first-time user what this sheet is for");
    QVERIFY(intro->isVisible());
    QVERIFY(textOf(intro).length() > 20);

    // ...and it gets out of the way once there is something to connect to.
    surface.root()->setProperty("profiles", twoProfiles());
    settle(40);
    QVERIFY2(!intro->isVisible(), "the first-run explainer never goes away");

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

void TstUxShell::sheetRejectsPartiallyNumericPorts()
{
    Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(900, 560));
    QVERIFY(surface.expose());
    QQuickItem *root = surface.root();
    QVERIFY(root);

    QObject *host = surface.child(QStringLiteral("hostField"));
    QObject *user = surface.child(QStringLiteral("userField"));
    QObject *port = surface.child(QStringLiteral("portField"));
    QVERIFY(host && user && port);
    host->setProperty("text", QStringLiteral("example.test"));
    user->setProperty("text", QStringLiteral("alice"));
    port->setProperty("text", QStringLiteral("22abc"));

    QVariant valid;
    QVERIFY(QMetaObject::invokeMethod(root, "formValid", Q_RETURN_ARG(QVariant, valid)));
    QVERIFY2(!valid.toBool(), "a port with a numeric prefix was accepted as valid");
    QVERIFY(QMetaObject::invokeMethod(root, "portValue", Q_RETURN_ARG(QVariant, valid)));
    QCOMPARE(valid.toInt(), 0);

    port->setProperty("text", QStringLiteral("2200"));
    QVERIFY(QMetaObject::invokeMethod(root, "formValid", Q_RETURN_ARG(QVariant, valid)));
    QVERIFY(valid.toBool());
    QVERIFY(QMetaObject::invokeMethod(root, "portValue", Q_RETURN_ARG(QVariant, valid)));
    QCOMPARE(valid.toInt(), 2200);
    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

// ServerProfiles refuses a profile whose host or login name carries whitespace
// or a control character: no hostname, IP literal or POSIX user name can
// contain one, so such a profile could only ever fail at connect time with an
// opaque resolver error. The refusal is silent by design (an invalid edit must
// not corrupt a working profile), which is exactly why the SHEET has to apply
// the same rule: a Save button that is live for a value the store will drop
// tells the user the server was saved and then produces no row at all.
//
// The rule is whitespace and control characters ONLY, never hostname grammar:
// ':' and '%' have to stay legal or an IPv6 literal with a zone identifier
// becomes unsavable.
void TstUxShell::sheetRejectsAHostOrUserThatIsNotASingleWord()
{
    Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(900, 560));
    QVERIFY(surface.expose());
    QQuickItem *root = surface.root();
    QVERIFY(root);

    QObject *host = surface.child(QStringLiteral("hostField"));
    QObject *user = surface.child(QStringLiteral("userField"));
    QObject *port = surface.child(QStringLiteral("portField"));
    auto *hint = qobject_cast<QQuickItem *>(surface.child(QStringLiteral("validationHint")));
    auto *save = qobject_cast<QQuickItem *>(surface.child(QStringLiteral("saveButton")));
    QVERIFY(host && user && port && hint && save);

    QVariant valid;

    // The way a real user gets here: pasting a whole ssh command line into the
    // host field.
    host->setProperty("text", QStringLiteral("box.local -p 2222"));
    user->setProperty("text", QStringLiteral("alice"));
    port->setProperty("text", QStringLiteral("22"));
    settle(40);

    QVERIFY(QMetaObject::invokeMethod(root, "formValid", Q_RETURN_ARG(QVariant, valid)));
    QVERIFY2(!valid.toBool(), "a host containing a space was accepted as valid");
    QVERIFY2(!save->isEnabled(),
             "Save is live for a host the store will silently drop");
    QVERIFY2(hint->isVisible(), "nothing on screen says why the host is refused");
    QVERIFY2(textOf(hint).contains(QStringLiteral("host"), Qt::CaseInsensitive),
             qPrintable(QStringLiteral("the hint does not name the host: \"%1\"")
                                .arg(textOf(hint))));

    // Same rule for the login name, and it has to say so.
    host->setProperty("text", QStringLiteral("box.local"));
    user->setProperty("text", QStringLiteral("al ice"));
    settle(40);
    QVERIFY(QMetaObject::invokeMethod(root, "formValid", Q_RETURN_ARG(QVariant, valid)));
    QVERIFY2(!valid.toBool(), "a user name containing a space was accepted as valid");
    QVERIFY2(textOf(hint).contains(QStringLiteral("user"), Qt::CaseInsensitive),
             qPrintable(QStringLiteral("the hint does not name the user: \"%1\"")
                                .arg(textOf(hint))));

    // A tab is whitespace too, and it is invisible in the field.
    host->setProperty("text", QStringLiteral("box\tlocal"));
    user->setProperty("text", QStringLiteral("alice"));
    settle(40);
    QVERIFY(QMetaObject::invokeMethod(root, "formValid", Q_RETURN_ARG(QVariant, valid)));
    QVERIFY2(!valid.toBool(), "a host containing a tab was accepted as valid");

    // ...and the gate must not have become hostname grammar. Surrounding
    // whitespace is trimmed, exactly as the store trims it, so it is not a
    // reason to refuse anything either.
    QSignalSpy saved(root, SIGNAL(profileSaved(QVariant)));
    QVERIFY(saved.isValid());
    host->setProperty("text", QStringLiteral("  fe80::1%eth0  "));
    settle(40);
    QVERIFY(QMetaObject::invokeMethod(root, "formValid", Q_RETURN_ARG(QVariant, valid)));
    QVERIFY2(valid.toBool(),
             "an IPv6 address with a zone identifier was refused; the gate has "
             "turned into hostname grammar");
    QVERIFY(save->isEnabled());
    QVERIFY2(!hint->isVisible(), "a saveable form still shows a validation hint");

    // The accepted path is not merely un-blocked, it actually saves.
    QMetaObject::invokeMethod(root, "save");
    settle(40);
    QCOMPARE(saved.size(), 1);

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

void TstUxShell::sheetModelRefreshKeepsUnsavedEdits()
{
    Surface surface(moduleUrl(QStringLiteral("ConnectSheet.qml")), QSize(900, 560));
    QVERIFY(surface.expose());
    QQuickItem *root = surface.root();
    QVERIFY(root);
    root->setProperty("profiles", twoProfiles());
    root->setProperty("activeId", QStringLiteral("id-a"));
    settle(40);

    QObject *host = surface.child(QStringLiteral("hostField"));
    QVERIFY(host);
    host->setProperty("text", QStringLiteral("draft.example"));
    QCOMPARE(host->property("text").toString(), QStringLiteral("draft.example"));

    QVariantList refreshed = twoProfiles();
    refreshed.append(QVariantMap{{QStringLiteral("id"), QStringLiteral("id-c")},
                                 {QStringLiteral("name"), QStringLiteral("New box")},
                                 {QStringLiteral("host"), QStringLiteral("192.0.2.4")},
                                 {QStringLiteral("port"), 22},
                                 {QStringLiteral("user"), QStringLiteral("alice")}});
    root->setProperty("profiles", refreshed);
    settle(40);
    QCOMPARE(host->property("text").toString(), QStringLiteral("draft.example"));

    // activeId is a host-side background update, not a profile-row click.
    // A real click calls selectIndex() and intentionally loads that profile;
    // this refresh must not destroy the draft while the host catches up.
    root->setProperty("activeId", QStringLiteral("id-b"));
    settle(40);
    QCOMPARE(host->property("text").toString(), QStringLiteral("draft.example"));
    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}


void TstUxShell::inertTerminalPaneExplainsItself()
{
    Surface surface(moduleUrl(QStringLiteral("TerminalPaneView.qml")), QSize(520, 300));
    QVERIFY(surface.expose());
    QQuickItem *root = surface.root();
    QVERIFY(root);
    root->setProperty("paneId", QStringLiteral("terminal-1"));
    settle(60);

    QObject *title = surface.child(QStringLiteral("paneTitle"));
    QVERIFY(title);
    // The headline is what this pane IS, not the internal id it is keyed by.
    QCOMPARE(textOf(title), QStringLiteral("Terminal"));

    QObject *reason = surface.child(QStringLiteral("paneReason"));
    QVERIFY(reason);
    QVERIFY2(textOf(reason).length() > 3, "an inert terminal gives no reason");

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

void TstUxShell::emptyViewerPaneExplainsItself()
{
    Surface surface(moduleUrl(QStringLiteral("ViewerPane.qml")), QSize(520, 300));
    QVERIFY(surface.expose());
    QQuickItem *root = surface.root();
    QVERIFY(root);
    root->setProperty("paneId", QStringLiteral("viewer-1"));
    settle(60);

    QObject *title = surface.child(QStringLiteral("emptyTitle"));
    QVERIFY2(title, "an empty viewer pane has no empty state");
    // Not the raw pane id: that is plumbing, not a message to a user.
    QVERIFY2(textOf(title) != QStringLiteral("viewer-1"),
             "the empty viewer pane shows its internal pane id as the headline");
    QVERIFY(textOf(title).length() > 3);

    QObject *hint = surface.child(QStringLiteral("emptyHint"));
    QVERIFY2(hint, "an empty viewer pane does not say how to fill it");
    QVERIFY(textOf(hint).length() > 10);

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

void TstUxShell::titleBarMaximiseButtonTogglesTheWindowState()
{
    StubWindow stub;
    Surface surface(titleBarHarness(), QSize(640, 120), QStringLiteral("stubWindow"), &stub);
    QVERIFY2(surface.root(), qPrintable(surface.componentError()));
    QVERIFY(surface.expose());
    settle(60);

    QObject *maximise = surface.child(QStringLiteral("maximiseButton"));
    QVERIFY2(maximise, "the title bar has no maximise button");
    QCOMPARE(stub.visibility(), int(QWindow::Windowed));

    // The button's own name is the ACTION it will perform, so it has to change
    // with the state: "Maximise window" on a window that is already maximised is
    // what a screen-reader user would act on.
    QCOMPARE(accessibleName(surface.root(), maximise), QStringLiteral("Maximise window"));

    QMetaObject::invokeMethod(maximise, "clicked");
    settle(60);
    QCOMPARE(stub.visibility(), int(QWindow::Maximized));
    QCOMPARE(accessibleName(surface.root(), maximise), QStringLiteral("Restore window"));

    QMetaObject::invokeMethod(maximise, "clicked");
    settle(60);
    QCOMPARE(stub.visibility(), int(QWindow::Windowed));

    // The bar's own double-click handler calls exactly this, so the gesture and
    // the button cannot drift apart.
    QObject *bar = surface.child(QStringLiteral("titleBar"));
    QVERIFY(bar);
    QVERIFY(QMetaObject::invokeMethod(bar, "toggleMaximised"));
    settle(60);
    QCOMPARE(stub.visibility(), int(QWindow::Maximized));

    // Drive the real pointer gesture as well: the drag threshold must not turn
    // an ordinary double-click into a move operation.
    stub.setVisibility(int(QWindow::Windowed));
    QTest::mouseDClick(&surface.view, Qt::LeftButton, Qt::NoModifier, QPoint(120, 15));
    settle(60);
    QCOMPARE(stub.visibility(), int(QWindow::Maximized));
    QTest::mouseDClick(&surface.view, Qt::LeftButton, Qt::NoModifier, QPoint(120, 15));
    settle(60);
    QCOMPARE(stub.visibility(), int(QWindow::Windowed));

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

void TstUxShell::titleBarDragUsesTheSystemMoveOperation()
{
    StubWindow stub;
    StubNativeHelper helper;
    Surface surface(titleBarHarness(), QSize(640, 120), QStringLiteral("stubWindow"), &stub);
    QVERIFY2(surface.root(), qPrintable(surface.componentError()));
    QVERIFY(surface.expose());
    settle(60);

    QObject *bar = surface.child(QStringLiteral("titleBar"));
    QVERIFY(bar);
    bar->setProperty("nativeHelper",
                     QVariant::fromValue(static_cast<QObject *>(&helper)));
    settle(20);
    QCOMPARE(helper.registerCalls(), 1);
    QCOMPARE(helper.window(), static_cast<QObject *>(&stub));
    QVERIFY(helper.button());
    QCOMPARE(helper.button()->objectName(), QStringLiteral("maximiseButton"));

    const QPoint start(120, 15);
    QTest::mousePress(&surface.view, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&surface.view, start + QPoint(2, 0));
    settle(20);
    QCOMPARE(stub.moveCalls(), 0);

    // The first movement beyond Qt's drag threshold must hand the pointer to
    // the window manager, rather than changing Window.x/y from QML. The stub
    // records the call because the offscreen platform cannot perform a native
    // move itself.
    QTest::mouseMove(&surface.view, start + QPoint(40, 0));
    settle(60);
    QCOMPARE(stub.moveCalls(), 1);
    QCOMPARE(stub.moveVisibility(), int(QWindow::Windowed));
    QTest::mouseRelease(&surface.view, Qt::LeftButton, Qt::NoModifier,
                        start + QPoint(40, 0));

    // A maximised window must be restored before the same system move begins;
    // otherwise the platform has no window rectangle it can move.
    stub.setVisibility(int(QWindow::Maximized));
    QTest::mousePress(&surface.view, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&surface.view, start + QPoint(40, 0));
    settle(60);
    QCOMPARE(stub.moveCalls(), 2);
    QCOMPARE(stub.moveVisibility(), int(QWindow::Windowed));
    QCOMPARE(stub.visibility(), int(QWindow::Windowed));
    QTest::mouseRelease(&surface.view, Qt::LeftButton, Qt::NoModifier,
                        start + QPoint(40, 0));

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

void TstUxShell::titleBarButtonsAreNamedAndCloseIsWired()
{
    StubWindow stub;
    Surface surface(titleBarHarness(), QSize(640, 120), QStringLiteral("stubWindow"), &stub);
    QVERIFY2(surface.root(), qPrintable(surface.componentError()));
    QVERIFY(surface.expose());
    settle(60);

    // Every window control is a glyph with no text, so Accessible.name is the
    // ONLY thing that says what it does.
    const QStringList buttons{QStringLiteral("minimiseButton"), QStringLiteral("maximiseButton"),
                              QStringLiteral("closeButton")};
    for (const QString &name : buttons) {
        QObject *button = surface.child(name);
        QVERIFY2(button, qPrintable(QStringLiteral("the title bar has no %1").arg(name)));
        QVERIFY2(!accessibleName(surface.root(), button).isEmpty(),
                 qPrintable(QStringLiteral("%1 has no Accessible.name, so a screen reader "
                                           "announces an unlabelled button")
                                    .arg(name)));
    }

    // ...and the destructive one actually closes the window it was handed. A
    // frameless window has no other pointer route out.
    QCOMPARE(stub.closeCalls(), 0);
    QMetaObject::invokeMethod(surface.child(QStringLiteral("closeButton")), "clicked");
    settle(60);
    QCOMPARE(stub.closeCalls(), 1);

    // The session in front of the user is named beside the window title, not left
    // to the pane headers alone.
    QCOMPARE(textOf(surface.child(QStringLiteral("windowTitleLabel"))),
             QStringLiteral("CodeHarbor"));
    QCOMPARE(textOf(surface.child(QStringLiteral("sessionLabel"))), QStringLiteral("codeharbor"));

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

void TstUxShell::overlaySheetsSwallowClicksToTheWorkspace_data()
{
    QTest::addColumn<QByteArray>("component");
    QTest::addColumn<QByteArray>("extra");
    QTest::addColumn<QPoint>("blankSpot");

    // A point on each sheet that carries no control of its own: the strip of
    // header between the title and the buttons, and (for the log) the sheet's
    // own outer margin.
    QTest::newRow("ConnectSheet") << QByteArray("ConnectSheet") << QByteArray()
                                  << QPoint(450, 22);
    QTest::newRow("SettingsWindow") << QByteArray("SettingsWindow")
                                    << QByteArray("shown: true") << QPoint(450, 23);
    QTest::newRow("LogView") << QByteArray("LogView") << QByteArray("shown: true")
                             << QPoint(450, 6);
}

// Each of these sheets fills the window at a high z on top of the sidebar, the
// viewer and the terminal. They are Rectangles, and a Rectangle accepts no
// mouse input, so Qt Quick hands any press that missed one of their controls to
// the next item DOWN — the user clicks the settings background and focuses, or
// types into, a terminal they cannot see.
void TstUxShell::overlaySheetsSwallowClicksToTheWorkspace()
{
    QFETCH(QByteArray, component);
    QFETCH(QByteArray, extra);
    QFETCH(QPoint, blankSpot);

    Surface surface(overlayHarness(component, extra), QSize(900, 560),
                    QStringLiteral("unusedContext"), nullptr);
    QVERIFY2(surface.root(), qPrintable(surface.componentError()));
    QVERIFY(surface.expose());
    settle(60);

    // Control case: with the sheet hidden the very same press DOES reach the
    // workspace, so a pass below cannot come from aiming at nothing.
    QTest::mouseClick(&surface.view, Qt::LeftButton, Qt::NoModifier, blankSpot);
    settle(60);
    QCOMPARE(surface.root()->property("behindClicks").toInt(), 1);

    surface.root()->setProperty("sheetVisible", true);
    settle(60);
    QTest::mouseClick(&surface.view, Qt::LeftButton, Qt::NoModifier, blankSpot);
    settle(60);
    QVERIFY2(surface.root()->property("behindClicks").toInt() == 1,
             "a click on the sheet reached the workspace behind it");

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

// The pane is a fixed 190-pixel sidebar plus the rest of the window, and the
// server fields are laid out two to a row. A Grid sizes each column to its
// widest child, so one full-width field in the left column pushes every field
// in the right column past the pane's edge and off screen.
void TstUxShell::settingsServerFieldsStayInsideThePane()
{
    Surface surface(moduleUrl(QStringLiteral("SettingsWindow.qml")), QSize(900, 560));
    QVERIFY(surface.expose());
    QQuickItem *root = surface.root();
    QVERIFY(root);
    root->setProperty("shown", true);
    // Wait for the sheet's group loader to finish its FIRST pane before asking
    // for another one. Switching `selectedGroup` while that first creation is
    // still in flight cancels it, and the cancelled creation logs two QML
    // warnings ("Cannot create delegate" and "Object or context destroyed
    // during incubation") that the warning check at the end of this test would
    // then report. A user cannot outrun the first frame; only a test can.
    auto *groupLoader = qobject_cast<QQuickItem *>(
            surface.child(QStringLiteral("settingsGroupLoader")));
    QVERIFY2(groupLoader, "the Settings sheet has no group loader");
    const auto firstPaneSettled = [&] {
        auto *first = groupLoader->property("item").value<QQuickItem *>();
        if (!first || first->width() <= 0 || first->height() <= 0)
            return false;
        const QQmlIncubationController *pending = surface.view.engine()->incubationController();
        return !pending || pending->incubatingObjectCount() == 0;
    };
    QTRY_VERIFY_WITH_TIMEOUT(firstPaneSettled(), 2000);
    root->setProperty("selectedGroup", QStringLiteral("server"));
    settle(120);

    auto *pane = qobject_cast<QQuickItem *>(surface.child(QStringLiteral("serverPane")));
    QVERIFY2(pane, "the Settings sheet has no server pane");
    // The positioners inside the pane lay out on a polish pass, so the
    // geometry below is only meaningful once the pane itself has a size.
    QTRY_VERIFY_WITH_TIMEOUT(pane->width() > 0 && pane->height() > 0, 2000);

    // Every field the pane is required to show, named at the source rather
    // than sniffed out by C++ class name: a Qt Quick Controls TextField is
    // instantiated from the style's own QML document, so its runtime
    // metaobject is a generated subclass whose name is not "QQuickTextField"
    // and a class-name scan finds nothing at all.
    const QStringList fieldNames{QStringLiteral("serverField:name"),
                                 QStringLiteral("serverField:host"),
                                 QStringLiteral("serverField:port"),
                                 QStringLiteral("serverField:user"),
                                 QStringLiteral("serverField:identityFile"),
                                 QStringLiteral("serverField:nodePath"),
                                 QStringLiteral("serverField:repoRoot")};

    const qreal paneRight = pane->width();
    for (const QString &name : fieldNames) {
        auto *field = qobject_cast<QQuickItem *>(surface.child(name));
        QVERIFY2(field, qPrintable(QStringLiteral("the server pane has no %1").arg(name)));
        QVERIFY2(field->isVisible(), qPrintable(QStringLiteral("%1 is not shown").arg(name)));
        // Guard against a vacuous pass: a field collapsed to nothing would
        // "fit" inside any pane.
        QVERIFY2(field->width() >= 200,
                 qPrintable(QStringLiteral("%1 is only %2 wide")
                                    .arg(name)
                                    .arg(field->width())));
        const qreal right = field->mapToItem(pane, QPointF(field->width(), 0)).x();
        QVERIFY2(right <= paneRight + 0.5,
                 qPrintable(QStringLiteral("%1 reaches x=%2, past the %3-pixel pane")
                                    .arg(name)
                                    .arg(right)
                                    .arg(paneRight)));
    }

    QVERIFY2(surface.warnings.isEmpty(), qPrintable(surface.warningReport()));
}

// QTEST_MAIN cannot be used: QtWebEngineQuick::initialize() must run before the
// QGuiApplication (TerminalPaneView.qml imports QtWebEngine), and the shot mode
// needs to intercept argv.
int main(int argc, char *argv[])
{
    QStandardPaths::setTestModeEnabled(true);
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setOrganizationName(QStringLiteral("CodeHarbor"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "-shots") == 0)
            return renderShots();
    }

    TstUxShell testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_uxshell.moc"
