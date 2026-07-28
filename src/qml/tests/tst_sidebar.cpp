// Behavioural gate for the sessions sidebar (SPEC 4.2): drag-and-drop
// reordering and selection affordances.
//
// The real qrc SessionsSidebar.qml is loaded into a QQuickView against a stub
// `app` context object that records the workspace invokables the sidebar calls
// and a real ch::SessionsModel (so the role names, the two-level shape and the
// required-property delegate contract are the production ones). Drags are
// driven with genuine mouse events through the window, so the DragHandler /
// Flickable grab interaction is exercised, not bypassed.
//
// The stub deliberately never mutates the model: that is exactly the rejected
// RPC case, and the sidebar must leave the visible order untouched.
//
// Runs headless; the ctest registration pins QT_QPA_PLATFORM=offscreen and the
// software Quick backend (see CMakeLists.txt).

#include "SessionsModel.h"

#include <QtTest>

#include <QAbstractItemModel>
#include <QGuiApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QPoint>
#include <QPointF>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickView>
#include <QSet>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <utility>

namespace {

constexpr auto kModuleRoot = "qrc:/qt/qml/CodeHarbor/";

QUrl sidebarUrl()
{
    return QUrl(QLatin1String(kModuleRoot) + QLatin1String("SessionsSidebar.qml"));
}

// Warning net: any QML warning naming a CodeHarbor file fails the test, mirroring
// the global tst_qmlload contract.
QMutex g_logMutex;
QStringList g_loggedQmlWarnings;
QtMessageHandler g_previousHandler = nullptr;

void qmlWarningMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    if (type != QtDebugMsg && type != QtInfoMsg && msg.contains(QLatin1String(kModuleRoot))) {
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

ch::GroupRow makeGroup(const QString &id, const QString &name, const QStringList &sessionIds,
                       bool collapsed = false)
{
    ch::GroupRow group;
    group.group.id = ch::GroupId{id};
    group.group.serverId = ch::ServerId{QStringLiteral("srv")};
    group.group.name = name;
    group.group.collapsed = collapsed;

    int position = 0;
    for (const QString &sessionId : sessionIds) {
        ch::SessionRow row;
        row.session.id = ch::DevSessionId{sessionId};
        row.session.serverId = ch::ServerId{QStringLiteral("srv")};
        row.session.groupId = ch::GroupId{id};
        row.session.name = sessionId.toUpper();
        row.session.repositoryRoot = QStringLiteral("/repo/") + sessionId;
        row.session.position = position++;
        row.subtitle = sessionId;
        group.sessions.append(row);
    }
    return group;
}

// Depth-first over BOTH the QObject child list and the visual child list: QML
// parents Repeater/DelegateModel content visually without always re-parenting
// the QObject, so neither list alone reaches every delegate.
void collectItems(QQuickItem *item, QSet<QQuickItem *> &seen, QList<QQuickItem *> &out)
{
    if (!item || seen.contains(item))
        return;
    seen.insert(item);
    out.append(item);

    const auto objectChildren = item->children();
    for (QObject *child : objectChildren) {
        if (auto *childItem = qobject_cast<QQuickItem *>(child))
            collectItems(childItem, seen, out);
    }
    const auto itemChildren = item->childItems();
    for (QQuickItem *child : itemChildren)
        collectItems(child, seen, out);
}

QList<QQuickItem *> allItems(QQuickItem *root)
{
    QSet<QQuickItem *> seen;
    QList<QQuickItem *> out;
    collectItems(root, seen, out);
    return out;
}

QQuickItem *findByName(QQuickItem *root, const QString &objectName)
{
    const auto items = allItems(root);
    for (QQuickItem *item : items) {
        if (item->objectName() == objectName)
            return item;
    }
    return nullptr;
}

// Every session row currently in the tree, in visual (top-to-bottom) order.
QStringList visibleSessionOrder(QQuickItem *root)
{
    QList<QPair<qreal, QString>> rows;
    const auto items = allItems(root);
    for (QQuickItem *item : items) {
        if (!item->objectName().startsWith(QLatin1String("sessionRow:")))
            continue;
        rows.append({item->mapToScene(QPointF(0, 0)).y(),
                     item->objectName().mid(QLatin1String("sessionRow:").size())});
    }
    std::sort(rows.begin(), rows.end(),
              [](const QPair<qreal, QString> &a, const QPair<qreal, QString> &b) {
                  return a.first < b.first;
              });
    QStringList ids;
    for (const auto &row : rows)
        ids.append(row.second);
    return ids;
}

QPoint centerOf(QQuickItem *item)
{
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

// A point just inside the top edge of an item: the half that resolves to
// "insert before this row".
QPoint upperEdgeOf(QQuickItem *item)
{
    return item->mapToScene(QPointF(item->width() / 2, 3)).toPoint();
}

// Real press/move/release through the window so the DragHandler must actually
// win the grab. Several intermediate moves are required: the handler only
// activates once the drag threshold is crossed, and the drop target is
// recomputed from the centroid on every move.
//
// Split so a test can inspect the live drag state before the button comes up.
void dragHoldAt(QQuickView *view, QQuickItem *source, const QPoint &target)
{
    const QPoint start = centerOf(source);
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, start);

    constexpr int steps = 12;
    for (int i = 1; i <= steps; ++i) {
        const QPoint step(start.x() + (target.x() - start.x()) * i / steps,
                          start.y() + (target.y() - start.y()) * i / steps);
        QTest::mouseMove(view, step);
        QTest::qWait(8);
    }
}

void dragRelease(QQuickView *view, const QPoint &target)
{
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, target);
    QTest::qWait(50);
}

void dragTo(QQuickView *view, QQuickItem *source, const QPoint &target)
{
    dragHoldAt(view, source, target);
    dragRelease(view, target);
}

qreal sceneTop(QQuickItem *item)
{
    return item->mapToScene(QPointF(0, 0)).y();
}

} // namespace

