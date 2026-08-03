// Command palette + global keyboard shortcuts (SPEC 15), plus the colour-
// vocabulary gate for the whole QML module (SPEC 4.1).
//
// Drives the REAL CommandPalette.qml, loaded from the CodeHarbor QML module this
// target links (qrc:/qt/qml/CodeHarbor/CommandPalette.qml). It used to be
// embedded in this test's own qrc instead, deliberately, so the test did not
// depend on the module registration — but a component in this module now reaches
// the `Theme` singleton for its colours, and a singleton only exists where the
// module is registered. Loading it out of a bare qrc path left `Theme` undefined,
// which in QML is a blank colour and not an error. A tiny inline harness document
// hosts the palette the way Main.qml does (one Loader, one `commands`
// assignment), and every command's `invoke` is a plain JS closure that bumps a
// counter on an injected C++ probe — the documented {id,title,shortcut,invoke}
// shape.
//
// The second half of the file is not about the palette at all. It holds the two
// checks that need nothing but a plain QQmlEngine with the module registered and
// no window to set up, which is the one thing this file already has:
//
//   * the COLOUR gate — every `color` role is read out of the live Theme
//     singleton, and no QML file in src/qml may then spell one of those colours
//     out as a hex literal. That is what stops the module drifting back into two
//     hundred copies of the same eight shades.
//   * the REMOTE PATH gate — RemotePath.js converts between a remote POSIX path
//     and the file:// URL the viewer stack passes around, in BOTH directions, and
//     the two directions have to be exact inverses. They are the module's only
//     copy of that rule, and getting it wrong reads the wrong remote file.
//
// Everything runs headless (offscreen QPA + software Quick backend, pinned by
// the ctest registration). Key events are posted to the QQuickView itself, so
// both the focus path (filter field) and the shortcut path (QShortcutMap, which
// needs an ACTIVE window) are the real ones the application uses.

#include <QtTest>

#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QList>
#include <QMap>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaType>
#include <QPoint>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickView>
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QtQuickControls2/QQuickStyle>

#include <memory>

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

class ThemeSettingsProbe : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)

public:
    QString theme() const { return m_theme; }
    void setTheme(const QString &theme)
    {
        if (m_theme == theme)
            return;
        m_theme = theme;
        emit themeChanged();
    }

signals:
    void themeChanged();

private:
    QString m_theme = QStringLiteral("dark");
};

class ThemeAppProbe : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *settings READ settings CONSTANT)

public:
    explicit ThemeAppProbe(ThemeSettingsProbe *settings)
        : m_settings(settings)
    {
    }

    QObject *settings() const { return m_settings; }

private:
    ThemeSettingsProbe *m_settings = nullptr;
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
        // The shipped module copy, so the palette's `Theme` colours resolve.
        source: "qrc:/qt/qml/CodeHarbor/CommandPalette.qml"
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

// ---------------------------------------------------------------------------
// Theme vocabulary (SPEC 4.1)
// ---------------------------------------------------------------------------

// Every `color` role the live Theme singleton publishes, role name -> value.
// Empty (with `why` filled in) when the singleton does not resolve, which is the
// failure mode a broken QML singleton registration produces: `Theme.surface`
// silently evaluates to undefined instead of raising anything.
QMap<QString, QColor> themeColours(QString *why)
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import CodeHarbor\n"
                                        "QtObject { readonly property var theme: Theme }\n"),
                      QUrl(QStringLiteral("qrc:/chtest/ThemeProbe.qml")));
    const std::unique_ptr<QObject> probe(component.create());
    if (!probe) {
        *why = QStringLiteral("the Theme probe document did not load: ") + component.errorString();
        return {};
    }
    QObject *theme = probe->property("theme").value<QObject *>();
    if (!theme) {
        *why = QStringLiteral("`Theme` did not resolve to an object, so the QML singleton "
                              "registration in src/qml/CMakeLists.txt is broken");
        return {};
    }

    QMap<QString, QColor> roles;
    const QMetaObject *mo = theme->metaObject();
    for (int i = 0; i < mo->propertyCount(); ++i) {
        const QMetaProperty property = mo->property(i);
        if (property.metaType().id() != QMetaType::QColor)
            continue;
        roles.insert(QString::fromLatin1(property.name()),
                     property.read(theme).value<QColor>());
    }
    return roles;
}

