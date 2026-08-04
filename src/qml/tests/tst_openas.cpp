#include <QtTest>

#include <QByteArray>
#include <QGuiApplication>
#include <QList>
#include <QMetaObject>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSet>
#include <QSignalSpy>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QStandardPaths>
#include <QtWebEngineQuick/QtWebEngineQuick>

#include "ViewerProfiles.h"
#include <QtQuickControls2/QQuickStyle>

#include <memory>


namespace {

constexpr auto kModuleRoot = "qrc:/qt/qml/CodeHarbor/";

QUrl moduleUrl(const QString &file)
{
    return QUrl(QLatin1String(kModuleRoot) + file);
}

class ViewerStub : public QObject
{
    Q_OBJECT
public:
    explicit ViewerStub(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE void listDirectory(const QString &path)
    {
        QMetaObject::invokeMethod(
            this,
            [this, path] { emit directoryListed(path, m_entries); },
            Qt::QueuedConnection);
    }

    Q_INVOKABLE QStringList applicableViewKinds(const QUrl &url) const
    {
        const QString path = url.path().toLower();
        if (path.endsWith(QLatin1Char('/')))
            return {QStringLiteral("directory")};
        if (path.endsWith(QLatin1String(".png")))
            return {QStringLiteral("image")};
        return {QStringLiteral("editor"), QStringLiteral("text")};
    }

    Q_INVOKABLE bool isValidApplicationScheme(const QString &scheme) const
    {
        return scheme == QStringLiteral("zed");
    }

    Q_INVOKABLE bool openWithApplication(const QString &scheme,
                                         const QString &path) const
    {
        return scheme == QStringLiteral("zed") && path.startsWith(QLatin1Char('/'));
    }

    Q_INVOKABLE QString viewKind(const QUrl &) const { return QStringLiteral("empty"); }

    void setEntries(const QVariantList &entries) { m_entries = entries; }

signals:
    void directoryListed(const QString &path, const QVariantList &entries);
    void directoryError(const QString &path, const QString &message);

private:
    QVariantList m_entries;
};

QObject *findText(QObject *root, const QString &text)
{
    if (!root)
        return nullptr;
    if (root->property("text").toString() == text)
        return root;
    for (QObject *child : root->children()) {
        if (QObject *found = findText(child, text))
            return found;
    }
    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        for (QQuickItem *child : item->childItems()) {
            if (QObject *found = findText(child, text))
                return found;
        }
    }
    return nullptr;
}

// findChild() walks QObject ownership, and a ListView's delegates hang off its
// contentItem's VISUAL children, so the ordinary lookup never sees them. This
// walks both trees, which is what it takes to reach a row's controls.
// The delegate for one listed row, located by the text it displays. Every row
// carries controls with the same object names, so a lookup that is not scoped
// to a row finds the ".." entry first and asserts against the wrong item.
QObject *findRow(QObject *root, const QString &displayText);

QObject *findNamed(QObject *root, const QString &objectName)
{
    if (!root)
        return nullptr;
    if (root->objectName() == objectName)
        return root;
    for (QObject *child : root->children()) {
        if (QObject *found = findNamed(child, objectName))
            return found;
    }
    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        for (QQuickItem *child : item->childItems()) {
            if (QObject *found = findNamed(child, objectName))
                return found;
        }
    }
    return nullptr;
}

QObject *findRow(QObject *root, const QString &displayText)
{
    QObject *label = findText(root, displayText);
    for (QObject *o = label; o; o = o->parent()) {
        if (findNamed(o, QStringLiteral("openAsButton")))
            return o;
    }
    return nullptr;
}

void collectPanes(QObject *root, QList<QObject *> &out, QSet<QObject *> &seen)
{
    if (!root || seen.contains(root))
        return;
    seen.insert(root);
    const QMetaObject *meta = root->metaObject();
    if (meta->indexOfProperty("paneId") >= 0
        && meta->indexOfProperty("node") < 0)
        out.append(root);
    for (QObject *child : root->children())
        collectPanes(child, out, seen);
    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        for (QQuickItem *child : item->childItems())
            collectPanes(child, out, seen);
    }
}

