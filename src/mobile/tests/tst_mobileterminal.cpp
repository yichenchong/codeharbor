#include <QtTest/QtTest>

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QIODevice>
#include <QImage>
#include <QMetaMethod>
#include <QPainter>
#include <QSignalSpy>
#include <QStringList>

#include <cmath>
#include <cstring>

#include "MobileTerminalSession.h"
#include "MobileTerminalView.h"
#include "SessionState.h"
#include "TerminalController.h"
#include "TerminalFactory.h"
#include "VtScreen.h"

using namespace ch;

namespace {

// Stand-in for the production PTY transport (ch::SshChannelDevice) at the seam
// the controller actually depends on, borrowed unchanged in spirit from
// src/terminal/tests/tst_terminalcontroller.cpp: a sequential QIODevice that can
// be handed bytes as if a remote PTY had produced them and records what was
// written back.
//
// NOT a QBuffer, and the reason is the same as it is there: a QBuffer's write()
// makes the same bytes readable, so every keystroke this test sends would come
// straight back as terminal output and every assertion about the screen would be
// measuring the test's own echo.
class FakeChannel : public QIODevice {
public:
    FakeChannel() { open(QIODevice::ReadWrite | QIODevice::Unbuffered); }

    void pushRemote(const QByteArray &bytes)
    {
        m_incoming.append(bytes);
        emit readyRead();
    }

    const QByteArray &written() const { return m_written; }
    void clearWritten() { m_written.clear(); }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override
    {
        return m_incoming.size() + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 taken = qMin<qint64>(maxSize, m_incoming.size());
        if (taken > 0) {
            std::memcpy(data, m_incoming.constData(), static_cast<size_t>(taken));
            m_incoming.remove(0, taken);
        }
        return taken;
    }
    qint64 writeData(const char *data, qint64 maxSize) override
    {
        m_written.append(data, maxSize);
        return maxSize;
    }

private:
    QByteArray m_incoming;
    QByteArray m_written;
};

// Text of one absolute row, trailing blanks trimmed, straight out of the engine's
// own extraction path so the assertions read like what a user would copy.
QString rowText(VtScreen *screen, int absoluteRow)
{
    return screen->textRange(absoluteRow, 0, absoluteRow, screen->columns());
}

// A factory that reports a live connection without having one — the only
// environmental gate ch::TerminalFactory makes virtual — plus a way to hand a
// pane an answer it never asked for. Both halves exist to reach exactly one
// branch: what a session does with a targetResolved() it is not waiting for.
class AnnouncingFactory : public TerminalFactory {
public:
    AnnouncingFactory()
        : TerminalFactory(nullptr)
    {
    }

    bool connected() const override { return true; }

