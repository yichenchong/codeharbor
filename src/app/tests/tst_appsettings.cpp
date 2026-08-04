#include <QSignalSpy>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <limits>

#include "AppSettings.h"

using namespace ch;

namespace {
// Every case gets its own .ini so nothing leaks between them and nothing
// touches the developer's real preferences.
QString scratchIni(const QTemporaryDir& dir, const QString& name)
{
    return dir.filePath(name + QStringLiteral(".ini"));
}
} // namespace

class TstAppSettings : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void freshStoreAnswersTheDocumentedDefaults();
    void aHandEditedFileFallsBackInsteadOfPropagating();
    void choicesRejectNamesNothingCanResolve();
    void numbersAreClampedIntoTheirRange();
    void aPixelRatioOfZeroMeansFollowTheScreen();
    void toolbarOrderDropsEmptyAndRepeatedIds();
    void writingTheSameValueIsSilent();
    void viewerDefaultsStoreAndNormalise();
    void viewerDefaultsRejectMalformedAndIncompatibleEntries();
    void genericPairsCannotEscapeTheirGroup();
    void valuesSurviveAReopen();
    void resetRestoresDefaultsAndAnnouncesWhatMoved();

private:
    QTemporaryDir m_dir;
};

void TstAppSettings::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

void TstAppSettings::freshStoreAnswersTheDocumentedDefaults()
{
    AppSettings s(scratchIni(m_dir, QStringLiteral("fresh")));
    QCOMPARE(s.theme(), QStringLiteral("dark"));
    QCOMPARE(s.groupPalette(), QStringLiteral("plain"));
    QCOMPARE(s.groupPaletteSize(), AppSettings::kDefaultPaletteSize);
    QCOMPARE(s.terminalFontSize(), AppSettings::kDefaultTerminalFontSize);
    QCOMPARE(s.terminalPixelRatio(), 0.0);
    QVERIFY(s.toolbarOrder().isEmpty());
}

// The backing file is a plain .ini a person can edit and a crash can truncate.
// A line that holds something unreadable must not reach a binding: a
// zero-point font or an empty theme name is worse than the default.
void TstAppSettings::aHandEditedFileFallsBackInsteadOfPropagating()
{
    const QString path = scratchIni(m_dir, QStringLiteral("handedited"));
    {
        QSettings raw(path, QSettings::IniFormat);
        raw.setValue(QStringLiteral("settings/appearance/terminalFontSize"),
                     QStringLiteral("big"));
        raw.setValue(QStringLiteral("settings/appearance/groupPaletteSize"),
                     QStringLiteral(""));
        raw.setValue(QStringLiteral("settings/appearance/terminalPixelRatio"),
                     QStringLiteral("crisp"));
    }
    AppSettings s(path);

    const QString nanPath = scratchIni(m_dir, QStringLiteral("nanratio"));
    {
        QSettings raw(nanPath, QSettings::IniFormat);
        raw.setValue(QStringLiteral("settings/appearance/terminalPixelRatio"),
                     std::numeric_limits<qreal>::quiet_NaN());
        raw.sync();
    }
    AppSettings nan(nanPath);
    QCOMPARE(nan.terminalPixelRatio(), 0.0);
    nan.setTerminalPixelRatio(std::numeric_limits<qreal>::quiet_NaN());
    QCOMPARE(nan.terminalPixelRatio(), 0.0);
    QCOMPARE(s.terminalFontSize(), AppSettings::kDefaultTerminalFontSize);
    QCOMPARE(s.groupPaletteSize(), AppSettings::kDefaultPaletteSize);
    QCOMPARE(s.terminalPixelRatio(), 0.0);
}