QVariantMap leaf(const QString &paneId, const QString &url)
{
    return {{QStringLiteral("paneId"), paneId},
            {QStringLiteral("url"), url},
            {QStringLiteral("children"), QVariantList{}}};
}

QVariantMap branch(const QVariantMap &first, const QVariantMap &second)
{
    return {{QStringLiteral("orientation"), QStringLiteral("horizontal")},
            {QStringLiteral("children"), QVariantList{first, second}},
            {QStringLiteral("ratios"), QVariantList{0.5, 0.5}}};
}

} // namespace

class TstOpenAs : public QObject
{
    Q_OBJECT
private slots:
    void menuContentsForTextFile();
    void menuContentsForImageFile();
    void openAsNewPanePreservesOriginal();

private:
    QQuickView *directoryView(ViewerStub *stub, const QVariantList &entries,
                              std::unique_ptr<QQuickView> &owner);
};

QQuickView *TstOpenAs::directoryView(ViewerStub *stub, const QVariantList &entries,
                                     std::unique_ptr<QQuickView> &owner)
{
    owner = std::make_unique<QQuickView>();
    owner->rootContext()->setContextProperty(QStringLiteral("viewers"), stub);
    owner->setResizeMode(QQuickView::SizeRootObjectToView);
    owner->resize(640, 360);
    owner->setSource(moduleUrl(QStringLiteral("ViewerDirectoryView.qml")));
    if (!owner->rootObject())
        return nullptr;
    stub->setEntries(entries);
    owner->rootObject()->setProperty("url",
                                     QUrl(QStringLiteral("file:///repo/")));
    owner->rootObject()->setProperty("entries", entries);
    owner->rootObject()->setProperty("loading", false);
    owner->show();
    QTest::qWait(100);
    return owner.get();
}

void TstOpenAs::menuContentsForTextFile()
{
    ViewerStub stub;
    std::unique_ptr<QQuickView> view;
    const QVariantList entries = {
        QVariantMap{{QStringLiteral("name"), QStringLiteral("main.cpp")},
                    {QStringLiteral("kind"), QStringLiteral("file")}}};
    QVERIFY(directoryView(&stub, entries, view));

    QObject *row = findRow(view->rootObject(),
                           entries.at(0).toMap()
                               .value(QStringLiteral("name")).toString());
    QVERIFY2(row, "the listing has no row for the file under test");
    QObject *button = findNamed(row, QStringLiteral("openAsButton"));
    QVERIFY(button);
    QVERIFY2(button->property("visible").toBool(),
             "a row with applicable viewers must offer Open as");
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTest::qWait(20);
    QObject *menu = findNamed(row, QStringLiteral("directoryContextMenu"));
    QVERIFY(menu);
    // The submenu builds its items when it opens, so the assertions below have
    // nothing to find until it does.
    QObject *submenu = findNamed(menu, QStringLiteral("openAsSubmenu"));
    QVERIFY(submenu);
    QVERIFY(QMetaObject::invokeMethod(submenu, "open"));
    QTest::qWait(20);
    QVERIFY(findText(menu, QStringLiteral("Editor (default)")));
    QVERIFY(findText(menu, QStringLiteral("Text")));
    QVERIFY(!findText(menu, QStringLiteral("Image")));
    QVERIFY(!findText(menu, QStringLiteral("PDF")));
}

