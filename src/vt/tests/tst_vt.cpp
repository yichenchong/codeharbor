#include <QtTest/QtTest>

#include <QByteArray>
#include <QSignalSpy>
#include <QString>

#include "VtKeys.h"
#include "VtParser.h"
#include "VtScreen.h"
#include "VtTypes.h"

using namespace ch;

namespace {

// A complete, comparable description of everything a consumer can observe about
// the screen: every cell of history and live screen with its rendition, plus the
// cursor, the modes and the title.
//
// This is what makes the byte-split gate meaningful. Comparing only the visible
// text would pass a parser that lost an SGR colour, dropped a mode, or left the
// cursor a column out when a sequence was split across writes - which is exactly
// the class of bug the state machine exists to prevent.
QString snapshot(const VtScreen &screen)
{
    QString out;
    for (int row = 0; row < screen.totalLines(); ++row) {
        const VtLine &line = screen.lineAt(row);
        for (const VtCell &cell : line) {
            out += QString::number(static_cast<uint>(cell.text), 16);
            out += u'/';
            out += QString::number(cell.width);
            out += u'/';
            out += QString::number(static_cast<uint>(cell.fg), 16);
            out += u'/';
            out += QString::number(static_cast<uint>(cell.bg), 16);
            out += u'/';
            out += QString::number(cell.attrs);
            out += u' ';
        }
        out += u'\n';
    }
    out += QStringLiteral("size=%1x%2 cursor=%3,%4 visible=%5 history=%6 alt=%7 "
                          "appkeys=%8 paste=%9 mouse=%10 wrap=%11 title=[%12]")
               .arg(screen.columns())
               .arg(screen.rows())
               .arg(screen.cursorColumn())
               .arg(screen.cursorRow())
               // Booleans are widened explicitly: QString::arg has no bool
               // overload, so passing one is ambiguous between int and uint.
               .arg(screen.cursorVisible() ? 1 : 0)
               .arg(screen.historyLines())
               .arg(screen.altScreenActive() ? 1 : 0)
               .arg(screen.applicationCursorKeys() ? 1 : 0)
               .arg(screen.bracketedPaste() ? 1 : 0)
               .arg(screen.mouseTrackingEnabled() ? 1 : 0)
               .arg(screen.autowrap() ? 1 : 0)
               .arg(screen.windowTitle());
    return out;
}

QString lineText(const VtScreen &screen, int absoluteRow)
{
    return screen.textRange(absoluteRow, 0, absoluteRow, screen.columns());
}

const VtCell &cellAt(const VtScreen &screen, int absoluteRow, int column)
{
    return screen.lineAt(absoluteRow).at(column);
}

void feedByte(VtScreen &screen, const QByteArray &bytes)
{
    for (qsizetype i = 0; i < bytes.size(); ++i)
        screen.write(bytes.mid(i, 1));
}

// CSI/CUP helper so the fixtures read as sequences rather than as escapes.
QByteArray csi(const QByteArray &body)
{
    return QByteArrayLiteral("\x1b[") + body;
}

} // namespace

class TstVt : public QObject {
    Q_OBJECT

private slots:
    void splitFeedMatchesWholeFeed_data();
    void splitFeedMatchesWholeFeed();
    void decodesUtf8SplitAcrossWrites();
    void reportsInvalidUtf8WithoutDesynchronising();
    void placesWideCharacterInTwoCells();
    void foldsCombiningMarkIntoPrecedingCell();
    void deferredWrapDoesNotScrollPrematurely();
    void autowrapOffOverwritesLastColumn();
    void honoursScrollRegionForLineFeedAndEditing();
    void scrollsUpAndDownWithinRegion();
    void scrollingPrimaryScreenFeedsHistory();
    void eraseDisplayThreeClearsHistory();
    void historyCapKeepsAbsoluteAddressingConsistent();
    void altScreenLeavesPrimaryAndHistoryIntact();
    void appliesIndexedAndTruecolourSgr();
    void appliesColonSeparatedSgr();
    void answersDeviceStatusReports();
    void setsWindowTitleFromOsc();
    void discardsUnknownOscAndDcsPayloads();
    void textRangeSpansWrappedWideCharacters();
    void tabStopsAndBackspaceMoveCursor();
    void insertsAndDeletesCharacters();
    void editingCharactersNeverOrphansWideHalves();
    void softHyphenOccupiesOneCell();
    void abortsSequencesOnCancel();
    void stripsBidiControlsFromTitle();
    void boundsParameterAndStringOverflow();
    void reportsHistoryAdvanceNotRegionScroll();
    void resizeReportsHistoryDamageAndKeepsUsableRegion();
    void nestedWriteFromReplyHandlerJoinsTheBatch();
    void resizeTruncatesWithoutReflowing();
    void resetRestoresDefaults();
    void coalescesNotificationsPerWrite();
    void encodesCursorKeysInBothModes();
    void encodesEditingAndFunctionKeys();
    void encodesControlAndAltKeys();
    void encodesPasteInBothModes();
    void reportsWheelInTheEncodingTheProgramSelected();
    void sgrModeIsIndependentOfTheModesThatAskForEvents();
};

// --- the byte-split gate ----------------------------------------------------

void TstVt::splitFeedMatchesWholeFeed_data()
{
    QTest::addColumn<QByteArray>("stream");

    QTest::newRow("plain text and wrap")
        << QByteArrayLiteral("hello world, this line is longer than the grid");
    QTest::newRow("c0 controls")
        // The string breaks matter: "\x0dc" would be parsed as the single hex
        // escape 0x0dc, not as CR followed by 'c'.
        << QByteArrayLiteral("a\tb\x08\x0d""c\ndeep\x0b\x0c""x\x0e\x0f\x07");
    QTest::newRow("cursor movement")
        << csi("5;3H") + QByteArrayLiteral("x") + csi("2A") + csi("3B") + csi("4C")
               + csi("2D") + csi("7G") + csi("2d") + QByteArrayLiteral("y") + csi("2E")
               + csi("1F") + QByteArrayLiteral("z");
    QTest::newRow("erase display and line")
        << QByteArrayLiteral("abc\ndef\nghi") + csi("2;2H") + csi("K") + csi("1K")
               + csi("0J") + csi("1J");
    QTest::newRow("insert and delete lines")
        << QByteArrayLiteral("one\ntwo\nthree\nfour") + csi("2;1H") + csi("2L")
               + csi("1M");
    QTest::newRow("scroll region and scrolling")
        << csi("2;4r") + QByteArrayLiteral("aaa\nbbb\nccc\nddd") + csi("2S") + csi("1T");
    QTest::newRow("character editing")
        << QByteArrayLiteral("abcdefgh") + csi("3G") + csi("2@") + csi("1P") + csi("3X");
    QTest::newRow("simple sgr")
        << csi("1;4;7;31;44m") + QByteArrayLiteral("A") + csi("22;24;27m")
               + QByteArrayLiteral("B") + csi("0m") + QByteArrayLiteral("C")
               + csi("2;3;5;9;90;107m") + QByteArrayLiteral("D");
    QTest::newRow("indexed sgr")
        << csi("38;5;196;48;5;21;1m") + QByteArrayLiteral("A");
    QTest::newRow("truecolour sgr")
        << csi("38;2;10;20;30;48;2;200;100;50m") + QByteArrayLiteral("A");
    QTest::newRow("colon sgr")
        << csi("38:2::10:20:30m") + QByteArrayLiteral("A") + csi("48:5:99m")
               + QByteArrayLiteral("B") + csi("4:3m") + QByteArrayLiteral("C");
    QTest::newRow("private modes")
        << csi("?1h") + csi("?7l") + csi("?25l") + csi("?2004h") + csi("?1000h")
               + csi("?1006h") + csi("?1002h") + csi("?1000l");
    QTest::newRow("alt screen round trip")
        << QByteArrayLiteral("primary\n") + csi("?1049h") + QByteArrayLiteral("alt")
               + csi("?1049l") + QByteArrayLiteral("!");
    QTest::newRow("save and restore cursor")
        << QByteArrayLiteral("\x1b[3;3H\x1b""7") + csi("31m") + QByteArrayLiteral("x")
               + csi("1;1H") + QByteArrayLiteral("\x1b""8y");
    QTest::newRow("alignment test and reset")
        // "\x1bc" would be the single out-of-range escape 0x1bc, not ESC then 'c'.
        << QByteArrayLiteral("\x1b#8") + csi("2;2H") + QByteArrayLiteral("z\x1b""c")
               + QByteArrayLiteral("after");
    QTest::newRow("soft reset")
        << csi("?1h") + csi("2;3r") + csi("31m") + csi("!p") + QByteArrayLiteral("x");
    QTest::newRow("osc title with bel")
        << QByteArrayLiteral("\x1b]0;a title\x07text");
    QTest::newRow("osc title with st")
        << QByteArrayLiteral("\x1b]2;another\x1b\\text");
    QTest::newRow("osc palette discarded")
        << QByteArrayLiteral("\x1b]4;1;rgb:ff/00/00\x07visible");
    QTest::newRow("dcs passthrough discarded")
        << QByteArrayLiteral("\x1bP1$r0m\x1b\\shown");
    QTest::newRow("apc discarded")
        << QByteArrayLiteral("\x1b_payload;that;must;not;print\x1b\\ok");
    QTest::newRow("device status report") << csi("4;7H") + csi("6n") + csi("5n");
    QTest::newRow("utf8 and wide characters")
        << QByteArrayLiteral("caf\xc3\xa9 \xe4\xb8\xad\xe6\x96\x87 e\xcc\x81 "
                             "\xf0\x9f\x9a\x80");
    QTest::newRow("invalid utf8")
        << QByteArrayLiteral("a\xe4\x41\xc0\x80\xff\x62");
    QTest::newRow("tab stops")
        << QByteArrayLiteral("\x1b[1;1H\tA\x1bH\x1b[1;1H\t\t") + csi("2Z")
               + QByteArrayLiteral("B") + csi("3g") + QByteArrayLiteral("\t\tC");
    QTest::newRow("malformed sequences")
        << QByteArrayLiteral("a\x1b[999999999;5;;;m\x1b[?\x1b[\x1b[<0;1;1Mb")
               + csi("99999X") + QByteArrayLiteral("c");
    QTest::newRow("cancelled sequences")
        << QByteArrayLiteral("\x1b[1?\x18""A\x1b#\x18""B\x1b[1!\x18""C\x1bP$\x18""D"
                             "\x1b[1?\x1a""E");
    QTest::newRow("soft hyphen and zero width joiner")
        << QByteArrayLiteral("a\xc2\xad""b \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb");
    QTest::newRow("wide character editing")
        << QByteArrayLiteral("\xe4\xb8\xad\xe6\x96\x87") + csi("1;1H") + csi("1@")
               + csi("1;2H") + csi("2P") + QByteArrayLiteral("\xe5\xad\x97")
               + csi("1;1H") + csi("3X");
    QTest::newRow("title with bidi override")
        << QByteArrayLiteral("\x1b]2;a\xe2\x80\xae""b\x07ok");
}

