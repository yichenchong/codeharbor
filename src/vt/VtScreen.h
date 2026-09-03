#pragma once

// The terminal screen model for the mobile client: a grid of cells, a scrollback
// history, cursor and mode state, fed with raw bytes from a PTY.
//
// WHY this exists at all: every rich desktop surface in CodeHarbor is Qt
// WebEngine, and the terminal pane specifically is xterm.js inside it
// (src/qml/TerminalPaneView.qml). Qt WebEngine ships on neither Android nor iOS,
// so on mobile there is no xterm.js and no HTML canvas - the grid has to be
// modelled in C++ and painted by a QQuickPaintedItem. This class is the model
// half of that; ch::MobileTerminalView is the painting half.
//
// WHY it is transport-free: it owns no socket, no channel and no QIODevice. The
// mobile terminal session drives it with the same raw byte batches
// ch::TerminalController already emits through flushReady(QByteArray)
// (src/terminal/TerminalController.h), which is what lets the whole engine be
// tested headlessly with string literals and no SSH.

#include "VtTypes.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

namespace ch {

class VtParser;

class VtScreen : public QObject {
    Q_OBJECT

    Q_PROPERTY(int columns READ columns NOTIFY sizeChanged)
    Q_PROPERTY(int rows READ rows NOTIFY sizeChanged)
    // The cursor properties share cursorMoved(): a renderer that has to repaint
    // the cursor cell does not care WHICH of position or visibility changed, and
    // one signal keeps the per-write emission down to a single wakeup.
    Q_PROPERTY(int cursorColumn READ cursorColumn NOTIFY cursorMoved)
    Q_PROPERTY(int cursorRow READ cursorRow NOTIFY cursorMoved)
    Q_PROPERTY(bool cursorVisible READ cursorVisible NOTIFY cursorMoved)
    Q_PROPERTY(int historyLines READ historyLines NOTIFY historyLinesChanged)
    Q_PROPERTY(QString windowTitle READ windowTitle NOTIFY windowTitleChanged)
    // The four mode properties share modesChanged() for the same reason: they
    // change together (an editor entering the alt screen also turns on
    // application cursor keys and bracketed paste in the same write), and the
    // consumers - the key bar and the key encoder - re-read all of them anyway.
    Q_PROPERTY(bool altScreenActive READ altScreenActive NOTIFY modesChanged)
    Q_PROPERTY(bool applicationCursorKeys READ applicationCursorKeys NOTIFY modesChanged)
    Q_PROPERTY(bool bracketedPaste READ bracketedPaste NOTIFY modesChanged)
    Q_PROPERTY(bool mouseTrackingEnabled READ mouseTrackingEnabled NOTIFY modesChanged)

public:
    // Scrollback cap.
    //
    // 5000 lines, and the number is a memory bound, not a preference. A cell is
    // 16 bytes, so one 200-column line is 3.2 KB and the full history is ~16 MB
    // per pane at that width - already the largest single allocation the mobile
    // client makes, on devices where the OS kills a process for far less. It is
    // also more than enough: tmux is the real scrollback owner here (every pane
    // is a tmux attach, see src/terminal/TerminalFactory.h), so anything older
    // than this is still reachable with tmux's own copy mode. The alternative -
    // an unbounded history - turns a runaway `yes` in one pane into an OOM kill
    // of the whole app.
    static constexpr int kMaxHistoryLines = 5000;

    // Grid bounds. A remote program cannot set these (there is no CSI for
    // resizing this engine), but resize() takes numbers derived from a view's
    // pixel geometry and a font metric, and a zero or absurd result there must
    // not be able to produce an empty or gigabantic grid.
    static constexpr int kMinColumns = 1;
    static constexpr int kMinRows = 1;
    static constexpr int kMaxColumns = 2000;
    static constexpr int kMaxRows = 2000;

    // The sentinel colours a cell carries when the stream has not chosen one.
    // Exposed as statics so a renderer can compare against them without
    // including VtTypes.h's constants directly, and so the comparison is
    // guaranteed to be against the same values the parser writes. See
    // ch::VtDefaultForeground for why a sentinel is needed at all.
    static constexpr QRgb defaultForeground() { return VtDefaultForeground; }
    static constexpr QRgb defaultBackground() { return VtDefaultBackground; }