struct HexHit {
    QString file;
    int line = 0;
    QString literal;
    QString role;
};

// Is this line nothing but a comment? Hex literals are quoted and discussed in
// the module's comments (Theme.qml names the palette it came from), and a comment
// is documentation rather than a colour anybody paints with.
bool isCommentLine(const QString &line)
{
    const QString trimmed = line.trimmed();
    return trimmed.startsWith(QLatin1String("//")) || trimmed.startsWith(QLatin1Char('*'))
           || trimmed.startsWith(QLatin1String("/*"));
}

// Every hex literal in src/qml that spells out a colour Theme already has a name
// for. Theme.qml itself is where those literals are DEFINED, so it is skipped.
QList<HexHit> themeColoursWrittenAsHex(const QMap<QString, QColor> &roles, QString *why)
{
    static const QRegularExpression hexLiteral(QStringLiteral("#[0-9A-Fa-f]{3,8}"));

    QDir dir(QStringLiteral(CODEHARBOR_QML_SOURCE_DIR));
    const QStringList files =
            dir.entryList(QStringList{QStringLiteral("*.qml")}, QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        *why = QStringLiteral("no .qml files found under ") + dir.absolutePath();
        return {};
    }

    QList<HexHit> hits;
    for (const QString &name : files) {
        if (name == QStringLiteral("Theme.qml"))
            continue;
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            *why = QStringLiteral("could not read ") + file.fileName();
            return {};
        }
        const QStringList lines =
                QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if (isCommentLine(lines.at(i)))
                continue;
            QRegularExpressionMatchIterator matches = hexLiteral.globalMatch(lines.at(i));
            while (matches.hasNext()) {
                const QString literal = matches.next().captured();
                const QColor colour(literal);
                if (!colour.isValid())
                    continue;
                // Several roles can legitimately share one shade (a selected row
                // and a raised control are the same grey), so the message names
                // every role that would do rather than picking one arbitrarily.
                QStringList named;
                for (auto role = roles.cbegin(); role != roles.cend(); ++role) {
                    if (role.value() == colour)
                        named.append(role.key());
                }
                if (!named.isEmpty())
                    hits.append({name, i + 1, literal, named.join(QStringLiteral(" / "))});
            }
        }
    }
    return hits;
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

    // The module's colour vocabulary. Both of these fail for the same underlying
    // reason a mis-registered singleton would, which is why they live together.
    void themeSingletonPublishesEveryColourRole();
    void themeSwitchChangesRole();
    void noQmlFileSpellsOutAThemeColour();

    // RemotePath.js: the module's one conversion between a remote POSIX path and
    // a file:// URL, in both directions.
    void remotePathAndFileUrlAreExactInverses();
    void remotePathLeavesANonFileAddressAlone();
};

