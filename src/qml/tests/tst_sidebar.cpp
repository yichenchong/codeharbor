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

#include <algorithm>
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
#include <QVariant>
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

// The same read for an attached property that is not a string. Kept separate
// from attachedString so a MISSING property can be told apart from one that is
// present and false: an accessible state nobody declared and an accessible
// state declared as "off" are different defects, and only the first of them is
// invisible to a screen reader.
QVariant attachedValue(QQuickItem *item, const QString &name)
{
    const QQmlProperty property(item, name, QQmlEngine::contextForObject(item));
    if (!property.isValid())
        return QVariant();
    return property.read();
}

// The `Accessible` attached object itself, which is what carries the action
// handlers (toggleAction and its siblings). QML creates it as a QObject child
// of the item it is attached to, so this is how a test can perform the action
// an assistive technology would perform, rather than only reading state back.
QObject *accessibleAttached(QQuickItem *item)
{
    const auto children = item->findChildren<QObject *>(QString(), Qt::FindDirectChildrenOnly);
    for (QObject *child : children) {
        if (child->inherits("QQuickAccessibleAttached"))
            return child;
    }
    return nullptr;
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
    // Settable, with a change signal, because "the link is down" is a state the
    // rows have to describe differently: a CONSTANT here would leave the
    // recovered case untestable. It still STARTS disconnected, which is what
    // every other test in this file was written against.
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionStateChanged)


public:
    explicit StubApp(QAbstractItemModel *model, QObject *parent = nullptr)
        : QObject(parent), m_model(model)
    {
    }

    QAbstractItemModel *sessionsModel() const { return m_model; }
    QString connectionState() const { return m_connectionState; }
    void setConnectionState(const QString &state)
    {
        if (m_connectionState == state)
            return;
        m_connectionState = state;
        emit connectionStateChanged();
    }

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
    Q_INVOKABLE void setSessionPinned(QString id, bool pinned)
    {
        m_calls.append(QStringLiteral("setSessionPinned(%1,%2)")
                               .arg(id, pinned ? QStringLiteral("true") : QStringLiteral("false")));
    }
    Q_INVOKABLE void archiveSession(QString id)
    {
        m_calls.append(QStringLiteral("archiveSession(%1)").arg(id));
    }
    Q_INVOKABLE void unarchiveSession(QString id)
    {
        m_calls.append(QStringLiteral("unarchiveSession(%1)").arg(id));
    }
    Q_INVOKABLE void reorderSessions(QString groupId, QStringList orderedIds)
    {
        m_calls.append(QStringLiteral("reorderSessions(%1,[%2])")
                               .arg(groupId, orderedIds.join(QLatin1Char(','))));
    }

signals:
    void connectionStateChanged();

private:
    QAbstractItemModel *m_model = nullptr;
    QStringList m_calls;
    QString m_connectionState = QStringLiteral("disconnected");
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

    // NON-DESTRUCTIVE, and that is load-bearing rather than tidy.
    // takeLoggedQmlWarnings() DRAINS the log net's queue, so what it hands back
    // has to be latched here. Every call site is
    //     QVERIFY2(fixture.warnings().isEmpty(), ... fixture.warnings() ...)
    // and the two arguments are evaluated in an unspecified order: without the
    // latch, evaluating the message first emptied the queue and the condition
    // that followed found nothing, so a logged QML warning passed the gate in
    // silence. Same fix, same reason, as ShellFixture::allWarnings() in
    // tst_qmlload.cpp.
    QStringList warnings()
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

private:
    // Everything drained out of the log net so far, so warnings() can be asked
    // twice and answer the same both times.
    QStringList m_logged;
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
    void appSettingsButtonEmitsRequest();
    void dialogContentFitsInsideItsDialog();
    void addButtonsAreLabelledWithoutAPointer();
    void hintsAreDrawnInTheApplicationsOwnPalette();
    void longNamesAreElidedInsteadOfOverflowingTheSidebar();
    void rowNameGetsTheRoomItsSiblingsLeave();
    void rowMenuActionsReachTheWorkspace();
    void groupMenuRenamesTheGroup();
    void rowArchiveButtonIsClickable();
    void enterPressesTheDialogsDefaultButton();
    void enterDoesNotAnswerADestructiveDialogWithDeletion();
    void theDefaultButtonIsMarkedForTheEyeAndForAScreenReader();
    void theSelectedRowIsAnnouncedAndNotOnlyDrawn();
    void aGroupsCollapseStateCanBeReadAndOperatedWithoutSight();
    void theFilterTogglesSayWhetherTheyAreOn();
    void everyDialogFieldCarriesAVisibleLabelThatNamesIt();
    void aStaleRowSaysSoToAScreenReaderToo();
    void everyRowActionIsReachableWithoutAPointer();

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