void TstOpenAs::menuContentsForImageFile()
{
    ViewerStub stub;
    std::unique_ptr<QQuickView> view;
    const QVariantList entries = {
        QVariantMap{{QStringLiteral("name"), QStringLiteral("logo.png")},
                    {QStringLiteral("kind"), QStringLiteral("file")}}};
    QVERIFY(directoryView(&stub, entries, view));

    QObject *row = findRow(view->rootObject(),
                           entries.at(0).toMap()
                               .value(QStringLiteral("name")).toString());
    QVERIFY2(row, "the listing has no row for the file under test");
    QObject *button = findNamed(row, QStringLiteral("openAsButton"));
    QVERIFY(button);
    QVERIFY2(button->property("visible").toBool(),
             "a row with applicable viewers must offer Open as");
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTest::qWait(20);
    QObject *menu = findNamed(row, QStringLiteral("directoryContextMenu"));
    QVERIFY(menu);
    // The submenu builds its items when it opens, so the assertions below have
    // nothing to find until it does.
    QObject *submenu = findNamed(menu, QStringLiteral("openAsSubmenu"));
    QVERIFY(submenu);
    QVERIFY(QMetaObject::invokeMethod(submenu, "open"));
    QTest::qWait(20);
    QVERIFY(findText(menu, QStringLiteral("Image (default)")));
    QVERIFY(!findText(menu, QStringLiteral("Editor")));
    QVERIFY(!findText(menu, QStringLiteral("Text")));
    QVERIFY(!findText(menu, QStringLiteral("PDF")));
}

void TstOpenAs::openAsNewPanePreservesOriginal()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral(
            "import QtQuick\n"
            "import QtQuick.Window\n"
            "import CodeHarbor\n"
            "Window { width: 900; height: 500; visible: true;\n"
            "  ViewerRegion { id: region; objectName: 'region'; anchors.fill: parent;\n"
            "    node: ({ paneId: 'pane-1', url: 'file:///repo/original/', children: [] })\n"
            "  }\n"
            "}\n"),
        QUrl(QStringLiteral("inline-openas.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
    QVERIFY(quickWindow);
    quickWindow->show();
    QTest::qWait(100);

    QObject *region = window->findChild<QObject *>(QStringLiteral("region"));
    QVERIFY(region);
    const QVariantMap first = leaf(QStringLiteral("pane-1"),
                                   QStringLiteral("file:///repo/original/"));
    const QVariantMap second = leaf(QStringLiteral("pane-2"), QString());
    region->setProperty("node", branch(first, second));
    QTest::qWait(100);

    QVariant accepted;
    QVERIFY(QMetaObject::invokeMethod(
        region, "openPaneTarget", Q_RETURN_ARG(QVariant, accepted),
        Q_ARG(QVariant, QStringLiteral("pane-2")),
        Q_ARG(QVariant, QStringLiteral("file:///repo/target/")),
        Q_ARG(QVariant, QStringLiteral("directory"))));
    QVERIFY(accepted.toBool());
    QTest::qWait(50);

    QList<QObject *> panes;
    QSet<QObject *> seen;
    collectPanes(window.get(), panes, seen);
    QCOMPARE(panes.size(), 2);
    QObject *original = nullptr;
    QObject *newPane = nullptr;
    for (QObject *pane : panes) {
        if (pane->property("paneId").toString() == QStringLiteral("pane-1"))
            original = pane;
        if (pane->property("paneId").toString() == QStringLiteral("pane-2"))
            newPane = pane;
    }
    QVERIFY(original);
    QVERIFY(newPane);
    QCOMPARE(original->property("url").toUrl(),
             QUrl(QStringLiteral("file:///repo/original/")));
    QCOMPARE(newPane->property("url").toUrl(),
             QUrl(QStringLiteral("file:///repo/target/")));
}

// Explicit main rather than QTEST_MAIN: the viewer surfaces under test build
// WebEngine views, and both the custom URL scheme and WebEngine itself must be
// initialised BEFORE the QGuiApplication exists, exactly as in main.cpp.
int main(int argc, char **argv)
{
    QStandardPaths::setTestModeEnabled(true);

    ch::ViewerProfiles::registerUrlScheme();
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setOrganizationName(QStringLiteral("CodeHarbor"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    TstOpenAs testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_openas.moc"
