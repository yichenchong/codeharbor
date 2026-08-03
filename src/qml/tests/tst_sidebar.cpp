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
#include <QColor>
#include <QGuiApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QPoint>
#include <QPointF>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlProperty>
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

// A row's context-menu entry, found by the words on it. MenuItem is the only
// thing here that both carries `text` and can be triggered, so the pair is
// unambiguous without giving every entry an objectName.
QObject *menuItemNamed(QQuickItem *owner, const QString &text)
{
    const auto children = owner->findChildren<QObject *>();
    for (QObject *child : children) {
        if (child->property("text").toString() == text
            && child->metaObject()->indexOfSignal("triggered()") >= 0)
            return child;
    }
    return nullptr;
}

// Reads an ATTACHED property such as `Accessible.name` or `ToolTip.text`. The
// attached type ("Accessible", "ToolTip") is resolved through the imports of the
// QML file that declared it, so the item's own QML context has to be handed to
// QQmlProperty; the two-argument constructor has no context and would silently
// yield an invalid property instead.
QString attachedString(QQuickItem *item, const QString &name)
{
    const QQmlProperty property(item, name, QQmlEngine::contextForObject(item));
    if (!property.isValid())
        return QString();
    return property.read().toString();
}

// Reads the hint carried by the module's own AppToolTip. The attached
// `ToolTip.text` form is deliberately no longer used here — the Basic style
// draws it in the style's light palette, i.e. a white box beside a dark panel —
// so the hint now lives on a named child ToolTip object. A ToolTip is a Popup
// and therefore not a QQuickItem, but declaring it inside an Item parents it to
// that Item as a plain QObject, which is what makes findChild reach it.
QObject *hintOf(QQuickItem *item, const QString &objectName)
{
    return item->findChild<QObject *>(objectName);
}

