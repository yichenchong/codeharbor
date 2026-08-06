// Regression proofs for the two small UI affordances that are easy to lose in a
// QML/WebEngine refactor:
//
//   * AppScrollBar must be absent and disabled when its Flickable fits, and
//     become a usable scrollbar as soon as content overflows.
//   * ViewerPane's full-pane click sniffer must not replace a WebEngine page's
//     CSS cursor. The headless test checks the sniffer's structural properties;
//     a live page is intentionally not required for this graphics-independent
//     assertion.
//
// Runs headless with the same software Quick and no-GPU WebEngine recipe as the
// other QML tests (see CMakeLists.txt).

#include <QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QtQuickControls2/QQuickStyle>
#include <QtWebEngineQuick/QtWebEngineQuick>

namespace {

constexpr auto kScrollbarHarness = R"QML(
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import CodeHarbor

Window {
    id: root
    objectName: "scrollbarWindow"
    width: 140
    height: 100
    visible: true
    property int itemCount: 2

    ListView {
        id: list
        objectName: "scrollbarList"
        anchors.fill: parent
        model: root.itemCount
        delegate: Rectangle {
            width: list.width
            height: 20
            color: "transparent"
        }
        ScrollBar.vertical: AppScrollBar {
            objectName: "scrollbar"
        }
    }
}
)QML";


QObject *findByName(QObject *root, const QString &name, QSet<const QObject *> &visited)
{
    if (!root || visited.contains(root))
        return nullptr;
    visited.insert(root);
    if (root->objectName() == name)
        return root;

    const auto children = root->children();
    for (QObject *child : children) {
        if (QObject *found = findByName(child, name, visited))
            return found;
    }
    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        const auto itemChildren = item->childItems();
        for (QQuickItem *child : itemChildren) {
            if (QObject *found = findByName(child, name, visited))
                return found;
        }
    }
    return nullptr;
}

QObject *findByName(QObject *root, const QString &name)
{
    QSet<const QObject *> visited;
    return findByName(root, name, visited);
}


QQmlComponent *inlineComponent(QQmlEngine &engine, const QByteArray &source, const QString &name)
{
    auto *component = new QQmlComponent(&engine, &engine);
    component->setData(source, QUrl(QStringLiteral("qrc:/codeharbor-ui-polish/%1.qml").arg(name)));
    return component;
}

} // namespace

class TstUiPolish final : public QObject
{
    Q_OBJECT

private slots:
    void appScrollBarIsDisabledAndHiddenWhenContentFits();
    void appScrollBarBecomesUsableWhenContentOverflows();
    void viewerPaneSnifferPreservesWebCursor();
};

void TstUiPolish::appScrollBarIsDisabledAndHiddenWhenContentFits()
{
    QQmlEngine engine;
    auto *component = inlineComponent(engine, QByteArray(kScrollbarHarness), QStringLiteral("ScrollbarHarness"));
    QVERIFY2(!component->isError(), qPrintable(component->errorString()));

    auto *window = qobject_cast<QQuickWindow *>(component->create());
    QVERIFY2(window != nullptr, qPrintable(component->errorString()));
    QScopedPointer<QQuickWindow> windowGuard(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));

    QObject *bar = findByName(window, QStringLiteral("scrollbar"));
    QVERIFY(bar != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(bar->property("contentFits").toBool(), 2000);
    QVERIFY(!bar->property("visible").toBool());
    QVERIFY(!bar->property("enabled").toBool());

    // A disabled control cannot become a hover target. Move over its geometry
    // after the fit state is settled and make sure that remains true.
    QTest::mouseMove(window, QPoint(130, 50));
    QTest::qWait(200);
    QVERIFY(!bar->property("visible").toBool());
    QVERIFY(!bar->property("enabled").toBool());
}