// A name outside the known set is either a typo or a preference written by a
// NEWER build. Both read back as the default, and neither can be written.
void TstAppSettings::choicesRejectNamesNothingCanResolve()
{
    AppSettings s(scratchIni(m_dir, QStringLiteral("choices")));
    QSignalSpy themeSpy(&s, &AppSettings::themeChanged);

    s.setTheme(QStringLiteral("solarized"));
    QCOMPARE(s.theme(), QStringLiteral("dark"));
    QCOMPARE(themeSpy.count(), 0);

    s.setTheme(QStringLiteral("light"));
    QCOMPARE(s.theme(), QStringLiteral("light"));
    QCOMPARE(themeSpy.count(), 1);

    s.setGroupPalette(QStringLiteral("no-such-palette"));
    QCOMPARE(s.groupPalette(), QStringLiteral("plain"));
    s.setGroupPalette(QStringLiteral("tokyonight"));
    QCOMPARE(s.groupPalette(), QStringLiteral("tokyonight"));
}

void TstAppSettings::numbersAreClampedIntoTheirRange()
{
    AppSettings s(scratchIni(m_dir, QStringLiteral("clamped")));

    s.setTerminalFontSize(1000);
    QCOMPARE(s.terminalFontSize(), AppSettings::kMaxTerminalFontSize);
    s.setTerminalFontSize(-4);
    QCOMPARE(s.terminalFontSize(), AppSettings::kMinTerminalFontSize);

    // The palette generator only ever ADDS to its seed, so a size below the
    // seed count is not a smaller palette - it is a request the generator
    // cannot answer.
    s.setGroupPaletteSize(2);
    QCOMPARE(s.groupPaletteSize(), AppSettings::kMinPaletteSize);
    s.setGroupPaletteSize(500);
    QCOMPARE(s.groupPaletteSize(), AppSettings::kMaxPaletteSize);
}

// 0 is not a ratio, it is the absence of an override, and it has to be
// reachable again after one has been set.
void TstAppSettings::aPixelRatioOfZeroMeansFollowTheScreen()
{
    AppSettings s(scratchIni(m_dir, QStringLiteral("ratio")));
    QSignalSpy spy(&s, &AppSettings::terminalPixelRatioChanged);

    s.setTerminalPixelRatio(2.0);
    QCOMPARE(s.terminalPixelRatio(), 2.0);
    QCOMPARE(spy.count(), 1);

    s.setTerminalPixelRatio(0.0);
    QCOMPARE(s.terminalPixelRatio(), 0.0);
    QCOMPARE(spy.count(), 2);

    // Out of range in the other direction is still an override, clamped.
    s.setTerminalPixelRatio(99.0);
    QCOMPARE(s.terminalPixelRatio(), AppSettings::kMaxTerminalPixelRatio);
}

// An id appearing twice would put one button in two places; an empty id names
// nothing at all.
void TstAppSettings::toolbarOrderDropsEmptyAndRepeatedIds()
{
    AppSettings s(scratchIni(m_dir, QStringLiteral("toolbar")));
    s.setToolbarOrder({QStringLiteral("back"), QStringLiteral(""),
                       QStringLiteral("forward"), QStringLiteral("back"),
                       QStringLiteral("  reload  ")});
    QCOMPARE(s.toolbarOrder(),
             (QStringList{QStringLiteral("back"), QStringLiteral("forward"),
                          QStringLiteral("reload")}));
}

// Bindings re-evaluate on a signal, so a write that changes nothing must not
// emit one - a settings pane that re-reads on every keystroke would otherwise
// churn the whole appearance of the window.
void TstAppSettings::writingTheSameValueIsSilent()
{
    AppSettings s(scratchIni(m_dir, QStringLiteral("silent")));
    s.setTerminalFontSize(20);
    QSignalSpy fontSpy(&s, &AppSettings::terminalFontSizeChanged);
    s.setTerminalFontSize(20);
    QCOMPARE(fontSpy.count(), 0);

    // Also true through the clamp: 1000 and 900 both land on the maximum, and
    // the second one moved nothing.
    s.setTerminalFontSize(1000);
    QCOMPARE(fontSpy.count(), 1);
    s.setTerminalFontSize(900);
    QCOMPARE(fontSpy.count(), 1);
}

