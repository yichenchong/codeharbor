// Command palette + global keyboard shortcuts (SPEC 15).
//
// Drives the REAL src/qml/CommandPalette.qml — embedded in this test's own qrc,
// so the component is exercised exactly as shipped and the test does not depend
// on the CodeHarbor QML module registration. A tiny inline harness document
// hosts it the way Main.qml will (one Loader, one `commands` assignment), and
// every command's `invoke` is a plain JS closure that bumps a counter on an
// injected C++ probe — the documented {id,title,shortcut,invoke} shape.
//
// Everything runs headless (offscreen QPA + software Quick backend, pinned by
// the ctest registration). Key events are posted to the QQuickView itself, so
// both the focus path (filter field) and the shortcut path (QShortcutMap, which
// needs an ACTIVE window) are the real ones the application uses.

#include <QtTest>

#include <QByteArray>
#include <QGuiApplication>
#include <QMap>
#include <QPoint>
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
#include <QtQuickControls2/QQuickStyle>

// ---------------------------------------------------------------------------
// Injected counter. Every fake command's invoke() closure calls probe.hit(id),
// so "invoked exactly once" is an assertion about observable behaviour rather
// than about the palette's internals.
// ---------------------------------------------------------------------------
class Probe : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE void hit(const QString &id)
    {
        ++m_counts[id];
        m_order.append(id);
    }

    int count(const QString &id) const { return m_counts.value(id); }
    int total() const { return m_order.size(); }
    QStringList order() const { return m_order; }
    void reset()
    {
        m_counts.clear();
        m_order.clear();
    }

private:
    QMap<QString, int> m_counts;
    QStringList m_order;
};

namespace {

// The command set used by every test but the empty-list one. Chosen so the
// documented ranking is actually distinguishable for the query "save":
//   rank 1 (title prefix)   "Save File", "Save All Files"
//   rank 2 (word prefix)    "Project Save Hook"
//   rank 3 (mid-word)       "Go To Unsaved Buffer"
constexpr auto kCommandsJs = R"JS([
    { id: "file.open",    title: "Open File",            shortcut: "Ctrl+Alt+O",
      invoke: function () { probe.hit("file.open"); } },
    { id: "file.save",    title: "Save File",            shortcut: "Ctrl+Alt+S",
      invoke: function () { probe.hit("file.save"); } },
    { id: "file.saveAll", title: "Save All Files",
      invoke: function () { probe.hit("file.saveAll"); } },
    { id: "proj.hook",    title: "Project Save Hook",
      invoke: function () { probe.hit("proj.hook"); } },
    { id: "nav.unsaved",  title: "Go To Unsaved Buffer",
      invoke: function () { probe.hit("nav.unsaved"); } },
    { id: "term.new",     title: "New Terminal",         shortcut: "Ctrl+Alt+T",
      invoke: function () { probe.hit("term.new"); } }
])JS";

QByteArray harnessQml(const QByteArray &commandsJs)
{
    QByteArray qml = R"QML(
import QtQuick

Item {
    id: harness
    objectName: "harness"
    width: 900
    height: 600

    function matchTitles() {
        var out = [];
        var m = paletteLoader.item ? paletteLoader.item.matches : [];
        for (var i = 0; i < m.length; ++i)
            out.push(String(m[i].title));
        return out;
    }

    Loader {
        id: paletteLoader
        objectName: "paletteLoader"
        source: "qrc:/chtest/CommandPalette.qml"
        onLoaded: {
            item.objectName = "palette";
            item.commands = @COMMANDS@;
        }
    }
}
)QML";
    qml.replace("@COMMANDS@", commandsJs);
    return qml;
}

// A shown, ACTIVE window hosting the palette. Members are ordered so the view
// (and with it the QML tree) is torn down before the probe it references.
class Fixture
{
public:
    Probe probe;
    QStringList warnings;
    QQuickView view;
    QObject *harness = nullptr;
    QObject *palette = nullptr;

    explicit Fixture(const QByteArray &commandsJs = QByteArray(kCommandsJs))
    {
        QObject::connect(view.engine(), &QQmlEngine::warnings, view.engine(),
                         [this](const QList<QQmlError> &list) {
                             for (const QQmlError &error : list)
                                 warnings.append(error.toString());
                         });

        view.rootContext()->setContextProperty(QStringLiteral("probe"), &probe);
        view.setResizeMode(QQuickView::SizeViewToRootObject);

        // Parented to the engine: QQuickView keeps the raw pointer for status().
        auto *component = new QQmlComponent(view.engine(), view.engine());
        component->setData(harnessQml(commandsJs),
                           QUrl(QStringLiteral("qrc:/chtest/PaletteHarness.qml")));
        m_componentError = component->errorString();
        QObject *root = component->create(view.rootContext());
        if (root)
            view.setContent(QUrl(QStringLiteral("qrc:/chtest/PaletteHarness.qml")), component, root);

        harness = root;
        if (harness)
            palette = harness->findChild<QObject *>(QStringLiteral("palette"));
    }