    explicit VtScreen(QObject *parent = nullptr);
    ~VtScreen() override;

    int columns() const { return m_columns; }
    int rows() const { return m_rows; }
    int cursorColumn() const { return m_cursorX; }
    int cursorRow() const { return m_cursorY; }
    bool cursorVisible() const { return m_cursorVisible; }
    int historyLines() const { return static_cast<int>(m_history.size()); }
    QString windowTitle() const { return m_windowTitle; }
    bool altScreenActive() const { return m_altActive; }
    bool applicationCursorKeys() const { return m_applicationCursorKeys; }
    bool bracketedPaste() const { return m_bracketedPaste; }
    bool mouseTrackingEnabled() const { return m_mouseTracking != 0; }
    // Which report form a wheel or click must take, if any. Kept separate from
    // mouseTrackingEnabled() above, which is the aggregate "any mouse bit set"
    // that drives the modesChanged signal: that aggregate is true for a program
    // which set only 1006, and such a program is selecting an encoding without
    // asking for events. Reporting to it would be wrong, so this returns None.
    VtMouseEncoding mouseEncoding() const;
    bool autowrap() const { return m_autowrap; }

    // Change the grid size.
    //
    // Lines are TRUNCATED (narrower) or PADDED with blanks (wider); they are
    // never reflowed, and rows are added at or removed from the BOTTOM.
    //
    // WHY no reflow: reflow means rewriting the history, and the history here is
    // not the authoritative copy of anything - tmux is (every pane is a tmux
    // attach). A reflow implementation has to track per-line "this line was a
    // continuation" flags, re-split them on every rotation of a phone, and it
    // still cannot restore the double-width and combining-mark decisions the
    // remote program made at the old width. Worse, the remote program is about
    // to be told the new size and will redraw the LIVE screen itself, so reflow
    // would only ever be visible in history, in exchange for the most bug-prone
    // code in any terminal emulator. Truncating is honest, cheap and cannot
    // desynchronise the cursor.
    //
    // The cursor and the scroll region are clamped into the new grid, and the
    // pending-wrap flag is dropped, because a deferred wrap computed at the old
    // width would put the next glyph in the wrong place.
    void resize(int columns, int rows);

    // Feed raw PTY output. Partial UTF-8 sequences and partial escape sequences
    // are RETAINED across calls, so the caller may split the stream anywhere -
    // which it will, since the bytes arrive in whatever chunks the SSH channel
    // produces. All change notifications for one call are coalesced and emitted
    // once, at the end.
    void write(const QByteArray &bytes);

    // The absolute coordinate space: row 0 is the OLDEST RETAINED history line,
    // rows [firstScreenRow(), totalLines()) are the live screen. History
    // eviction shifts existing absolute indices down; a consumer holding
    // absolute rows must re-derive them after historyLinesChanged().
    int totalLines() const { return static_cast<int>(m_history.size()) + m_rows; }
    int firstScreenRow() const { return static_cast<int>(m_history.size()); }

    // Any absolute row in [0, totalLines()). Out-of-range answers a shared blank
    // line of the current width rather than asserting: a renderer painting a
    // viewport races with the stream, and one stale row index must not crash the
    // app.
    const VtLine &lineAt(int absoluteRow) const;

    // Plain text for selection and copy, from (startRow, startCol) inclusive up
    // to (endRow, endCol) EXCLUSIVE, in absolute rows. Trailing blanks are
    // trimmed per line, the covered halves of double-width characters are
    // skipped, and rows are joined with '\n'.
    QString textRange(int startRow, int startCol, int endRow, int endCol) const;