void TstAppSettings::viewerDefaultsStoreAndNormalise()
{
    const QString path = scratchIni(m_dir, QStringLiteral("viewerdefaults"));
    AppSettings s(path);
    QSignalSpy changed(&s, &AppSettings::viewerDefaultsChanged);

    QVERIFY(s.setViewerDefault(QStringLiteral(".MD"), QStringLiteral("text")));
    QVERIFY(s.setViewerDefault(QStringLiteral("JSON"), QStringLiteral("binary")));
    QCOMPARE(s.viewerDefaults(),
             (QVariantMap{{QStringLiteral("json"), QStringLiteral("binary")},
                          {QStringLiteral("md"), QStringLiteral("text")}}));
    QCOMPARE(changed.count(), 2);

    AppSettings reopened(path);
    QCOMPARE(reopened.viewerDefaults(), s.viewerDefaults());
    QSettings raw(path, QSettings::IniFormat);
    QCOMPARE(raw.value(QStringLiteral("settings/viewerDefaults/md")).toString(),
             QStringLiteral("text"));
}

void TstAppSettings::viewerDefaultsRejectMalformedAndIncompatibleEntries()
{
    const QString path = scratchIni(m_dir, QStringLiteral("viewerinvalid"));
    AppSettings s(path);

    QVERIFY(!s.setViewerDefault(QStringLiteral("png"),
                                QStringLiteral("markdown")));
    QVERIFY(!s.setViewerDefault(QStringLiteral("pdf"),
                                QStringLiteral("image")));
    QVERIFY(!s.setViewerDefault(QStringLiteral("md"),
                                QStringLiteral("no-such-kind")));
    QVERIFY(s.viewerDefaults().isEmpty());

    // The setter accepts a dot for typed input, but every malformed value is
    // dropped before it reaches the plain-text store. A well-formed extension
    // unknown to the registry remains useful for the generic text/binary
    // handlers.
    QVariantMap mixed;
    mixed.insert(QStringLiteral(""), QStringLiteral("text"));
    mixed.insert(QStringLiteral(".md"), QStringLiteral("text"));
    mixed.insert(QStringLiteral("has-dash"), QStringLiteral("text"));
    mixed.insert(QString(65, QLatin1Char('x')), QStringLiteral("text"));
    mixed.insert(QStringLiteral("png"), QStringLiteral("pdf"));
    mixed.insert(QStringLiteral("future"), QStringLiteral("text"));
    mixed.insert(QStringLiteral("json"), 7);
    s.setViewerDefaults(mixed);
    QCOMPARE(s.viewerDefaults(),
             (QVariantMap{{QStringLiteral("future"), QStringLiteral("text")},
                          {QStringLiteral("md"), QStringLiteral("text")}}));

    // Hand-edited leading-dot keys and wrong value types are rejected on read,
    // and do not disturb the valid entry beside them.
    QSettings raw(path, QSettings::IniFormat);
    raw.setValue(QStringLiteral("settings/viewerDefaults/.md"),
                 QStringLiteral("binary"));
    raw.setValue(QStringLiteral("settings/viewerDefaults/also"),
                 QStringLiteral("nonsense"));
    raw.setValue(QStringLiteral("settings/viewerDefaults/wrong"), 42);
    raw.sync();
    AppSettings reread(path);
    QCOMPARE(reread.viewerDefaults(),
             (QVariantMap{{QStringLiteral("future"), QStringLiteral("text")},
                          {QStringLiteral("md"), QStringLiteral("text")}}));
}