    QString componentError() const { return m_componentError; }

    // Shows and ACTIVATES the window; shortcut delivery depends on activation.
    bool activate(QString *why)
    {
        view.show();
        if (!QTest::qWaitForWindowExposed(&view)) {
            *why = QStringLiteral("window was never exposed");
            return false;
        }
        view.requestActivate();
        if (!QTest::qWaitForWindowActive(&view)) {
            *why = QStringLiteral("window never became active");
            return false;
        }
        return true;
    }

    QObject *child(const QString &name) const
    {
        return palette ? palette->findChild<QObject *>(name) : nullptr;
    }

    bool opened() const { return palette && palette->property("opened").toBool(); }
    int highlighted() const { return palette ? palette->property("highlightedIndex").toInt() : -2; }

    QStringList matchTitles() const
    {
        QVariant result;
        if (!harness
            || !QMetaObject::invokeMethod(harness, "matchTitles", Q_RETURN_ARG(QVariant, result)))
            return QStringList{QStringLiteral("<invokeMethod failed>")};
        return result.toStringList();
    }

    void call(const char *method) { QMetaObject::invokeMethod(palette, method); }

    QString warningReport() const
    {
        QString report = QStringLiteral("%1 QML warning(s):").arg(warnings.size());
        for (const QString &warning : warnings)
            report += QStringLiteral("\n  * ") + warning;
        return report;
    }

private:
    QString m_componentError;
};

// QTest::keyClicks() is QWidget-only; windows get one keyClick per character.
void typeText(QQuickView &view, const QString &text)
{
    for (const QChar &character : text)
        QTest::keyClick(&view, character.toLatin1());
}

void settle(int ms = 60)
{
    QTest::qWait(ms);
    QCoreApplication::processEvents();
}

} // namespace

class TstPalette : public QObject
{
    Q_OBJECT

private slots:
    void harnessLoadsPalette();
    void openFocusesFilterAndListsEveryCommand();
    void typingNarrowsResults();
    void prefixMatchOutranksWordAndMidWordMatches();
    void subsequenceMatchesRankLast();
    void arrowKeysAndEnterInvokeTheHighlightedCommandOnce();
    void clickInvokesTheClickedCommand();
    void escapeClosesWithoutInvoking();
    void perCommandShortcutFiresWhileClosed();
    void activationShortcutOpensPalette();
    void noMatchIsSafeAndEnterDoesNothing();
    void emptyCommandListIsSafe();
};

