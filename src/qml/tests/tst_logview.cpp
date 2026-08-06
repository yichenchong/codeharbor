#include <QtTest/QtTest>

#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QSet>
#include <QUrl>
#include <QVariant>

#include <memory>
#include "LogBuffer.h"

using namespace ch;

namespace {

// An attached `ScrollBar.vertical: ...` object is parented into the VISUAL
// tree, not the QObject tree, so QObject::findChild() never reaches it. Walk
// both, the way the other QML tests in this directory do.
QObject *findByName(QObject *root, const QString &name, QSet<const QObject *> &seen)
{
    if (!root || seen.contains(root))
        return nullptr;
    seen.insert(root);
    if (root->objectName() == name)
        return root;
    const auto objectChildren = root->children();
    for (QObject *child : objectChildren) {
        if (QObject *found = findByName(child, name, seen))
            return found;
    }
    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        const auto itemChildren = item->childItems();
        for (QQuickItem *child : itemChildren) {
            if (QObject *found = findByName(child, name, seen))
                return found;
        }
    }
    return nullptr;
}

QObject *findByName(QObject *root, const QString &name)
{
    QSet<const QObject *> seen;
    return findByName(root, name, seen);
}

} // namespace

class TstLogView final : public QObject {
    Q_OBJECT

private slots:
    void messageSurvivesClosedSheet();
    void severityFilterUsesCanonicalKeys();
    void longLinesStayReachable();
    void severityFilterHidesOtherSeverities();
};