// '/' is the separator that keeps a group's keys inside its own subtree. A pair
// carrying one could be spelt to land on a validated appearance key and write
// it without passing that key's validation.
void TstAppSettings::genericPairsCannotEscapeTheirGroup()
{
    const QString path = scratchIni(m_dir, QStringLiteral("generic"));
    AppSettings s(path);
    QSignalSpy spy(&s, &AppSettings::settingChanged);

    s.setValue(QStringLiteral("appearance/theme"), QStringLiteral("x"),
               QStringLiteral("nonsense"));
    s.setValue(QStringLiteral("appearance"), QStringLiteral("../theme"),
               QStringLiteral("solarized"));
    s.setValue(QString(), QStringLiteral("theme"), QStringLiteral("solarized"));
    QCOMPARE(spy.count(), 0);
    QCOMPARE(s.theme(), QStringLiteral("dark"));

    s.setValue(QStringLiteral("tmux"), QStringLiteral("prefix"),
               QStringLiteral("C-a"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(s.value(QStringLiteral("tmux"), QStringLiteral("prefix"))
                 .toString(),
             QStringLiteral("C-a"));

    // A rejected read answers the caller's fallback rather than an empty
    // QVariant, which a binding would render as a blank field.
    QCOMPARE(s.value(QString(), QStringLiteral("prefix"),
                     QStringLiteral("fallback"))
                 .toString(),
             QStringLiteral("fallback"));
}

void TstAppSettings::valuesSurviveAReopen()
{
    const QString path = scratchIni(m_dir, QStringLiteral("reopen"));
    {
        AppSettings s(path);
        s.setTheme(QStringLiteral("light"));
        s.setGroupPalette(QStringLiteral("tokyonight"));
        s.setGroupPaletteSize(12);
        s.setTerminalFontSize(17);
        s.setToolbarOrder({QStringLiteral("home"), QStringLiteral("back")});
        s.setValue(QStringLiteral("server"), QStringLiteral("keepAlive"), 30);
    }
    AppSettings s(path);
    QCOMPARE(s.theme(), QStringLiteral("light"));
    QCOMPARE(s.groupPalette(), QStringLiteral("tokyonight"));
    QCOMPARE(s.groupPaletteSize(), 12);
    QCOMPARE(s.terminalFontSize(), 17);
    QCOMPARE(s.toolbarOrder(),
             (QStringList{QStringLiteral("home"), QStringLiteral("back")}));
    QCOMPARE(s.value(QStringLiteral("server"), QStringLiteral("keepAlive"))
                 .toInt(),
             30);
}

void TstAppSettings::resetRestoresDefaultsAndAnnouncesWhatMoved()
{
    const QString path = scratchIni(m_dir, QStringLiteral("reset"));
    AppSettings s(path);
    s.setTheme(QStringLiteral("light"));
    s.setTerminalFontSize(21);
    QVERIFY(s.setViewerDefault(QStringLiteral("md"), QStringLiteral("text")));
    s.setValue(QStringLiteral("tmux"), QStringLiteral("prefix"),
               QStringLiteral("C-a"));

    QSignalSpy themeSpy(&s, &AppSettings::themeChanged);
    QSignalSpy fontSpy(&s, &AppSettings::terminalFontSizeChanged);
    QSignalSpy paletteSpy(&s, &AppSettings::groupPaletteChanged);
    QSignalSpy viewerSpy(&s, &AppSettings::viewerDefaultsChanged);
    QSignalSpy settingSpy(&s, &AppSettings::settingChanged);
    s.resetToDefaults();

    QCOMPARE(s.theme(), QStringLiteral("dark"));
    QCOMPARE(s.terminalFontSize(), AppSettings::kDefaultTerminalFontSize);
    QVERIFY(s.viewerDefaults().isEmpty());
    QVERIFY(s.value(QStringLiteral("tmux"), QStringLiteral("prefix"))
                .toString()
                .isEmpty());
    QCOMPARE(themeSpy.count(), 1);
    QCOMPARE(fontSpy.count(), 1);
    QCOMPARE(viewerSpy.count(), 1);
    QCOMPARE(settingSpy.count(), 1);
    // The palette was never changed, so nothing moved and nothing is announced.
    QCOMPARE(paletteSpy.count(), 0);
    // UiStateStore shares this file. Its keys live outside `settings/` and a
    // preferences reset must not take the user's window layout with it.
    {
        QSettings raw(path, QSettings::IniFormat);
        raw.setValue(QStringLiteral("layout/sidebarWidth"), 321);
    }
    AppSettings again(path);
    QSignalSpy noGeneric(&again, &AppSettings::settingChanged);
    again.resetToDefaults();
    QCOMPARE(noGeneric.count(), 0);
    QSettings raw(path, QSettings::IniFormat);
    QCOMPARE(raw.value(QStringLiteral("layout/sidebarWidth")).toInt(), 321);
}

QTEST_GUILESS_MAIN(TstAppSettings)
#include "tst_appsettings.moc"