// The settings window must remain reachable after a failed/disconnected start,
// not only through an undocumented command-palette shortcut. It is the sidebar's
// ONE settings affordance: the footer used to carry a second "Server…" button
// that opened the connect sheet's editor, and that editor no longer exists.
void TstSidebar::appSettingsButtonEmitsRequest()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QSignalSpy requested(fixture.root(), SIGNAL(appSettingsRequested()));
    QVERIFY(requested.isValid());

    QQuickItem *button = findByName(fixture.root(), QStringLiteral("appSettingsButton"));
    QVERIFY2(button, "the sidebar header offers no settings control");
    QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier, centerOf(button));

    QCOMPARE(requested.size(), 1);

    // Exactly one: a second control in the footer is what this replaced, and
    // two ways to reach the same window is how they drift apart again.
    QVERIFY2(!findByName(fixture.root(), QStringLiteral("serverSettingsButton")),
             "the sidebar still carries the old footer server-settings button");

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

    // Compact, but not smaller than a reliable pointer target. The sidebar's
    // square actions were trimmed from 24 to 22 to give the session names back
    // the room they were taking; 22 is the floor below which they stop being
    // comfortable to hit.
    for (QQuickItem *button : {addGroup, addToAlpha}) {
        QVERIFY2(button->width() >= 22 && button->height() >= 22,
                 qPrintable(QStringLiteral("%1 is %2x%3, below the 22x22 hit-area floor")
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
        { "sessionRow:s1", "sessionRowTip:s1" },
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

// The session name is the only thing in a row the user actually reads, and its
// width is not laid out by a positioner: it is computed in QML by subtracting
// the row's indent, its status dot and its three action buttons from the row's
// own width. Any one of those terms going stale is invisible — the label just
// quietly loses or steals room — so the same sum is re-derived here from the
// children the row really draws.
void TstSidebar::rowNameGetsTheRoomItsSiblingsLeave()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *row = findByName(fixture.root(), QStringLiteral("sessionRow:s1"));
    QQuickItem *names = findByName(fixture.root(), QStringLiteral("sessionNames:s1"));
    QQuickItem *dot = findByName(fixture.root(), QStringLiteral("statusDot:s1"));
    QQuickItem *pin = findByName(fixture.root(), QStringLiteral("pinButton:s1"));
    QQuickItem *archive = findByName(fixture.root(), QStringLiteral("archiveButton:s1"));
    QQuickItem *remove = findByName(fixture.root(), QStringLiteral("deleteButton:s1"));
    QVERIFY(row && names && dot && pin && archive && remove);

    auto *content = row->property("contentItem").value<QQuickItem *>();
    QVERIFY(content);
    const qreal spacing = content->property("spacing").toReal();

    // s1 is not archived, so the archived marker is not drawn: five visible
    // children, and therefore four gaps between them. The delegate's padding
    // is already excluded from content.width by Qt Quick.
    const qreal expected = content->width() - dot->width() - pin->width()
                           - archive->width() - remove->width() - spacing * 4;
    QVERIFY2(qAbs(names->width() - expected) <= 0.5,
             qPrintable(QStringLiteral("the name column is %1 wide, but its siblings "
                                       "leave %2")
                                .arg(names->width())
                                .arg(expected)));

    // Self-consistent arithmetic is not enough: it also has to stop the name
    // before the first action button rather than under it.
    QVERIFY2(names->mapToItem(row, QPointF(names->width(), 0)).x()
                     <= pin->mapToItem(row, QPointF(0, 0)).x() + 0.5,
             "the session name runs under the pin button");

    // The report behind this test was that the name had nowhere to go: the
    // indent in front of the row and the buttons behind it together claimed
    // more of the row than the name they surround.
    QVERIFY2(names->width() >= row->width() / 2,
             qPrintable(QStringLiteral("the name column gets %1 pixels of a %2-pixel row")
                                .arg(names->width())
                                .arg(row->width())));

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

// The reported defect: a session row can be renamed from its right-click menu,
// but a group could not be renamed at all. AppController::renameGroup existed
// and nothing on screen reached it. This drives the real gesture — a right-click
// on the header — rather than poking the menu's model, because the missing part
// was the affordance, not the call.
void TstSidebar::groupMenuRenamesTheGroup()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *header = findByName(fixture.root(), QStringLiteral("groupHeader:g1"));
    QVERIFY(header);
    QObject *const menu = header->findChild<QObject *>(QStringLiteral("groupMenu:g1"));
    QVERIFY2(menu, "the group header has no context menu");
    QVERIFY2(!menu->property("visible").toBool(), "the group menu is open before anything asked");

    fixture.app.clearCalls();

    // Left of the header's trailing buttons, so the press reaches the header
    // itself rather than a control sitting on top of it.
    const QPoint hit = header->mapToScene(QPointF(24, header->height() / 2)).toPoint();
    QTest::mouseClick(&fixture.view, Qt::RightButton, Qt::NoModifier, hit);
    QTest::qWait(100);
    QVERIFY2(menu->property("visible").toBool(), "right-clicking a group header opened no menu");

    QObject *const renameEntry = menuItemNamed(header, QStringLiteral("Rename"));
    QVERIFY2(renameEntry, "the group menu has no \"Rename\" action");
    QObject *const dialog = header->findChild<QObject *>(QStringLiteral("renameGroupDialog:g1"));
    QQuickItem *const field =
        header->findChild<QQuickItem *>(QStringLiteral("renameGroupField:g1"));
    QVERIFY(dialog && field);

    // Cancelling must leave the workspace exactly as it was.
    QVERIFY(QMetaObject::invokeMethod(renameEntry, "triggered"));
    QTest::qWait(100);
    QVERIFY2(dialog->property("visible").toBool(), "\"Rename\" opened no dialog");
    QCOMPARE(field->property("text").toString(), QStringLiteral("Alpha"));
    QMetaObject::invokeMethod(dialog, "reject");
    QTest::qWait(50);
    QVERIFY2(fixture.app.calls().isEmpty(), "cancelling the rename still renamed the group");

    // And the accepted path, typed the way a user types it: the dialog selects
    // the old name on open, so the first keystroke replaces it.
    QVERIFY(QMetaObject::invokeMethod(renameEntry, "triggered"));
    QTest::qWait(100);
    for (const QChar character : QStringLiteral("Renamed"))
        QTest::keyClick(&fixture.view, character.toLatin1());
    QTest::qWait(50);
    QCOMPARE(field->property("text").toString(), QStringLiteral("Renamed"));
    QMetaObject::invokeMethod(dialog, "accept");
    QTest::qWait(50);
    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("renameGroup(g1,Renamed)")}));

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