void TstPalette::harnessLoadsPalette()
{
    Fixture fixture;
    QVERIFY2(fixture.harness != nullptr, qPrintable(fixture.componentError()));
    QVERIFY2(fixture.palette != nullptr, "CommandPalette.qml did not load from qrc:/chtest");
    QVERIFY(!fixture.opened()); // starts closed
    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

void TstPalette::openFocusesFilterAndListsEveryCommand()
{
    Fixture fixture;
    QString why;
    QVERIFY2(fixture.activate(&why), qPrintable(why));

    QVERIFY(!fixture.opened());
    fixture.call("open");
    settle();

    QVERIFY(fixture.opened());

    QObject *field = fixture.child(QStringLiteral("filterField"));
    QVERIFY2(field != nullptr, "filter field not found");
    QVERIFY2(field->property("activeFocus").toBool(), "open() did not focus the filter field");
    QCOMPARE(field->property("text").toString(), QString());

    // No query: every command, in the host's declared order.
    QCOMPARE(fixture.matchTitles().size(), 6);
    QCOMPARE(fixture.matchTitles().constFirst(), QStringLiteral("Open File"));
    QCOMPARE(fixture.highlighted(), 0);
    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

void TstPalette::typingNarrowsResults()
{
    Fixture fixture;
    QString why;
    QVERIFY2(fixture.activate(&why), qPrintable(why));

    fixture.call("open");
    settle();
    QCOMPARE(fixture.matchTitles().size(), 6);

    typeText(fixture.view, QStringLiteral("terminal"));
    settle();
    QCOMPARE(fixture.matchTitles(), QStringList{QStringLiteral("New Terminal")});
    QCOMPARE(fixture.highlighted(), 0);

    // open() must clear the previous query.
    fixture.call("close");
    settle();
    fixture.call("open");
    settle();
    QCOMPARE(fixture.child(QStringLiteral("filterField"))->property("text").toString(), QString());
    QCOMPARE(fixture.matchTitles().size(), 6);
    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

void TstPalette::prefixMatchOutranksWordAndMidWordMatches()
{
    Fixture fixture;
    QString why;
    QVERIFY2(fixture.activate(&why), qPrintable(why));

    fixture.call("open");
    settle();
    typeText(fixture.view, QStringLiteral("save"));
    settle();

    // rank 1 title prefixes first (shorter title breaking the tie), then the
    // rank 2 word prefix, then the rank 3 mid-word hit.
    const QStringList expected{QStringLiteral("Save File"), QStringLiteral("Save All Files"),
                               QStringLiteral("Project Save Hook"),
                               QStringLiteral("Go To Unsaved Buffer")};
    QCOMPARE(fixture.matchTitles(), expected);
    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

void TstPalette::subsequenceMatchesRankLast()
{
    Fixture fixture;
    QString why;
    QVERIFY2(fixture.activate(&why), qPrintable(why));

    fixture.call("open");
    settle();
    // "svf" is a substring of no title; only the fuzzy subsequence fallback can
    // match it. Three titles contain s..v..f in order; all score rank 4, so the
    // tie breaks on first-hit position (0, 0, 8) and then on title length.
    typeText(fixture.view, QStringLiteral("svf"));
    settle();

    const QStringList expected{QStringLiteral("Save File"), QStringLiteral("Save All Files"),
                               QStringLiteral("Go To Unsaved Buffer")};
    QCOMPARE(fixture.matchTitles(), expected);

    // A word prefix must still beat that fuzzy match: "fil" hits "Open File"
    // and "Save File" at a word boundary before "Save All Files" mid-word.
    for (int i = 0; i < 3; ++i)
        QTest::keyClick(&fixture.view, Qt::Key_Backspace);
    typeText(fixture.view, QStringLiteral("fil"));
    settle();
    const QStringList byWord{QStringLiteral("Open File"), QStringLiteral("Save File"),
                             QStringLiteral("Save All Files")};
    QCOMPARE(fixture.matchTitles(), byWord);
    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

void TstPalette::arrowKeysAndEnterInvokeTheHighlightedCommandOnce()
{
    Fixture fixture;
    QString why;
    QVERIFY2(fixture.activate(&why), qPrintable(why));

    fixture.call("open");
    settle();
    typeText(fixture.view, QStringLiteral("save"));
    settle();
    QCOMPARE(fixture.highlighted(), 0);

    QTest::keyClick(&fixture.view, Qt::Key_Down);
    QTest::keyClick(&fixture.view, Qt::Key_Down);
    settle();
    QCOMPARE(fixture.highlighted(), 2); // "Project Save Hook"

    QTest::keyClick(&fixture.view, Qt::Key_Up);
    settle();
    QCOMPARE(fixture.highlighted(), 1); // "Save All Files"

    QSignalSpy invoked(fixture.palette, SIGNAL(commandInvoked(QString)));
    QTest::keyClick(&fixture.view, Qt::Key_Return);
    settle();

    QCOMPARE(fixture.probe.total(), 1);
    QCOMPARE(fixture.probe.count(QStringLiteral("file.saveAll")), 1);
    QCOMPARE(invoked.size(), 1);
    QCOMPARE(invoked.constFirst().constFirst().toString(), QStringLiteral("file.saveAll"));
    QVERIFY2(!fixture.opened(), "Enter must close the palette");
    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

void TstPalette::clickInvokesTheClickedCommand()
{
    Fixture fixture;
    QString why;
    QVERIFY2(fixture.activate(&why), qPrintable(why));

    fixture.call("open");
    settle();
    typeText(fixture.view, QStringLiteral("save"));
    settle();

    auto *list = qobject_cast<QQuickItem *>(fixture.child(QStringLiteral("resultsList")));
    QVERIFY2(list != nullptr, "results list not found");

    QQuickItem *row = nullptr;
    QVERIFY(QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row),
                                      Q_ARG(int, 2)));
    QVERIFY2(row != nullptr, "third result row was not realised");

    const QPoint center =
            row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint();
    QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier, center);
    settle();

    QCOMPARE(fixture.probe.total(), 1);
    QCOMPARE(fixture.probe.count(QStringLiteral("proj.hook")), 1);
    QVERIFY(!fixture.opened());
    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

void TstPalette::escapeClosesWithoutInvoking()
{
    Fixture fixture;
    QString why;
    QVERIFY2(fixture.activate(&why), qPrintable(why));

    fixture.call("open");
    settle();
    typeText(fixture.view, QStringLiteral("save"));
    settle();
    QVERIFY(fixture.opened());

    QTest::keyClick(&fixture.view, Qt::Key_Escape);
    settle();

    QVERIFY2(!fixture.opened(), "Esc must close the palette");
    QCOMPARE(fixture.probe.total(), 0);
    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

void TstPalette::perCommandShortcutFiresWhileClosed()
{
    Fixture fixture;
    QString why;
    QVERIFY2(fixture.activate(&why), qPrintable(why));

    QVERIFY2(!fixture.opened(), "precondition: the palette must be CLOSED");

    QTest::keyClick(&fixture.view, Qt::Key_T, Qt::ControlModifier | Qt::AltModifier);
    settle();
    QCOMPARE(fixture.probe.count(QStringLiteral("term.new")), 1);

    QTest::keyClick(&fixture.view, Qt::Key_O, Qt::ControlModifier | Qt::AltModifier);
    settle();
    QCOMPARE(fixture.probe.count(QStringLiteral("file.open")), 1);
    QCOMPARE(fixture.probe.total(), 2);

    // Firing a command from its shortcut must not have opened anything.
    QVERIFY(!fixture.opened());

    // ...and while the palette IS open it owns the keyboard: the same chord
    // must not reach the command behind the overlay.
    fixture.call("open");
    settle();
    QVERIFY(fixture.opened());
    QTest::keyClick(&fixture.view, Qt::Key_T, Qt::ControlModifier | Qt::AltModifier);
    settle();
    QCOMPARE(fixture.probe.count(QStringLiteral("term.new")), 1);
    QCOMPARE(fixture.probe.total(), 2);
    QVERIFY2(fixture.opened(), "a swallowed shortcut must not close the palette");

    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

void TstPalette::activationShortcutOpensPalette()
{
    Fixture fixture;
    QString why;
    QVERIFY2(fixture.activate(&why), qPrintable(why));

    QTest::keyClick(&fixture.view, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
    settle();

    QVERIFY2(fixture.opened(), "Ctrl+Shift+P did not open the palette");
    QVERIFY(fixture.child(QStringLiteral("filterField"))->property("activeFocus").toBool());
    QCOMPARE(fixture.probe.total(), 0);
    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

void TstPalette::noMatchIsSafeAndEnterDoesNothing()
{
    Fixture fixture;
    QString why;
    QVERIFY2(fixture.activate(&why), qPrintable(why));

    fixture.call("open");
    settle();
    typeText(fixture.view, QStringLiteral("zzqq"));
    settle();

    QVERIFY(fixture.matchTitles().isEmpty());
    QCOMPARE(fixture.highlighted(), -1);

    QObject *empty = fixture.child(QStringLiteral("emptyLabel"));
    QVERIFY2(empty != nullptr, "empty-state label not found");
    QVERIFY2(empty->property("visible").toBool(), "'no matches' message is not visible");
    QCOMPARE(empty->property("text").toString(), QStringLiteral("No matching commands"));

    QTest::keyClick(&fixture.view, Qt::Key_Return);
    QTest::keyClick(&fixture.view, Qt::Key_Down);
    QTest::keyClick(&fixture.view, Qt::Key_Up);
    QTest::keyClick(&fixture.view, Qt::Key_Return);
    settle();

    QCOMPARE(fixture.probe.total(), 0);
    QVERIFY2(fixture.opened(), "a fruitless Enter must leave the palette open");
    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

void TstPalette::emptyCommandListIsSafe()
{
    Fixture fixture(QByteArray("[]"));
    QString why;
    QVERIFY2(fixture.activate(&why), qPrintable(why));

    fixture.call("open");
    settle();

    QVERIFY(fixture.opened());
    QVERIFY(fixture.matchTitles().isEmpty());
    QCOMPARE(fixture.highlighted(), -1);

    QObject *empty = fixture.child(QStringLiteral("emptyLabel"));
    QVERIFY(empty != nullptr);
    QCOMPARE(empty->property("text").toString(), QStringLiteral("No commands available"));

    QTest::keyClick(&fixture.view, Qt::Key_Down);
    QTest::keyClick(&fixture.view, Qt::Key_Return);
    settle();
    QCOMPARE(fixture.probe.total(), 0);
    QVERIFY(fixture.opened());

    QTest::keyClick(&fixture.view, Qt::Key_Escape);
    settle();
    QVERIFY(!fixture.opened());
    QVERIFY2(fixture.warnings.isEmpty(), qPrintable(fixture.warningReport()));
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("CodeHarbor"));
    QGuiApplication::setOrganizationName(QStringLiteral("CodeHarbor"));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    TstPalette testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_palette.moc"