void TstVt::splitFeedMatchesWholeFeed()
{
    QFETCH(QByteArray, stream);

    // A small grid so that wrapping, scrolling and history all engage on short
    // fixtures.
    VtScreen whole;
    whole.resize(12, 5);
    QByteArray wholeReplies;
    connect(&whole, &VtScreen::reply, this,
            [&wholeReplies](const QByteArray &bytes) { wholeReplies += bytes; });
    whole.write(stream);

    VtScreen split;
    split.resize(12, 5);
    QByteArray splitReplies;
    connect(&split, &VtScreen::reply, this,
            [&splitReplies](const QByteArray &bytes) { splitReplies += bytes; });
    feedByte(split, stream);

    // The whole point: a sequence cut at ANY byte boundary must produce the same
    // screen, and the same answers back to the PTY, as the same sequence arriving
    // in one piece.
    QCOMPARE(snapshot(split), snapshot(whole));
    QCOMPARE(splitReplies, wholeReplies);
}

// --- UTF-8 ------------------------------------------------------------------

void TstVt::decodesUtf8SplitAcrossWrites()
{
    VtScreen screen;
    screen.resize(10, 3);
    // U+4E2D in three writes of one byte each.
    screen.write(QByteArrayLiteral("\xe4"));
    QCOMPARE(lineText(screen, 0), QString());
    screen.write(QByteArrayLiteral("\xb8"));
    QCOMPARE(lineText(screen, 0), QString());
    screen.write(QByteArrayLiteral("\xad"));
    QCOMPARE(cellAt(screen, 0, 0).text, char32_t(0x4E2D));
    QCOMPARE(screen.cursorColumn(), 2);

    // A four-byte character split between the surrogate-free code point's bytes.
    screen.write(QByteArrayLiteral("\xf0\x9f"));
    screen.write(QByteArrayLiteral("\x9a\x80"));
    QCOMPARE(cellAt(screen, 0, 2).text, char32_t(0x1F680));
    QCOMPARE(cellAt(screen, 0, 2).width, quint8(2));
}

void TstVt::reportsInvalidUtf8WithoutDesynchronising()
{
    VtScreen screen;
    screen.resize(10, 3);
    // Truncated three-byte sequence followed by an ASCII letter: the letter must
    // survive, which is what "does not desynchronise" means.
    screen.write(QByteArrayLiteral("\xe4""A"));
    QCOMPARE(cellAt(screen, 0, 0).text, char32_t(0xFFFD));
    QCOMPARE(cellAt(screen, 0, 1).text, char32_t('A'));

    // Overlong encoding of NUL must never decode to NUL.
    VtScreen overlong;
    overlong.resize(10, 3);
    overlong.write(QByteArrayLiteral("\xc0\x80""B"));
    QCOMPARE(cellAt(overlong, 0, 0).text, char32_t(0xFFFD));
    QCOMPARE(cellAt(overlong, 0, 1).text, char32_t('B'));

    // A surrogate half encoded as UTF-8.
    VtScreen surrogate;
    surrogate.resize(10, 3);
    surrogate.write(QByteArrayLiteral("\xed\xa0\x80""C"));
    QCOMPARE(cellAt(surrogate, 0, 0).text, char32_t(0xFFFD));
    QCOMPARE(cellAt(surrogate, 0, 1).text, char32_t('C'));

    // A partial character interrupted by an escape sequence: the sequence must
    // still be recognised.
    VtScreen interrupted;
    interrupted.resize(10, 3);
    interrupted.write(QByteArrayLiteral("\xe4") + csi("31m") + QByteArrayLiteral("D"));
    QCOMPARE(cellAt(interrupted, 0, 0).text, char32_t(0xFFFD));
    QCOMPARE(cellAt(interrupted, 0, 1).text, char32_t('D'));
    QCOMPARE(cellAt(interrupted, 0, 1).fg, vt::xterm256Color(1));
}

// --- character width --------------------------------------------------------

void TstVt::placesWideCharacterInTwoCells()
{
    VtScreen screen;
    screen.resize(10, 3);
    screen.write(QByteArrayLiteral("\xe4\xb8\xad""x"));

    QCOMPARE(cellAt(screen, 0, 0).text, char32_t(0x4E2D));
    QCOMPARE(cellAt(screen, 0, 0).width, quint8(2));
    QCOMPARE(cellAt(screen, 0, 1).width, quint8(0));
    QCOMPARE(cellAt(screen, 0, 2).text, char32_t('x'));
    QCOMPARE(screen.cursorColumn(), 3);

    // Overwriting the leading half must not leave the trailing half orphaned.
    screen.write(csi("1;1H") + QByteArrayLiteral("y"));
    QCOMPARE(cellAt(screen, 0, 0).text, char32_t('y'));
    QCOMPARE(cellAt(screen, 0, 0).width, quint8(1));
    QCOMPARE(cellAt(screen, 0, 1).width, quint8(1));
    QCOMPARE(cellAt(screen, 0, 1).text, char32_t(' '));

    // And overwriting the trailing half must demote the leading one.
    VtScreen other;
    other.resize(10, 3);
    other.write(QByteArrayLiteral("\xe4\xb8\xad") + csi("2G") + QByteArrayLiteral("z"));
    QCOMPARE(cellAt(other, 0, 0).width, quint8(1));
    QCOMPARE(cellAt(other, 0, 0).text, char32_t(' '));
    QCOMPARE(cellAt(other, 0, 1).text, char32_t('z'));
}

void TstVt::foldsCombiningMarkIntoPrecedingCell()
{
    VtScreen screen;
    screen.resize(10, 3);
    // "e" then U+0301 COMBINING ACUTE ACCENT: one cell, composed, no advance.
    screen.write(QByteArrayLiteral("e\xcc\x81"));
    QCOMPARE(cellAt(screen, 0, 0).text, char32_t(0x00E9));
    QCOMPARE(cellAt(screen, 0, 0).width, quint8(1));
    QCOMPARE(screen.cursorColumn(), 1);

    // A mark with no precomposed form is dropped rather than advancing the cursor
    // or overwriting the base character.
    screen.write(QByteArrayLiteral("\xe4\xb8\xad\xcc\x81"));
    QCOMPARE(cellAt(screen, 0, 1).text, char32_t(0x4E2D));
    QCOMPARE(cellAt(screen, 0, 1).width, quint8(2));
    QCOMPARE(screen.cursorColumn(), 3);

    // A mark at the very start of a line has nothing to attach to and must not
    // move the cursor or crash.
    VtScreen bare;
    bare.resize(10, 3);
    bare.write(QByteArrayLiteral("\xcc\x81"));
    QCOMPARE(bare.cursorColumn(), 0);
    QCOMPARE(cellAt(bare, 0, 0).text, char32_t(' '));
}

// --- wrapping ---------------------------------------------------------------

void TstVt::deferredWrapDoesNotScrollPrematurely()
{
    VtScreen screen;
    screen.resize(10, 3);
    screen.write(csi("3;1H") + QByteArrayLiteral("0123456789"));

    // The last row is full, but nothing has scrolled: the wrap is still pending.
    QCOMPARE(screen.historyLines(), 0);
    QCOMPARE(screen.cursorRow(), 2);
    QCOMPARE(screen.cursorColumn(), 9);
    QCOMPARE(lineText(screen, 2), QStringLiteral("0123456789"));

    // A cursor move cancels the pending wrap: this must NOT scroll either.
    screen.write(csi("3;10H") + QByteArrayLiteral("!"));
    QCOMPARE(screen.historyLines(), 0);
    QCOMPARE(lineText(screen, 2), QStringLiteral("012345678!"));

    // Only the next printable glyph performs the wrap, and only then does the
    // screen scroll.
    screen.write(QByteArrayLiteral("A"));
    QCOMPARE(screen.historyLines(), 1);
    QCOMPARE(lineText(screen, screen.totalLines() - 1), QStringLiteral("A"));
    QCOMPARE(screen.cursorColumn(), 1);
}

void TstVt::autowrapOffOverwritesLastColumn()
{
    VtScreen screen;
    screen.resize(5, 3);
    screen.write(csi("?7l") + QByteArrayLiteral("abcdefg"));
    QCOMPARE(screen.autowrap(), false);
    QCOMPARE(screen.historyLines(), 0);
    QCOMPARE(screen.cursorRow(), 0);
    // With autowrap off the last column keeps being overwritten, so the final
    // glyph wins and nothing spills onto the next line.
    QCOMPARE(lineText(screen, 0), QStringLiteral("abcdg"));
    QCOMPARE(lineText(screen, 1), QString());
}

// --- scrolling --------------------------------------------------------------