void TstSidebar::rowArchiveButtonIsClickable()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *archive = findByName(fixture.root(), QStringLiteral("archiveButton:s2"));
    QVERIFY(archive);
    fixture.app.clearCalls();
    QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier, centerOf(archive));
    QTest::qWait(50);
    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("archiveSession(s2)")}));

    QVector<ch::GroupRow> archived = twoGroups();
    archived[0].sessions[1].session.archived = true;
    fixture.model.setGroups(archived);
    QTest::qWait(100);
    QVERIFY(!visibleSessionOrder(fixture.root()).contains(QStringLiteral("s2")));

    QQuickItem *filter = findByName(fixture.root(), QStringLiteral("archiveFilterButton"));
    QVERIFY(filter);
    fixture.app.clearCalls();
    QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier, centerOf(filter));
    QTest::qWait(100);
    QVERIFY(visibleSessionOrder(fixture.root()).contains(QStringLiteral("s2")));
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// A dialog with a name typed into it is answered with the keyboard, so Enter has
// to mean something. Before this it meant nothing at all: the only way to finish
// a rename was to take your hands off the keys and click OK.
//
// Driven with a REAL key press at the window rather than by calling accept(),
// because "the key reaches the dialog" is the entire behaviour under test.
void TstSidebar::enterPressesTheDialogsDefaultButton()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *const row = findByName(fixture.root(), QStringLiteral("sessionRow:s2"));
    QVERIFY(row);
    QObject *const renameEntry = menuItemNamed(row, QStringLiteral("Rename"));
    QVERIFY(renameEntry);
    QVERIFY(QMetaObject::invokeMethod(renameEntry, "triggered"));
    QTest::qWait(100);

    QObject *const dialog = row->findChild<QObject *>(QStringLiteral("renameDialog:s2"));
    QQuickItem *const field =
        row->findChild<QQuickItem *>(QStringLiteral("renameField:s2"));
    QVERIFY(dialog && field);
    QVERIFY2(dialog->property("visible").toBool(), "the rename dialog did not open");

    // Typed, not assigned: the dialog opens with the field focused and its text
    // selected, and this must survive that.
    fixture.app.clearCalls();
    for (const QChar character : QStringLiteral("Typed"))
        QTest::keyClick(&fixture.view, character.toLatin1());
    QTest::qWait(50);
    QCOMPARE(field->property("text").toString(), QStringLiteral("Typed"));

    QTest::keyClick(&fixture.view, Qt::Key_Return);
    QTest::qWait(100);

    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("renameSession(s2,Typed)")}));
    QVERIFY2(!dialog->property("visible").toBool(),
             "Enter renamed the session but left the dialog open");
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// The other half of the rule, and the reason it is safe to have at all: a dialog
// whose affirmative answer DESTROYS something answers Enter with Cancel. A user
// dismissing a stack of dialogs by holding Enter must not be able to delete a
// Dev Session by accident.
void TstSidebar::enterDoesNotAnswerADestructiveDialogWithDeletion()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *const row = findByName(fixture.root(), QStringLiteral("sessionRow:s2"));
    QVERIFY(row);
    QObject *const deleteEntry = menuItemNamed(row, QStringLiteral("Delete"));
    QVERIFY(deleteEntry);
    QVERIFY(QMetaObject::invokeMethod(deleteEntry, "triggered"));
    QTest::qWait(100);

    QObject *const dialog =
        row->findChild<QObject *>(QStringLiteral("deleteSessionDialog:s2"));
    QVERIFY(dialog);
    QVERIFY2(dialog->property("visible").toBool(), "the delete confirmation did not open");

    fixture.app.clearCalls();
    QTest::keyClick(&fixture.view, Qt::Key_Return);
    QTest::qWait(100);

    QVERIFY2(fixture.app.calls().isEmpty(),
             qPrintable(QStringLiteral("Enter on the delete confirmation destroyed something: %1")
                                .arg(fixture.app.calls().join(QLatin1Char(',')))));
    // Cancel, not "ignored": the key still answers the question, it just answers
    // it the safe way, so the dialog is gone and the user is not stuck.
    QVERIFY2(!dialog->property("visible").toBool(),
             "Enter left the delete confirmation on screen with no visible effect");
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// A key that answers a dialog is useless if the answer is a secret. The button
// Enter presses is drawn in the accent colour and says what it is to a screen
// reader, and BOTH have to name the same button as the key does — otherwise the
// dialog lies about what Enter will do, which is worse than doing nothing.
void TstSidebar::theDefaultButtonIsMarkedForTheEyeAndForAScreenReader()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *const row = findByName(fixture.root(), QStringLiteral("sessionRow:s2"));
    struct Case {
        QString menuEntry;
        QString dialogName;
        QString expectedButtonText;
        QString otherButtonText;
    };
    // Two opposite answers: the rename's default is its affirmative button, the
    // delete's is deliberately Cancel.
    const QList<Case> cases{
        {QStringLiteral("Rename"), QStringLiteral("renameDialog:s2"), QStringLiteral("OK"),
         QStringLiteral("Cancel")},
        {QStringLiteral("Delete"), QStringLiteral("deleteSessionDialog:s2"),
         QStringLiteral("Cancel"), QStringLiteral("OK")},
    };

    for (const Case &testCase : cases) {
        QObject *const entry = menuItemNamed(row, testCase.menuEntry);
        QVERIFY(entry);
        QVERIFY(QMetaObject::invokeMethod(entry, "triggered"));
        QTest::qWait(100);

        QObject *const dialog = row->findChild<QObject *>(testCase.dialogName);
        QVERIFY(dialog);

        // The dialog's own answer to "which button does Enter press", which is
        // the same object it marks and the same one the shortcut activates.
        auto *const button =
            qobject_cast<QQuickItem *>(dialog->property("defaultButtonItem").value<QObject *>());
        QVERIFY2(button, qPrintable(QStringLiteral("%1 names no default button")
                                            .arg(testCase.dialogName)));
        QCOMPARE(button->property("text").toString(), testCase.expectedButtonText);

        // Drawn differently from the button beside it, so the eye can see which
        // one answers. Compared against the sibling rather than against a colour
        // literal, so the check still means something under another theme.
        QQuickItem *other = nullptr;
        const auto candidates = dialog->findChildren<QQuickItem *>();
        for (QQuickItem *candidate : candidates) {
            if (candidate->property("text").toString() == testCase.otherButtonText)
                other = candidate;
        }
        QVERIFY2(other, qPrintable(QStringLiteral("%1 has no %2 button to compare against")
                                           .arg(testCase.dialogName, testCase.otherButtonText)));
        const auto colourOf = [](QQuickItem *item) {
            return QQmlProperty(item, QStringLiteral("palette.button"),
                                QQmlEngine::contextForObject(item))
                .read()
                .value<QColor>();
        };
        QVERIFY2(colourOf(button).isValid(), "the default button exposes no button colour");
        QVERIFY2(colourOf(button) != colourOf(other),
                 qPrintable(QStringLiteral("the %1 button is drawn exactly like the %2 button "
                                           "beside it, so nothing shows which one Enter presses")
                                    .arg(testCase.expectedButtonText,
                                         testCase.otherButtonText)));

        // And announced, so a screen reader user is told the same thing.
        const QString description = attachedString(button, QStringLiteral("Accessible.description"));
        QVERIFY2(description.contains(QStringLiteral("Enter"), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("the %1 button does not say it answers Enter: \"%2\"")
                                    .arg(testCase.expectedButtonText, description)));

        QMetaObject::invokeMethod(dialog, "reject");
        QTest::qWait(50);
    }

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// Selection in this panel is drawn three ways — a wash, a coloured rail and a
// focus ring — and a screen reader can see none of them. Without
// Accessible.selected the sidebar is announced as a stack of rows with no
// current one among them, so a blind user cannot tell which Dev Session they
// are about to open, or which one Enter would load.
void TstSidebar::theSelectedRowIsAnnouncedAndNotOnlyDrawn()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *const first = findByName(fixture.root(), QStringLiteral("sessionRow:s1"));
    QQuickItem *const second = findByName(fixture.root(), QStringLiteral("sessionRow:s2"));
    QQuickItem *const header = findByName(fixture.root(), QStringLiteral("groupHeader:g1"));
    QVERIFY(first && second && header);

    // Read by value throughout: the attached property resolves whether or not
    // the QML ever set it, and an undeclared one simply reads false forever —
    // which is precisely the defect, so the value is the only honest check.
    const QString selected = QStringLiteral("Accessible.selected");

    QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier, centerOf(second));
    QTest::qWait(50);
    QVERIFY2(attachedValue(second, selected).toBool(),
             "clicking a session row does not announce it as the selected one");
    QVERIFY2(!attachedValue(first, selected).toBool(),
             "two session rows announce themselves as selected at once");
    QVERIFY2(!attachedValue(header, selected).toBool(),
             "a group header announces itself as selected while a session row is");

    // The cursor is one list over both kinds of row, so the announcement has to
    // follow it onto a group header and off the session it left.
    QTest::keyClick(&fixture.view, Qt::Key_Up); // s2 -> s1
    QTest::keyClick(&fixture.view, Qt::Key_Up); // s1 -> the Alpha header
    QTest::qWait(50);
    QCOMPARE(fixture.root()->property("currentId").toString(), QStringLiteral("g1"));
    QVERIFY2(attachedValue(header, selected).toBool(),
             "moving the keyboard cursor onto a group header does not announce it as selected");
    QVERIFY2(!attachedValue(second, selected).toBool(),
             "the session row still claims to be selected after the cursor moved off it");

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// A group header described its collapse in prose and nowhere else. Prose is not
// a state: it cannot be queried, it cannot be acted on, and a user of assistive
// technology therefore had no way to open a folded group at all. Qt's QML
// Accessible attached type has no `expanded`, so the open/shut pair is
// checkable + checked, and the toggle action is what actually opens it.
void TstSidebar::aGroupsCollapseStateCanBeReadAndOperatedWithoutSight()
{
    SidebarFixture fixture(twoGroups(/*betaCollapsed=*/true));
    expose(fixture);

    QQuickItem *const alpha = findByName(fixture.root(), QStringLiteral("groupHeader:g1"));
    QQuickItem *const beta = findByName(fixture.root(), QStringLiteral("groupHeader:g2"));
    QVERIFY(alpha && beta);

    for (QQuickItem *item : {alpha, beta}) {
        QVERIFY2(attachedValue(item, QStringLiteral("Accessible.checkable")).toBool(),
                 qPrintable(QStringLiteral("%1 is announced as a plain list item, so nothing "
                                           "says it is a group that opens and shuts")
                                    .arg(item->objectName())));
    }
    QVERIFY2(attachedValue(alpha, QStringLiteral("Accessible.checked")).toBool(),
             "the expanded group does not announce itself as open");
    QVERIFY2(!attachedValue(beta, QStringLiteral("Accessible.checked")).toBool(),
             "the collapsed group announces itself as open, so its hidden sessions look "
             "like sessions that do not exist");

    // The words stay too: the state and the sentence say the same thing.
    QVERIFY2(attachedString(beta, QStringLiteral("Accessible.description"))
                     .contains(QStringLiteral("Collapsed")),
             qPrintable(attachedString(beta, QStringLiteral("Accessible.description"))));

    // And it can be operated, not only read. This is the action assistive
    // technology invokes, and it has to take the same route the click takes —
    // through the sidebar, which is the object that holds `app`.
    QObject *const attached = accessibleAttached(beta);
    QVERIFY2(attached, "the group header exposes no Accessible object to act on");
    fixture.app.clearCalls();
    QVERIFY(QMetaObject::invokeMethod(attached, "toggleAction"));
    QTest::qWait(50);
    QCOMPARE(fixture.app.calls(),
             (QStringList{QStringLiteral("setGroupCollapsed(g2,false)")}));

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// The two filters in the header bar are toggles, and the ONLY thing that used to
// change when one went on was the sentence naming what the next click would do.
// A control that renames itself is indistinguishable from a different control:
// a screen-reader user could not find out whether archived sessions were being
// hidden, which is the difference between "I have no archived sessions" and "I
// cannot see them".
void TstSidebar::theFilterTogglesSayWhetherTheyAreOn()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    struct Case {
        QString buttonName;
        QByteArray sidebarProperty;
        QString expectedLabel;
    };
    const QList<Case> cases{
        {QStringLiteral("pinFilterButton"), QByteArrayLiteral("pinnedOnly"),
         QStringLiteral("Pinned sessions only")},
        {QStringLiteral("archiveFilterButton"), QByteArrayLiteral("showArchived"),
         QStringLiteral("Include archived sessions")},
    };

    for (const Case &testCase : cases) {
        QQuickItem *const button = findByName(fixture.root(), testCase.buttonName);
        QVERIFY2(button, qPrintable(QStringLiteral("no %1").arg(testCase.buttonName)));

        QVERIFY2(attachedValue(button, QStringLiteral("Accessible.checkable")).toBool(),
                 qPrintable(QStringLiteral("%1 is announced as a plain button, so nothing "
                                           "says it is a filter that is on or off")
                                    .arg(testCase.buttonName)));
        QVERIFY2(!attachedValue(button, QStringLiteral("Accessible.checked")).toBool(),
                 qPrintable(QStringLiteral("%1 starts announced as on while its filter is off")
                                    .arg(testCase.buttonName)));

        // The name is the FILTER, not the click. The verb stays on the tooltip,
        // which is a pointer hint and must not be the thing carrying state.
        const QString restingName = attachedString(button, QStringLiteral("Accessible.name"));
        QCOMPARE(restingName, testCase.expectedLabel);
        const QString hint = button->property("actionText").toString();
        QVERIFY2(!hint.isEmpty(), "the filter button lost its pointer hint");

        QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier, centerOf(button));
        QTest::qWait(100);
        QVERIFY2(fixture.root()->property(testCase.sidebarProperty.constData()).toBool(),
                 qPrintable(QStringLiteral("clicking %1 did not turn its filter on")
                                    .arg(testCase.buttonName)));
        QVERIFY2(attachedValue(button, QStringLiteral("Accessible.checked")).toBool(),
                 qPrintable(QStringLiteral("%1 filters the list but still announces itself as "
                                           "off, so a screen-reader user cannot tell that rows "
                                           "are being hidden")
                                    .arg(testCase.buttonName)));
        QCOMPARE(attachedString(button, QStringLiteral("Accessible.name")), restingName);
        QVERIFY2(button->property("actionText").toString() != hint,
                 "the pointer hint no longer says what the next click would do");

        // Operable through the accessibility action as well as the pointer, and
        // it leaves the sidebar exactly as it found it.
        QObject *const attached = accessibleAttached(button);
        QVERIFY2(attached, "the filter button exposes no Accessible object to act on");
        QVERIFY(QMetaObject::invokeMethod(attached, "toggleAction"));
        QTest::qWait(100);
        QVERIFY2(!fixture.root()->property(testCase.sidebarProperty.constData()).toBool(),
                 qPrintable(QStringLiteral("the accessibility toggle on %1 does not switch the "
                                           "filter it announces")
                                    .arg(testCase.buttonName)));
        QVERIFY(!attachedValue(button, QStringLiteral("Accessible.checked")).toBool());
    }

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// Every text field the sidebar owns had a placeholder and nothing else — and
// three of them open PREFILLED, so even the placeholder was never on screen. A
// sighted user met a box with a word in it and no title; a screen-reader user
// met an unnamed edit box. Each one now carries a visible label above it, and
// the field's accessible name is that same label rather than a second copy of
// the words that can drift away from it.
void TstSidebar::everyDialogFieldCarriesAVisibleLabelThatNamesIt()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);
    fixture.app.clearCalls();

    struct Case {
        // Empty means the sidebar root; the two rename dialogs are declared per
        // delegate, which is parented visually rather than as a QObject child.
        QString ownerName;
        QString dialogName;
        QString fieldName;
        QString labelName;
        QString expectedLabel;
    };
    const QList<Case> cases{
        {QString(), QStringLiteral("newGroupDialog"), QStringLiteral("newGroupField"),
         QStringLiteral("newGroupFieldLabel"), QStringLiteral("Group name")},
        {QString(), QStringLiteral("newSessionDialog"), QStringLiteral("newSessionField"),
         QStringLiteral("newSessionFieldLabel"), QStringLiteral("Session name")},
        {QString(), QStringLiteral("newSessionDialog"), QStringLiteral("newSessionRepoField"),
         QStringLiteral("newSessionRepoFieldLabel"),
         QStringLiteral("Repository path on the server")},
        {QStringLiteral("sessionRow:s1"), QStringLiteral("renameDialog:s1"),
         QStringLiteral("renameField:s1"), QStringLiteral("renameFieldLabel:s1"),
         QStringLiteral("Session name")},
        {QStringLiteral("groupHeader:g1"), QStringLiteral("renameGroupDialog:g1"),
         QStringLiteral("renameGroupField:g1"), QStringLiteral("renameGroupFieldLabel:g1"),
         QStringLiteral("Group name")},
    };

    for (const Case &testCase : cases) {
        QQuickItem *owner = fixture.root();
        if (!testCase.ownerName.isEmpty()) {
            owner = findByName(fixture.root(), testCase.ownerName);
            QVERIFY2(owner, qPrintable(QStringLiteral("no %1").arg(testCase.ownerName)));
        }

        QObject *const dialog = owner->findChild<QObject *>(testCase.dialogName);
        QVERIFY2(dialog, qPrintable(QStringLiteral("no %1").arg(testCase.dialogName)));
        QQuickItem *const field = owner->findChild<QQuickItem *>(testCase.fieldName);
        QVERIFY2(field, qPrintable(QStringLiteral("no %1").arg(testCase.fieldName)));

        QMetaObject::invokeMethod(dialog, "open");
        QTest::qWait(100);

        QQuickItem *const label = owner->findChild<QQuickItem *>(testCase.labelName);
        QVERIFY2(label,
                 qPrintable(QStringLiteral("%1 has no visible label, so the only thing naming "
                                           "it is a placeholder — which a prefilled field never "
                                           "shows")
                                    .arg(testCase.fieldName)));
        QVERIFY2(label->isVisible(),
                 qPrintable(QStringLiteral("the label for %1 is not on screen")
                                    .arg(testCase.fieldName)));
        QCOMPARE(label->property("text").toString(), testCase.expectedLabel);
        QVERIFY2(sceneTop(label) < sceneTop(field),
                 qPrintable(QStringLiteral("the label for %1 is not above the field it names")
                                    .arg(testCase.fieldName)));

        QCOMPARE(attachedString(field, QStringLiteral("Accessible.name")),
                 label->property("text").toString());

        QMetaObject::invokeMethod(dialog, "reject");
        QTest::qWait(50);
    }

    // Reading a dialog must not have created or renamed anything.
    QVERIFY2(fixture.app.calls().isEmpty(), qPrintable(fixture.app.calls().join(QLatin1Char(','))));
    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// While the link is down every status in the sidebar is the last one the server