// Stand-in for ch::AppController exposing exactly the surface the sidebar uses.
// Every workspace mutation is recorded verbatim and none of them touches the
// model: the sidebar must never depend on an optimistic local reorder.
class StubApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *sessionsModel READ sessionsModel CONSTANT)
    Q_PROPERTY(QString connectionState READ connectionState CONSTANT)


public:
    explicit StubApp(QAbstractItemModel *model, QObject *parent = nullptr)
        : QObject(parent), m_model(model)
    {
    }

    QAbstractItemModel *sessionsModel() const { return m_model; }
    QString connectionState() const { return QStringLiteral("disconnected"); }

    const QStringList &calls() const { return m_calls; }
    void clearCalls() { m_calls.clear(); }

    Q_INVOKABLE void refresh() { m_calls.append(QStringLiteral("refresh()")); }

    Q_INVOKABLE void createGroup(QString name)
    {
        m_calls.append(QStringLiteral("createGroup(%1)").arg(name));
    }
    Q_INVOKABLE void renameGroup(QString id, QString name)
    {
        m_calls.append(QStringLiteral("renameGroup(%1,%2)").arg(id, name));
    }
    Q_INVOKABLE void setGroupCollapsed(QString id, bool collapsed)
    {
        m_calls.append(QStringLiteral("setGroupCollapsed(%1,%2)")
                               .arg(id, collapsed ? QStringLiteral("true") : QStringLiteral("false")));
    }
    Q_INVOKABLE void reorderGroups(QStringList orderedIds)
    {
        m_calls.append(QStringLiteral("reorderGroups([%1])").arg(orderedIds.join(QLatin1Char(','))));
    }
    Q_INVOKABLE void createSession(QString groupId, QString name, QString repoRoot)
    {
        m_calls.append(QStringLiteral("createSession(%1,%2,%3)").arg(groupId, name, repoRoot));
    }
    Q_INVOKABLE void renameSession(QString id, QString name)
    {
        m_calls.append(QStringLiteral("renameSession(%1,%2)").arg(id, name));
    }
    Q_INVOKABLE void duplicateSession(QString id)
    {
        m_calls.append(QStringLiteral("duplicateSession(%1)").arg(id));
    }
    Q_INVOKABLE void moveSession(QString id, QString groupId, int position)
    {
        m_calls.append(QStringLiteral("moveSession(%1,%2,%3)").arg(id, groupId).arg(position));
    }
    Q_INVOKABLE void deleteSession(QString id)
    {
        m_calls.append(QStringLiteral("deleteSession(%1)").arg(id));
    }
    Q_INVOKABLE void reorderSessions(QString groupId, QStringList orderedIds)
    {
        m_calls.append(QStringLiteral("reorderSessions(%1,[%2])")
                               .arg(groupId, orderedIds.join(QLatin1Char(','))));
    }