void TstLogView::severityFilterUsesCanonicalKeys()
{
    QQmlEngine engine;
    QQmlComponent component(&engine,
                           QUrl(QStringLiteral("qrc:/qt/qml/CodeHarbor/LogView.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    std::unique_ptr<QObject> view(component.create());
    QVERIFY2(view != nullptr, qPrintable(component.errorString()));
    QObject *severity = view->findChild<QObject *>(QStringLiteral("severityFilter"));
    QVERIFY(severity);

    severity->setProperty("currentIndex", 3);
    QTRY_COMPARE(view->property("severityFilter").toString(), QStringLiteral("warning"));
    severity->setProperty("currentIndex", 0);
    QTRY_COMPARE(view->property("severityFilter").toString(), QString());
    severity->setProperty("currentIndex", 5);
    QTRY_COMPARE(view->property("severityFilter").toString(), QStringLiteral("fatal"));
}

void TstLogView::messageSurvivesClosedSheet()
{
    LogBuffer buffer;
    QQmlEngine engine;
    QQmlComponent component(&engine,
                           QUrl(QStringLiteral("qrc:/qt/qml/CodeHarbor/LogView.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    std::unique_ptr<QObject> view(component.create());
    QVERIFY2(view != nullptr, qPrintable(component.errorString()));
    QQuickItem* const item = qobject_cast<QQuickItem*>(view.get());
    QVERIFY(item != nullptr);
    item->setProperty("logBuffer", QVariant::fromValue(&buffer));
    item->setProperty("shown", false);

    buffer.appendRemote(QStringLiteral("daemon"), QStringLiteral("rpc"),
                        QStringLiteral("bridge"), QStringLiteral("message while closed"));
    QTRY_VERIFY_WITH_TIMEOUT(
        item->property("visibleText").toString().contains(QStringLiteral("message while closed")),
        1000);
    QCOMPARE(item->property("shown").toBool(), false);

    item->setProperty("shown", true);
    QTRY_VERIFY_WITH_TIMEOUT(
        item->property("visibleText").toString().contains(QStringLiteral("message while closed")),
        1000);
}

// Entries are shown unwrapped, one message per line, so a remote stack trace or
// a long RPC payload is routinely wider than the sheet. Without a horizontal
// axis the overflow is clipped and there is no gesture that can bring it back:
// the text is in the buffer and the user cannot read it.
void TstLogView::longLinesStayReachable()
{
    LogBuffer buffer;
    // A real (offscreen) window: the sheet's content sits in a ColumnLayout,
    // and a layout only runs its polish pass once the item is in a window, so
    // a windowless component reports no geometry to assert on.
    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(400, 300);
    view.setSource(QUrl(QStringLiteral("qrc:/qt/qml/CodeHarbor/LogView.qml")));
    QQuickItem *const item = view.rootObject();
    QVERIFY(item != nullptr);
    item->setProperty("logBuffer", QVariant::fromValue(&buffer));
    item->setProperty("shown", true);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    auto *const flick =
        qobject_cast<QQuickItem *>(findByName(item, QStringLiteral("logScroll")));
    QVERIFY(flick != nullptr);

    buffer.appendRemote(QStringLiteral("daemon"), QStringLiteral("rpc"),
                        QStringLiteral("bridge"), QString(600, QLatin1Char('x')));
    QTRY_VERIFY_WITH_TIMEOUT(
        item->property("visibleText").toString().length() > 500, 1000);

    QTRY_VERIFY_WITH_TIMEOUT(
        flick->property("contentWidth").toReal()
            > flick->property("width").toReal() + 1.0,
        1000);
    // AppScrollBar disables and hides itself whenever the viewport already
    // holds the whole content, so a horizontal bar that is live is the
    // observable proof that the overflow can be reached.
    QObject *const bar = findByName(item, QStringLiteral("logHorizontalScroll"));
    QVERIFY2(bar != nullptr, "the log well has no horizontal scrollbar");
    QTRY_VERIFY_WITH_TIMEOUT(bar->property("enabled").toBool(), 1000);
}

// The canonical key is only half of it. severityFilterUsesCanonicalKeys checks
// what the combo box computes; nothing checked that the value is handed to
// LogBuffer and that the pane then shows the answer, so a view that never
// re-queried would pass that test while showing every entry regardless of the
// filter the user picked.
void TstLogView::severityFilterHidesOtherSeverities()
{
    LogBuffer buffer;
    QQmlEngine engine;
    QQmlComponent component(&engine,
                           QUrl(QStringLiteral("qrc:/qt/qml/CodeHarbor/LogView.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    std::unique_ptr<QObject> view(component.create());
    QVERIFY2(view != nullptr, qPrintable(component.errorString()));
    view->setProperty("logBuffer", QVariant::fromValue(&buffer));
    view->setProperty("shown", true);

    buffer.appendRemote(QStringLiteral("daemon"), QStringLiteral("rpc"),
                        QStringLiteral("bridge"), QStringLiteral("routine-chatter"),
                        QtInfoMsg);
    buffer.appendRemote(QStringLiteral("daemon"), QStringLiteral("rpc"),
                        QStringLiteral("bridge"), QStringLiteral("disk-nearly-full"),
                        QtWarningMsg);

    const auto shows = [&view](const QString &needle) {
        return view->property("visibleText").toString().contains(needle);
    };
    QTRY_VERIFY(shows(QStringLiteral("routine-chatter")));
    QVERIFY(shows(QStringLiteral("disk-nearly-full")));

    QObject *severity = view->findChild<QObject *>(QStringLiteral("severityFilter"));
    QVERIFY(severity);
    severity->setProperty("currentIndex", 3); // Warning
    QTRY_VERIFY2(!shows(QStringLiteral("routine-chatter")),
                 "an info entry is still on screen with the filter set to warnings");
    QVERIFY2(shows(QStringLiteral("disk-nearly-full")),
             "the warning the filter selected is not on screen");

    // The filter is a view, not a delete: All brings the hidden entry back.
    severity->setProperty("currentIndex", 0);
    QTRY_VERIFY(shows(QStringLiteral("routine-chatter")));
    QVERIFY(shows(QStringLiteral("disk-nearly-full")));
}

QTEST_MAIN(TstLogView)
#include "tst_logview.moc"