    // RIS: blank primary screen, empty history, default modes and attributes.
    // Also drops any partially received sequence, so a reset genuinely resets
    // the parser and not just the grid.
    void reset();

signals:
    // The rows whose CONTENT changed during one write(), as an inclusive
    // absolute range. Coalesced per write() and never per cell: a full-screen
    // redraw is tens of thousands of cells, and one signal per cell would spend
    // more time in the metaobject system than in the parser.
    void linesChanged(int firstAbsoluteRow, int lastAbsoluteRow);
    // How many lines entered the scrollback during this write, i.e. by how much
    // the live screen moved DOWN the absolute row space. Always positive; it is
    // emitted only when history actually advanced.
    //
    // WHY this and not "the live screen scrolled by N": those are different
    // numbers, and only this one is actionable. A consumer scrolled back into
    // history keeps its place by moving its anchor by exactly this many rows -
    // whether the cap evicted the oldest lines (every absolute index shifted
    // down) or not (the new lines were appended above the live screen), the
    // correction is the same. Scrolling the ALT screen, or a scroll region that
    // starts below row 0, moves content on the live screen without touching the
    // absolute space at all, and reporting those would make a page of scrollback
    // crawl under the user's eyes while a full-screen program redraws. Which rows
    // to repaint is linesChanged()'s job in every case.
    void screenScrolled(int lines);
    void cursorMoved();
    void bell();
    void windowTitleChanged();
    void screenReset();
    void sizeChanged();
    void modesChanged();
    void historyLinesChanged();

    // Bytes the terminal must send BACK to the PTY.
    //
    // WHY this is not optional: DSR (CSI 5n / CSI 6n) is a question, and a
    // terminal that never answers it hangs the asker. `bash` with
    // bracketed-paste-aware prompt redrawing, `zsh`, `vim`'s startup probe and
    // most TUI frameworks issue a cursor-position report and block on the reply.
    // The engine cannot write to the PTY itself (it owns no transport), so it
    // hands the answer to whoever fed it - ch::MobileTerminalSession forwards it
    // to the terminal controller's input path.
    void reply(const QByteArray &bytes);

private:
    // The parser is the only thing that drives the operations below. Making it a
    // friend keeps ~40 mutators off the public API, where a QML consumer could
    // reach them and desynchronise the grid from the stream.
    friend class VtParser;

    struct SavedCursor {
        int x = 0;
        int y = 0;
        quint16 attrs = 0;
        QRgb fg = VtDefaultForeground;
        QRgb bg = VtDefaultBackground;
        bool valid = false;
    };

    // --- parser-facing operations -------------------------------------------
    void printCodePoint(char32_t codePoint);
    void requestBell();
    void backspace();
    void horizontalTab(int count);
    void reverseTab(int count);
    void lineFeed();
    void reverseIndex();
    void nextLine();
    void carriageReturn();
    void setTabStopAtCursor();
    void clearTabStop(int mode);
    void cursorUp(int count);
    void cursorDown(int count);
    void cursorForward(int count);
    void cursorBack(int count);
    void cursorNextLine(int count);
    void cursorPreviousLine(int count);
    void setCursorColumn(int column1Based);
    void setCursorRow(int row1Based);
    void setCursorPosition(int row1Based, int column1Based);
    void eraseInDisplay(int mode);
    void eraseInLine(int mode);
    void insertLines(int count);
    void deleteLines(int count);
    void insertCharacters(int count);
    void deleteCharacters(int count);
    void eraseCharacters(int count);
    void scrollUpLines(int count);
    void scrollDownLines(int count);
    void setScrollRegion(int top1Based, int bottom1Based);
    void applySgr(const int *params, const bool *isSubParam, int count);
    void deviceStatusReport(int mode);
    void setMode(int mode, bool enable, bool privateMode);
    void saveCursor();
    void restoreCursor();
    void alignmentTest();
    void setWindowTitle(const QString &title);

    // --- internals -----------------------------------------------------------
    VtCell blankCell() const;
    VtLine blankLine() const;
    void allocateGrid();
    void resetTabStops();
    void eraseCells(int row, int fromColumn, int toColumn);
    void scrollRegionUp(int count);
    void scrollRegionDown(int count);
    void pushToHistory(VtLine &&line);
    void markDamage(int gridRow);
    void markDamage(int firstGridRow, int lastGridRow);
    void markAllDamaged();
    void moveCursor(int x, int y);
    void clampCursorAndRegion();
    void enterAltScreen(bool clearOnEnter);
    // No parameter: the alt grid is DISCARDED on leave, never retained, so
    // xterm's "clear on leave" distinction between DECSET 47 and 1047 has no
    // observable effect here. Retaining a second full grid per pane would double
    // the resident grid size on a phone to preserve content that every program
    // using those modes redraws from scratch on re-entry anyway.
    void leaveAltScreen();
    void softReset();
    // RIS. Separate from the public reset() because the parser calls it from
    // inside a feed, where resetting the parser as well would clobber the state
    // the caller is about to set.
    void hardReset();
    void resetAttributes();
    void applySimpleSgr(int code);
    // Fold a zero-width mark into the cell that precedes the cursor.
    void attachCombiningMark(char32_t codePoint);
    void clearWholeGrid();
    void resizeLine(VtLine &line) const;
    void emitPending();
    // Reduce the cell at (row, column) to a blank if it is one half of a
    // double-width character whose other half is gone. Called on each seam an
    // edit could have torn, AFTER the edit; see the definition for why the order
    // matters.
    void demoteOrphanCell(int row, int column);