private:
    QAbstractItemModel *m_model = nullptr;
    QStringList m_calls;
};

// Loads the real sidebar QML over a real SessionsModel.
class SidebarFixture
{
public:
    ch::SessionsModel model;
    StubApp app{&model};
    QQuickView view;
    QStringList engineWarnings;

    explicit SidebarFixture(QVector<ch::GroupRow> groups)
    {
        model.setGroups(std::move(groups));

        QObject::connect(view.engine(), &QQmlEngine::warnings, view.engine(),
                         [this](const QList<QQmlError> &warnings) {
                             for (const QQmlError &error : warnings)
                                 engineWarnings.append(error.toString());
                         });

        view.engine()->rootContext()->setContextProperty(QStringLiteral("app"), &app);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(320, 700);
        view.setSource(sidebarUrl());
    }

    QQuickItem *root() const { return view.rootObject(); }

    QStringList warnings()
    {
        QStringList combined = engineWarnings;
        for (const QString &logged : takeLoggedQmlWarnings()) {
            if (!combined.contains(logged))
                combined.append(logged);
        }
        return combined;
    }
};

class TstSidebar : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void reordersWithinAGroup();
    void movesSessionBetweenGroups();
    void showsDropIndicatorWhileDragging();
    void dropOntoCollapsedGroupAppends();
    void reordersGroups();
    void rejectedReorderLeavesVisibleOrderUnchanged();
    void clickSelectsAndActivatesSession();
    void keyboardSelectionActivatesSession();
    void spaceTogglesGroupCollapse();
    void newSessionButtonTargetsItsGroupWithoutCollapsing();
    void serverSettingsButtonEmitsRequest();