// managed to report. The tooltip has always said so and the rows dim, but the
// accessible description repeated the stale status as if it were current — so
// the one user who cannot see the dimming was the one user told nothing.
void TstSidebar::aStaleRowSaysSoToAScreenReaderToo()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    QQuickItem *const row = findByName(fixture.root(), QStringLiteral("sessionRow:s1"));
    QVERIFY(row);
    QObject *const hint = hintOf(row, QStringLiteral("sessionRowTip:s1"));
    QVERIFY2(hint, "the session row carries no status hint");

    // The stub starts disconnected, which is exactly the case this is about.
    QVERIFY2(fixture.root()->property("stale").toBool(),
             "the fixture is not in the stale state this test exists to check");

    const QString warned = QStringLiteral("last known");
    const QString staleDescription =
        attachedString(row, QStringLiteral("Accessible.description"));
    QVERIFY2(staleDescription.contains(warned),
             qPrintable(QStringLiteral("a row whose status predates a dropped link is described "
                                       "as \"%1\", which reads as the truth right now")
                                .arg(staleDescription)));
    QVERIFY2(hint->property("text").toString().contains(warned),
             "the tooltip stopped warning that the status is out of date");

    // ...and it stops saying it when the link comes back. A warning that never
    // clears is the same as no warning.
    fixture.app.setConnectionState(QStringLiteral("connected"));
    QTest::qWait(100);
    QVERIFY2(!fixture.root()->property("stale").toBool(),
             "the sidebar still considers itself stale after the link came back");
    const QString liveDescription =
        attachedString(row, QStringLiteral("Accessible.description"));
    QVERIFY2(!liveDescription.isEmpty(), "a connected row is described as nothing at all");
    QVERIFY2(!liveDescription.contains(warned),
             qPrintable(QStringLiteral("a live row is still described as out of date: \"%1\"")
                                .arg(liveDescription)));

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