    void announce(TerminalController *controller, const QString &target)
    {
        emit targetResolved(controller, target);
    }
};

// Drive one paint, which is also what CLEARS the pending damage, and hand back
// what was drawn. A real QQuickPaintedItem gets this from the scene graph;
// without a window the test calls it directly, painting into an image of the
// item's size.
QImage paintOnce(MobileTerminalView &view)
{
    QImage image(qMax(1, static_cast<int>(view.width())),
                 qMax(1, static_cast<int>(view.height())),
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    view.paint(&painter);
    return image;
}

} // namespace

class TstMobileTerminal : public QObject {
    Q_OBJECT

private slots:
    void acknowledgesTheExactPtyByteCountAcrossSplitFlushes();
    void creditWindowSurvivesAStreamOfMultiByteOutput();
    void visibilityGatesFlushesAndReplaysWhatWasRetained();
    void aHiddenReplayLargerThanTheCreditWindowArrivesInOrder();
    void sendKeyEncodesControlAndArrowsInBothCursorModes();
    void pasteBracketsOnlyWhenTheRemoteAskedForIt();
    void aSwipeScrollsAnAttachedTmuxThroughWheelReports();
    void answerbackFromTheScreenReachesThePty();
    void closeStopsFlushingAndNeverExposesKill();
    void anAnswerThisPaneIsNotWaitingForIsNeverAttached();
    void resizeNeverOutgrowsTheGridTheScreenCanHold();
    void gridIsDerivedFromTheFontMetric();
    void singleRowDamageDoesNotDirtyTheWholeGrid();
    void aScrolledBackViewStaysPinnedWhileOutputArrives();
    void theCursorIsFilledOnlyWhileThePaneOwnsTheKeyboard();
};

// The flow-control round trip ch::TerminalBridge used to carry through the page.
// A batch is acknowledged in PTY BYTES, so a stream split anywhere — including
// inside a multi-byte character — leaves nothing outstanding.
void TstMobileTerminal::acknowledgesTheExactPtyByteCountAcrossSplitFlushes()
{
    MobileTerminalSession session(nullptr);
    FakeChannel channel;
    session.controller()->setTransport(&channel);

    // "hé→!" as UTF-8, fed ONE BYTE AT A TIME so that the two-byte é and the
    // three-byte arrow are each split across arrivals. The controller retains an
    // incomplete character rather than flushing it, and charges only the bytes it
    // did emit; a renderer that acknowledged DECODED CHARACTERS would under-report
    // every one of these flushes and leak credit until the pane went silent.
    const QByteArray text = QStringLiteral("h\u00e9\u2192!").toUtf8();
    QCOMPARE(text.size(), qsizetype(7));
    for (qsizetype at = 0; at < text.size(); ++at) {
        session.controller()->ingestOutput(text.mid(at, 1));
        // Zero after every single flush, not merely at the end: an off-by-a-few
        // accounting bug is invisible in a total and obvious here.
        QTRY_COMPARE(session.controller()->unacknowledgedBytes(), 0);
    }
    QTRY_COMPARE(rowText(session.screen(), session.screen()->firstScreenRow()),
                 QStringLiteral("h\u00e9\u2192!"));
    QCOMPARE(session.controller()->unacknowledgedBytes(), 0);
    // Nothing was withheld: with an exact acknowledgement the credit window never
    // closes, so the rolling buffer stays empty.
    QVERIFY(session.controller()->hiddenBuffer().isEmpty());
}

// The behavioural consequence of the unit being right: more output than the
// credit window, all of it multi-byte, still arrives in full. A per-flush leak of
// (bytes - characters) would exhaust kMaxUnacknowledgedBytes and the tail of the
// stream would never be drawn.
void TstMobileTerminal::creditWindowSurvivesAStreamOfMultiByteOutput()
{
    MobileTerminalSession session(nullptr);
    session.resize(20, 4);

    const QByteArray arrows = QByteArray("\xe2\x86\x92").repeated(20);  // 20 x U+2192
    const int chunks = (TerminalController::kMaxUnacknowledgedBytes / arrows.size()) + 8;
    for (int i = 0; i < chunks; ++i) {
        session.controller()->ingestOutput(arrows);
        session.controller()->ingestOutput(QByteArray("\r\n"));
    }
    QTRY_COMPARE(session.controller()->unacknowledgedBytes(), 0);
    QVERIFY(session.controller()->hiddenBuffer().isEmpty());

    // The LAST line the remote printed is on screen, which is what "nothing was
    // dropped on the way" looks like from the user's side.
    const QString expected = QString(20, QChar(0x2192));
    // Steady state with a 4-row screen: the last completed line sits one row above
    // the blank row the trailing line feed left the cursor on.
    QCOMPARE(rowText(session.screen(), session.screen()->firstScreenRow() + 2), expected);
}

void TstMobileTerminal::visibilityGatesFlushesAndReplaysWhatWasRetained()
{
    MobileTerminalSession session(nullptr);
    session.resize(20, 4);
    const int firstRow = session.screen()->firstScreenRow();

    session.setVisible(false);
    session.controller()->ingestOutput(QByteArray("hidden"));
    QTRY_VERIFY(!session.controller()->hiddenBuffer().isEmpty());
    // A hidden pane draws nothing: the bytes are in the controller's bounded
    // rolling buffer, not in the screen.
    QCOMPARE(rowText(session.screen(), firstRow), QString());

    session.setVisible(true);
    QTRY_COMPARE(rowText(session.screen(), firstRow), QStringLiteral("hidden"));
    QCOMPARE(session.controller()->unacknowledgedBytes(), 0);
    QVERIFY(session.controller()->hiddenBuffer().isEmpty());
}

// ORDER, which is the other half of the flow-control contract and the one a
// "tidy-up" is most likely to break.
//
// ch::MobileTerminalSession::onFlushReady() writes to the screen and THEN
// acknowledges, because acknowledging releases the next retained slice
// SYNCHRONOUSLY — TerminalController::acknowledgeOutput() calls
// releaseRetained(), which emits flushReady() again and re-enters the handler.
// Acknowledging first would therefore recurse all the way down the retained
// buffer before writing anything, and the batches would reach the screen in
// REVERSE. This test makes a replay that the credit window cannot carry in one
// batch and then reads consecutive lines off the bottom of the grid.
void TstMobileTerminal::aHiddenReplayLargerThanTheCreditWindowArrivesInOrder()
{
    MobileTerminalSession session(nullptr);
    session.resize(40, 6);

    // Deliberately more than TWO credit windows and still inside the rolling
    // buffer's 2 MiB cap, so the replay is cut into several batches and nothing
    // is evicted before it can be replayed.
    const QByteArray filler(30, 'x');
    const int lineCount = 34000;
    QByteArray stream;
    stream.reserve(lineCount * (filler.size() + 8));
    for (int i = 0; i < lineCount; ++i)
        stream += QByteArray::number(i) + filler + QByteArray("\r\n");
    QVERIFY(stream.size() > 2 * qint64(TerminalController::kMaxUnacknowledgedBytes));
    QVERIFY(stream.size() < TerminalController::kHiddenBufferMaxBytes);

    QSignalSpy flushes(session.controller(), &TerminalController::flushReady);
    session.setVisible(false);
    session.controller()->ingestOutput(stream);
    QTRY_VERIFY(!session.controller()->hiddenBuffer().isEmpty());
    // Nothing was handed to a hidden renderer, so nothing is outstanding either.
    QCOMPARE(flushes.count(), 0);

    session.setVisible(true);
    QTRY_VERIFY(session.controller()->hiddenBuffer().isEmpty());
    QCOMPARE(session.controller()->unacknowledgedBytes(), 0);
    // The credit window really did cut the replay: one batch would prove nothing
    // about the re-entrant release path this test exists for.
    QVERIFY(flushes.count() > 1);

    const QString tail = QString::fromLatin1(filler);
    const int firstRow = session.screen()->firstScreenRow();
    // The trailing line feed left the cursor on a fresh blank row, so the last
    // completed line is one above it. CONSECUTIVE rows, because a single correct
    // last line could survive a stream that arrived out of order.
    QCOMPARE(rowText(session.screen(), firstRow + 4),
             QString::number(lineCount - 1) + tail);
    QCOMPARE(rowText(session.screen(), firstRow + 3),
             QString::number(lineCount - 2) + tail);
    QCOMPARE(rowText(session.screen(), firstRow + 2),
             QString::number(lineCount - 3) + tail);
}

void TstMobileTerminal::sendKeyEncodesControlAndArrowsInBothCursorModes()
{
    MobileTerminalSession session(nullptr);
    FakeChannel channel;
    session.controller()->setTransport(&channel);

    session.sendKey(Qt::Key_C, Qt::ControlModifier, QStringLiteral("c"));
    QCOMPARE(channel.written(), QByteArray("\x03"));

    // THE MAPPING TerminalPage.qml's IME COMMIT PATH DEPENDS ON. An
    // input-method commit carries text and no key, so the page derives the key
    // from the character by folding a lowercase ASCII letter to its uppercase
    // code point — which is only correct because a Qt::Key value for an ASCII
    // printable IS that code point. Pinned here because the derivation lives in
    // QML, where nothing else would notice if the identity ever stopped holding
    // and a latched Ctrl-C silently started typing a literal "c".
    QCOMPARE(int(Qt::Key_C), int('c') - 0x20);
    channel.clearWritten();
    session.sendKey(int('c') - 0x20, Qt::ControlModifier, QStringLiteral("c"));
    QCOMPARE(channel.written(), QByteArray("\x03"));

    channel.clearWritten();
    session.sendKey(Qt::Key_Up, Qt::NoModifier, QString());
    QCOMPARE(channel.written(), QByteArray("\x1b[A"));

    // DECCKM. The mode lives on the SCREEN, and the session must read it there on
    // every keystroke: a cached copy would keep sending CSI A into a program that
    // has switched the keypad, which types a literal "[A" into vi.
    session.controller()->ingestOutput(QByteArray("\x1b[?1h"));
    QTRY_VERIFY(session.screen()->applicationCursorKeys());
    channel.clearWritten();
    session.sendKey(Qt::Key_Up, Qt::NoModifier, QString());
    QCOMPARE(channel.written(), QByteArray("\x1bOA"));

    session.controller()->ingestOutput(QByteArray("\x1b[?1l"));
    QTRY_VERIFY(!session.screen()->applicationCursorKeys());
    channel.clearWritten();
    session.sendKey(Qt::Key_Down, Qt::NoModifier, QString());
    QCOMPARE(channel.written(), QByteArray("\x1b[B"));

    // Plain text rides through unencoded, which is what the key bar's punctuation
    // keys and an IME commit both depend on.
    channel.clearWritten();
    session.sendText(QStringLiteral("ls -la\n"));
    QCOMPARE(channel.written(), QByteArray("ls -la\n"));
}

void TstMobileTerminal::pasteBracketsOnlyWhenTheRemoteAskedForIt()
{
    MobileTerminalSession session(nullptr);
    FakeChannel channel;
    session.controller()->setTransport(&channel);

    session.paste(QStringLiteral("echo hi"));
    QCOMPARE(channel.written(), QByteArray("echo hi"));

    session.controller()->ingestOutput(QByteArray("\x1b[?2004h"));
    QTRY_VERIFY(session.screen()->bracketedPaste());
    channel.clearWritten();
    session.paste(QStringLiteral("echo hi"));
    QCOMPARE(channel.written(), QByteArray("\x1b[200~echo hi\x1b[201~"));
}

// The bug this feature exists for, reproduced at the layer that has it.
//
// tmux runs on the ALTERNATE screen. VtScreen deliberately feeds no scrollback
// from there, so totalLines() never exceeds the visible rows, MobileTerminalView
// clamps its offset to 0, and a swipe moved nothing at all - which is exactly
// what "scrolling does not work on the phone" was. tmux owns that history and
// reveals it only on a wheel event, which is why the session enables `mouse on`.
//
// A synthetic PRIMARY-screen fixture would pass while proving none of this, so
// this test puts the screen where the real one is: alt active, tmux's own modes
// set, and history provably empty.
void TstMobileTerminal::aSwipeScrollsAnAttachedTmuxThroughWheelReports()
{
    MobileTerminalSession session(nullptr);
    FakeChannel channel;
    session.controller()->setTransport(&channel);
    session.resize(80, 24);

    // Before any program asks: no reports, so the caller keeps its local
    // scrollback behaviour and we say so by returning false.
    QVERIFY(!session.sendMouseWheel(1, 5, 5));
    QCOMPARE(channel.written(), QByteArray());

    // What `tmux attach` actually does: alternate screen, mouse on, SGR.
    session.controller()->ingestOutput(QByteArrayLiteral(
        "\x1b[?1049h\x1b[?1000h\x1b[?1002h\x1b[?1006h"));
    QTRY_VERIFY(session.screen()->altScreenActive());
    QCOMPARE(session.screen()->mouseEncoding(), VtMouseEncoding::Sgr);

    // Fill the screen and then some. On the alt screen NONE of it becomes
    // history: this is the assertion that pins why a local offset cannot work.
    for (int line = 0; line < 200; ++line)
        session.controller()->ingestOutput(QByteArrayLiteral("filler\r\n"));
    QTRY_COMPARE(session.screen()->historyLines(), 0);
    QCOMPARE(session.screen()->totalLines(), 24);

    // A swipe up by three notches, at a cell inside the pane.
    channel.clearWritten();
    QVERIFY(session.sendMouseWheel(3, 7, 9));
    QCOMPARE(channel.written(),
             QByteArray("\x1b[<64;7;9M\x1b[<64;7;9M\x1b[<64;7;9M"));

    // And down, which is the other button.
    channel.clearWritten();
    QVERIFY(session.sendMouseWheel(-2, 7, 9));
    QCOMPARE(channel.written(), QByteArray("\x1b[<65;7;9M\x1b[<65;7;9M"));

    // A program in the legacy mode gets the legacy bytes, not SGR - sending SGR
    // there produces no wheel at all.
    session.controller()->ingestOutput(QByteArrayLiteral("\x1b[?1006l"));
    QTRY_COMPARE(session.screen()->mouseEncoding(), VtMouseEncoding::Legacy);
    channel.clearWritten();
    QVERIFY(session.sendMouseWheel(1, 1, 1));
    QCOMPARE(channel.written(), QByteArray("\x1b[M`!!"));

    // Zero notches is not an event.
    channel.clearWritten();
    QVERIFY(!session.sendMouseWheel(0, 1, 1));
    QCOMPARE(channel.written(), QByteArray());

    // One flick cannot queue an unbounded burst on the same channel as typing.
    channel.clearWritten();
    QVERIFY(session.sendMouseWheel(10000, 1, 1));
    QCOMPARE(channel.written().size(),
             MobileTerminalSession::kMaxWheelNotchesPerGesture * 6);

    // When tmux detaches and the shell is back on the primary screen, reporting
    // stops and the local scrollback - which is real there - takes over again.
    session.controller()->ingestOutput(
        QByteArrayLiteral("\x1b[?1000l\x1b[?1002l\x1b[?1049l"));
    QTRY_VERIFY(!session.screen()->altScreenActive());
    QCOMPARE(session.screen()->mouseEncoding(), VtMouseEncoding::None);
    channel.clearWritten();
    QVERIFY(!session.sendMouseWheel(2, 1, 1));
    QCOMPARE(channel.written(), QByteArray());
}

// A Device Status Report is a QUESTION, and a terminal that never answers hangs
// the asker. The screen produces the answer and the session is what puts it on
// the PTY.
void TstMobileTerminal::answerbackFromTheScreenReachesThePty()
{
    MobileTerminalSession session(nullptr);
    FakeChannel channel;
    session.controller()->setTransport(&channel);
    session.resize(20, 4);

    session.controller()->ingestOutput(QByteArray("\x1b[3;5H\x1b[6n"));
    QTRY_COMPARE(channel.written(), QByteArray("\x1b[3;5R"));
}

void TstMobileTerminal::closeStopsFlushingAndNeverExposesKill()
{
    // A real factory with no connection pool: attach() and detach() are the
    // production ones, and with no attachment recorded detach() is a no-op — which
    // is exactly the point. close() must not reach past detach() and do anything
    // to the remote session itself.
    TerminalFactory factory(nullptr);
    MobileTerminalSession session(&factory);
    FakeChannel channel;
    session.controller()->setTransport(&channel);

    session.close();

    // Nothing was sent to the remote: no `tmux kill-session`, no command at all.
    // Killing a pane destroys every process in it, and a phone's long-press is not
    // a place that may happen.
    QCOMPARE(channel.written(), QByteArray());
    QVERIFY(factory.targetFor(session.controller()).isEmpty());
    // The channel is still open, so the tmux session on the other end is still
    // attachable — that IS the reattach mechanism (SPEC 2.2).
    QVERIFY(channel.isOpen());
    // And the pane stopped drawing: output after a close accumulates in the
    // rolling buffer instead of being handed to a screen whose page is going away.
    session.controller()->ingestOutput(QByteArray("after close"));
    QTRY_VERIFY(!session.controller()->hiddenBuffer().isEmpty());
    QCOMPARE(rowText(session.screen(), session.screen()->firstScreenRow()), QString());

    // The published surface, pinned. QML on mobile renders untrusted remote
    // content, so what a page can call on its own pane is a security property, not
    // an implementation detail: there is no kill, no attach, no tmux target and no
    // transport anywhere on it.
    QStringList callable;
    const QMetaObject *meta = &MobileTerminalSession::staticMetaObject;
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        if (method.methodType() == QMetaMethod::Method
            || method.methodType() == QMetaMethod::Slot) {
            callable << QString::fromUtf8(method.name());
        }
    }
    callable.sort();
    QCOMPARE(callable,
             QStringList({QStringLiteral("close"),
                          QStringLiteral("copyToClipboard"),
                          QStringLiteral("open"),
                          QStringLiteral("paste"),
                          QStringLiteral("pasteFromClipboard"),
                          QStringLiteral("resize"),
                          QStringLiteral("sendKey"),
                          // Scrolls the remote by writing WHEEL REPORTS. It is
                          // on this list because it writes to the PTY, and it is
                          // safe for the same reason paste() is: the page
                          // supplies a notch count and a cell, never bytes.
                          // Sorted between sendKey and sendText: the list is
                          // compared against a sorted enumeration.
                          QStringLiteral("sendMouseWheel"),
                          QStringLiteral("sendText"),
                          QStringLiteral("setVisible")}));
}