void TstUiPolish::appScrollBarBecomesUsableWhenContentOverflows()
{
    QQmlEngine engine;
    auto *component = inlineComponent(engine, QByteArray(kScrollbarHarness), QStringLiteral("ScrollbarHarnessOverflow"));
    QVERIFY2(!component->isError(), qPrintable(component->errorString()));

    auto *window = qobject_cast<QQuickWindow *>(component->create());
    QVERIFY2(window != nullptr, qPrintable(component->errorString()));
    QScopedPointer<QQuickWindow> windowGuard(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));

    QObject *bar = findByName(window, QStringLiteral("scrollbar"));
    QVERIFY(bar != nullptr);
    QObject *root = window;
    root->setProperty("itemCount", 20);

    QTRY_VERIFY_WITH_TIMEOUT(!bar->property("contentFits").toBool(), 2000);
    QVERIFY(bar->property("visible").toBool());
    QVERIFY(bar->property("enabled").toBool());

    // Hovering an overflowing bar still leaves it interactive and visible;
    // this is the state the fit guard must not accidentally suppress.
    QTest::mouseMove(window, QPoint(130, 50));
    QTest::qWait(200);
    QVERIFY(bar->property("visible").toBool());
    QVERIFY(bar->property("enabled").toBool());
}

// What the fix actually is: the full-pane click sniffer must not claim the
// cursor, so whatever is underneath it - the web page's own CSS cursor, an
// I-beam over code - reaches the user.
//
// Asserted structurally rather than by hovering a live page. Driving a real
// WebEngine view needs a GPU the headless runner does not have (Chromium falls
// back through GLES and Vulkan and then crashes), so a hover test here would
// report the absence of a graphics stack, not the behaviour under test. The
// visible result is confirmed by hand; what a machine can check reliably is
// that the sniffer neither hovers nor imposes a shape.
void TstUiPolish::viewerPaneSnifferPreservesWebCursor()
{
    // A REAL, shown, sized window. Built bare — as this used to be — the pane
    // root is 0x0, so the "covers the whole pane" filter below degenerates
    // into "any MouseArea at all" (0 >= 0), and `containsMouse`/`hasActiveFocus`
    // are false for the trivial reason that nothing was ever shown or hovered.
    // Every assertion then held however the sniffer was written.
    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(480, 320);
    view.setSource(QUrl(QStringLiteral("qrc:/qt/qml/CodeHarbor/ViewerPane.qml")));
    QQuickItem *const item = view.rootObject();
    QVERIFY2(item != nullptr, "ViewerPane.qml failed to instantiate");
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(item->width() > 100 && item->height() > 100);

    // The sniffer is the one MouseArea that covers the whole pane.
    QQuickItem *sniffer = nullptr;
    const auto children = item->childItems();
    for (QQuickItem *child : children) {
        if (QByteArray(child->metaObject()->className()).startsWith("QQuickMouseArea")
            && child->width() >= item->width() && child->height() >= item->height()) {
            sniffer = child;
            break;
        }
    }
    QVERIFY2(sniffer != nullptr, "the viewer pane has no full-pane click sniffer");

    QVERIFY2(!sniffer->property("hoverEnabled").toBool(),
             "the click sniffer tracks hover, which is what lets it take the cursor");
    QCOMPARE(sniffer->cursor().shape(), Qt::ArrowCursor);

    // Now actually put the pointer on it. A MouseArea that does not track
    // hover never reports containsMouse and never claims the cursor from the
    // content underneath, so this is the reading that would change if the
    // sniffer started hovering.
    QTest::mouseMove(&view, QPoint(int(item->width() / 2), int(item->height() / 2)));
    QTest::qWait(150);
    QVERIFY2(!sniffer->property("containsMouse").toBool(),
             "a sniffer that reports containsMouse is hovering after all");
    QVERIFY2(!sniffer->hasActiveFocus(), "the click sniffer must not hold focus either");
}

int main(int argc, char **argv)
{
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    // Every other QML test in this directory pins the Basic style, so the
    // controls under test are drawn by the same style the assertions were
    // written against rather than by whatever this host defaults to.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    TstUiPolish testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_ui_polish.moc"