void TstPalette::harnessLoadsPalette()
{
    Fixture fixture;
    QVERIFY2(fixture.harness != nullptr, qPrintable(fixture.componentError()));
    QVERIFY2(fixture.palette != nullptr,
             "CommandPalette.qml did not load from the CodeHarbor module");
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

// The registration check the rest of the module depends on. A QML singleton that
// is declared `pragma Singleton` but not marked QT_QML_SINGLETON_TYPE (or the
// other way round) still lets `import CodeHarbor` succeed, and every `Theme.x`
// then evaluates to undefined — a blank colour, no warning, no error. Nothing
// else in the suite can tell that apart from a working theme.
void TstPalette::themeSingletonPublishesEveryColourRole()
{
    QString why;
    const QMap<QString, QColor> roles = themeColours(&why);
    QVERIFY2(!roles.isEmpty(), qPrintable(why));

    // The roles every file in the module binds to. A rename here without a rename
    // there is a silently blank colour, so the names are pinned.
    const QStringList required{QStringLiteral("surface"),      QStringLiteral("surfaceDeep"),
                               QStringLiteral("surfaceSunken"), QStringLiteral("surfaceRaised"),
                               QStringLiteral("surfaceHover"), QStringLiteral("surfaceSelected"),
                               QStringLiteral("border"),       QStringLiteral("borderSubtle"),
                               QStringLiteral("text"),         QStringLiteral("textDim"),
                               QStringLiteral("textOnAccent"), QStringLiteral("textFaint"),
                               QStringLiteral("accent"),       QStringLiteral("success"),
                               QStringLiteral("warning"),      QStringLiteral("danger"),
                               QStringLiteral("busy")};
    for (const QString &role : required) {
        QVERIFY2(roles.contains(role),
                 qPrintable(QStringLiteral("Theme has no colour role \"%1\"; it publishes: %2")
                                    .arg(role, QStringList(roles.keys()).join(QStringLiteral(", ")))));
        QVERIFY2(roles.value(role).isValid(),
                 qPrintable(QStringLiteral("Theme.%1 is not a valid colour").arg(role)));
    }
}

void TstPalette::themeSwitchChangesRole()
{
    ThemeSettingsProbe settings;
    ThemeAppProbe appProbe(&settings);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &appProbe);

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import CodeHarbor\n"
                                        "QtObject {\n"
                                        "    property color surface: Theme.surface\n"
                                        "    property string themeName: Theme.themeName\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/chtest/ThemeSwitchProbe.qml")));
    const std::unique_ptr<QObject> probe(component.create());
    QVERIFY2(probe != nullptr, qPrintable(component.errorString()));

    const QColor dark = probe->property("surface").value<QColor>();
    QCOMPARE(dark, QColor(QStringLiteral("#1e1e2e")));
    QCOMPARE(probe->property("themeName").toString(), QStringLiteral("dark"));

    settings.setTheme(QStringLiteral("light"));
    QTRY_COMPARE(probe->property("surface").value<QColor>(),
                 QColor(QStringLiteral("#eff1f5")));
    QCOMPARE(probe->property("themeName").toString(), QStringLiteral("light"));
    QVERIFY(dark != probe->property("surface").value<QColor>());
}

// Colour drift gate. A hex literal that spells out a colour Theme already names is
// a copy of the vocabulary: it is what let the four modal dialogs and a scrollbar
// drift out of the palette unnoticed, because nothing tied them to it. A literal
// Theme has NO name for is a different thing — a colour the vocabulary is missing
// — and is deliberately left alone here rather than guessed at.
void TstPalette::noQmlFileSpellsOutAThemeColour()
{
    QString why;
    const QMap<QString, QColor> roles = themeColours(&why);
    QVERIFY2(!roles.isEmpty(), qPrintable(why));

    const QList<HexHit> hits = themeColoursWrittenAsHex(roles, &why);
    QVERIFY2(why.isEmpty(), qPrintable(why));

    QStringList report;
    for (const HexHit &hit : hits) {
        report.append(QStringLiteral("  %1:%2 writes %3 — use Theme.%4")
                              .arg(hit.file)
                              .arg(hit.line)
                              .arg(hit.literal, hit.role));
    }
    QVERIFY2(hits.isEmpty(),
             qPrintable(QStringLiteral("%1 hard-coded colour(s) that Theme already names:\n%2")
                                .arg(hits.size())
                                .arg(report.join(QLatin1Char('\n')))));
}

// ---------------------------------------------------------------------------
// RemotePath.js (SPEC 8.3)
//
// Inside CodeHarbor a file:// URL ALWAYS names a file on the remote SSH server,
// never a local one, and the remote file service speaks plain server-absolute
// POSIX paths. RemotePath.js is the module's only copy of the conversion between
// the two spellings, in both directions, and every viewer/editor surface calls
// it. If the two directions are not exact inverses, a pane silently reads a
// DIFFERENT file from the one whose name is on screen — which is why this is
// asserted on the shipped module copy rather than on a re-implementation.
// ---------------------------------------------------------------------------

namespace {

// A document that pulls in the module's own RemotePath.js and exposes both
// directions as invokable functions.
class RemotePathProbe
{
public:
    RemotePathProbe()
    {
        m_component.setData(QByteArrayLiteral(
                                "import QtQml\n"
                                // Absolute qrc path: this document is synthetic,
                                // so a relative import has no directory to
                                // resolve against.
                                "import \"qrc:/qt/qml/CodeHarbor/RemotePath.js\" as RemotePath\n"
                                "QtObject {\n"
                                "    function toPath(url) { return RemotePath.fileUrlToPath(url) }\n"
                                "    function toUrl(path) { return RemotePath.pathToFileUrl(path) }\n"
                                "}\n"),
                            QUrl(QStringLiteral("qrc:/chtest/RemotePathProbe.qml")));
        m_object.reset(m_component.create());
    }