// The factory tags its answers with the CONTROLLER and nothing else, so it
// cannot say WHICH question it is answering. A pane must therefore attach an
// answer only while it is still waiting for one: a resolution abandoned by
// close() (or superseded by re-opening this session on another pane) would
// otherwise land afterwards and open a fresh PTY channel behind a pane the user
// has already left — or, worse, put this page on another pane's shell.
void TstMobileTerminal::anAnswerThisPaneIsNotWaitingForIsNeverAttached()
{
    AnnouncingFactory factory;
    MobileTerminalSession session(&factory);

    // Nothing has ever been asked, so nothing may be attached. targetFor() is the
    // witness: ch::TerminalFactory::attach() records the target it AIMED at
    // before anything downstream can fail, so a session that let this through
    // would leave a name here even though no channel could open.
    factory.announce(session.controller(), QStringLiteral("ch_somebody_elses_pane"));
    QVERIFY(factory.targetFor(session.controller()).isEmpty());

    // And after a close(), which is where this actually bites: leaving the page
    // abandons the resolution the attach started, so the answer that arrives a
    // moment later has nobody waiting for it.
    session.open(QStringLiteral("dev-1"), QStringLiteral("terminal-1"),
                 QStringLiteral("row-1"), QString(), 40, 12);
    session.close();
    factory.announce(session.controller(), QStringLiteral("ch_dev_1_row_1"));
    QVERIFY(factory.targetFor(session.controller()).isEmpty());
}

