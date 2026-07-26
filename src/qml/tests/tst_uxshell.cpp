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
// publishes), its dismissible error banner, and the two "nothing here yet"
// panes — a terminal that cannot attach and a viewer with no file — which must
// explain themselves instead of showing a raw pane id.
//
// Runs headless; the ctest registration pins the offscreen QPA, the software
// Quick backend and Chromium's no-GPU flags (see CMakeLists.txt).

#include "SessionsModel.h"

#include <QtTest>

#include <QAbstractItemModel>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickView>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
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
        return {};
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

// The slice of ch::AppController the sidebar reads. Mirrors tst_sidebar's stub
// and adds the connection surface the status footer binds to.
class ShotApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *sessionsModel READ sessionsModel CONSTANT)
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionStateChanged)
    Q_PROPERTY(QString activeSessionId READ activeSessionId NOTIFY activeSessionChanged)

public:
    explicit ShotApp(QAbstractItemModel *model, QObject *parent = nullptr)
        : QObject(parent), m_model(model)
    {
    }

    QAbstractItemModel *sessionsModel() const { return m_model; }

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
    Q_INVOKABLE void reorderGroups(QStringList) {}
    Q_INVOKABLE void createSession(QString, QString, QString) {}
    Q_INVOKABLE void renameSession(QString, QString) {}
    Q_INVOKABLE void duplicateSession(QString) {}
    Q_INVOKABLE void moveSession(QString, QString, int) {}
    Q_INVOKABLE void deleteSession(QString) {}
    Q_INVOKABLE void reorderSessions(QString, QStringList) {}

signals:
    void connectionStateChanged();
    void activeSessionChanged();

private:
    QAbstractItemModel *m_model = nullptr;
    QString m_connectionState = QStringLiteral("disconnected");
    QString m_activeSessionId;
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
        QObject::connect(view.engine(), &QQmlEngine::warnings, view.engine(),
                         [this](const QList<QQmlError> &list) {
                             for (const QQmlError &error : list)
                                 warnings.append(error.toString());
                         });
        if (app)
            view.rootContext()->setContextProperty(QStringLiteral("app"), app);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(size);
        view.setSource(url);
    }

    QQuickItem *root() const { return view.rootObject(); }

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

    Rectangle { anchors.fill: parent; color: "#1e1e2e" }

    CommandPalette {
        id: palette
        objectName: "palette"
        commands: [
            { id: "server.connect", title: "Connect to Server\u2026", shortcut: "Ctrl+Shift+O",
              invoke: function () {} },
            { id: "server.disconnect", title: "Disconnect from Server", invoke: function () {} },
            { id: "session.refresh", title: "Refresh Workspace", shortcut: "Ctrl+R",
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

    // A long RPC error must be readable and the banner must be dismissible,
    // then come back for the next failure.
    void sheetErrorIsReadableAndDismissible();

    // Cold start: the sheet has to say what this dialog is for, not just show
    // an empty list.
    void sheetColdStartExplainsItself();

    // A pane with nothing behind it explains itself instead of showing an id.
    void inertTerminalPaneExplainsItself();
    void emptyViewerPaneExplainsItself();
};

void TstUxShell::sheetStatusMatchesTheStatesTheAppPublishes_data()
{
    QTest::addColumn<QString>("state");
    QTest::addColumn<QString>("colour");
    QTest::addColumn<bool>("busy");

    // The exact strings ch::AppController::setConnectionState() emits.
    QTest::newRow("disconnected") << "disconnected" << "#6c7086" << false;
    QTest::newRow("connecting") << "connecting" << "#f9e2af" << true;
    QTest::newRow("hostkey") << "hostkey" << "#f9e2af" << true;
    QTest::newRow("connected") << "connected" << "#a6e3a1" << false;
    QTest::newRow("reconnecting") << "reconnecting" << "#fab387" << true;
    QTest::newRow("failed") << "failed" << "#f38ba8" << false;
}

void TstUxShell::sheetStatusMatchesTheStatesTheAppPublishes()
{
    QFETCH(QString, state);
    QFETCH(QString, colour);
    QFETCH(bool, busy);

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