void TstVt::honoursScrollRegionForLineFeedAndEditing()
{
    VtScreen screen;
    screen.resize(10, 5);
    screen.write(QByteArrayLiteral("A\r\nB\r\nC\r\nD\r\nE"));
    // Rows 2-4 (1-based) become the region; DECSTBM homes the cursor.
    screen.write(csi("2;4r"));
    QCOMPARE(screen.cursorRow(), 0);
    QCOMPARE(screen.cursorColumn(), 0);

    // A line feed on the last row OF THE REGION scrolls the region only, and does
    // not touch the rows outside it or the history (the region does not start at
    // the top of the screen).
    screen.write(csi("4;1H") + QByteArrayLiteral("\n"));
    QCOMPARE(screen.historyLines(), 0);
    QCOMPARE(lineText(screen, 0), QStringLiteral("A"));
    QCOMPARE(lineText(screen, 1), QStringLiteral("C"));
    QCOMPARE(lineText(screen, 2), QStringLiteral("D"));
    QCOMPARE(lineText(screen, 3), QString());
    QCOMPARE(lineText(screen, 4), QStringLiteral("E"));

    // IL inside the region pushes lines down and drops the one at the bottom of
    // the REGION, never the one below it.
    screen.write(csi("2;1H") + csi("L"));
    QCOMPARE(lineText(screen, 1), QString());
    QCOMPARE(lineText(screen, 2), QStringLiteral("C"));
    QCOMPARE(lineText(screen, 3), QStringLiteral("D"));
    QCOMPARE(lineText(screen, 4), QStringLiteral("E"));

    // DL likewise.
    screen.write(csi("2;1H") + csi("2M"));
    QCOMPARE(lineText(screen, 1), QStringLiteral("D"));
    QCOMPARE(lineText(screen, 2), QString());
    QCOMPARE(lineText(screen, 3), QString());
    QCOMPARE(lineText(screen, 4), QStringLiteral("E"));

    // Cursor movement cannot escape the region upwards while inside it.
    screen.write(csi("3;1H") + csi("9A"));
    QCOMPARE(screen.cursorRow(), 1);

    // IL with the cursor OUTSIDE the region is a no-op: the rows the region
    // protects must stay put.
    screen.write(csi("1;1H") + csi("3L"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("A"));
    QCOMPARE(lineText(screen, 1), QStringLiteral("D"));
}

void TstVt::scrollsUpAndDownWithinRegion()
{
    VtScreen screen;
    screen.resize(10, 4);
    screen.write(QByteArrayLiteral("1\r\n2\r\n3\r\n4"));
    screen.write(csi("1;3r")); // rows 1-3 (0-based 0..2)

    screen.write(csi("2S"));
    // SU inside a region that starts at row 0 DOES feed history, because those
    // lines genuinely left the screen - so the two scrolled-off lines are now
    // addressed as absolute rows 0 and 1, and the live screen starts at 2.
    QCOMPARE(screen.historyLines(), 2);
    QCOMPARE(screen.firstScreenRow(), 2);
    QCOMPARE(lineText(screen, 0), QStringLiteral("1"));
    QCOMPARE(lineText(screen, 1), QStringLiteral("2"));
    QCOMPARE(lineText(screen, 2), QStringLiteral("3"));
    QCOMPARE(lineText(screen, 3), QString());
    QCOMPARE(lineText(screen, 4), QString());
    // The row below the region is untouched by SU.
    QCOMPARE(lineText(screen, 5), QStringLiteral("4"));

    screen.write(csi("1T"));
    QCOMPARE(lineText(screen, screen.firstScreenRow() + 0), QString());
    QCOMPARE(lineText(screen, screen.firstScreenRow() + 1), QStringLiteral("3"));
    QCOMPARE(lineText(screen, screen.firstScreenRow() + 3), QStringLiteral("4"));
    // Scrolling down never removes lines from history.
    QCOMPARE(screen.historyLines(), 2);
}

void TstVt::scrollingPrimaryScreenFeedsHistory()
{
    VtScreen screen;
    screen.resize(10, 3);
    screen.write(QByteArrayLiteral("a\r\nb\r\nc\r\nd\r\n"));

    QCOMPARE(screen.historyLines(), 2);
    QCOMPARE(screen.firstScreenRow(), 2);
    QCOMPARE(screen.totalLines(), 5);
    QCOMPARE(lineText(screen, 0), QStringLiteral("a"));
    QCOMPARE(lineText(screen, 1), QStringLiteral("b"));
    QCOMPARE(lineText(screen, 2), QStringLiteral("c"));
    QCOMPARE(lineText(screen, 3), QStringLiteral("d"));
    QCOMPARE(lineText(screen, 4), QString());
}

void TstVt::eraseDisplayThreeClearsHistory()
{
    VtScreen screen;
    screen.resize(10, 3);
    screen.write(QByteArrayLiteral("a\r\nb\r\nc\r\nd"));
    QCOMPARE(screen.historyLines(), 1);

    QSignalSpy historySpy(&screen, &VtScreen::historyLinesChanged);
    screen.write(csi("3J"));

    QCOMPARE(screen.historyLines(), 0);
    QCOMPARE(screen.firstScreenRow(), 0);
    QCOMPARE(screen.totalLines(), 3);
    QCOMPARE(historySpy.count(), 1);
    // ED 3 erases only the scrollback; the live screen is untouched.
    QCOMPARE(lineText(screen, 0), QStringLiteral("b"));
    QCOMPARE(lineText(screen, 2), QStringLiteral("d"));

    // ED 2 by contrast clears the screen and leaves history alone.
    screen.write(QByteArrayLiteral("\r\ne"));
    const int history = screen.historyLines();
    QVERIFY(history > 0);
    screen.write(csi("2J"));
    QCOMPARE(screen.historyLines(), history);
    QCOMPARE(lineText(screen, screen.firstScreenRow()), QString());
}

void TstVt::historyCapKeepsAbsoluteAddressingConsistent()
{
    VtScreen screen;
    screen.resize(10, 3);

    // Deliberately more lines than the cap, so eviction happens many times.
    constexpr int kLines = VtScreen::kMaxHistoryLines + 200;
    for (int i = 0; i < kLines; ++i)
        screen.write(QByteArrayLiteral("L") + QByteArray::number(i) + "\r\n");

    QCOMPARE(screen.historyLines(), VtScreen::kMaxHistoryLines);
    QCOMPARE(screen.firstScreenRow(), VtScreen::kMaxHistoryLines);
    QCOMPARE(screen.totalLines(), VtScreen::kMaxHistoryLines + 3);

    // Every line ever written occupies one absolute row, in order, so after E
    // evictions the oldest retained row is the (E)th line written. E is
    // (lines written + the blank current line) - (retained rows).
    const int evicted = (kLines + 1) - screen.totalLines();
    QCOMPARE(lineText(screen, 0), QStringLiteral("L%1").arg(evicted));
    QCOMPARE(lineText(screen, 1), QStringLiteral("L%1").arg(evicted + 1));
    // The live screen holds the last two labels plus the blank line the cursor is
    // on, addressed through the same absolute space.
    QCOMPARE(lineText(screen, screen.firstScreenRow()),
             QStringLiteral("L%1").arg(kLines - 2));
    QCOMPARE(lineText(screen, screen.firstScreenRow() + 1),
             QStringLiteral("L%1").arg(kLines - 1));
    QCOMPARE(lineText(screen, screen.totalLines() - 1), QString());
    // One past the end must answer a blank line, not crash.
    QCOMPARE(screen.lineAt(screen.totalLines()).size(), qsizetype(10));
}

void TstVt::altScreenLeavesPrimaryAndHistoryIntact()
{
    VtScreen screen;
    screen.resize(10, 3);
    screen.write(QByteArrayLiteral("a\r\nb\r\nc\r\nd"));
    screen.write(csi("2;3H"));
    const int history = screen.historyLines();
    QCOMPARE(history, 1);

    screen.write(csi("?1049h"));
    QVERIFY(screen.altScreenActive());
    // The alt screen starts blank and is homed.
    QCOMPARE(lineText(screen, screen.firstScreenRow()), QString());
    QCOMPARE(screen.cursorRow(), 0);
    QCOMPARE(screen.cursorColumn(), 0);
    QCOMPARE(screen.historyLines(), history);

    // Scrolling the ALT screen must never contribute scrollback - that is the
    // single most complained-about terminal bug there is.
    screen.write(QByteArrayLiteral("x\r\ny\r\nz\r\nw\r\nv"));
    QCOMPARE(screen.historyLines(), history);

    screen.write(csi("?1049l"));
    QVERIFY(!screen.altScreenActive());
    QCOMPARE(screen.historyLines(), history);
    // The primary screen and the cursor come back exactly as they were.
    QCOMPARE(lineText(screen, screen.firstScreenRow() + 0), QStringLiteral("b"));
    QCOMPARE(lineText(screen, screen.firstScreenRow() + 1), QStringLiteral("c"));
    QCOMPARE(lineText(screen, screen.firstScreenRow() + 2), QStringLiteral("d"));
    QCOMPARE(screen.cursorRow(), 1);
    QCOMPARE(screen.cursorColumn(), 2);
}

// --- rendition --------------------------------------------------------------

void TstVt::appliesIndexedAndTruecolourSgr()
{
    VtScreen screen;
    screen.resize(20, 3);

    screen.write(csi("1;31;44m") + QByteArrayLiteral("A"));
    QCOMPARE(cellAt(screen, 0, 0).attrs, quint16(VtBold));
    QCOMPARE(cellAt(screen, 0, 0).fg, vt::xterm256Color(1));
    QCOMPARE(cellAt(screen, 0, 0).bg, vt::xterm256Color(4));

    // The semicolon form's parameters must be CONSUMED, or the trailing "1" would
    // be read as a colour index instead of as bold.
    screen.write(csi("0m") + csi("38;5;196;48;5;21;1m") + QByteArrayLiteral("B"));
    QCOMPARE(cellAt(screen, 0, 1).fg, vt::xterm256Color(196));
    QCOMPARE(cellAt(screen, 0, 1).bg, vt::xterm256Color(21));
    QCOMPARE(cellAt(screen, 0, 1).attrs, quint16(VtBold));

    screen.write(csi("0m") + csi("38;2;10;20;30;48;2;200;100;50;3m")
                 + QByteArrayLiteral("C"));
    QCOMPARE(cellAt(screen, 0, 2).fg, qRgb(10, 20, 30));
    QCOMPARE(cellAt(screen, 0, 2).bg, qRgb(200, 100, 50));
    QCOMPARE(cellAt(screen, 0, 2).attrs, quint16(VtItalic));

    // Bright colours and the explicit defaults.
    screen.write(csi("0;92;105m") + QByteArrayLiteral("D") + csi("39;49m")
                 + QByteArrayLiteral("E"));
    QCOMPARE(cellAt(screen, 0, 3).fg, vt::xterm256Color(10));
    QCOMPARE(cellAt(screen, 0, 3).bg, vt::xterm256Color(13));
    QCOMPARE(cellAt(screen, 0, 4).fg, VtScreen::defaultForeground());
    QCOMPARE(cellAt(screen, 0, 4).bg, VtScreen::defaultBackground());
    // A default is distinguishable from any real colour, which is what lets a
    // renderer theme it.
    QVERIFY(VtScreen::defaultForeground() != vt::xterm256Color(0));
    QVERIFY(VtScreen::defaultBackground() != vt::xterm256Color(0));

    // Every attribute and its reset.
    screen.write(csi("0;1;2;3;4;5;7;8;9m") + QByteArrayLiteral("F")
                 + csi("22;23;24;25;27;28;29m") + QByteArrayLiteral("G"));
    const quint16 all = VtBold | VtDim | VtItalic | VtUnderline | VtBlink | VtInverse
                        | VtHidden | VtStrike;
    QCOMPARE(cellAt(screen, 0, 5).attrs, all);
    QCOMPARE(cellAt(screen, 0, 6).attrs, quint16(0));

    // A bare "CSI m" is a full reset.
    screen.write(csi("31;1m") + csi("m") + QByteArrayLiteral("H"));
    QCOMPARE(cellAt(screen, 0, 7).fg, VtScreen::defaultForeground());
    QCOMPARE(cellAt(screen, 0, 7).attrs, quint16(0));
}

void TstVt::appliesColonSeparatedSgr()
{
    VtScreen screen;
    screen.resize(20, 3);

    screen.write(csi("38:5:196m") + QByteArrayLiteral("A"));
    QCOMPARE(cellAt(screen, 0, 0).fg, vt::xterm256Color(196));

    screen.write(csi("0m") + csi("38:2:10:20:30m") + QByteArrayLiteral("B"));
    QCOMPARE(cellAt(screen, 0, 1).fg, qRgb(10, 20, 30));

    // The spelling with an empty colour-space id, which is what libvte and many
    // Rust TUI crates emit.
    screen.write(csi("0m") + csi("48:2::40:50:60m") + QByteArrayLiteral("C"));
    QCOMPARE(cellAt(screen, 0, 2).bg, qRgb(40, 50, 60));

    // A colon sub-parameter on a NON-colour code must not be read as a code of
    // its own: "4:3" is a curly underline, not "underline; italic".
    screen.write(csi("0m") + csi("4:3m") + QByteArrayLiteral("D"));
    QCOMPARE(cellAt(screen, 0, 3).attrs, quint16(VtUnderline));

    // Mixed colon and semicolon in one sequence.
    screen.write(csi("0m") + csi("1;38:5:33;4m") + QByteArrayLiteral("E"));
    QCOMPARE(cellAt(screen, 0, 4).fg, vt::xterm256Color(33));
    QCOMPARE(cellAt(screen, 0, 4).attrs, quint16(VtBold | VtUnderline));

    // Underline colour (58) is parsed and discarded, and must not leak its
    // parameters into the foreground or the attributes.
    screen.write(csi("0m") + csi("58:2::1:2:3;31m") + QByteArrayLiteral("F"));
    QCOMPARE(cellAt(screen, 0, 5).fg, vt::xterm256Color(1));
    QCOMPARE(cellAt(screen, 0, 5).attrs, quint16(0));
}

// --- reports and strings ----------------------------------------------------

void TstVt::answersDeviceStatusReports()
{
    VtScreen screen;
    screen.resize(20, 5);
    QByteArray replies;
    connect(&screen, &VtScreen::reply, this,
            [&replies](const QByteArray &bytes) { replies += bytes; });

    screen.write(csi("5n"));
    QCOMPARE(replies, QByteArrayLiteral("\x1b[0n"));

    replies.clear();
    screen.write(csi("6n"));
    QCOMPARE(replies, QByteArrayLiteral("\x1b[1;1R"));

    replies.clear();
    screen.write(csi("3;7H") + csi("6n"));
    QCOMPARE(replies, QByteArrayLiteral("\x1b[3;7R"));

    // The report is a SCREEN position: scrollback must not shift it, or every
    // full-screen program would draw in the wrong place after the first scroll.
    replies.clear();
    screen.write(QByteArrayLiteral("\r\n\r\n\r\n\r\n\r\n") + csi("6n"));
    QVERIFY(screen.historyLines() > 0);
    QCOMPARE(replies, QByteArrayLiteral("\x1b[5;1R"));

    // A private DECDSR is consumed and NOT answered.
    replies.clear();
    screen.write(csi("?6n"));
    QCOMPARE(replies, QByteArray());
}

void TstVt::setsWindowTitleFromOsc()
{
    VtScreen screen;
    screen.resize(20, 3);
    QSignalSpy titleSpy(&screen, &VtScreen::windowTitleChanged);

    screen.write(QByteArrayLiteral("\x1b]0;first\x07"));
    QCOMPARE(screen.windowTitle(), QStringLiteral("first"));
    QCOMPARE(titleSpy.count(), 1);

    // ST terminator instead of BEL.
    screen.write(QByteArrayLiteral("\x1b]2;second\x1b\\"));
    QCOMPARE(screen.windowTitle(), QStringLiteral("second"));

    // OSC 1 (icon name) is accepted as a title too.
    screen.write(QByteArrayLiteral("\x1b]1;third\x07"));
    QCOMPARE(screen.windowTitle(), QStringLiteral("third"));

    // Split across writes, including between the ESC and the ']'.
    feedByte(screen, QByteArrayLiteral("\x1b]2;split title\x07"));
    QCOMPARE(screen.windowTitle(), QStringLiteral("split title"));

    // Embedded control characters never reach the title: a newline in it would
    // wreck the layout of the QML chrome that shows it.
    screen.write(QByteArrayLiteral("\x1b]2;a\x01\x0a""b\x07"));
    QCOMPARE(screen.windowTitle(), QStringLiteral("ab"));

    // UTF-8 payloads survive intact.
    screen.write(QByteArrayLiteral("\x1b]2;caf\xc3\xa9\x07"));
    QCOMPARE(screen.windowTitle(), QStringLiteral("caf\u00e9"));

    // An absurdly long title is bounded rather than stored whole.
    screen.write(QByteArrayLiteral("\x1b]2;") + QByteArray(4096, 'x')
                 + QByteArrayLiteral("\x07"));
    QVERIFY(screen.windowTitle().size() <= 256);
}

void TstVt::discardsUnknownOscAndDcsPayloads()
{
    VtScreen screen;
    screen.resize(20, 3);
    const QString before = screen.windowTitle();

    // OSC 4 (palette) must be consumed whole; not one byte of its payload may
    // reach the screen.
    screen.write(QByteArrayLiteral("\x1b]4;1;rgb:ff/00/00\x07ok"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("ok"));
    QCOMPARE(screen.windowTitle(), before);

    // OSC 52 (clipboard) likewise, including a base64 payload full of characters
    // that would otherwise print.
    screen.write(csi("2J") + csi("1;1H")
                 + QByteArrayLiteral("\x1b]52;c;YWJjZGVm\x1b\\fine"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("fine"));

    // DCS: the payload is passed through the parser and dropped, then normal
    // output resumes.
    screen.write(csi("2J") + csi("1;1H")
                 + QByteArrayLiteral("\x1bP1$r0;1m\x1b\\after"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("after"));

    // APC / PM / SOS strings, which some agents emit, must not print either.
    screen.write(csi("2J") + csi("1;1H")
                 + QByteArrayLiteral("\x1b_G a=T,f=100;payload\x1b\\end"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("end"));

    // An unterminated OSC followed by a stray ESC must not swallow the rest of
    // the stream: the escape aborts the string and the next sequence is honoured.
    screen.write(csi("2J") + csi("1;1H")
                 + QByteArrayLiteral("\x1b]2;never\x1b") + csi("31m")
                 + QByteArrayLiteral("Z"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("Z"));
    QCOMPARE(cellAt(screen, 0, 0).fg, vt::xterm256Color(1));
}

// --- text extraction --------------------------------------------------------

void TstVt::textRangeSpansWrappedWideCharacters()
{
    VtScreen screen;
    screen.resize(5, 3);
    // Three double-width characters on a five-column grid: two fit, the third
    // wraps whole rather than being split across the boundary.
    screen.write(QByteArrayLiteral("\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97"));

    QCOMPARE(cellAt(screen, 0, 0).width, quint8(2));
    QCOMPARE(cellAt(screen, 0, 2).width, quint8(2));
    QCOMPARE(cellAt(screen, 0, 4).text, char32_t(' '));
    QCOMPARE(cellAt(screen, 1, 0).text, char32_t(0x5B57));

    // Covered halves contribute no text, and the trailing pad column is trimmed.
    QCOMPARE(screen.textRange(0, 0, 0, 5), QStringLiteral("\u4e2d\u6587"));
    QCOMPARE(screen.textRange(0, 0, 1, 5), QStringLiteral("\u4e2d\u6587\n\u5b57"));

    // endCol is exclusive: stopping inside the second character's leading cell
    // excludes it.
    QCOMPARE(screen.textRange(0, 0, 0, 2), QStringLiteral("\u4e2d"));
    // Starting on a covered half skips it rather than emitting a stray blank.
    QCOMPARE(screen.textRange(0, 1, 0, 5), QStringLiteral("\u6587"));
    // Empty and inverted ranges answer nothing.
    QCOMPARE(screen.textRange(0, 2, 0, 2), QString());
    QCOMPARE(screen.textRange(1, 0, 0, 5), QString());
    // Out-of-range rows are clamped, not refused.
    QCOMPARE(screen.textRange(-5, 0, 99, 99),
             QStringLiteral("\u4e2d\u6587\n\u5b57\n"));
}

// --- cursor mechanics -------------------------------------------------------

void TstVt::tabStopsAndBackspaceMoveCursor()
{
    VtScreen screen;
    screen.resize(20, 3);

    screen.write(QByteArrayLiteral("a\tb"));
    QCOMPARE(cellAt(screen, 0, 0).text, char32_t('a'));
    QCOMPARE(cellAt(screen, 0, 8).text, char32_t('b'));
    QCOMPARE(screen.cursorColumn(), 9);

    // A tab past the last stop parks on the final column and never wraps.
    screen.write(csi("1;1H") + QByteArrayLiteral("\t\t\t\t"));
    QCOMPARE(screen.cursorColumn(), 19);
    QCOMPARE(screen.cursorRow(), 0);

    // CBT walks back through the stops.
    screen.write(csi("2Z"));
    QCOMPARE(screen.cursorColumn(), 8);
    screen.write(csi("9Z"));
    QCOMPARE(screen.cursorColumn(), 0);

    // HTS adds a stop, TBC 3 removes them all.
    screen.write(csi("1;5H") + QByteArrayLiteral("\x1bH") + csi("1;1H")
                 + QByteArrayLiteral("\t"));
    QCOMPARE(screen.cursorColumn(), 4);
    screen.write(csi("3g") + csi("1;1H") + QByteArrayLiteral("\t"));
    QCOMPARE(screen.cursorColumn(), 19);

    // Backspace stops at the left edge and never wraps to the previous line.
    screen.write(csi("2;1H") + QByteArrayLiteral("\x08\x08X"));
    QCOMPARE(screen.cursorRow(), 1);
    QCOMPARE(cellAt(screen, 1, 0).text, char32_t('X'));
}

void TstVt::insertsAndDeletesCharacters()
{
    VtScreen screen;
    screen.resize(10, 2);
    screen.write(QByteArrayLiteral("abcdefgh")); // columns 0-7, 8 and 9 blank

    // ICH shifts the tail right; nothing falls off, because the line had room.
    screen.write(csi("1;3H") + csi("2@"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("ab  cdefgh"));

    // DCH is its exact inverse here.
    screen.write(csi("1;3H") + csi("2P"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("abcdefgh"));

    // ECH blanks in place without moving anything.
    screen.write(csi("1;2H") + csi("3X"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("a   efgh"));

    // ICH and DCH are clamped to the line: a count far past the right edge
    // touches only the columns that exist, and never reads or writes past them.
    screen.write(csi("1;9H") + csi("99@") + csi("1;9H") + csi("99P"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("a   efgh"));

    // A large ICH near the middle pushes the tail off the right edge instead of
    // growing the line.
    screen.write(csi("1;6H") + csi("99@"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("a   e"));

    // EL 1 erases through the cursor cell inclusive.
    // Columns 0-2 erased; column 3 was already blank, so four blanks precede 'e'.
    screen.write(QByteArrayLiteral("XYZ") + csi("1;3H") + csi("1K"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("    eXYZ"));
}

void TstVt::editingCharactersNeverOrphansWideHalves()
{
    // ICH in front of a double-width character shifts the character as a PAIR.
    // Repairing the seam before the shift instead of after it would copy the
    // repaired cell rightwards and leave a leading half with no covered half,
    // which a renderer draws as a two-column glyph inside one column.
    VtScreen ich;
    ich.resize(6, 2);
    ich.write(QByteArrayLiteral("\xe4\xb8\xad\xe6\x96\x87")); // CJK at columns 0-3
    ich.write(csi("1;1H") + csi("1@"));
    QCOMPARE(cellAt(ich, 0, 1).text, char32_t(0x4E2D));
    QCOMPARE(cellAt(ich, 0, 1).width, quint8(2));
    QCOMPARE(cellAt(ich, 0, 2).width, quint8(0));
    QCOMPARE(cellAt(ich, 0, 3).text, char32_t(0x6587));
    QCOMPARE(cellAt(ich, 0, 4).width, quint8(0));
    QCOMPARE(lineText(ich, 0), QStringLiteral(" \u4e2d\u6587"));

    // ICH INSIDE a double-width character genuinely tears it apart, and then
    // BOTH halves have to become blanks: one half alone is either an oversized
    // glyph or an invisible cell.
    VtScreen inside;
    inside.resize(6, 2);
    inside.write(QByteArrayLiteral("\xe4\xb8\xad\xe6\x96\x87"));
    inside.write(csi("1;2H") + csi("1@"));
    QCOMPARE(cellAt(inside, 0, 0).width, quint8(1));
    QCOMPARE(cellAt(inside, 0, 0).text, char32_t(' '));
    QCOMPARE(cellAt(inside, 0, 2).width, quint8(1));
    QCOMPARE(cellAt(inside, 0, 2).text, char32_t(' '));
    QCOMPARE(cellAt(inside, 0, 3).text, char32_t(0x6587));
    QCOMPARE(lineText(inside, 0), QStringLiteral("   \u6587"));

    // DCH that deletes only the LEADING half leaves the covered half behind. An
    // undemoted covered half renders as nothing at all and is skipped by
    // textRange(), so the column silently disappears from the screen and a copy.
    VtScreen dch;
    dch.resize(6, 2);
    dch.write(QByteArrayLiteral("ab\xe4\xb8\xad""cd"));
    dch.write(csi("1;2H") + csi("2P"));
    QCOMPARE(cellAt(dch, 0, 1).width, quint8(1));
    QCOMPARE(cellAt(dch, 0, 1).text, char32_t(' '));
    QCOMPARE(lineText(dch, 0), QStringLiteral("a cd"));

    // ICH that pushes a pair off the right edge must not leave the leading half
    // claiming a column that no longer exists.
    VtScreen edge;
    edge.resize(6, 2);
    edge.write(QByteArrayLiteral("abcd\xe4\xb8\xad")); // CJK at columns 4-5
    edge.write(csi("1;1H") + csi("1@"));
    QCOMPARE(cellAt(edge, 0, 5).width, quint8(1));
    QCOMPARE(cellAt(edge, 0, 5).text, char32_t(' '));
    QCOMPARE(lineText(edge, 0), QStringLiteral(" abcd"));
}

void TstVt::softHyphenOccupiesOneCell()
{
    // U+00AD SOFT HYPHEN is category Other_Format, so a plain category test calls
    // it zero width - and a zero-width code point is folded into the preceding
    // cell, where "a" plus a soft hyphen has no precomposed form and the
    // character is dropped. groff and `man` emit it in justified text.
    QCOMPARE(vt::charWidth(0x00AD), 1);
    QCOMPARE(vt::charWidth(0x0301), 0);  // combining acute, genuinely zero width
    QCOMPARE(vt::charWidth(0x200D), 0);  // zero width joiner

    VtScreen screen;
    screen.resize(10, 2);
    screen.write(QByteArrayLiteral("a\xc2\xad""b"));
    QCOMPARE(cellAt(screen, 0, 0).text, char32_t('a'));
    QCOMPARE(cellAt(screen, 0, 1).text, char32_t(0x00AD));
    QCOMPARE(cellAt(screen, 0, 1).width, quint8(1));
    QCOMPARE(cellAt(screen, 0, 2).text, char32_t('b'));
    QCOMPARE(screen.cursorColumn(), 3);
}

void TstVt::abortsSequencesOnCancel()
{
    // CAN and SUB abort the sequence they land in, in EVERY state that can be
    // inside one. A state that keeps collecting instead swallows the bytes after
    // the abort as part of a sequence the sender has already cancelled.
    VtScreen csiIgnore;
    csiIgnore.resize(10, 2);
    // The '?' is not legal after a parameter, so the sequence is being ignored
    // when the CAN arrives.
    csiIgnore.write(QByteArrayLiteral("\x1b[1?\x18""A"));
    QCOMPARE(lineText(csiIgnore, 0), QStringLiteral("A"));

    VtScreen escIntermediate;
    escIntermediate.resize(10, 2);
    escIntermediate.write(QByteArrayLiteral("\x1b#\x18""B"));
    QCOMPARE(lineText(escIntermediate, 0), QStringLiteral("B"));

    VtScreen csiIntermediate;
    csiIntermediate.resize(10, 2);
    csiIntermediate.write(QByteArrayLiteral("\x1b[1!\x18""C"));
    QCOMPARE(lineText(csiIntermediate, 0), QStringLiteral("C"));

    // A DCS aborted among its intermediate bytes is the worst case: without the
    // abort the machine waits for a string terminator that never comes, and the
    // rest of the session's output is consumed as payload.
    VtScreen dcs;
    dcs.resize(10, 2);
    dcs.write(QByteArrayLiteral("\x1bP$\x18""D"));
    QCOMPARE(lineText(dcs, 0), QStringLiteral("D"));

    VtScreen sub;
    sub.resize(10, 2);
    sub.write(QByteArrayLiteral("\x1b[1?\x1a""E"));
    QCOMPARE(lineText(sub, 0), QStringLiteral("E"));
}

void TstVt::stripsBidiControlsFromTitle()
{
    VtScreen screen;
    screen.resize(20, 2);

    // U+202E RIGHT-TO-LEFT OVERRIDE reorders the text AROUND the title in the QML
    // chrome, which is how a harmless-looking name is made to read as another
    // one. Text.PlainText stops markup, not reordering.
    screen.write(QByteArrayLiteral("\x1b]2;a\xe2\x80\xae""b\x07"));
    QCOMPARE(screen.windowTitle(), QStringLiteral("ab"));

    // Line and paragraph separators break the chrome's layout exactly as a
    // newline does.
    screen.write(QByteArrayLiteral("\x1b]2;c\xe2\x80\xa8""d\xe2\x80\xa9""e\x07"));
    QCOMPARE(screen.windowTitle(), QStringLiteral("cde"));

    // A zero width joiner is CONTENT even though it is also Other_Format: an
    // emoji sequence in a title has to survive the filter intact.
    screen.write(QByteArrayLiteral("\x1b]2;\xf0\x9f\x91\xa9\xe2\x80\x8d"
                                   "\xf0\x9f\x92\xbb\x07"));
    QCOMPARE(screen.windowTitle(), QStringLiteral("\U0001F469\u200D\U0001F4BB"));
}

void TstVt::boundsParameterAndStringOverflow()
{
    VtScreen screen;
    screen.resize(20, 3);

    // 32 parameters are stored and applied; the 33rd is DROPPED, and dropping it
    // must not corrupt the 32nd either (the digits have to go to a sink, not to
    // the last valid slot).
    QByteArray many = QByteArrayLiteral("\x1b[1");
    for (int i = 0; i < 31; ++i)
        many += ";1";
    many += ";31m";
    screen.write(many + QByteArrayLiteral("A"));
    QCOMPARE(cellAt(screen, 0, 0).attrs, quint16(VtBold));
    QCOMPARE(cellAt(screen, 0, 0).fg, VtScreen::defaultForeground());

    // A parameter far past any plausible value is clamped on the way in, so it
    // can never wrap into a negative and address a row above the screen.
    screen.write(csi("0m") + csi("999999999999;999999999999H"));
    QCOMPARE(screen.cursorRow(), screen.rows() - 1);
    QCOMPARE(screen.cursorColumn(), screen.columns() - 1);

    // An OSC string longer than the cap is discarded rather than retained: a
    // remote program with no terminator must not be able to grow the parser's
    // buffer without bound. The stream after its terminator is still honoured.
    screen.write(QByteArrayLiteral("\x1b]2;ok\x07"));
    QCOMPARE(screen.windowTitle(), QStringLiteral("ok"));
    screen.write(QByteArrayLiteral("\x1b]2;") + QByteArray(64 * 1024, 'y')
                 + QByteArrayLiteral("\x07") + csi("2J") + csi("1;1H")
                 + QByteArrayLiteral("after"));
    QCOMPARE(screen.windowTitle(), QStringLiteral("ok"));
    QCOMPARE(lineText(screen, screen.firstScreenRow()), QStringLiteral("after"));
}

// --- geometry and reset -----------------------------------------------------

void TstVt::resizeTruncatesWithoutReflowing()
{
    VtScreen screen;
    screen.resize(6, 3);
    screen.write(QByteArrayLiteral("abcd\xe4\xb8\xad"));   // wide char at columns 4-5
    screen.write(QByteArrayLiteral("\r\nsecond\r\nthird"));
    QCOMPARE(screen.historyLines(), 0);

    QSignalSpy sizeSpy(&screen, &VtScreen::sizeChanged);
    screen.resize(5, 2);
    QCOMPARE(sizeSpy.count(), 1);
    QCOMPARE(screen.columns(), 5);
    QCOMPARE(screen.rows(), 2);

    // Lines are TRUNCATED, never reflowed onto a following line.
    QCOMPARE(lineText(screen, 0), QStringLiteral("abcd"));
    QCOMPARE(lineText(screen, 1), QStringLiteral("secon"));
    QCOMPARE(screen.totalLines(), 2);
    // The double-width character was cut in half, so its surviving leading cell
    // is demoted rather than left claiming two columns.
    QCOMPARE(cellAt(screen, 0, 4).width, quint8(1));
    QCOMPARE(cellAt(screen, 0, 4).text, char32_t(' '));
    // The cursor is clamped into the smaller grid.
    QVERIFY(screen.cursorRow() < 2);
    QVERIFY(screen.cursorColumn() < 5);

    // Growing pads with blanks and leaves the existing content alone.
    screen.resize(8, 4);
    QCOMPARE(lineText(screen, 0), QStringLiteral("abcd"));
    QCOMPARE(screen.lineAt(0).size(), qsizetype(8));
    QCOMPARE(screen.totalLines(), 4);
    // A no-op resize notifies nothing.
    sizeSpy.clear();
    screen.resize(8, 4);
    QCOMPARE(sizeSpy.count(), 0);
    // Absurd geometry is clamped instead of producing an empty grid.
    screen.resize(0, -3);
    QCOMPARE(screen.columns(), VtScreen::kMinColumns);
    QCOMPARE(screen.rows(), VtScreen::kMinRows);
}

void TstVt::resetRestoresDefaults()
{
    VtScreen screen;
    screen.resize(10, 3);
    screen.write(QByteArrayLiteral("a\r\nb\r\nc\r\nd") + csi("?1h") + csi("?2004h")
                 + csi("?25l") + csi("31m") + csi("2;3r")
                 + QByteArrayLiteral("\x1b]2;title\x07"));
    QVERIFY(screen.historyLines() > 0);
    QVERIFY(screen.applicationCursorKeys());
    QVERIFY(screen.bracketedPaste());
    QVERIFY(!screen.cursorVisible());

    QSignalSpy resetSpy(&screen, &VtScreen::screenReset);
    screen.write(QByteArrayLiteral("\x1b""c")); // RIS; 'c' is a hex digit, so split

    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(screen.historyLines(), 0);
    QCOMPARE(screen.totalLines(), 3);
    QVERIFY(screen.cursorVisible());
    QVERIFY(!screen.applicationCursorKeys());
    QVERIFY(!screen.bracketedPaste());
    QCOMPARE(screen.windowTitle(), QString());
    QCOMPARE(screen.cursorRow(), 0);
    QCOMPARE(screen.cursorColumn(), 0);
    QCOMPARE(lineText(screen, 0), QString());

    // The scroll region came back too, so a plain line feed at the bottom scrolls
    // the whole screen again.
    screen.write(QByteArrayLiteral("x\r\ny\r\nz\r\nw"));
    QCOMPARE(screen.historyLines(), 1);
    QCOMPARE(lineText(screen, 0), QStringLiteral("x"));

    // DECALN fills the grid, which is what makes it usable as a probe.
    screen.write(QByteArrayLiteral("\x1b#8"));
    QCOMPARE(lineText(screen, screen.firstScreenRow()), QStringLiteral("EEEEEEEEEE"));
    QCOMPARE(screen.cursorRow(), 0);

    // The public reset() also drops a partially received sequence, so a stream cut
    // mid-escape cannot leak into the next session on the same screen.
    screen.write(QByteArrayLiteral("\x1b["));
    screen.reset();
    screen.write(QByteArrayLiteral("31mQ"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("31mQ"));
}

void TstVt::coalescesNotificationsPerWrite()
{
    VtScreen screen;
    screen.resize(10, 4);

    QSignalSpy linesSpy(&screen, &VtScreen::linesChanged);
    QSignalSpy cursorSpy(&screen, &VtScreen::cursorMoved);
    QSignalSpy bellSpy(&screen, &VtScreen::bell);
    QSignalSpy scrollSpy(&screen, &VtScreen::screenScrolled);

    // A whole line of glyphs is ONE notification, not one per cell.
    screen.write(csi("2;1H") + QByteArrayLiteral("abcdefg"));
    QCOMPARE(linesSpy.count(), 1);
    QCOMPARE(linesSpy.at(0).at(0).toInt(), 1);
    QCOMPARE(linesSpy.at(0).at(1).toInt(), 1);
    QCOMPARE(cursorSpy.count(), 1);

    // Many bells in one write ring once.
    bellSpy.clear();
    screen.write(QByteArrayLiteral("\x07\x07\x07"));
    QCOMPARE(bellSpy.count(), 1);

    // A scroll reports how many lines moved, ONCE for the write, and the damaged
    // range reaches back over the row that just became history. The cursor is on
    // row 1 of a 4-row grid, so three line feeds reach the bottom row and only the
    // third one scrolls.
    linesSpy.clear();
    scrollSpy.clear();
    screen.write(QByteArrayLiteral("\r\n\r\n\r\n"));
    QCOMPARE(screen.historyLines(), 1);
    QCOMPARE(scrollSpy.count(), 1);
    QCOMPARE(scrollSpy.at(0).at(0).toInt(), 1);
    QCOMPARE(linesSpy.count(), 1);
    QCOMPARE(linesSpy.at(0).at(0).toInt(), 0);
    QCOMPARE(linesSpy.at(0).at(1).toInt(), screen.totalLines() - 1);

    // Two scrolls in one write are reported as a single total, not twice.
    scrollSpy.clear();
    screen.write(QByteArrayLiteral("\r\n\r\n"));
    QCOMPARE(scrollSpy.count(), 1);
    QCOMPARE(scrollSpy.at(0).at(0).toInt(), 2);
    QCOMPARE(screen.historyLines(), 3);

    // Mode changes notify once per write, and only when the aggregate changes.
    QSignalSpy modesSpy(&screen, &VtScreen::modesChanged);
    screen.write(csi("?1000h") + csi("?1002h") + csi("?1006h"));
    QCOMPARE(modesSpy.count(), 1);
    QVERIFY(screen.mouseTrackingEnabled());
    modesSpy.clear();
    screen.write(csi("?1002l"));
    QCOMPARE(modesSpy.count(), 0);
    QVERIFY(screen.mouseTrackingEnabled());
    screen.write(csi("?1000l") + csi("?1006l"));
    QVERIFY(!screen.mouseTrackingEnabled());

    // An empty write does nothing at all.
    linesSpy.clear();
    cursorSpy.clear();
    screen.write(QByteArray());
    QCOMPARE(linesSpy.count(), 0);
    QCOMPARE(cursorSpy.count(), 0);
}

void TstVt::reportsHistoryAdvanceNotRegionScroll()
{
    VtScreen screen;
    screen.resize(10, 3);
    QSignalSpy scrollSpy(&screen, &VtScreen::screenScrolled);
    QSignalSpy linesSpy(&screen, &VtScreen::linesChanged);

    screen.write(QByteArrayLiteral("a\r\nb\r\nc\r\nd"));
    QCOMPARE(screen.historyLines(), 1);
    QCOMPARE(scrollSpy.count(), 1);
    QCOMPARE(scrollSpy.at(0).at(0).toInt(), 1);

    // A scroll UP followed by a scroll DOWN in the same write does not cancel
    // out: the line that went into history is still there. Netting the two would
    // report nothing at all, so a consumer anchored in history would drift by a
    // row, and the damaged range would never cover the row that became history.
    scrollSpy.clear();
    linesSpy.clear();
    screen.write(QByteArrayLiteral("\n") + csi("1;1H") + QByteArrayLiteral("\x1bM"));
    QCOMPARE(screen.historyLines(), 2);
    QCOMPARE(scrollSpy.count(), 1);
    QCOMPARE(scrollSpy.at(0).at(0).toInt(), 1);
    QCOMPARE(linesSpy.count(), 1);
    QCOMPARE(linesSpy.at(0).at(0).toInt(), screen.firstScreenRow() - 1);
    QCOMPARE(linesSpy.at(0).at(1).toInt(), screen.totalLines() - 1);

    // Scrolling the ALT screen moves content on the live screen without touching
    // the absolute row space, so nothing is reported: a user reading scrollback
    // must not be dragged along by a full-screen program's redraws.
    screen.write(csi("?1049h"));
    const int history = screen.historyLines();
    scrollSpy.clear();
    screen.write(QByteArrayLiteral("1\r\n2\r\n3\r\n4\r\n5"));
    QCOMPARE(screen.historyLines(), history);
    QCOMPARE(scrollSpy.count(), 0);
    screen.write(csi("?1049l"));

    // Nor does a scroll region that starts below the top of the screen: a pager
    // window's lines are not leaving the screen at all.
    scrollSpy.clear();
    screen.write(csi("2;3r") + csi("3;1H") + QByteArrayLiteral("\n\n"));
    QCOMPARE(screen.historyLines(), history);
    QCOMPARE(scrollSpy.count(), 0);
}

void TstVt::resizeReportsHistoryDamageAndKeepsUsableRegion()
{
    VtScreen screen;
    screen.resize(10, 3);
    screen.write(QByteArrayLiteral("aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc"
                                   "\r\ndddddddddd"));
    QVERIFY(screen.historyLines() > 0);

    QSignalSpy linesSpy(&screen, &VtScreen::linesChanged);
    screen.resize(5, 3);
    // A width change truncates every RETAINED history line, so their content
    // changed even though their indices did not. Reporting only the live screen
    // leaves a consumer painting history rows at the old width.
    QCOMPARE(linesSpy.count(), 1);
    QCOMPARE(linesSpy.at(0).at(0).toInt(), 0);
    QCOMPARE(linesSpy.at(0).at(1).toInt(), screen.totalLines() - 1);
    QCOMPARE(lineText(screen, 0), QStringLiteral("aaaaa"));

    // A rows-only resize rewrites no history line, so it does not claim to.
    linesSpy.clear();
    screen.resize(5, 4);
    QCOMPARE(linesSpy.count(), 1);
    QCOMPARE(linesSpy.at(0).at(0).toInt(), screen.firstScreenRow());

    // A scroll region clamped into a shorter grid must not collapse to a single
    // row. A one-row region traps the cursor: every line feed scrolls the row the
    // cursor is on, so output overwrites itself in place, nothing reaches the
    // scrollback, and the screen never advances again.
    VtScreen region;
    region.resize(10, 24);
    region.write(csi("20;24r"));
    region.resize(10, 20);
    const int history = region.historyLines();
    region.write(csi("20;1H") + QByteArrayLiteral("x\r\ny"));
    // The whole screen scrolled, so the (blank) top row went to the scrollback
    // and the two written lines are the last two rows of the live screen. With a
    // collapsed one-row region nothing would reach the scrollback at all and 'y'
    // would have overwritten 'x' in place.
    QCOMPARE(region.historyLines(), history + 1);
    QCOMPARE(lineText(region, region.totalLines() - 2), QStringLiteral("x"));
    QCOMPARE(lineText(region, region.totalLines() - 1), QStringLiteral("y"));
}

void TstVt::nestedWriteFromReplyHandlerJoinsTheBatch()
{
    // reply() is the one signal emitted from inside a feed, so a handler that
    // answers by feeding this screen again is the one way write() can re-enter.
    // The inner call must NOT flush the batch: the outer feed has bytes left, and
    // a listener woken half way through sees a screen that is still changing.
    VtScreen screen;
    screen.resize(10, 2);
    QSignalSpy cursorSpy(&screen, &VtScreen::cursorMoved);
    QSignalSpy linesSpy(&screen, &VtScreen::linesChanged);
    connect(&screen, &VtScreen::reply, &screen, [&screen](const QByteArray &) {
        screen.write(QByteArrayLiteral("R"));
    });

    screen.write(csi("6n") + QByteArrayLiteral("X"));
    QCOMPARE(lineText(screen, 0), QStringLiteral("RX"));
    QCOMPARE(cursorSpy.count(), 1);
    QCOMPARE(linesSpy.count(), 1);
}

// --- key encoding -----------------------------------------------------------

void TstVt::encodesCursorKeysInBothModes()
{
    const auto normal = [](int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        return vt::encodeKey(key, mods, QString(), false);
    };
    const auto application = [](int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        return vt::encodeKey(key, mods, QString(), true);
    };

    QCOMPARE(normal(Qt::Key_Up), QByteArrayLiteral("\x1b[A"));
    QCOMPARE(normal(Qt::Key_Down), QByteArrayLiteral("\x1b[B"));
    QCOMPARE(normal(Qt::Key_Right), QByteArrayLiteral("\x1b[C"));
    QCOMPARE(normal(Qt::Key_Left), QByteArrayLiteral("\x1b[D"));
    QCOMPARE(normal(Qt::Key_Home), QByteArrayLiteral("\x1b[H"));
    QCOMPARE(normal(Qt::Key_End), QByteArrayLiteral("\x1b[F"));

    QCOMPARE(application(Qt::Key_Up), QByteArrayLiteral("\x1bOA"));
    QCOMPARE(application(Qt::Key_Down), QByteArrayLiteral("\x1bOB"));
    QCOMPARE(application(Qt::Key_Right), QByteArrayLiteral("\x1bOC"));
    QCOMPARE(application(Qt::Key_Left), QByteArrayLiteral("\x1bOD"));
    QCOMPARE(application(Qt::Key_Home), QByteArrayLiteral("\x1bOH"));
    QCOMPARE(application(Qt::Key_End), QByteArrayLiteral("\x1bOF"));

    // A modified cursor key is the parameterised CSI form in BOTH modes: SS3 has
    // nowhere to put the modifier.
    QCOMPARE(normal(Qt::Key_Up, Qt::ControlModifier), QByteArrayLiteral("\x1b[1;5A"));
    QCOMPARE(application(Qt::Key_Up, Qt::ControlModifier),
             QByteArrayLiteral("\x1b[1;5A"));
    QCOMPARE(normal(Qt::Key_Left, Qt::ShiftModifier), QByteArrayLiteral("\x1b[1;2D"));
    QCOMPARE(normal(Qt::Key_Right, Qt::AltModifier), QByteArrayLiteral("\x1b[1;3C"));
    QCOMPARE(normal(Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier),
             QByteArrayLiteral("\x1b[1;6B"));

    // The application-mode flag comes from VtScreen, so DECCKM really does change
    // what the arrow keys send.
    VtScreen screen;
    QCOMPARE(screen.applicationCursorKeys(), false);
    screen.write(QByteArrayLiteral("\x1b[?1h"));
    QCOMPARE(screen.applicationCursorKeys(), true);
    QCOMPARE(vt::encodeKey(Qt::Key_Up, Qt::NoModifier, QString(),
                           screen.applicationCursorKeys()),
             QByteArrayLiteral("\x1bOA"));
    screen.write(QByteArrayLiteral("\x1b[?1l"));
    QCOMPARE(vt::encodeKey(Qt::Key_Up, Qt::NoModifier, QString(),
                           screen.applicationCursorKeys()),
             QByteArrayLiteral("\x1b[A"));
}

void TstVt::encodesEditingAndFunctionKeys()
{
    const auto key = [](int k, Qt::KeyboardModifiers mods = Qt::NoModifier,
                        const QString &text = QString()) {
        return vt::encodeKey(k, mods, text, false);
    };

    QCOMPARE(key(Qt::Key_Insert), QByteArrayLiteral("\x1b[2~"));
    QCOMPARE(key(Qt::Key_Delete), QByteArrayLiteral("\x1b[3~"));
    QCOMPARE(key(Qt::Key_PageUp), QByteArrayLiteral("\x1b[5~"));
    QCOMPARE(key(Qt::Key_PageDown), QByteArrayLiteral("\x1b[6~"));
    QCOMPARE(key(Qt::Key_PageUp, Qt::ShiftModifier), QByteArrayLiteral("\x1b[5;2~"));
    QCOMPARE(key(Qt::Key_Delete, Qt::ControlModifier), QByteArrayLiteral("\x1b[3;5~"));

    // F1-F4 are SS3 in both cursor modes; F5 upwards are tilde sequences, with
    // the historical gaps at 16 and 22.
    QCOMPARE(key(Qt::Key_F1), QByteArrayLiteral("\x1bOP"));
    QCOMPARE(vt::encodeKey(Qt::Key_F1, Qt::NoModifier, QString(), true),
             QByteArrayLiteral("\x1bOP"));
    QCOMPARE(key(Qt::Key_F2), QByteArrayLiteral("\x1bOQ"));
    QCOMPARE(key(Qt::Key_F3), QByteArrayLiteral("\x1bOR"));
    QCOMPARE(key(Qt::Key_F4), QByteArrayLiteral("\x1bOS"));
    QCOMPARE(key(Qt::Key_F5), QByteArrayLiteral("\x1b[15~"));
    QCOMPARE(key(Qt::Key_F6), QByteArrayLiteral("\x1b[17~"));
    QCOMPARE(key(Qt::Key_F7), QByteArrayLiteral("\x1b[18~"));
    QCOMPARE(key(Qt::Key_F8), QByteArrayLiteral("\x1b[19~"));
    QCOMPARE(key(Qt::Key_F9), QByteArrayLiteral("\x1b[20~"));
    QCOMPARE(key(Qt::Key_F10), QByteArrayLiteral("\x1b[21~"));
    QCOMPARE(key(Qt::Key_F11), QByteArrayLiteral("\x1b[23~"));
    QCOMPARE(key(Qt::Key_F12), QByteArrayLiteral("\x1b[24~"));
    QCOMPARE(key(Qt::Key_F4, Qt::ControlModifier), QByteArrayLiteral("\x1b[1;5S"));

    QCOMPARE(key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r")),
             QByteArrayLiteral("\r"));
    QCOMPARE(key(Qt::Key_Enter, Qt::NoModifier, QStringLiteral("\r")),
             QByteArrayLiteral("\r"));
    QCOMPARE(key(Qt::Key_Backspace), QByteArrayLiteral("\x7f"));
    QCOMPARE(key(Qt::Key_Backspace, Qt::ControlModifier), QByteArrayLiteral("\x08"));
    QCOMPARE(key(Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t")),
             QByteArrayLiteral("\t"));
    QCOMPARE(key(Qt::Key_Backtab), QByteArrayLiteral("\x1b[Z"));
    QCOMPARE(key(Qt::Key_Tab, Qt::ShiftModifier), QByteArrayLiteral("\x1b[Z"));
    QCOMPARE(key(Qt::Key_Escape), QByteArrayLiteral("\x1b"));
}

void TstVt::encodesControlAndAltKeys()
{
    const auto ctrl = [](int k) {
        return vt::encodeKey(k, Qt::ControlModifier, QString(), false);
    };

    QCOMPARE(ctrl(Qt::Key_A), QByteArrayLiteral("\x01"));
    QCOMPARE(ctrl(Qt::Key_C), QByteArrayLiteral("\x03"));
    QCOMPARE(ctrl(Qt::Key_D), QByteArrayLiteral("\x04"));
    QCOMPARE(ctrl(Qt::Key_Z), QByteArrayLiteral("\x1a"));
    QCOMPARE(ctrl(Qt::Key_Space), QByteArray(1, '\0'));
    QCOMPARE(ctrl(Qt::Key_At), QByteArray(1, '\0'));
    QCOMPARE(ctrl(Qt::Key_BracketLeft), QByteArrayLiteral("\x1b"));
    QCOMPARE(ctrl(Qt::Key_Backslash), QByteArrayLiteral("\x1c"));
    QCOMPARE(ctrl(Qt::Key_BracketRight), QByteArrayLiteral("\x1d"));
    QCOMPARE(ctrl(Qt::Key_AsciiCircum), QByteArrayLiteral("\x1e"));
    QCOMPARE(ctrl(Qt::Key_Underscore), QByteArrayLiteral("\x1f"));
    QCOMPARE(ctrl(Qt::Key_Question), QByteArrayLiteral("\x7f"));
    // The digit spellings, which are how these codes are reachable on a phone.
    QCOMPARE(ctrl(Qt::Key_2), QByteArray(1, '\0'));
    QCOMPARE(ctrl(Qt::Key_3), QByteArrayLiteral("\x1b"));
    QCOMPARE(ctrl(Qt::Key_8), QByteArrayLiteral("\x7f"));

    // Alt is an ESC prefix.
    QCOMPARE(vt::encodeKey(Qt::Key_B, Qt::AltModifier, QStringLiteral("b"), false),
             QByteArrayLiteral("\x1b""b"));
    QCOMPARE(vt::encodeKey(Qt::Key_Return, Qt::AltModifier, QStringLiteral("\r"), false),
             QByteArrayLiteral("\x1b\r"));
    // Alt+Ctrl combines both: ESC then the control code.
    QCOMPARE(vt::encodeKey(Qt::Key_C, Qt::AltModifier | Qt::ControlModifier, QString(),
                           false),
             QByteArrayLiteral("\x1b\x03"));

    // Plain text passthrough, including multi-byte UTF-8 from a software keyboard.
    QCOMPARE(vt::encodeKey(Qt::Key_A, Qt::NoModifier, QStringLiteral("a"), false),
             QByteArrayLiteral("a"));
    QCOMPARE(vt::encodeKey(Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A"), false),
             QByteArrayLiteral("A"));
    QCOMPARE(vt::encodeKey(0, Qt::NoModifier, QStringLiteral("\u00e9"), false),
             QByteArrayLiteral("\xc3\xa9"));
    // A bare modifier press produces nothing.
    QCOMPARE(vt::encodeKey(Qt::Key_Control, Qt::ControlModifier, QString(), false),
             QByteArray());
    QCOMPARE(vt::encodeKey(Qt::Key_Shift, Qt::ShiftModifier, QString(), false),
             QByteArray());
}

void TstVt::encodesPasteInBothModes()
{
    QCOMPARE(vt::encodePaste(QStringLiteral("hello"), false), QByteArrayLiteral("hello"));
    QCOMPARE(vt::encodePaste(QStringLiteral("hello"), true),
             QByteArrayLiteral("\x1b[200~hello\x1b[201~"));

    // Newlines become CR in both modes; CRLF collapses to one CR so a pasted
    // Windows file does not submit every line twice.
    QCOMPARE(vt::encodePaste(QStringLiteral("a\nb"), false), QByteArrayLiteral("a\rb"));
    QCOMPARE(vt::encodePaste(QStringLiteral("a\r\nb"), false), QByteArrayLiteral("a\rb"));
    QCOMPARE(vt::encodePaste(QStringLiteral("a\r\nb"), true),
             QByteArrayLiteral("\x1b[200~a\rb\x1b[201~"));

    // Tabs survive - pasting indented code must keep its indentation.
    QCOMPARE(vt::encodePaste(QStringLiteral("a\tb"), false), QByteArrayLiteral("a\tb"));

    // An ESC in the payload is REMOVED: otherwise a paste could close the bracket
    // early and have the rest of itself executed.
    QCOMPARE(vt::encodePaste(QStringLiteral("x\x1b[201~y"), true),
             QByteArrayLiteral("\x1b[200~x[201~y\x1b[201~"));
    QCOMPARE(vt::encodePaste(QStringLiteral("a\x07\x01""b"), false),
             QByteArrayLiteral("ab"));

    // UTF-8 payloads pass through unharmed.
    QCOMPARE(vt::encodePaste(QStringLiteral("caf\u00e9"), false),
             QByteArrayLiteral("caf\xc3\xa9"));

    // The mode comes from VtScreen, so DECSET 2004 really does change the wrapping.
    VtScreen screen;
    QCOMPARE(screen.bracketedPaste(), false);
    screen.write(QByteArrayLiteral("\x1b[?2004h"));
    QVERIFY(screen.bracketedPaste());
    QCOMPARE(vt::encodePaste(QStringLiteral("z"), screen.bracketedPaste()),
             QByteArrayLiteral("\x1b[200~z\x1b[201~"));
}

// A wheel notch is the ONLY way to scroll a program that owns its own history,
// and tmux is exactly that: it runs on the alternate screen, where this engine
// deliberately feeds no scrollback, so a client cannot scroll it by moving a
// local view offset. These are the bytes that do the work.
void TstVt::reportsWheelInTheEncodingTheProgramSelected()
{
    // Nothing asked for events: send nothing, so the caller can fall back to
    // whatever local scrollback it has.
    QCOMPARE(vt::encodeMouseWheel(true, 1, 1, VtMouseEncoding::None),
             QByteArray());

    // SGR (1006). Button 64 is wheel up, 65 is wheel down, and both are a PRESS
    // ('M'); a wheel has no release, so the lower-case 'm' form never appears.
    QCOMPARE(vt::encodeMouseWheel(true, 12, 34, VtMouseEncoding::Sgr),
             QByteArrayLiteral("\x1b[<64;12;34M"));
    QCOMPARE(vt::encodeMouseWheel(false, 12, 34, VtMouseEncoding::Sgr),
             QByteArrayLiteral("\x1b[<65;12;34M"));
    // SGR is decimal text, so a coordinate past the legacy ceiling is exact.
    QCOMPARE(vt::encodeMouseWheel(true, 300, 999, VtMouseEncoding::Sgr),
             QByteArrayLiteral("\x1b[<64;300;999M"));

    // Legacy: CSI M then three bytes, each offset by 32. 32+64 = 96 = '`',
    // 32+1 = 33 = '!'.
    QCOMPARE(vt::encodeMouseWheel(true, 1, 1, VtMouseEncoding::Legacy),
             QByteArrayLiteral("\x1b[M`!!"));
    QCOMPARE(vt::encodeMouseWheel(false, 1, 1, VtMouseEncoding::Legacy),
             QByteArrayLiteral("\x1b[Ma!!"));

    // A coordinate that cannot survive the packing is CLAMPED to 223, not
    // wrapped. A wrapped column reports the wheel at a different cell, which in
    // a split tmux window scrolls the wrong pane - silently.
    const QByteArray clamped =
        vt::encodeMouseWheel(true, 400, 400, VtMouseEncoding::Legacy);
    QCOMPARE(clamped.size(), 6);
    QCOMPARE(static_cast<unsigned char>(clamped.at(4)), 255u);  // 32 + 223
    QCOMPARE(static_cast<unsigned char>(clamped.at(5)), 255u);

    // Coordinates are 1-based on the wire; a caller that computed 0 or a
    // negative from a press above the grid must not emit row 0.
    QCOMPARE(vt::encodeMouseWheel(true, 0, -3, VtMouseEncoding::Sgr),
             QByteArrayLiteral("\x1b[<64;1;1M"));
}

// DECSET 1006 selects an ENCODING; 1000/1002/1003 are what ask for events at
// all. Conflating them - "any mouse bit is set" - would send SGR to a program in
// the legacy mode, which receives no wheel whatsoever, and would report events
// to a program that set only 1006 and asked for none.
void TstVt::sgrModeIsIndependentOfTheModesThatAskForEvents()
{
    VtScreen screen;
    screen.resize(80, 24);
    QCOMPARE(screen.mouseEncoding(), VtMouseEncoding::None);

    // 1006 ALONE: an encoding was chosen, but nothing was requested.
    screen.write(csi("?1006h"));
    QVERIFY(screen.mouseTrackingEnabled());  // the aggregate is true...
    QCOMPARE(screen.mouseEncoding(), VtMouseEncoding::None);  // ...this is not

    // Now a program asks for events, and the earlier 1006 decides the form.
    screen.write(csi("?1000h"));
    QCOMPARE(screen.mouseEncoding(), VtMouseEncoding::Sgr);

    // Turning SGR off leaves the request standing, in the legacy form.
    screen.write(csi("?1006l"));
    QCOMPARE(screen.mouseEncoding(), VtMouseEncoding::Legacy);

    // Any of the three request modes is enough on its own.
    screen.write(csi("?1000l") + csi("?1002h"));
    QCOMPARE(screen.mouseEncoding(), VtMouseEncoding::Legacy);
    screen.write(csi("?1002l") + csi("?1003h"));
    QCOMPARE(screen.mouseEncoding(), VtMouseEncoding::Legacy);
    screen.write(csi("?1003l"));
    QCOMPARE(screen.mouseEncoding(), VtMouseEncoding::None);

    // This is what tmux actually sets, and it must resolve to SGR.
    screen.write(csi("?1000h") + csi("?1002h") + csi("?1006h"));
    QCOMPARE(screen.mouseEncoding(), VtMouseEncoding::Sgr);

    // A reset clears the lot, so a program that exits without tidying up does
    // not leave the next one being reported to.
    screen.write(QByteArrayLiteral("\x1b" "c"));
    QCOMPARE(screen.mouseEncoding(), VtMouseEncoding::None);
}

QTEST_GUILESS_MAIN(TstVt)
#include "tst_vt.moc"