// The local grid and the remote PTY must be told the SAME size. A QML layout
// mid-transition can report an absurd cell count, and clamping the wire value
// somewhere ch::VtScreen::resize() would not follow leaves the remote drawing
// more columns than the screen can hold — every line then wraps in the wrong
// place until some later resize happens to agree.
void TstMobileTerminal::resizeNeverOutgrowsTheGridTheScreenCanHold()
{
    MobileTerminalSession session(nullptr);

    // One dimension at a time, so a clamp applied to only one is caught, and both
    // well past any plausible layout value.
    session.resize(1'000'000, 24);
    QCOMPARE(session.screen()->columns(), MobileTerminalSession::kMaxColumns);
    QCOMPARE(session.controller()->columns(), session.screen()->columns());
    QCOMPARE(session.controller()->rows(), session.screen()->rows());

    session.resize(80, 1'000'000);
    QCOMPARE(session.screen()->rows(), MobileTerminalSession::kMaxRows);
    QCOMPARE(session.controller()->columns(), session.screen()->columns());
    QCOMPARE(session.controller()->rows(), session.screen()->rows());

    // The same bound on the open() path, which records geometry for the attach
    // without pushing a window-change.
    MobileTerminalSession opened(nullptr);
    opened.open(QStringLiteral("dev-1"), QStringLiteral("terminal-1"),
                QStringLiteral("row-1"), QString(), 1'000'000, 24);
    QCOMPARE(opened.screen()->columns(), MobileTerminalSession::kMaxColumns);
}