    VtParser *m_parser = nullptr;

    int m_columns = 80;
    int m_rows = 24;

    // The live screen, always exactly m_rows lines of m_columns cells. When the
    // alt screen is active this holds the ALT grid and m_primaryGrid holds the
    // primary one, so switching back is a swap and never a redraw.
    QList<VtLine> m_grid;
    QList<VtLine> m_primaryGrid;
    // QList (not std::deque) because Qt 6's QList grows and shrinks at both ends
    // in O(1) amortised, which is exactly the access pattern here: append at the
    // back on every scroll, removeFirst() at the cap.
    QList<VtLine> m_history;
    // Answer for an out-of-range lineAt(): a blank line of the current width,
    // kept as a member so the accessor can return a reference without a
    // per-call allocation.
    VtLine m_outOfRangeLine;

    int m_cursorX = 0;
    int m_cursorY = 0;
    // Deferred wrap: a glyph written into the last column leaves the cursor ON
    // that column with this flag set, and only the NEXT printable glyph performs
    // the wrap. Without it, a program that fills the last column of the last row
    // would scroll the screen before it had any reason to, losing a line every
    // time - and a CR or a cursor move after that glyph must cancel the wrap
    // rather than honour it.
    bool m_pendingWrap = false;

    int m_scrollTop = 0;
    int m_scrollBottom = 23;

    quint16 m_attrs = 0;
    QRgb m_fg = VtDefaultForeground;
    QRgb m_bg = VtDefaultBackground;

    bool m_cursorVisible = true;
    bool m_autowrap = true;
    bool m_applicationCursorKeys = false;
    bool m_bracketedPaste = false;
    bool m_altActive = false;
    // A bitmask of the DECSET mouse modes (1000/1002/1003/1006) that are on, so
    // turning one off does not clear the others. Only the aggregate reaches QML.
    quint16 m_mouseTracking = 0;

    QString m_windowTitle;
    QList<bool> m_tabStops;

    SavedCursor m_savedCursor;
    SavedCursor m_savedCursorAlt;

    // Coalesced per-write() notification state. Damage is tracked in GRID rows
    // and converted to absolute rows at emit time, after history growth for the
    // whole write is known.
    int m_damageTop = -1;
    int m_damageBottom = -1;
    // Lines that entered the scrollback during this write. Both the value
    // screenScrolled() carries and the distance linesChanged() has to reach above
    // the live screen, because they are the same quantity: a line that became
    // history holds new content at an absolute index that was not history when it
    // was written.
    int m_historyAdvance = 0;
    // Set when the content of the RETAINED history rows changed rather than grown
    // - only a width change does that, since every line is truncated or padded -
    // so the damage range has to cover the whole history and not just the rows
    // m_historyAdvance accounts for.
    bool m_historyContentDirty = false;
    // Set whenever history GREW, shrank, or was evicted. A plain size comparison
    // is not enough: appending one line and evicting one at the cap leaves the
    // count identical while shifting every absolute row index by one.
    bool m_historyDirty = false;
    bool m_cursorDirty = false;
    bool m_bellPending = false;
    bool m_titleDirty = false;
    bool m_modesDirty = false;
    bool m_sizeDirty = false;
    bool m_resetPending = false;
    // True while write() is running, so a resize() or reset() triggered from a
    // reply() handler joins the write's coalesced batch instead of emitting a
    // half-finished state. reply() is the only signal this class emits mid-feed,
    // which is exactly why it is the only re-entry point that needs guarding.
    bool m_inWrite = false;
};

} // namespace ch