// Rename, Duplicate, Move to top and Archive lived in a menu that only a
// right-click opened, so a user who works from the keyboard could not reach any
// of them — and renaming had no other affordance anywhere in the product. The
// menu key opens that menu on whichever row the cursor is on, Shift+F10 is the
// same request from a keyboard without one, and F2 goes straight to the rename.
// Escape gets back out of both, which is the other half of the rule: a surface
// the keyboard can enter and not leave is worse than one it cannot enter.
void TstSidebar::everyRowActionIsReachableWithoutAPointer()
{
    SidebarFixture fixture(twoGroups());
    expose(fixture);

    fixture.root()->forceActiveFocus();
    QTest::keyClick(&fixture.view, Qt::Key_Down); // the Alpha header
    QTest::keyClick(&fixture.view, Qt::Key_Down); // s1
    QCOMPARE(fixture.root()->property("currentId").toString(), QStringLiteral("s1"));

    QQuickItem *const row = findByName(fixture.root(), QStringLiteral("sessionRow:s1"));
    QVERIFY(row);
    QObject *const rowMenu = row->findChild<QObject *>(QStringLiteral("sessionMenu:s1"));
    QVERIFY2(rowMenu, "the session row has no context menu to reach");
    QVERIFY2(!rowMenu->property("visible").toBool(), "the row menu is open before anything asked");

    QTest::keyClick(&fixture.view, Qt::Key_Menu);
    QTest::qWait(100);
    QVERIFY2(rowMenu->property("visible").toBool(),
             "the menu key on the focused session row opened nothing, so Rename, Duplicate, "
             "Move to top and Archive stay behind a right-click");

    QTest::keyClick(&fixture.view, Qt::Key_Escape);
    QTest::qWait(100);
    QVERIFY2(!rowMenu->property("visible").toBool(), "Escape did not close the row menu");
    QVERIFY2(fixture.root()->hasActiveFocus(),
             "closing the row menu left the keyboard nowhere: the sidebar no longer has focus, "
             "so the next arrow key goes to whatever does");

    // F2 skips the menu entirely and renames the cursor's row, typed and
    // answered with real keys the whole way.
    fixture.app.clearCalls();
    QTest::keyClick(&fixture.view, Qt::Key_F2);
    QTest::qWait(100);
    QObject *const renameDialog = row->findChild<QObject *>(QStringLiteral("renameDialog:s1"));
    QVERIFY(renameDialog);
    QVERIFY2(renameDialog->property("visible").toBool(),
             "F2 on the focused session row opened no rename dialog");
    for (const QChar character : QStringLiteral("Typed"))
        QTest::keyClick(&fixture.view, character.toLatin1());
    QTest::keyClick(&fixture.view, Qt::Key_Return);
    QTest::qWait(100);
    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("renameSession(s1,Typed)")}));

    // The same two keys on a group header, where Rename was equally unreachable.
    fixture.root()->forceActiveFocus();
    QTest::keyClick(&fixture.view, Qt::Key_Up); // s1 -> the Alpha header
    QCOMPARE(fixture.root()->property("currentId").toString(), QStringLiteral("g1"));

    QQuickItem *const header = findByName(fixture.root(), QStringLiteral("groupHeader:g1"));
    QVERIFY(header);
    QObject *const groupMenu = header->findChild<QObject *>(QStringLiteral("groupMenu:g1"));
    QVERIFY(groupMenu);

    QTest::keyClick(&fixture.view, Qt::Key_F10, Qt::ShiftModifier);
    QTest::qWait(100);
    QVERIFY2(groupMenu->property("visible").toBool(),
             "Shift+F10 on the focused group header opened nothing, so a keyboard without a "
             "menu key cannot reach the group's actions");
    QTest::keyClick(&fixture.view, Qt::Key_Escape);
    QTest::qWait(100);
    QVERIFY2(!groupMenu->property("visible").toBool(), "Escape did not close the group menu");
    QVERIFY2(fixture.root()->hasActiveFocus(),
             "closing the group menu left the keyboard nowhere");

    fixture.app.clearCalls();
    QTest::keyClick(&fixture.view, Qt::Key_F2);
    QTest::qWait(100);
    QObject *const renameGroupDialog =
        header->findChild<QObject *>(QStringLiteral("renameGroupDialog:g1"));
    QVERIFY(renameGroupDialog);
    QVERIFY2(renameGroupDialog->property("visible").toBool(),
             "F2 on the focused group header opened no rename dialog");
    for (const QChar character : QStringLiteral("Renamed"))
        QTest::keyClick(&fixture.view, character.toLatin1());
    QTest::keyClick(&fixture.view, Qt::Key_Return);
    QTest::qWait(100);
    QCOMPARE(fixture.app.calls(), (QStringList{QStringLiteral("renameGroup(g1,Renamed)")}));

    QVERIFY2(fixture.warnings().isEmpty(), qPrintable(fixture.warnings().join(QLatin1Char('\n'))));
}

QTEST_MAIN(TstSidebar)

#include "tst_sidebar.moc"