void TstMobileTerminal::gridIsDerivedFromTheFontMetric()
{
    MobileTerminalView view;
    QFont font;
    font.setStyleHint(QFont::Monospace);
    font.setFamily(QStringLiteral("monospace"));
    font.setFixedPitch(true);
    font.setPixelSize(16);
    view.setFont(font);

    const QFontMetricsF metrics(font);
    const qreal advance = metrics.horizontalAdvance(QChar(u'M'));
    QVERIFY(advance > 0.0);
    QVERIFY(metrics.height() > 0.0);
    QCOMPARE(view.cellWidth(), advance);
    QCOMPARE(view.lineHeight(), metrics.height());

    // Deliberately not a whole multiple of a cell: the grid is the number of
    // cells that FIT, so the remainder must be dropped rather than rounded up —
    // a column the remote believes in but the view cannot draw is a column of
    // output the user never sees.
    const qreal width = advance * 37.6;
    const qreal height = metrics.height() * 12.4;
    view.setSize(QSizeF(width, height));
    QCOMPARE(view.columns(), 37);
    QCOMPARE(view.rows(), 12);

    QSignalSpy grid(&view, &MobileTerminalView::gridChanged);
    view.setSize(QSizeF(advance * 20.0, metrics.height() * 6.0));
    QCOMPARE(view.columns(), 20);
    QCOMPARE(view.rows(), 6);
    QCOMPARE(grid.count(), 1);
    // A size change that does not cross a cell boundary is not a grid change:
    // every one of those would otherwise become an SSH window-change and a full
    // remote redraw.
    view.setSize(QSizeF(advance * 20.0 + advance / 4.0, metrics.height() * 6.0));
    QCOMPARE(grid.count(), 1);
}