private:
    // Two expanded groups: Alpha[s1,s2,s3], Beta[s4,s5].
    static QVector<ch::GroupRow> twoGroups(bool betaCollapsed = false)
    {
        return {makeGroup(QStringLiteral("g1"), QStringLiteral("Alpha"),
                          {QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3")}),
                makeGroup(QStringLiteral("g2"), QStringLiteral("Beta"),
                          {QStringLiteral("s4"), QStringLiteral("s5")}, betaCollapsed)};
    }

    static void expose(SidebarFixture &fixture)
    {
        QVERIFY2(fixture.root() != nullptr, "SessionsSidebar.qml failed to instantiate");
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
        QTest::qWait(100);
    }
};

void TstSidebar::init()
{
    takeLoggedQmlWarnings();
    g_previousHandler = qInstallMessageHandler(qmlWarningMessageHandler);
}

void TstSidebar::cleanup()
{
    qInstallMessageHandler(g_previousHandler);
    g_previousHandler = nullptr;
}

// Dragging the third row of a group onto the first row's upper half asks the
// server for the full new ordering of that group.
void TstSidebar::reordersWithinAGroup()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QCOMPARE(visibleSessionOrder(fixture.root()),
             (QStringList{QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3"),
                          QStringLiteral("s4"), QStringLiteral("s5")}));

    QQuickItem *third = findByName(fixture.root(), QStringLiteral("sessionRow:s3"));
    QQuickItem *first = findByName(fixture.root(), QStringLiteral("sessionRow:s1"));
    QVERIFY(third && first);

    fixture.app.clearCalls();
    dragTo(&fixture.view, third, upperEdgeOf(first));

    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("reorderSessions(g1,[s3,s1,s2])")}));
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// Dragging into another expanded group is a move, addressed by target index.
void TstSidebar::movesSessionBetweenGroups()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *source = findByName(fixture.root(), QStringLiteral("sessionRow:s1"));
    QQuickItem *target = findByName(fixture.root(), QStringLiteral("sessionRow:s5"));
    QVERIFY(source && target);

    fixture.app.clearCalls();
    dragTo(&fixture.view, source, upperEdgeOf(target));

    // s5 is index 1 of Beta, so "insert before s5" is index 1.
    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("moveSession(s1,g2,1)")}));
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// The affordance itself: while a session is being dragged, an insertion line
// sits exactly at the boundary the drop would use, and a proxy follows the
// cursor. Both disappear when the gesture ends.
void TstSidebar::showsDropIndicatorWhileDragging()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *third = findByName(fixture.root(), QStringLiteral("sessionRow:s3"));
    QQuickItem *second = findByName(fixture.root(), QStringLiteral("sessionRow:s2"));
    QQuickItem *indicator = findByName(fixture.root(), QStringLiteral("dropIndicator"));
    QQuickItem *proxy = findByName(fixture.root(), QStringLiteral("dragProxy"));
    QVERIFY(third && second && indicator && proxy);
    QVERIFY(!indicator->isVisible());
    QVERIFY(!proxy->isVisible());

    dragHoldAt(&fixture.view, third, upperEdgeOf(second));

    QVERIFY2(indicator->isVisible(), "no drop indicator while dragging a session");
    QVERIFY2(proxy->isVisible(), "no drag proxy while dragging a session");
    // The line marks "insert before s2", i.e. it sits on s2's top edge.
    const qreal lineCenter = sceneTop(indicator) + indicator->height() / 2;
    QVERIFY2(qAbs(lineCenter - sceneTop(second)) <= 1.0,
             qPrintable(QStringLiteral("indicator at %1, expected %2")
                                .arg(lineCenter)
                                .arg(sceneTop(second))));
    // The source row stays where it is: nothing is reordered before the server
    // answers.
    QCOMPARE(visibleSessionOrder(fixture.root()).mid(0, 3),
             (QStringList{QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3")}));

    dragRelease(&fixture.view, upperEdgeOf(second));
    QVERIFY(!indicator->isVisible());
    QVERIFY(!proxy->isVisible());
    QCOMPARE(fixture.app.calls().last(), QStringLiteral("reorderSessions(g1,[s1,s3,s2])"));
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// A collapsed group shows no rows to aim between, so a drop on its header
// appends to it.
void TstSidebar::dropOntoCollapsedGroupAppends()
{
    SidebarFixture fixture(twoGroups(/*betaCollapsed=*/true));
    expose(fixture);

    // Beta is collapsed: only Alpha's rows are visible.
    QCOMPARE(visibleSessionOrder(fixture.root()).mid(0, 3),
             (QStringList{QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3")}));

    QQuickItem *source = findByName(fixture.root(), QStringLiteral("sessionRow:s1"));
    QQuickItem *betaHeader = findByName(fixture.root(), QStringLiteral("groupHeader:g2"));
    QVERIFY(source && betaHeader);

    fixture.app.clearCalls();
    dragHoldAt(&fixture.view, source, centerOf(betaHeader));

    // No insertion line for a collapsed target; the whole header lights up as
    // the "appends here" affordance.
    QQuickItem *highlight = findByName(fixture.root(), QStringLiteral("dropHighlight"));
    QQuickItem *indicator = findByName(fixture.root(), QStringLiteral("dropIndicator"));
    QVERIFY(highlight && indicator);
    QVERIFY2(highlight->isVisible(), "no collapsed-group drop highlight while hovering the header");
    QVERIFY(!indicator->isVisible());
    QCOMPARE(highlight->height(), betaHeader->height());

    dragRelease(&fixture.view, centerOf(betaHeader));

    // Beta already holds s4 and s5, so the append index is 2.
    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("moveSession(s1,g2,2)")}));
    QVERIFY(!highlight->isVisible());
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// Dragging a header reorders the top level.
void TstSidebar::reordersGroups()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *beta = findByName(fixture.root(), QStringLiteral("groupHeader:g2"));
    QQuickItem *alpha = findByName(fixture.root(), QStringLiteral("groupHeader:g1"));
    QVERIFY(beta && alpha);

    fixture.app.clearCalls();
    dragTo(&fixture.view, beta, upperEdgeOf(alpha));

    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("reorderGroups([g2,g1])")}));
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// The stub never mutates the model, i.e. the RPC was rejected. The sidebar must
// not have moved anything on its own.
void TstSidebar::rejectedReorderLeavesVisibleOrderUnchanged()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *third = findByName(fixture.root(), QStringLiteral("sessionRow:s3"));
    QQuickItem *first = findByName(fixture.root(), QStringLiteral("sessionRow:s1"));
    QVERIFY(third && first);

    fixture.app.clearCalls();
    dragTo(&fixture.view, third, upperEdgeOf(first));
    QCOMPARE(fixture.app.calls().size(), 1);

    // Model order, straight from the model, is what the sidebar must still show.
    QStringList modelOrder;
    for (int g = 0; g < fixture.model.rowCount(); ++g) {
        const QModelIndex group = fixture.model.index(g, 0);
        for (int s = 0; s < fixture.model.rowCount(group); ++s) {
            modelOrder.append(
                    fixture.model.data(fixture.model.index(s, 0, group), ch::SessionsModel::IdRole)
                            .toString());
        }
    }
    QCOMPARE(visibleSessionOrder(fixture.root()), modelOrder);

    // And no drop indicator is left behind once the gesture ends.
    QQuickItem *indicator = findByName(fixture.root(), QStringLiteral("dropIndicator"));
    QVERIFY(indicator);
    QVERIFY(!indicator->isVisible());
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// A plain click (no drag threshold crossed) selects the row and activates it,
// and must not be mistaken for a zero-distance reorder.
void TstSidebar::clickSelectsAndActivatesSession()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QSignalSpy activated(fixture.root(), SIGNAL(sessionActivated(QString)));
    QVERIFY(activated.isValid());

    QQuickItem *row = findByName(fixture.root(), QStringLiteral("sessionRow:s4"));
    QVERIFY(row);

    fixture.app.clearCalls();
    QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier, centerOf(row));
    QTest::qWait(50);

    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.first().first().toString(), QStringLiteral("s4"));
    QCOMPARE(fixture.root()->property("selectedSessionId").toString(), QStringLiteral("s4"));
    QCOMPARE(fixture.root()->property("currentIsGroup").toBool(), false);
    QVERIFY2(fixture.app.calls().isEmpty(), qPrintable(fixture.app.calls().join(QLatin1Char(','))));

    // Selecting focuses the sidebar, so the keyboard cursor continues from here.
    QVERIFY(fixture.root()->hasActiveFocus());
    QTest::keyClick(&fixture.view, Qt::Key_Down);
    QCOMPARE(fixture.root()->property("currentId").toString(), QStringLiteral("s5"));

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// Down/Up walk headers and rows; Enter emits the activation the host consumes.
void TstSidebar::keyboardSelectionActivatesSession()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QSignalSpy activated(fixture.root(), SIGNAL(sessionActivated(QString)));
    QVERIFY(activated.isValid());

    fixture.root()->forceActiveFocus();
    QVERIFY(fixture.root()->hasActiveFocus());

    // Cursor order: [g1] s1 s2 s3 [g2] s4 s5.
    QTest::keyClick(&fixture.view, Qt::Key_Down); // g1 header
    QTest::keyClick(&fixture.view, Qt::Key_Down); // s1
    QTest::keyClick(&fixture.view, Qt::Key_Down); // s2
    QTest::keyClick(&fixture.view, Qt::Key_Down); // s3
    QTest::keyClick(&fixture.view, Qt::Key_Up);   // back to s2
    QCOMPARE(fixture.root()->property("currentId").toString(), QStringLiteral("s2"));
    QCOMPARE(fixture.root()->property("currentIsGroup").toBool(), false);
    QCOMPARE(fixture.root()->property("selectedSessionId").toString(), QStringLiteral("s2"));

    // Selection alone must not activate.
    QCOMPARE(activated.size(), 0);

    QTest::keyClick(&fixture.view, Qt::Key_Return);
    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.first().first().toString(), QStringLiteral("s2"));

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// The only route into a Dev Session, so it must exist, target the right group,
// and not fold that group away as a side effect of being clicked.
void TstSidebar::newSessionButtonTargetsItsGroupWithoutCollapsing()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem* button = findByName(fixture.root(),
                                    QStringLiteral("newSessionButton:g1"));
    QVERIFY2(button, "the group header offers no way to create a Dev Session");

    fixture.app.clearCalls();
    const QPointF centre = button->mapToScene(
        QPointF(button->width() / 2, button->height() / 2));
    QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier,
                      centre.toPoint());
    // Creating a session must not fold the group away underneath the click: the
    // Button has to consume the press before the header's collapse handler runs.
    QCOMPARE(fixture.app.calls(), QStringList{});
    // ...and it must target the group whose header was clicked.
    QCOMPARE(fixture.root()->property("pendingSessionGroupId").toString(),
             QStringLiteral("g1"));

    // Opening a dialog that creates nothing would be the same dead affordance
    // this test exists to kill, so drive the accept and check what was sent.
    QObject* dialog = fixture.root()->findChild<QObject*>(QStringLiteral("newSessionDialog"));
    QVERIFY2(dialog, "no new-session dialog to accept");
    // Distinct values: an argument-order swap must not be able to hide.
    QObject* nameField = fixture.root()->findChild<QObject*>(QStringLiteral("newSessionField"));
    QObject* repoField = fixture.root()->findChild<QObject*>(QStringLiteral("newSessionRepoField"));
    QVERIFY(nameField && repoField);
    nameField->setProperty("text", QStringLiteral("api"));
    repoField->setProperty("text", QStringLiteral("/srv/repos/api"));
    QMetaObject::invokeMethod(dialog, "accept");

    QCOMPARE(fixture.app.calls(),
             (QStringList{QStringLiteral("createSession(g1,api,/srv/repos/api)")}));
    QVERIFY2(fixture.warnings().isEmpty(),
             qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// The server profile sheet must remain reachable after a failed/disconnected
// start, not only through an undocumented command-palette shortcut.
void TstSidebar::serverSettingsButtonEmitsRequest()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QSignalSpy requested(fixture.root(), SIGNAL(serverSettingsRequested()));
    QVERIFY(requested.isValid());

    QQuickItem *button = findByName(fixture.root(), QStringLiteral("serverSettingsButton"));
    QVERIFY2(button, "the connection status footer offers no server settings control");
    QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier, centerOf(button));

    QCOMPARE(requested.size(), 1);
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// Space on a group header asks the server to toggle its collapsed flag.
void TstSidebar::spaceTogglesGroupCollapse()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    fixture.root()->forceActiveFocus();
    QTest::keyClick(&fixture.view, Qt::Key_Down); // g1 header
    QCOMPARE(fixture.root()->property("currentId").toString(), QStringLiteral("g1"));
    QCOMPARE(fixture.root()->property("currentIsGroup").toBool(), true);

    fixture.app.clearCalls();
    QTest::keyClick(&fixture.view, Qt::Key_Space);
    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("setGroupCollapsed(g1,true)")}));
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

QTEST_MAIN(TstSidebar)

#include "tst_sidebar.moc"
