#include "VtScreen.h"

#include "VtParser.h"

#include <QChar>
#include <QStringView>

#include <utility>

namespace ch {
namespace {

// Tab stops every 8 columns, the hardware default every shell assumes.
constexpr int kTabWidth = 8;

// Bit positions inside VtScreen::m_mouseTracking. Only the aggregate ("is any
// mouse reporting on") reaches QML, but the individual bits must be kept apart
// so that a program turning 1006 off does not also turn 1002 off.
enum MouseModeBit : quint16 {
    MouseClick = 1 << 0,   // DECSET 1000
    MouseDrag = 1 << 1,    // DECSET 1002
    MouseMotion = 1 << 2,  // DECSET 1003
    MouseSgr = 1 << 3      // DECSET 1006
};

int clampInt(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

void appendCodePoint(QString &out, char32_t codePoint)
{
    // No temporary QString per glyph: fromUcs4() yields a small stack struct of
    // one or two UTF-16 units.
    const auto encoded = QChar::fromUcs4(codePoint);
    out.append(reinterpret_cast<const QChar *>(encoded.begin()),
               static_cast<qsizetype>(encoded.size()));
}

// Decode one extended-colour specification in its COLON form, where the exact
// number of sub-parameters is known: "38:5:n" or "38:2:r:g:b", plus the
// "38:2::r:g:b" spelling in which the (always empty) colour-space id is present.
bool parseColonColour(const int *subs, int subCount, QRgb *out)
{
    if (subCount < 1)
        return false;
    const int selector = subs[0] < 0 ? 0 : subs[0];
    if (selector == 5) {
        if (subCount < 2 || subs[1] < 0 || subs[1] > 255)
            return false;
        *out = vt::xterm256Color(subs[1]);
        return true;
    }
    if (selector == 2) {
        if (subCount < 4)
            return false;
        // The last three sub-parameters are always the channels, which covers
        // both the 4-element and the colour-space-id 5-element spelling.
        const int r = subs[subCount - 3];
        const int g = subs[subCount - 2];
        const int b = subs[subCount - 1];
        if (r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255)
            return false;
        *out = qRgb(r, g, b);
        return true;
    }
    return false;
}

} // namespace

VtScreen::VtScreen(QObject *parent)
    : QObject(parent)
    , m_parser(new VtParser(this))
{
    allocateGrid();
    resetTabStops();
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
}

VtScreen::~VtScreen()
{
    delete m_parser;
}

// --- geometry ---------------------------------------------------------------

VtCell VtScreen::blankCell() const
{
    // Back-colour erase: an erased cell keeps the CURRENT background, which is
    // what makes `clear` inside a program that set a background paint the whole
    // screen in that background rather than in the theme's. The foreground and
    // the rendition are reset, because a space has no foreground and a lingering
    // underline on erased cells is visible as a stray rule.
    VtCell cell;
    cell.text = U' ';
    cell.width = 1;
    cell.fg = VtDefaultForeground;
    cell.bg = m_bg;
    cell.attrs = 0;
    return cell;
}

VtLine VtScreen::blankLine() const
{
    return VtLine(m_columns, blankCell());
}

void VtScreen::allocateGrid()
{
    const VtLine blank = blankLine();
    m_grid.clear();
    m_grid.reserve(m_rows);
    for (int row = 0; row < m_rows; ++row)
        m_grid.append(blank);
    m_outOfRangeLine = blank;
}

void VtScreen::clearWholeGrid()
{
    const VtLine blank = blankLine();
    for (int row = 0; row < m_grid.size(); ++row)
        m_grid[row] = blank;
    markAllDamaged();
}

void VtScreen::resetTabStops()
{
    m_tabStops.resize(m_columns);
    m_tabStops.fill(false);
    for (int column = 0; column < m_columns; column += kTabWidth)
        m_tabStops[column] = true;
}

void VtScreen::resizeLine(VtLine &line) const
{
    const qsizetype before = line.size();
    line.resize(m_columns);
    if (m_columns > before) {
        // QVector value-initialises the new cells, which VtCell's member
        // initialisers make a proper blank; only the background has to be taken
        // from the erase colour.
        for (qsizetype column = before; column < m_columns; ++column)
            line[column].bg = m_bg;
    } else if (m_columns > 0 && m_columns < before) {
        // A truncation may have cut a double-width character in half, leaving a
        // leading cell whose partner is gone. Demote it, or a renderer would
        // draw a two-column glyph in one column.
        VtCell &last = line[m_columns - 1];
        if (last.width == 2) {
            last.text = U' ';
            last.width = 1;
        }
    }
}

void VtScreen::resize(int columns, int rows)
{
    const int newColumns = clampInt(columns, kMinColumns, kMaxColumns);
    const int newRows = clampInt(rows, kMinRows, kMaxRows);
    if (newColumns == m_columns && newRows == m_rows)
        return;

    const int oldRows = m_rows;
    const int oldColumns = m_columns;
    m_columns = newColumns;
    m_rows = newRows;

    for (VtLine &line : m_history)
        resizeLine(line);
    for (VtLine &line : m_grid)
        resizeLine(line);
    for (VtLine &line : m_primaryGrid)
        resizeLine(line);

    // Rows are added to, and removed from, the BOTTOM. Removing from the bottom
    // discards those lines rather than pushing them into history: they are below
    // the cursor in every realistic case (a shell prompt sits at the top of the
    // empty part of the screen), and the remote program is about to redraw at the
    // new size anyway. Taking them from the top instead would move the content
    // the user is looking at.
    const VtLine blank = blankLine();
    while (m_grid.size() > m_rows)
        m_grid.removeLast();
    while (m_grid.size() < m_rows)
        m_grid.append(blank);
    if (!m_primaryGrid.isEmpty()) {
        while (m_primaryGrid.size() > m_rows)
            m_primaryGrid.removeLast();
        while (m_primaryGrid.size() < m_rows)
            m_primaryGrid.append(blank);
    }

    m_outOfRangeLine = blank;

    // Tab stops belong to the width, so they are rebuilt rather than clamped: a
    // stop past the new right edge is unreachable, and a new column past the old
    // edge would otherwise have no stop at all.
    if (m_columns != m_tabStops.size())
        resetTabStops();

    // A deferred wrap computed at the old width would put the next glyph in the
    // wrong column.
    m_pendingWrap = false;
    const bool regionWasFullScreen = (m_scrollTop == 0 && m_scrollBottom == oldRows - 1);
    if (regionWasFullScreen) {
        m_scrollTop = 0;
        m_scrollBottom = m_rows - 1;
    }
    clampCursorAndRegion();

    m_savedCursor.x = clampInt(m_savedCursor.x, 0, m_columns - 1);
    m_savedCursor.y = clampInt(m_savedCursor.y, 0, m_rows - 1);
    m_savedCursorAlt.x = clampInt(m_savedCursorAlt.x, 0, m_columns - 1);
    m_savedCursorAlt.y = clampInt(m_savedCursorAlt.y, 0, m_rows - 1);

    m_sizeDirty = true;
    m_cursorDirty = true;
    markAllDamaged();
    if (newColumns != oldColumns) {
        // Every retained history line was just truncated or padded, so their
        // CONTENT changed even though their indices did not. Without this the
        // damage range would cover only the live screen and a consumer scrolled
        // back into history would keep painting rows at the old width.
        m_historyContentDirty = true;
    }
    if (!m_inWrite)
        emitPending();
}

void VtScreen::clampCursorAndRegion()
{
    m_scrollTop = clampInt(m_scrollTop, 0, m_rows - 1);
    m_scrollBottom = clampInt(m_scrollBottom, m_scrollTop, m_rows - 1);
    if (m_scrollTop >= m_scrollBottom) {
        // Clamping a region into a shorter grid can collapse it to a single row:
        // a region of rows 20-24 on a 24-row screen becomes rows 20..20 when a
        // phone rotates to a 20-row grid. setScrollRegion() refuses to create
        // such a region because it traps the cursor - every line feed scrolls the
        // one row the cursor is on, so output overwrites itself in place, nothing
        // reaches the scrollback and the screen never advances again - and a
        // region that collapses here is the same thing arrived at by a different
        // route, so it gets the same answer: no region at all.
        m_scrollTop = 0;
        m_scrollBottom = m_rows - 1;
    }
    m_cursorX = clampInt(m_cursorX, 0, m_columns - 1);
    m_cursorY = clampInt(m_cursorY, 0, m_rows - 1);
}

// --- absolute addressing ----------------------------------------------------

const VtLine &VtScreen::lineAt(int absoluteRow) const
{
    if (absoluteRow < 0 || absoluteRow >= totalLines())
        return m_outOfRangeLine;
    const int historySize = static_cast<int>(m_history.size());
    if (absoluteRow < historySize)
        return m_history.at(absoluteRow);
    return m_grid.at(absoluteRow - historySize);
}

QString VtScreen::textRange(int startRow, int startCol, int endRow, int endCol) const
{
    // endRow is INCLUSIVE, endCol is EXCLUSIVE: one whole line N is
    // textRange(N, 0, N, columns()). Rows and columns are clamped rather than
    // refused, because the caller is a selection racing a live stream and a stale
    // row index must not lose the whole copy.
    const int last = totalLines() - 1;
    const int firstRow = clampInt(startRow, 0, last);
    const int lastRow = clampInt(endRow, 0, last);
    if (lastRow < firstRow)
        return {};
    if (firstRow == lastRow && endCol <= startCol)
        return {};

    QString out;
    for (int row = firstRow; row <= lastRow; ++row) {
        const VtLine &line = lineAt(row);
        const int lineSize = static_cast<int>(line.size());
        const int from = (row == firstRow) ? clampInt(startCol, 0, lineSize) : 0;
        const int to = (row == lastRow) ? clampInt(endCol, 0, lineSize) : lineSize;
        if (to <= from) {
            if (row != lastRow)
                out += u'\n';
            continue;
        }

        QString text;
        text.reserve(to - from);
        for (int column = from; column < to; ++column) {
            const VtCell &cell = line.at(column);
            // The covered half of a double-width character carries no text of its
            // own; emitting its placeholder space would put a spurious blank
            // after every CJK glyph in the copied text.
            if (cell.width == 0)
                continue;
            appendCodePoint(text, cell.text);
        }
        // Trailing blanks are padding, not content: the grid is always
        // rectangular, so without trimming, copying a line of a shell prompt
        // would yield 80 columns of spaces.
        while (text.endsWith(u' '))
            text.chop(1);

        out += text;
        if (row != lastRow)
            out += u'\n';
    }
    return out;
}

// --- feeding ----------------------------------------------------------------

void VtScreen::write(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    // Saved and restored rather than set and cleared: reply() is emitted from
    // inside feed(), and a handler that answers by feeding this screen again
    // (a loopback, a test harness) would otherwise clear the flag and flush a
    // half-finished batch while the outer feed was still running. A nested write
    // joins the outer batch instead.
    const bool wasInWrite = m_inWrite;
    m_inWrite = true;
    m_parser->feed(bytes);
    m_inWrite = wasInWrite;
    if (!m_inWrite)
        emitPending();
}

void VtScreen::reset()
{
    m_parser->reset();
    hardReset();
    if (!m_inWrite)
        emitPending();
}

void VtScreen::emitPending()
{
    if (m_sizeDirty) {
        m_sizeDirty = false;
        emit sizeChanged();
    }
    if (m_resetPending) {
        m_resetPending = false;
        emit screenReset();
    }
    if (m_historyDirty) {
        m_historyDirty = false;
        emit historyLinesChanged();
    }
    // Damage is tracked in GRID rows and converted here, once the history growth
    // for the whole write is final. The reach over rows that became history is
    // carried in a LOCAL, so that -1 stays unambiguously the "nothing damaged"
    // sentinel in the members even when history row -1 is genuinely damaged.
    const bool hasDamage = (m_damageTop != -1);
    int damageTop = m_damageTop;
    const int advance = m_historyAdvance;
    m_historyAdvance = 0;
    if (advance > 0) {
        // Reported BEFORE linesChanged: a consumer anchored in history has to
        // re-derive its top row before it is told which rows to repaint.
        emit screenScrolled(advance);
    }
    // Lines that entered history during this write hold new content at absolute
    // indices that were not history when they were written, and a width change
    // rewrites every retained history line; in both cases the damaged range has
    // to reach back above the live screen.
    const int historyReach = m_historyContentDirty ? firstScreenRow()
                                                   : qMin(advance, firstScreenRow());
    m_historyContentDirty = false;
    if (hasDamage) {
        if (historyReach > 0)
            damageTop = qMin(damageTop, -historyReach);
        const int base = firstScreenRow();
        const int first = clampInt(base + damageTop, 0, totalLines() - 1);
        const int lastRow = clampInt(base + m_damageBottom, first, totalLines() - 1);
        m_damageTop = -1;
        m_damageBottom = -1;
        emit linesChanged(first, lastRow);
    }
    if (m_cursorDirty) {
        m_cursorDirty = false;
        emit cursorMoved();
    }
    if (m_titleDirty) {
        m_titleDirty = false;
        emit windowTitleChanged();
    }
    if (m_modesDirty) {
        m_modesDirty = false;
        emit modesChanged();
    }
    if (m_bellPending) {
        // One bell per write(), not one per BEL byte: a program that prints a
        // thousand bells must not produce a thousand vibrations on a phone.
        m_bellPending = false;
        emit bell();
    }
}

void VtScreen::markDamage(int gridRow)
{
    markDamage(gridRow, gridRow);
}

void VtScreen::markDamage(int firstGridRow, int lastGridRow)
{
    if (m_damageTop == -1 || firstGridRow < m_damageTop)
        m_damageTop = firstGridRow;
    if (m_damageBottom == -1 || lastGridRow > m_damageBottom)
        m_damageBottom = lastGridRow;
}

void VtScreen::markAllDamaged()
{
    m_damageTop = 0;
    m_damageBottom = m_rows - 1;
}

// --- printing ---------------------------------------------------------------

void VtScreen::demoteOrphanCell(int row, int column)
{
    // Repair one half of a double-width character whose partner is gone.
    //
    // Called AFTER the operation that could have torn a pair apart, on each
    // column where the pair could straddle the edit: an orphan is not merely
    // untidy, it makes the grid stop describing what the remote program drew. A
    // leading cell (width 2) without its covered half renders a two-column glyph
    // inside one column, overpainting its neighbour; a covered half (width 0)
    // without its leading cell renders as NOTHING and is skipped by textRange(),
    // so the character silently disappears from both the screen and a copy. The
    // demoted cell keeps its own colours - it stays inside whatever the character
    // was painted on - and only stops claiming a glyph it can no longer show.
    //
    // Everything is read through at() until a change is actually needed: this
    // runs twice per printed glyph, and the common answer is "nothing to do".
    if (row < 0 || row >= m_grid.size())
        return;
    VtLine &line = m_grid[row];
    if (column < 0 || column >= line.size())
        return;
    const quint8 width = line.at(column).width;
    if (width == 1)
        return; // an ordinary cell is never half of anything
    const bool paired = (width == 2)
        ? (column + 1 < line.size() && line.at(column + 1).width == 0)
        : (column > 0 && line.at(column - 1).width == 2);
    if (paired)
        return;
    VtCell &cell = line[column];
    cell.text = U' ';
    cell.width = 1;
}

void VtScreen::attachCombiningMark(char32_t codePoint)
{
    int row = m_cursorY;
    int column;
    if (m_pendingWrap)
        column = m_columns - 1; // the glyph that armed the wrap is there
    else if (m_cursorX > 0)
        column = m_cursorX - 1;
    else
        return; // nothing precedes the mark; dropping it is the only option

    VtLine &line = m_grid[row];
    if (line.at(column).width == 0 && column > 0)
        --column; // attach to the LEADING half of a double-width character

    VtCell &cell = line[column];
    // VtCell holds a single code point, deliberately (see VtTypes.h), so the mark
    // is folded into the base character by canonical composition. When no
    // precomposed form exists - a Devanagari cluster, an emoji ZWJ sequence - the
    // mark is dropped rather than stored somewhere the renderer would never look.
    // The allocation here is acceptable because it happens per combining mark, not
    // per glyph, and combining marks are a vanishing fraction of terminal output.
    const char32_t pair[2] = {cell.text, codePoint};
    const QString composed = QString::fromUcs4(pair, 2)
                                 .normalized(QString::NormalizationForm_C);
    const auto ucs4 = composed.toUcs4();
    if (ucs4.size() == 1) {
        cell.text = static_cast<char32_t>(ucs4.at(0));
        markDamage(row);
    }
}

void VtScreen::printCodePoint(char32_t codePoint)
{
    const int width = vt::charWidth(codePoint);
    if (width == 0) {
        attachCombiningMark(codePoint);
        return;
    }

    if (m_pendingWrap) {
        // The deferred wrap finally happens, on the glyph AFTER the one that
        // filled the last column.
        m_pendingWrap = false;
        if (m_autowrap) {
            m_cursorX = 0;
            lineFeed();
        }
    }

    if (width == 2) {
        if (m_columns < 2)
            return; // a double-width glyph cannot be represented at all
        if (m_cursorX + 1 >= m_columns) {
            if (!m_autowrap)
                return; // no room and no wrap: the glyph is dropped, not split
            m_cursorX = 0;
            lineFeed();
        }
    }

    VtLine &line = m_grid[m_cursorY];
    VtCell &cell = line[m_cursorX];
    cell.text = codePoint;
    cell.width = static_cast<quint8>(width);
    cell.fg = m_fg;
    cell.bg = m_bg;
    cell.attrs = m_attrs;
    if (width == 2) {
        VtCell &trailing = line[m_cursorX + 1];
        trailing.text = U' ';
        trailing.width = 0;
        trailing.fg = m_fg;
        trailing.bg = m_bg;
        trailing.attrs = m_attrs;
    }
    // The glyph overwrote [cursorX, cursorX + width), so a double-width
    // character that straddled either edge of that span has lost one half.
    demoteOrphanCell(m_cursorY, m_cursorX - 1);
    demoteOrphanCell(m_cursorY, m_cursorX + width);
    markDamage(m_cursorY);

    if (m_cursorX + width >= m_columns) {
        m_cursorX = m_columns - 1;
        m_pendingWrap = m_autowrap;
    } else {
        m_cursorX += width;
    }
    m_cursorDirty = true;
}

// --- C0 and cursor ----------------------------------------------------------

void VtScreen::requestBell()
{
    m_bellPending = true;
}

void VtScreen::backspace()
{
    // A pending wrap is cancelled first: the cursor is visually on the last
    // column, so one BS must land on the column before it.
    m_pendingWrap = false;
    if (m_cursorX > 0)
        --m_cursorX;
    m_cursorDirty = true;
}

void VtScreen::horizontalTab(int count)
{
    m_pendingWrap = false;
    for (int step = 0; step < count; ++step) {
        int column = m_cursorX + 1;
        while (column < m_columns && !m_tabStops.at(column))
            ++column;
        // Past the last stop the cursor parks on the final column, exactly like a
        // hardware terminal; it never wraps.
        m_cursorX = qMin(column, m_columns - 1);
    }
    m_cursorDirty = true;
}

void VtScreen::reverseTab(int count)
{
    m_pendingWrap = false;
    for (int step = 0; step < count; ++step) {
        int column = m_cursorX - 1;
        while (column > 0 && !m_tabStops.at(column))
            --column;
        m_cursorX = qMax(column, 0);
    }
    m_cursorDirty = true;
}

void VtScreen::setTabStopAtCursor()
{
    if (m_cursorX >= 0 && m_cursorX < m_tabStops.size())
        m_tabStops[m_cursorX] = true;
}

void VtScreen::clearTabStop(int mode)
{
    if (mode == 3) {
        m_tabStops.fill(false);
    } else if (mode == 0 && m_cursorX < m_tabStops.size()) {
        m_tabStops[m_cursorX] = false;
    }
}

void VtScreen::lineFeed()
{
    m_pendingWrap = false;
    if (m_cursorY == m_scrollBottom)
        scrollRegionUp(1);
    else if (m_cursorY < m_rows - 1)
        ++m_cursorY;
    m_cursorDirty = true;
}

void VtScreen::reverseIndex()
{
    m_pendingWrap = false;
    if (m_cursorY == m_scrollTop)
        scrollRegionDown(1);
    else if (m_cursorY > 0)
        --m_cursorY;
    m_cursorDirty = true;
}

void VtScreen::nextLine()
{
    lineFeed();
    m_cursorX = 0;
}

void VtScreen::carriageReturn()
{
    m_pendingWrap = false;
    m_cursorX = 0;
    m_cursorDirty = true;
}

void VtScreen::moveCursor(int x, int y)
{
    m_pendingWrap = false;
    m_cursorX = clampInt(x, 0, m_columns - 1);
    m_cursorY = clampInt(y, 0, m_rows - 1);
    m_cursorDirty = true;
}

void VtScreen::cursorUp(int count)
{
    // Inside the scroll region the cursor cannot escape it upwards; outside it
    // the whole screen is the limit. That distinction is what stops a full-screen
    // program's status line from being scribbled over by a CUU inside its pager
    // region.
    const int limit = (m_cursorY >= m_scrollTop) ? m_scrollTop : 0;
    moveCursor(m_cursorX, qMax(m_cursorY - count, limit));
}

void VtScreen::cursorDown(int count)
{
    const int limit = (m_cursorY <= m_scrollBottom) ? m_scrollBottom : m_rows - 1;
    moveCursor(m_cursorX, qMin(m_cursorY + count, limit));
}

void VtScreen::cursorForward(int count)
{
    moveCursor(m_cursorX + count, m_cursorY);
}

void VtScreen::cursorBack(int count)
{
    moveCursor(m_cursorX - count, m_cursorY);
}

void VtScreen::cursorNextLine(int count)
{
    cursorDown(count);
    m_cursorX = 0;
}

void VtScreen::cursorPreviousLine(int count)
{
    cursorUp(count);
    m_cursorX = 0;
}

void VtScreen::setCursorColumn(int column1Based)
{
    moveCursor(column1Based - 1, m_cursorY);
}

void VtScreen::setCursorRow(int row1Based)
{
    moveCursor(m_cursorX, row1Based - 1);
}

void VtScreen::setCursorPosition(int row1Based, int column1Based)
{
    // DECOM (origin mode) is not implemented, so row 1 is always screen row 1
    // rather than the top of the scroll region. Nothing in the mobile client can
    // turn origin mode on, and a program that sets it also sets the region and
    // then addresses absolutely, so honouring the absolute meaning is the safe
    // reading.
    moveCursor(column1Based - 1, row1Based - 1);
}

// --- erasing and editing ----------------------------------------------------

void VtScreen::eraseCells(int row, int fromColumn, int toColumn)
{
    if (row < 0 || row >= m_grid.size())
        return;
    VtLine &line = m_grid[row];
    const int first = clampInt(fromColumn, 0, static_cast<int>(line.size()));
    const int last = clampInt(toColumn, first, static_cast<int>(line.size()));
    if (first == last)
        return;
    const VtCell blank = blankCell();
    for (int column = first; column < last; ++column)
        line[column] = blank;
    // Erasing part of a double-width character must also demote the half that
    // survived, or a stray width==2 or width==0 cell outlives the erase.
    demoteOrphanCell(row, first - 1);
    demoteOrphanCell(row, last);
    markDamage(row);
}

void VtScreen::eraseInDisplay(int mode)
{
    switch (mode) {
    case 0:
        eraseCells(m_cursorY, m_cursorX, m_columns);
        for (int row = m_cursorY + 1; row < m_rows; ++row)
            eraseCells(row, 0, m_columns);
        break;
    case 1:
        for (int row = 0; row < m_cursorY; ++row)
            eraseCells(row, 0, m_columns);
        eraseCells(m_cursorY, 0, m_cursorX + 1);
        break;
    case 2:
        for (int row = 0; row < m_rows; ++row)
            eraseCells(row, 0, m_columns);
        break;
    case 3:
        // ED 3 is xterm's "erase saved lines". It is the ONLY sequence that
        // discards scrollback, and `clear` on most systems emits it, so a user who
        // asks for a clean screen genuinely gets one.
        if (!m_history.isEmpty()) {
            m_history.clear();
            m_historyDirty = true;
            markAllDamaged();
        }
        break;
    default:
        break;
    }
    m_pendingWrap = false;
}

void VtScreen::eraseInLine(int mode)
{
    switch (mode) {
    case 0:
        eraseCells(m_cursorY, m_cursorX, m_columns);
        break;
    case 1:
        eraseCells(m_cursorY, 0, m_cursorX + 1);
        break;
    case 2:
        eraseCells(m_cursorY, 0, m_columns);
        break;
    default:
        break;
    }
    m_pendingWrap = false;
}

void VtScreen::insertLines(int count)
{
    // IL and DL are no-ops with the cursor outside the scroll region: the region
    // is the program's declared editing window, and honouring them outside it
    // would corrupt the status lines the region exists to protect.
    if (m_cursorY < m_scrollTop || m_cursorY > m_scrollBottom)
        return;
    const int limit = m_scrollBottom - m_cursorY + 1;
    const int lines = qMin(count, limit);
    const VtLine blank = blankLine();
    for (int step = 0; step < lines; ++step) {
        m_grid.removeAt(m_scrollBottom);
        m_grid.insert(m_cursorY, blank);
    }
    markDamage(m_cursorY, m_scrollBottom);
    m_pendingWrap = false;
}

void VtScreen::deleteLines(int count)
{
    if (m_cursorY < m_scrollTop || m_cursorY > m_scrollBottom)
        return;
    const int limit = m_scrollBottom - m_cursorY + 1;
    const int lines = qMin(count, limit);
    const VtLine blank = blankLine();
    for (int step = 0; step < lines; ++step) {
        // Deleted lines are DISCARDED, never pushed to history: DL is an editing
        // operation inside a window the program is repainting, not a scroll.
        m_grid.removeAt(m_cursorY);
        m_grid.insert(m_scrollBottom, blank);
    }
    markDamage(m_cursorY, m_scrollBottom);
    m_pendingWrap = false;
}

void VtScreen::insertCharacters(int count)
{
    VtLine &line = m_grid[m_cursorY];
    const int shift = qMin(count, m_columns - m_cursorX);
    if (shift <= 0)
        return;
    // Copied through a value, not reference-to-reference: rows of a freshly
    // allocated grid all share one QVector buffer, so the non-const line[]
    // detaches mid-expression and a reference taken from line.at() would point
    // into the pre-detach copy.
    for (int column = m_columns - 1; column >= m_cursorX + shift; --column) {
        const VtCell moved = line.at(column - shift);
        line[column] = moved;
    }
    const VtCell blank = blankCell();
    for (int column = m_cursorX; column < m_cursorX + shift; ++column)
        line[column] = blank;
    // Three seams can now hold half a double-width character, and all three have
    // to be repaired AFTER the shift, never before it: the cells at and after the
    // cursor are copies of what was there, so demoting one first would copy the
    // demoted cell rightwards and lose the glyph. The seams are the cell left of
    // the insertion (its covered half moved away), the first shifted cell (its
    // leading half stayed behind), and the last column (its covered half was
    // pushed off the right edge).
    demoteOrphanCell(m_cursorY, m_cursorX - 1);
    demoteOrphanCell(m_cursorY, m_cursorX + shift);
    demoteOrphanCell(m_cursorY, m_columns - 1);
    markDamage(m_cursorY);
    m_pendingWrap = false;
}

void VtScreen::deleteCharacters(int count)
{
    VtLine &line = m_grid[m_cursorY];
    const int shift = qMin(count, m_columns - m_cursorX);
    if (shift <= 0)
        return;
    for (int column = m_cursorX; column < m_columns - shift; ++column) {
        const VtCell moved = line.at(column + shift);
        line[column] = moved;
    }
    const VtCell blank = blankCell();
    for (int column = m_columns - shift; column < m_columns; ++column)
        line[column] = blank;
    // Same three seams as ICH, mirrored, and repaired after the shift for the
    // same reason: the cell left of the cursor (its covered half was deleted),
    // the cursor cell (the leading half it belonged to was deleted), and the last
    // cell that survived the shift (its covered half fell into the blanked tail).
    demoteOrphanCell(m_cursorY, m_cursorX - 1);
    demoteOrphanCell(m_cursorY, m_cursorX);
    demoteOrphanCell(m_cursorY, m_columns - shift - 1);
    markDamage(m_cursorY);
    m_pendingWrap = false;
}

void VtScreen::eraseCharacters(int count)
{
    eraseCells(m_cursorY, m_cursorX, m_cursorX + count);
    m_pendingWrap = false;
}

void VtScreen::pushToHistory(VtLine &&line)
{
    m_history.append(std::move(line));
    while (m_history.size() > kMaxHistoryLines) {
        // Eviction shifts every absolute row index down by one, which is why
        // m_historyDirty exists rather than a size comparison.
        m_history.removeFirst();
    }
    m_historyDirty = true;
    // One more line of scrollback, whether or not the cap evicted one to make
    // room: either way the live screen has moved one row within the absolute row
    // space, which is exactly what screenScrolled() reports.
    ++m_historyAdvance;
}

void VtScreen::scrollRegionUp(int count)
{
    const int height = m_scrollBottom - m_scrollTop + 1;
    const int lines = qMin(count, height);
    if (lines <= 0)
        return;
    // Only the PRIMARY screen contributes scrollback, and only when the region
    // starts at the top of the screen. The alt screen is a program's private
    // canvas - putting vim's redraws into the scrollback is the single most
    // complained-about terminal bug there is - and a region that starts below row
    // 0 is a pager window whose lines are not leaving the screen at all.
    const bool feedsHistory = (!m_altActive && m_scrollTop == 0);
    const VtLine blank = blankLine();
    for (int step = 0; step < lines; ++step) {
        if (feedsHistory)
            pushToHistory(std::move(m_grid[m_scrollTop]));
        m_grid.removeAt(m_scrollTop);
        m_grid.insert(m_scrollBottom, blank);
    }
    // No m_historyAdvance here: pushToHistory() counts the lines that genuinely
    // entered the absolute row space, which is not the same as the number of
    // lines the region scrolled (the alt screen and a region below row 0 scroll
    // without feeding history at all).
    markDamage(m_scrollTop, m_scrollBottom);
}

void VtScreen::scrollRegionDown(int count)
{
    const int height = m_scrollBottom - m_scrollTop + 1;
    const int lines = qMin(count, height);
    if (lines <= 0)
        return;
    const VtLine blank = blankLine();
    for (int step = 0; step < lines; ++step) {
        m_grid.removeAt(m_scrollBottom);
        m_grid.insert(m_scrollTop, blank);
    }
    // No history bookkeeping: scrolling DOWN never removes a history line and
    // never adds one, so the absolute row space does not move and a consumer
    // anchored in history must not adjust. The rows that changed are reported
    // through the damage range below like any other edit.
    markDamage(m_scrollTop, m_scrollBottom);
}

void VtScreen::scrollUpLines(int count)
{
    scrollRegionUp(count);
}

void VtScreen::scrollDownLines(int count)
{
    scrollRegionDown(count);
}

void VtScreen::setScrollRegion(int top1Based, int bottom1Based)
{
    int top = top1Based - 1;
    int bottom = bottom1Based - 1;
    if (bottom <= 0 || bottom >= m_rows)
        bottom = m_rows - 1;
    if (top < 0)
        top = 0;
    if (top >= bottom) {
        // A degenerate region (equal or inverted) means "no region" and resets to
        // the full screen; honouring a one-line region would trap the cursor.
        top = 0;
        bottom = m_rows - 1;
    }
    m_scrollTop = top;
    m_scrollBottom = bottom;
    // DECSTBM homes the cursor, which programs rely on: they set the region and
    // then start drawing without an explicit CUP.
    moveCursor(0, 0);
}

// --- rendition --------------------------------------------------------------

void VtScreen::resetAttributes()
{
    m_attrs = 0;
    m_fg = VtDefaultForeground;
    m_bg = VtDefaultBackground;
}

void VtScreen::applySimpleSgr(int code)
{
    switch (code) {
    case 0:
        resetAttributes();
        break;
    case 1:
        m_attrs |= VtBold;
        break;
    case 2:
        m_attrs |= VtDim;
        break;
    case 3:
        m_attrs |= VtItalic;
        break;
    case 4:
        m_attrs |= VtUnderline;
        break;
    case 5:
    case 6: // rapid blink; no renderer distinguishes the two
        m_attrs |= VtBlink;
        break;
    case 7:
        m_attrs |= VtInverse;
        break;
    case 8:
        m_attrs |= VtHidden;
        break;
    case 9:
        m_attrs |= VtStrike;
        break;
    case 21:
        // ECMA-48 says doubly underlined; xterm historically read it as "bold
        // off". Underline is the reading that cannot lose information: a program
        // that means "bold off" also sends 22, which does clear bold.
        m_attrs |= VtUnderline;
        break;
    case 22:
        m_attrs &= static_cast<quint16>(~(VtBold | VtDim));
        break;
    case 23:
        m_attrs &= static_cast<quint16>(~VtItalic);
        break;
    case 24:
        m_attrs &= static_cast<quint16>(~VtUnderline);
        break;
    case 25:
        m_attrs &= static_cast<quint16>(~VtBlink);
        break;
    case 27:
        m_attrs &= static_cast<quint16>(~VtInverse);
        break;
    case 28:
        m_attrs &= static_cast<quint16>(~VtHidden);
        break;
    case 29:
        m_attrs &= static_cast<quint16>(~VtStrike);
        break;
    case 39:
        m_fg = VtDefaultForeground;
        break;
    case 49:
        m_bg = VtDefaultBackground;
        break;
    default:
        if (code >= 30 && code <= 37)
            m_fg = vt::xterm256Color(code - 30);
        else if (code >= 40 && code <= 47)
            m_bg = vt::xterm256Color(code - 40);
        else if (code >= 90 && code <= 97)
            m_fg = vt::xterm256Color(8 + code - 90);
        else if (code >= 100 && code <= 107)
            m_bg = vt::xterm256Color(8 + code - 100);
        // 10-20 (fonts), 26/50 (proportional spacing), 51-55 (frames, overline),
        // 59 (default underline colour) and 73-75 (super/subscript) are parsed
        // and ignored: nothing in the mobile renderer can express them, and
        // treating an unknown code as an error would drop the codes around it.
        break;
    }
}

void VtScreen::applySgr(const int *params, const bool *isSubParam, int count)
{
    if (count == 0) {
        // A bare "CSI m" is "CSI 0 m".
        resetAttributes();
        return;
    }

    int index = 0;
    while (index < count) {
        const int code = params[index] < 0 ? 0 : params[index];
        // Extent of this parameter's colon-separated sub-parameters.
        int subEnd = index + 1;
        while (subEnd < count && isSubParam[subEnd])
            ++subEnd;
        const int subCount = subEnd - index - 1;

        if (code == 38 || code == 48 || code == 58) {
            QRgb colour = VtDefaultForeground;
            bool ok = false;
            if (subCount > 0) {
                // Colon form: "38:5:n" / "38:2:r:g:b". Self-delimiting, which is
                // exactly why modern programs prefer it.
                ok = parseColonColour(params + index + 1, subCount, &colour);
                index = subEnd;
            } else {
                // Legacy semicolon form: the selector and the channels are
                // ordinary parameters that follow, so they must be CONSUMED here
                // or they would be read as further SGR codes ("38;5;1" would
                // become "default fg, blink, bold").
                const int selector = (index + 1 < count) ? qMax(0, params[index + 1]) : -1;
                if (selector == 5) {
                    const int value = (index + 2 < count) ? params[index + 2] : -1;
                    ok = value >= 0 && value <= 255;
                    if (ok)
                        colour = vt::xterm256Color(value);
                    index += 3;
                } else if (selector == 2) {
                    const int r = (index + 2 < count) ? params[index + 2] : -1;
                    const int g = (index + 3 < count) ? params[index + 3] : -1;
                    const int b = (index + 4 < count) ? params[index + 4] : -1;
                    ok = r >= 0 && g >= 0 && b >= 0 && r <= 255 && g <= 255 && b <= 255;
                    if (ok)
                        colour = qRgb(r, g, b);
                    index += 5;
                } else {
                    index += 2; // unknown selector: drop it and its parameter
                }
            }
            if (ok && code == 38)
                m_fg = colour;
            else if (ok && code == 48)
                m_bg = colour;
            // 58 is the underline colour, which the renderer draws in the
            // foreground colour; parsed so its parameters cannot leak, discarded
            // because there is nowhere to put it.
            continue;
        }

        applySimpleSgr(code);
        // Skip any sub-parameters: "4:3" is a curly underline, and reading the 3
        // as a separate code would turn it into italics.
        index = subEnd;
    }
}

// --- reports and modes ------------------------------------------------------

void VtScreen::deviceStatusReport(int mode)
{
    switch (mode) {
    case 5:
        // "Terminal OK". CSI 0 n.
        emit reply(QByteArrayLiteral("\x1b[0n"));
        break;
    case 6: {
        // CPR. 1-based, and the row is a SCREEN row - history is invisible to the
        // remote program, which knows nothing about our scrollback.
        QByteArray answer;
        answer.reserve(16);
        answer += "\x1b[";
        answer += QByteArray::number(m_cursorY + 1);
        answer += ';';
        answer += QByteArray::number(m_cursorX + 1);
        answer += 'R';
        emit reply(answer);
        break;
    }
    default:
        break;
    }
}

void VtScreen::setMode(int mode, bool enable, bool privateMode)
{
    if (!privateMode)
        return;

    switch (mode) {
    case 1: // DECCKM
        if (m_applicationCursorKeys != enable) {
            m_applicationCursorKeys = enable;
            m_modesDirty = true;
        }
        break;
    case 7: // DECAWM
        m_autowrap = enable;
        if (!enable)
            m_pendingWrap = false;
        break;
    case 25: // DECTCEM
        if (m_cursorVisible != enable) {
            m_cursorVisible = enable;
            m_cursorDirty = true;
        }
        break;
    case 47:
        // The oldest alt-screen mode: switch only, no cursor save.
        if (enable)
            enterAltScreen(false);
        else
            leaveAltScreen();
        break;
    case 1000:
    case 1002:
    case 1003:
    case 1006: {
        const quint16 bit = (mode == 1000) ? MouseClick
                            : (mode == 1002) ? MouseDrag
                            : (mode == 1003) ? MouseMotion
                                             : MouseSgr;
        const quint16 before = m_mouseTracking;
        if (enable)
            m_mouseTracking = static_cast<quint16>(m_mouseTracking | bit);
        else
            m_mouseTracking = static_cast<quint16>(m_mouseTracking & ~bit);
        // Only the aggregate is observable, so notify only when it flips: a
        // program turning 1002 off while 1006 stays on has not changed anything
        // the view can see.
        if ((before != 0) != (m_mouseTracking != 0))
            m_modesDirty = true;
        break;
    }
    case 1047:
        if (enable)
            enterAltScreen(false);
        else
            leaveAltScreen();
        break;
    case 1049:
        // The mode everything modern uses: save the cursor, switch, and start
        // from a blank alt screen; on the way out, restore both.
        if (enable) {
            saveCursor();
            enterAltScreen(true);
        } else {
            leaveAltScreen();
            restoreCursor();
        }
        break;
    case 2004: // bracketed paste
        if (m_bracketedPaste != enable) {
            m_bracketedPaste = enable;
            m_modesDirty = true;
        }
        break;
    default:
        // 12 (blinking cursor), 1004 (focus reporting), 1005/1015 (obsolete mouse
        // encodings), 2026 (synchronised output) and the rest: accepted and
        // ignored. A program must never be able to break the grid by setting a
        // mode this engine does not model.
        break;
    }
}

void VtScreen::enterAltScreen(bool clearOnEnter)
{
    if (m_altActive) {
        if (clearOnEnter) {
            clearWholeGrid();
            moveCursor(0, 0);
        }
        return;
    }
    m_primaryGrid = std::move(m_grid);
    m_grid.clear();
    allocateGrid();
    m_altActive = true;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_pendingWrap = false;
    if (clearOnEnter)
        moveCursor(0, 0);
    m_modesDirty = true;
    m_cursorDirty = true;
    markAllDamaged();
}

void VtScreen::leaveAltScreen()
{
    if (!m_altActive)
        return;
    m_grid = std::move(m_primaryGrid);
    m_primaryGrid.clear();
    m_altActive = false;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_pendingWrap = false;
    clampCursorAndRegion();
    m_modesDirty = true;
    m_cursorDirty = true;
    markAllDamaged();
}

void VtScreen::saveCursor()
{
    // DEC keeps one saved cursor PER screen, so a program that saves on the
    // primary screen and then saves again inside the alt screen does not lose the
    // first one - which is exactly what DECSET 1049 relies on.
    SavedCursor &slot = m_altActive ? m_savedCursorAlt : m_savedCursor;
    slot.x = m_cursorX;
    slot.y = m_cursorY;
    slot.attrs = m_attrs;
    slot.fg = m_fg;
    slot.bg = m_bg;
    slot.valid = true;
}

void VtScreen::restoreCursor()
{
    const SavedCursor &slot = m_altActive ? m_savedCursorAlt : m_savedCursor;
    if (!slot.valid) {
        // Restoring without a save homes the cursor, per DEC: the alternative
        // (doing nothing) leaves programs that only ever emit DECRC stuck
        // wherever the previous output ended.
        moveCursor(0, 0);
        resetAttributes();
        return;
    }
    m_attrs = slot.attrs;
    m_fg = slot.fg;
    m_bg = slot.bg;
    moveCursor(slot.x, slot.y);
}

void VtScreen::alignmentTest()
{
    // DECALN: fill the screen with 'E' in the DEFAULT rendition, home the cursor
    // and drop the scroll region.
    resetAttributes();
    const VtCell blank = blankCell();
    VtCell fill = blank;
    fill.text = U'E';
    for (int row = 0; row < m_rows; ++row) {
        VtLine &line = m_grid[row];
        for (int column = 0; column < m_columns; ++column)
            line[column] = fill;
    }
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    moveCursor(0, 0);
    markAllDamaged();
}

void VtScreen::setWindowTitle(const QString &title)
{
    if (m_windowTitle == title)
        return;
    m_windowTitle = title;
    m_titleDirty = true;
}

void VtScreen::softReset()
{
    // DECSTR. Deliberately does NOT touch the grid, the history or the alt-screen
    // selection: it is what a program emits to put the terminal into a known
    // state before drawing, not a request to erase what is on screen.
    resetAttributes();
    m_cursorVisible = true;
    m_autowrap = true;
    m_applicationCursorKeys = false;
    m_bracketedPaste = false;
    m_mouseTracking = 0;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_pendingWrap = false;
    m_savedCursor = SavedCursor{};
    m_savedCursorAlt = SavedCursor{};
    moveCursor(0, 0);
    m_modesDirty = true;
    m_cursorDirty = true;
}

void VtScreen::hardReset()
{
    m_altActive = false;
    m_primaryGrid.clear();
    resetAttributes();
    allocateGrid();
    resetTabStops();
    if (!m_history.isEmpty()) {
        m_history.clear();
        m_historyDirty = true;
    }
    m_cursorVisible = true;
    m_autowrap = true;
    m_applicationCursorKeys = false;
    m_bracketedPaste = false;
    m_mouseTracking = 0;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_cursorX = 0;
    m_cursorY = 0;
    m_pendingWrap = false;
    m_savedCursor = SavedCursor{};
    m_savedCursorAlt = SavedCursor{};
    // The scrollback those counters described no longer exists, so reporting a
    // scroll or a history reach for it would point a consumer at rows that were
    // just discarded.
    m_historyAdvance = 0;
    m_historyContentDirty = false;
    if (!m_windowTitle.isEmpty()) {
        m_windowTitle.clear();
        m_titleDirty = true;
    }
    m_resetPending = true;
    m_cursorDirty = true;
    m_modesDirty = true;
    markAllDamaged();
}

} // namespace ch