void TstMobileTerminal::singleRowDamageDoesNotDirtyTheWholeGrid()
{
    MobileTerminalView view;
    QFont font;
    font.setStyleHint(QFont::Monospace);
    font.setFamily(QStringLiteral("monospace"));
    font.setFixedPitch(true);
    font.setPixelSize(16);
    view.setFont(font);
    view.setSize(QSizeF(view.cellWidth() * 40.0, view.lineHeight() * 12.0));
    QCOMPARE(view.rows(), 12);

    VtScreen screen;
    screen.resize(40, 12);
    view.setScreen(&screen);
    // The first paint of a fresh item is legitimately a full one; damage is only
    // interesting afterwards.
    paintOnce(view);
    QVERIFY(view.pendingDamage().isNull());

    // Print into row 5 without scrolling. The cursor leaves row 0 and lands in
    // row 5, so exactly two rows are dirty — and NOT the ten between them.
    screen.write(QByteArray("\x1b[6;1Hx"));
    QCOMPARE(view.pendingDamagedRows(), QList<int>({0, 5}));
    const QRect damage = view.pendingDamage();
    QVERIFY(!damage.isNull());
    // The union band spans rows 0..5 and no further: a whole-grid repaint would
    // reach the bottom of the item.
    QVERIFY(damage.top() <= 0);
    QVERIFY(damage.bottom() < static_cast<int>(view.height()) - 1);
    QVERIFY(damage.height() <= static_cast<int>(std::ceil(6 * view.lineHeight())));

    paintOnce(view);
    QVERIFY(view.pendingDamage().isNull());
    QVERIFY(view.pendingDamagedRows().isEmpty());

    // One more character on the SAME row is one row of damage, cursor included.
    screen.write(QByteArray("y"));
    QCOMPARE(view.pendingDamagedRows(), QList<int>({5}));
    // One row's band, plus the at most two pixels a floor at the top and a ceil at
    // the bottom can add when the line height is fractional.
    QVERIFY(view.pendingDamage().height() <= static_cast<int>(std::ceil(view.lineHeight())) + 2);
}