    QObject *object() const { return m_object.get(); }
    QString error() const { return m_component.errorString(); }

    QString call(const char *method, const QString &argument) const
    {
        QVariant result;
        if (!m_object
            || !QMetaObject::invokeMethod(m_object.get(), method, Q_RETURN_ARG(QVariant, result),
                                          Q_ARG(QVariant, argument)))
            return QStringLiteral("<invokeMethod failed>");
        return result.toString();
    }

private:
    QQmlEngine m_engine;
    QQmlComponent m_component{&m_engine};
    std::unique_ptr<QObject> m_object;
};

} // namespace

void TstPalette::remotePathAndFileUrlAreExactInverses()
{
    const RemotePathProbe probe;
    QVERIFY2(probe.object() != nullptr, qPrintable(probe.error()));

    struct Case {
        const char *path;
        const char *url;
        const char *why;
    };
    // Every entry is a path that only survives if each SEGMENT is encoded on its
    // own and the "/" separators are left alone.
    const Case cases[] = {
        {"/srv/repos/app/README.md", "file:///srv/repos/app/README.md", "an ordinary file"},
        // "#" and "?" are URL delimiters: leaving either unescaped (which
        // encodeURI does) turns the rest of the name into a fragment or a query
        // and reads the wrong file.
        {"/tmp/notes#1.txt", "file:///tmp/notes%231.txt", "a fragment delimiter in a file name"},
        {"/srv/a b/c?d", "file:///srv/a%20b/c%3Fd", "a space and a query delimiter"},
        // Paths here are ALWAYS remote POSIX paths, so a backslash is an ordinary
        // character in a file name and must never be treated as a separator.
        {"/tmp/we\\ird", "file:///tmp/we%5Cird", "a backslash is not a separator"},
        // A trailing slash is what marks a directory for the handler registry, so
        // it has to survive both directions verbatim.
        {"/srv/repos/", "file:///srv/repos/", "a directory's trailing slash"},
        {"/", "file:///", "the filesystem root"},
        {"/srv/\xC3\xBC/ok", "file:///srv/%C3%BC/ok", "a non-ASCII segment"},
        // A path that already CONTAINS a percent sequence must be escaped again,
        // or decoding would hand back something the server never named.
        {"/a%20b/c", "file:///a%2520b/c", "a literal percent sequence in a path"},
    };

    for (const Case &testCase : cases) {
        const QString path = QString::fromUtf8(testCase.path);
        const QString url = QString::fromUtf8(testCase.url);
        QVERIFY2(probe.call("toUrl", path) == url,
                 qPrintable(QStringLiteral("%1: pathToFileUrl(\"%2\") gave \"%3\", expected \"%4\"")
                                .arg(QLatin1String(testCase.why), path, probe.call("toUrl", path),
                                     url)));
        QVERIFY2(probe.call("toPath", url) == path,
                 qPrintable(QStringLiteral("%1: fileUrlToPath(\"%2\") gave \"%3\", expected \"%4\"")
                                .arg(QLatin1String(testCase.why), url, probe.call("toPath", url),
                                     path)));
    }
}

// An address that is not a file:// URL is not a remote path at all — an https://
// page IS its address — and a plain path is already in the spelling the RPC layer
// wants. Both must come back untouched, because a viewer pane hands whatever it
// is showing to this function before deciding what to do with it.
void TstPalette::remotePathLeavesANonFileAddressAlone()
{
    const RemotePathProbe probe;
    QVERIFY2(probe.object() != nullptr, qPrintable(probe.error()));

    QCOMPARE(probe.call("toPath", QStringLiteral("https://example.com/docs?a=1#b")),
             QStringLiteral("https://example.com/docs?a=1#b"));
    QCOMPARE(probe.call("toPath", QStringLiteral("/srv/repos/app")),
             QStringLiteral("/srv/repos/app"));
    QCOMPARE(probe.call("toPath", QString()), QString());
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