// A QtQuick.Controls Dialog sizes itself from its content item's IMPLICIT width.
// A content item that under-reports that width therefore produces a dialog
// narrower than the field drawn inside it, and the field hangs out past the
// frame. The invariant that catches it: the visible field plus the dialog's own
// horizontal padding has to fit within the dialog's width.
void verifyContentFits(QObject *dialog, QQuickItem *field, const QString &what)
{
    QVERIFY2(dialog->property("visible").toBool(),
             qPrintable(QStringLiteral("%1 dialog did not open").arg(what)));

    const qreal dialogWidth = dialog->property("width").toReal();
    const qreal padding = dialog->property("leftPadding").toReal()
                          + dialog->property("rightPadding").toReal();

    // Guard against a vacuous pass: a field collapsed to nothing would "fit".
    QVERIFY2(field->width() >= 200,
             qPrintable(QStringLiteral("%1 field is only %2 wide")
                                .arg(what)
                                .arg(field->width())));
    QVERIFY2(field->width() + padding <= dialogWidth + 0.5,
             qPrintable(QStringLiteral("%1 field (%2) plus padding (%3) exceeds the "
                                       "dialog width (%4): the field is drawn outside "
                                       "the dialog")
                                .arg(what)
                                .arg(field->width())
                                .arg(padding)
                                .arg(dialogWidth)));
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
    void dialogContentFitsInsideItsDialog();
    void addButtonsAreLabelledWithoutAPointer();
    void hintsAreDrawnInTheApplicationsOwnPalette();
    void longNamesAreElidedInsteadOfOverflowingTheSidebar();
    void rowMenuActionsReachTheWorkspace();

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

// The reported defect: the new-group dialog's field was a bare TextField with an
// explicit width, which a Dialog does not size itself to, so the field spilled
// out past the dialog's edge. Every dialog the sidebar owns is checked against
// the same invariant, including the new-session dialog that already got it right
// — if the mechanism ever changes, all three must move together.
void TstSidebar::dialogContentFitsInsideItsDialog()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);
    fixture.app.clearCalls();

    struct Case
    {
        // The item the dialog is declared inside. Empty means the sidebar root;
        // the rename dialog is declared per row, and a row is parented visually
        // rather than as a QObject child, so it has to be found by objectName
        // first.
        QString ownerName;
        QString dialogName;
        QString fieldName;
        QString label;
    };
    const QList<Case> cases{
        {QString(), QStringLiteral("newGroupDialog"), QStringLiteral("newGroupField"),
         QStringLiteral("new-group")},
        {QString(), QStringLiteral("newSessionDialog"), QStringLiteral("newSessionField"),
         QStringLiteral("new-session")},
        {QStringLiteral("sessionRow:s1"), QStringLiteral("renameDialog:s1"),
         QStringLiteral("renameField:s1"), QStringLiteral("rename-session")},
    };

    for (const Case &testCase : cases) {
        QQuickItem *owner = fixture.root();
        if (!testCase.ownerName.isEmpty()) {
            owner = findByName(fixture.root(), testCase.ownerName);
            QVERIFY2(owner, qPrintable(QStringLiteral("no %1").arg(testCase.ownerName)));
        }

        QObject *dialog = owner->findChild<QObject *>(testCase.dialogName);
        QVERIFY2(dialog, qPrintable(QStringLiteral("no %1").arg(testCase.dialogName)));
        QQuickItem *field = owner->findChild<QQuickItem *>(testCase.fieldName);
        QVERIFY2(field, qPrintable(QStringLiteral("no %1").arg(testCase.fieldName)));

        QMetaObject::invokeMethod(dialog, "open");
        QTest::qWait(100);
        verifyContentFits(dialog, field, testCase.label);
        // Reject rather than accept: accepting would create a group or a session.
        QMetaObject::invokeMethod(dialog, "reject");
        QTest::qWait(50);
    }

    // Measuring a dialog must not have mutated the workspace.
    QVERIFY2(fixture.app.calls().isEmpty(), qPrintable(fixture.app.calls().join(QLatin1Char(','))));
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// Both add actions are a bare "+" glyph. A glyph that small says nothing on its
// own, so each one must carry a sentence naming exactly what it adds — as its
// accessible name, not only as a pointer-only tooltip — must stay a target big
// enough to hit, and must stay reachable by keyboard.
void TstSidebar::addButtonsAreLabelledWithoutAPointer()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *addGroup = findByName(fixture.root(), QStringLiteral("newGroupButton"));
    QVERIFY2(addGroup, "the sidebar header offers no way to add a group");
    QQuickItem *addToAlpha = findByName(fixture.root(), QStringLiteral("newSessionButton:g1"));
    QQuickItem *addToBeta = findByName(fixture.root(), QStringLiteral("newSessionButton:g2"));
    QVERIFY(addToAlpha && addToBeta);

    const QString groupName = attachedString(addGroup, QStringLiteral("Accessible.name"));
    const QString alphaName = attachedString(addToAlpha, QStringLiteral("Accessible.name"));
    const QString betaName = attachedString(addToBeta, QStringLiteral("Accessible.name"));

    QVERIFY2(!groupName.isEmpty(), "the add-group button exposes no Accessible.name");
    QCOMPARE(groupName, QStringLiteral("Add a group"));
    QVERIFY2(!alphaName.isEmpty(), "the add-session button exposes no Accessible.name");

    // There is one add-session button per group, so the name has to say WHICH
    // group: two buttons announcing the same sentence would be unusable.
    QVERIFY2(alphaName.contains(QStringLiteral("Alpha")), qPrintable(alphaName));
    QVERIFY2(betaName.contains(QStringLiteral("Beta")), qPrintable(betaName));
    QVERIFY(alphaName != betaName);

    // The tooltip is a hint that repeats the name; it must never be the only
    // place the sentence exists. It is the module's own AppToolTip, not the
    // attached form: see hintsAreDrawnInTheApplicationsOwnPalette below.
    QObject *groupHint = hintOf(addGroup, QStringLiteral("newGroupButtonTip"));
    QObject *alphaHint = hintOf(addToAlpha, QStringLiteral("newSessionButtonTip:g1"));
    QVERIFY2(groupHint, "the add-group button carries no hint");
    QVERIFY2(alphaHint, "the add-session button carries no hint");
    QCOMPARE(groupHint->property("text").toString(), groupName);
    QCOMPARE(alphaHint->property("text").toString(), alphaName);

    // Compact, but not smaller than a reliable pointer target.
    for (QQuickItem *button : {addGroup, addToAlpha}) {
        QVERIFY2(button->width() >= 24 && button->height() >= 24,
                 qPrintable(QStringLiteral("%1 is %2x%3, below the 24x24 hit-area floor")
                                    .arg(button->objectName())
                                    .arg(button->width())
                                    .arg(button->height())));
        // Keyboard focus must still be able to land here.
        QVERIFY2((button->property("focusPolicy").toInt() & Qt::TabFocus) != 0,
                 qPrintable(QStringLiteral("%1 is not reachable by Tab")
                                    .arg(button->objectName())));
    }

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// Every hint in the sidebar has to be drawn in the application's own dark
// palette. The attached `ToolTip.text` form is rendered by the Basic style's
// shared tooltip, in the STYLE's palette — near-black text on white — so a hint
// about this dark panel arrived as a white box, and the application ended up
// with two tooltip appearances depending on which call site drew the hint.
//
// The gate is deliberately structural rather than a screenshot: the attached
// form must be UNUSED (nothing left for the style to draw), and the hint that
// replaced it must paint its own surface darker than its own text.
void TstSidebar::hintsAreDrawnInTheApplicationsOwnPalette()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    struct Site {
        const char *owner;
        const char *hint;
    };
    const Site sites[] = {
        { "newGroupButton", "newGroupButtonTip" },
        { "newSessionButton:g1", "newSessionButtonTip:g1" },
        { "sessionRow:s1", "sessionRowTip" },
    };

    for (const Site &site : sites) {
        QQuickItem *owner = findByName(fixture.root(), QString::fromLatin1(site.owner));
        QVERIFY2(owner, site.owner);

        // Nothing may be left on the attached form: whatever is still there is
        // what the Basic style would draw in its own light colours.
        QCOMPARE(attachedString(owner, QStringLiteral("ToolTip.text")), QString());

        QObject *hint = hintOf(owner, QString::fromLatin1(site.hint));
        QVERIFY2(hint, site.hint);
        QVERIFY2(!hint->property("text").toString().isEmpty(), site.hint);

        auto *surface = hint->property("background").value<QQuickItem *>();
        auto *label = hint->property("contentItem").value<QQuickItem *>();
        QVERIFY2(surface && label, site.hint);

        const QColor fill = surface->property("color").value<QColor>();
        const QColor ink = label->property("color").value<QColor>();
        QVERIFY2(fill.lightnessF() < ink.lightnessF(),
                 qPrintable(QStringLiteral("%1 draws %2 text on a %3 plate")
                                    .arg(QString::fromLatin1(site.hint),
                                         ink.name(), fill.name())));
        QVERIFY2(fill.lightnessF() < 0.5,
                 qPrintable(QStringLiteral("%1 is a light plate (%2) on dark chrome")
                                    .arg(QString::fromLatin1(site.hint), fill.name())));
    }

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// The sidebar is a few hundred pixels wide and the names in it come off a
// remote server: repository paths and Dev Session names are routinely longer
// than that. Every label here already asks to be elided, but `elide` does
// nothing to a Text with no width of its own — and a Label inside a Row/Column
// positioner has none, so
// the text simply grew and painted straight over the panel's edge, over the
// group's add-session button, and over whatever sat beside it.
//
// Asserted as GEOMETRY rather than as "the text ends in an ellipsis": eliding
// is the mechanism, staying inside the panel is the requirement.
void TstSidebar::longNamesAreElidedInsteadOfOverflowingTheSidebar()
{
    ch::GroupRow group = makeGroup(QStringLiteral("g1"),
                                   QString(120, QLatin1Char('G')),
                                   {QStringLiteral("s1")});
    group.sessions[0].session.name = QString(160, QLatin1Char('S'));
    group.sessions[0].subtitle = QString(160, QLatin1Char('p'));

    SidebarFixture fixture({group});
    expose(fixture);

    QQuickItem *row = findByName(fixture.root(), QStringLiteral("sessionRow:s1"));
    QQuickItem *header = findByName(fixture.root(), QStringLiteral("groupHeader:g1"));
    QQuickItem *addButton = findByName(fixture.root(), QStringLiteral("newSessionButton:g1"));
    QVERIFY(row && header && addButton);

    const qreal panelRight = fixture.root()->width();
    for (QQuickItem *owner : {row, header}) {
        const auto items = allItems(owner);
        for (QQuickItem *item : items) {
            // On screen only: a row also owns a rename dialog whose field is
            // pre-filled with the same long name, and that field is not part of
            // the panel until it is opened.
            if (!item->isVisible())
                continue;
            const QVariant text = item->property("text");
            if (!text.isValid() || text.toString().length() < 10)
                continue;
            const qreal right = item->mapToScene(QPointF(item->width(), 0)).x();
            QVERIFY2(right <= panelRight + 0.5,
                     qPrintable(QStringLiteral("a %1-character label in %2 reaches x=%3, past the "
                                               "%4-pixel sidebar")
                                        .arg(text.toString().length())
                                        .arg(owner->objectName())
                                        .arg(right)
                                        .arg(panelRight)));
        }
    }

    // The group name must also stop before the button that adds a session to
    // that group: a name painted over it makes the only route into a Dev
    // Session unreadable, and the panel edge alone would not catch it.
    const qreal buttonLeft = addButton->mapToScene(QPointF(0, 0)).x();
    const auto headerItems = allItems(header);
    for (QQuickItem *item : headerItems) {
        if (item->property("text").toString() != group.group.name)
            continue;
        QVERIFY2(item->mapToScene(QPointF(item->width(), 0)).x() <= buttonLeft + 0.5,
                 "the group name runs under its own add-session button");
    }

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// The row's right-click menu is the only place Rename, Duplicate, Move to top
// and Delete exist at all. Nothing exercised it, so it could be — and was —
// wired in a way that works only by accident: the entries called `app.*`
// directly out of a delegate, while every other mutation in this panel goes
// through the sidebar, which is the object that actually holds `app`. A
// delegate reaching for a context property it is documented not to need is one
// refactor away from a ReferenceError that nothing would catch.
void TstSidebar::rowMenuActionsReachTheWorkspace()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *row = findByName(fixture.root(), QStringLiteral("sessionRow:s2"));
    QVERIFY(row);
    fixture.app.clearCalls();

    // Delete is deliberately absent from this loop: it is destructive, so it
    // opens a confirmation instead of calling straight through. Exercised below.
    const QStringList entries{QStringLiteral("Duplicate"), QStringLiteral("Move to top")};
    for (const QString &entry : entries) {
        QObject *item = menuItemNamed(row, entry);
        QVERIFY2(item, qPrintable(QStringLiteral("the session row has no \"%1\" action")
                                          .arg(entry)));
        QVERIFY(QMetaObject::invokeMethod(item, "triggered"));
    }
    QTest::qWait(50);

    // s2 sits in Alpha, so "move to top" is position 0 of g1.
    QCOMPARE(fixture.app.calls(),
             (QStringList{QStringLiteral("duplicateSession(s2)"),
                          QStringLiteral("moveSession(s2,g1,0)")}));

    // Deleting asks first. Opening the question must destroy nothing, declining
    // must destroy nothing, and the question has to NAME the session so it
    // cannot be confused with another row's.
    fixture.app.clearCalls();
    QObject *const deleteEntry = menuItemNamed(row, QStringLiteral("Delete"));
    QVERIFY2(deleteEntry, "the session row has no \"Delete\" action");
    QObject *const deleteDialog =
        row->findChild<QObject *>(QStringLiteral("deleteSessionDialog:s2"));
    QVERIFY(deleteDialog);
    QVERIFY(QMetaObject::invokeMethod(deleteEntry, "triggered"));
    QTest::qWait(50);
    QVERIFY2(fixture.app.calls().isEmpty(),
             "opening the confirmation already deleted the session");
    QObject *const deleteMessage =
        row->findChild<QObject *>(QStringLiteral("deleteSessionMessage:s2"));
    QVERIFY(deleteMessage);
    QVERIFY2(deleteMessage->property("text").toString().contains(
                 QStringLiteral("s2"), Qt::CaseInsensitive),
             qPrintable(QStringLiteral("the confirmation does not name the session: %1")
                                .arg(deleteMessage->property("text").toString())));
    QMetaObject::invokeMethod(deleteDialog, "reject");
    QTest::qWait(50);
    QVERIFY2(fixture.app.calls().isEmpty(), "declining the confirmation still deleted");

    // Ask again and agree this time: the accepted path is a separate journey
    // through the menu, not a second answer to a question already dismissed.
    QVERIFY(QMetaObject::invokeMethod(deleteEntry, "triggered"));
    QTest::qWait(50);
    QMetaObject::invokeMethod(deleteDialog, "accept");
    QTest::qWait(50);
    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("deleteSession(s2)")}));

    // Rename is the one entry that goes through a dialog first.
    fixture.app.clearCalls();
    QObject *dialog = row->findChild<QObject *>(QStringLiteral("renameDialog:s2"));
    QObject *field = row->findChild<QObject *>(QStringLiteral("renameField:s2"));
    QVERIFY(dialog && field);
    field->setProperty("text", QStringLiteral("renamed"));
    QMetaObject::invokeMethod(dialog, "accept");
    QTest::qWait(50);
    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("renameSession(s2,renamed)")}));

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

QTEST_MAIN(TstSidebar)

#include "tst_sidebar.moc"