// A user reading history must not have the page crawl upward under them while a
// build prints. Absolute rows are stable, so the view follows the scroll and
// keeps showing the same content.
void TstMobileTerminal::aScrolledBackViewStaysPinnedWhileOutputArrives()
{
    MobileTerminalView view;
    QFont font;
    font.setStyleHint(QFont::Monospace);
    font.setFamily(QStringLiteral("monospace"));
    font.setFixedPitch(true);
    font.setPixelSize(16);
    view.setFont(font);
    view.setSize(QSizeF(view.cellWidth() * 40.0, view.lineHeight() * 6.0));
    QCOMPARE(view.rows(), 6);

    VtScreen screen;
    screen.resize(40, 6);
    view.setScreen(&screen);
    for (int i = 0; i < 40; ++i)
        screen.write(QByteArray("line") + QByteArray::number(i) + QByteArray("\r\n"));

    view.setScrollOffset(10);
    QCOMPARE(view.scrollOffset(), 10);
    const int pinnedTop = view.topAbsoluteRow();
    const QString pinnedLine = screen.textRange(pinnedTop, 0, pinnedTop, screen.columns());
    QVERIFY(!pinnedLine.isEmpty());

    paintOnce(view);
    screen.write(QByteArray("more\r\nmore\r\n"));

    // The window moved with the history, so the same absolute rows are still on
    // screen and nothing had to be repainted for the scroll itself.
    QCOMPARE(view.topAbsoluteRow(), pinnedTop);
    QCOMPARE(screen.textRange(pinnedTop, 0, pinnedTop, screen.columns()), pinnedLine);
    QCOMPARE(view.scrollOffset(), 12);

    // Returning to the bottom follows the live output again.
    view.setScrollOffset(0);
    QCOMPARE(view.topAbsoluteRow(), screen.totalLines() - view.rows());
}

// A terminal says "your keystrokes go elsewhere" with a hollow cursor and "type
// here" with a filled one, and on this client that distinction cannot come from
// the item's own hasActiveFocus(): the focus lives on the invisible TextInput the
// soft keyboard needs, so this item never has it and the pane drew the hollow
// cursor even while the user was typing. The page reports the state instead.
void TstMobileTerminal::theCursorIsFilledOnlyWhileThePaneOwnsTheKeyboard()
{
    MobileTerminalView view;
    QFont font;
    font.setStyleHint(QFont::Monospace);
    font.setFamily(QStringLiteral("monospace"));
    font.setFixedPitch(true);
    font.setPixelSize(16);
    view.setFont(font);
    view.setSize(QSizeF(view.cellWidth() * 20.0, view.lineHeight() * 4.0));
    // Explicit rather than inherited, so the pixel comparisons below say what
    // they mean instead of depending on the defaults.
    view.setDefaultForegroundColor(QColor(Qt::white));
    view.setDefaultBackgroundColor(QColor(Qt::black));

    VtScreen screen;
    screen.resize(20, 4);
    view.setScreen(&screen);
    QCOMPARE(view.rows(), 4);
    QCOMPARE(view.columns(), 20);

    // The probe below needs a cell with an interior: a 1px outline must not be
    // able to cover the pixel that distinguishes hollow from filled.
    QVERIFY(view.cellWidth() >= 4.0);
    QVERIFY(view.lineHeight() >= 4.0);
    // The middle of the cursor cell: inside a filled block, and inside the empty
    // interior of a hollow one.
    const int probeX = static_cast<int>(view.cellWidth() / 2.0);
    const int probeY = static_cast<int>(view.lineHeight() / 2.0);

    // Default: the pane has not been touched, so the keyboard is elsewhere.
    QVERIFY(!view.cursorFocused());
    QCOMPARE(paintOnce(view).pixelColor(probeX, probeY), QColor(Qt::black));

    view.setCursorFocused(true);
    // One cell changed, so one row was asked for — not the whole grid.
    QCOMPARE(view.pendingDamagedRows(), QList<int>({0}));
    QCOMPARE(paintOnce(view).pixelColor(probeX, probeY), QColor(Qt::white));

    view.setCursorFocused(false);
    QCOMPARE(view.pendingDamagedRows(), QList<int>({0}));
    QCOMPARE(paintOnce(view).pixelColor(probeX, probeY), QColor(Qt::black));

    // And a repeated report is not a change: it must not queue a repaint.
    view.setCursorFocused(false);
    QVERIFY(view.pendingDamagedRows().isEmpty());
}

QTEST_MAIN(TstMobileTerminal)
#include "tst_mobileterminal.moc"
