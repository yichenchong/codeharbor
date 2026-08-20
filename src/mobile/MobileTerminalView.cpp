#include "MobileTerminalView.h"

#include <QPainter>
#include <QVarLengthArray>

#include <algorithm>
#include <cmath>
#include <utility>

#include "VtScreen.h"
#include "VtTypes.h"

namespace ch {
namespace {

// Cell the glyph metrics are measured on. Any character does for a monospace
// font; 'M' is the conventional choice and is present in every font that could
// plausibly be configured here, so a missing glyph cannot collapse the grid.
constexpr char16_t kMetricSample = u'M';

// How far a dim cell is pulled toward its own background. SGR 2 has no defined
// intensity, and halfway is what xterm and every terminal the user has seen do:
// visibly quieter, still legible on both a light and a dark theme.
constexpr int kDimBlendPercent = 50;

QColor blend(const QColor &from, const QColor &to, int percent)
{
    const auto mix = [percent](int a, int b) { return (a * (100 - percent) + b * percent) / 100; };
    return QColor(mix(from.red(), to.red()), mix(from.green(), to.green()),
                  mix(from.blue(), to.blue()));
}

} // namespace

MobileTerminalView::MobileTerminalView(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    // The whole item is painted every time it is asked to paint a band, so Qt
    // need not clear underneath it.
    setOpaquePainting(true);
    // Image, which is the default, is load-bearing here and must not be changed
    // to FramebufferObject: only the image render target honours a partial
    // update(QRect), and partial updates are the entire point of the damage
    // tracking below.
    setRenderTarget(QQuickPaintedItem::Image);
    // Text is the only thing drawn; antialiasing it is what makes a phone screen
    // legible at these sizes.
    setAntialiasing(true);

    m_font.setStyleHint(QFont::Monospace);
    m_font.setFamily(QStringLiteral("monospace"));
    m_font.setFixedPitch(true);
    recomputeGrid();

    // The cursor is drawn filled while this pane owns the keyboard and hollow
    // when it does not — the standard way a terminal says "your keystrokes go
    // elsewhere". Only the cursor cell changes, so only its row is repainted;
    // see setCursorFocused(), which is where that report arrives.
}

VtScreen *MobileTerminalView::screen() const
{
    return m_screen.data();
}

void MobileTerminalView::setScreen(VtScreen *screen)
{
    if (m_screen == screen)
        return;
    if (m_screen)
        m_screen->disconnect(this);
    m_screen = screen;
    if (m_screen) {
        connect(m_screen, &VtScreen::linesChanged, this, &MobileTerminalView::onLinesChanged);
        connect(m_screen, &VtScreen::screenScrolled, this, &MobileTerminalView::onScreenScrolled);
        connect(m_screen, &VtScreen::cursorMoved, this, &MobileTerminalView::onCursorMoved);
        connect(m_screen, &VtScreen::screenReset, this, &MobileTerminalView::onScreenReset);
        // The screen's OWN geometry (which the session sets from this item's
        // grid) decides where the live screen starts, so a reflow moves every
        // absolute row this item is showing.
        connect(m_screen, &VtScreen::sizeChanged, this, [this]() {
            clampScrollOffset();
            markAllDamaged();
            emit maxScrollOffsetChanged();
        });
        // History growing or being evicted changes how far back the view may
        // scroll, and QML binds a scrollbar to it.
        connect(m_screen, &VtScreen::historyLinesChanged, this, [this]() {
            clampScrollOffset();
            emit maxScrollOffsetChanged();
        });
    }
    m_paintedCursorAbsoluteRow = -1;
    clampScrollOffset();
    markAllDamaged();
    emit screenChanged();
    emit maxScrollOffsetChanged();
}

QFont MobileTerminalView::font() const
{
    return m_font;
}

void MobileTerminalView::setFont(const QFont &font)
{
    if (m_font == font)
        return;
    m_font = font;
    recomputeGrid();
    markAllDamaged();
    emit fontChanged();
}

int MobileTerminalView::scrollOffset() const
{
    return m_scrollOffset;
}

void MobileTerminalView::setScrollOffset(int offset)
{
    const int clamped = qBound(0, offset, maxScrollOffset());
    if (clamped == m_scrollOffset)
        return;
    m_scrollOffset = clamped;
    // Every visible row now shows a different absolute row: this is the one
    // interaction that genuinely dirties the whole item.
    markAllDamaged();
    emit scrollOffsetChanged();
}

int MobileTerminalView::maxScrollOffset() const
{
    if (!m_screen)
        return 0;
    // The oldest thing the view can reach is absolute row 0, and the bottom row
    // is totalLines() - 1 - scrollOffset, so the deepest offset is however many
    // lines exist above the row the item's top currently shows when following.
    return qMax(0, m_screen->totalLines() - m_rows);
}

int MobileTerminalView::columns() const
{
    return m_columns;
}

int MobileTerminalView::rows() const
{
    return m_rows;
}

qreal MobileTerminalView::lineHeight() const
{
    return m_lineHeight;
}

qreal MobileTerminalView::cellWidth() const
{
    return m_cellWidth;
}

QColor MobileTerminalView::defaultForegroundColor() const
{
    return m_defaultForeground;
}

void MobileTerminalView::setDefaultForegroundColor(const QColor &color)
{
    if (m_defaultForeground == color)
        return;
    m_defaultForeground = color;
    markAllDamaged();
    emit defaultColorsChanged();
}

QColor MobileTerminalView::defaultBackgroundColor() const
{
    return m_defaultBackground;
}

void MobileTerminalView::setDefaultBackgroundColor(const QColor &color)
{
    if (m_defaultBackground == color)
        return;
    m_defaultBackground = color;
    markAllDamaged();
    emit defaultColorsChanged();
}

bool MobileTerminalView::cursorFocused() const
{
    return m_cursorFocused;
}

void MobileTerminalView::setCursorFocused(bool focused)
{
    if (m_cursorFocused == focused)
        return;
    m_cursorFocused = focused;
    // Only the cursor cell looks different, so only the cursor's row is repainted
    // — and it is derived from the SCREEN rather than from
    // m_paintedCursorAbsoluteRow, because the very first report can arrive before
    // this item has ever painted.
    if (m_screen) {
        const int cursorRow = m_screen->firstScreenRow() + m_screen->cursorRow()
            - topAbsoluteRow();
        markRowsDamaged(cursorRow, cursorRow);
    }
    emit cursorFocusedChanged();
}

int MobileTerminalView::columnAt(qreal x) const
{
    if (m_cellWidth <= 0.0 || m_columns <= 0)
        return 0;
    return qBound(0, static_cast<int>(std::floor(x / m_cellWidth)), m_columns - 1);
}

int MobileTerminalView::topAbsoluteRow() const
{
    if (!m_screen || m_rows <= 0)
        return 0;
    // Anchored on the BOTTOM, because that is what the user's mental model is:
    // offset 0 shows the newest line last. Anchoring on firstScreenRow() instead
    // would misplace the window whenever the screen holds a different number of
    // rows than this item can draw (a reflow in flight, a keyboard sliding up).
    return qMax(0, m_screen->totalLines() - m_rows - m_scrollOffset);
}

int MobileTerminalView::absoluteRowAt(qreal y) const
{
    if (m_lineHeight <= 0.0)
        return topAbsoluteRow();
    const int row = static_cast<int>(std::floor(y / m_lineHeight));
    const int absolute = topAbsoluteRow() + qBound(0, row, qMax(0, m_rows - 1));
    if (!m_screen)
        return absolute;
    return qBound(0, absolute, qMax(0, m_screen->totalLines() - 1));
}

void MobileTerminalView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size())
        return;
    recomputeGrid();
    markAllDamaged();
}

void MobileTerminalView::recomputeGrid()
{
    m_metrics = QFontMetricsF(m_font);
    const qreal advance = m_metrics.horizontalAdvance(QChar(kMetricSample));
    m_cellWidth = advance > 0.0 ? advance : 1.0;
    const qreal metricHeight = m_metrics.height();
    m_lineHeight = metricHeight > 0.0 ? metricHeight : 1.0;
    m_baseline = m_metrics.ascent();

    const int columns = qMax(0, static_cast<int>(std::floor(width() / m_cellWidth)));
    const int rows = qMax(0, static_cast<int>(std::floor(height() / m_lineHeight)));
    const bool gridChangedNow = columns != m_columns || rows != m_rows;
    m_columns = columns;
    m_rows = rows;
    clampScrollOffset();
    if (gridChangedNow) {
        // QML answers this by reporting the grid to the session, which reflows the
        // local screen and pushes an SSH window-change. This item deliberately
        // does neither itself.
        emit gridChanged();
        emit maxScrollOffsetChanged();
    }
}

void MobileTerminalView::clampScrollOffset()
{
    const int clamped = qBound(0, m_scrollOffset, maxScrollOffset());
    if (clamped == m_scrollOffset)
        return;
    m_scrollOffset = clamped;
    emit scrollOffsetChanged();
}

QRect MobileTerminalView::rowBand(int firstRow, int lastRow) const
{
    const int top = static_cast<int>(std::floor(firstRow * m_lineHeight));
    const int bottom = static_cast<int>(std::ceil((lastRow + 1) * m_lineHeight));
    return QRect(0, top, static_cast<int>(std::ceil(width())), qMax(1, bottom - top));
}

void MobileTerminalView::markRowsDamaged(int firstRow, int lastRow)
{
    if (m_rows <= 0)
        return;
    const int first = qMax(0, qMin(firstRow, lastRow));
    const int last = qMin(m_rows - 1, qMax(firstRow, lastRow));
    if (first > last)
        return;  // the change is entirely outside what this item is showing
    for (int row = first; row <= last; ++row)
        m_damagedRows.insert(row);
    const QRect band = rowBand(first, last);
    m_pendingDamage = m_pendingDamage.isNull() ? band : m_pendingDamage.united(band);
    // One update() per contiguous band, never one per row and never a whole-item
    // repaint: Qt coalesces overlapping regions itself, and paint() clips to
    // whatever it is finally given.
    update(band);
}

void MobileTerminalView::markAllDamaged()
{
    if (m_rows <= 0) {
        update();
        return;
    }
    markRowsDamaged(0, m_rows - 1);
}

void MobileTerminalView::onLinesChanged(int firstAbsoluteRow, int lastAbsoluteRow)
{
    const int top = topAbsoluteRow();
    markRowsDamaged(firstAbsoluteRow - top, lastAbsoluteRow - top);
}

void MobileTerminalView::onScreenScrolled(int lines)
{
    // A NEGATIVE count (content pushed DOWN by a reverse index or an insert-line)
    // is deliberately ignored here rather than mirrored: it never touches the
    // history, so the absolute row at the top of this item has not moved and the
    // scroll offset must not follow. The rows whose content changed arrive as
    // linesChanged() in the same write, which is what repaints them.
    if (lines <= 0)
        return;
    if (m_scrollOffset > 0) {
        // The user is reading history. Absolute rows are stable, so following the
        // scroll by the same number of lines keeps EXACTLY the same content under
        // their eyes and needs no repaint at all — the alternative, letting the
        // window slide, makes a page of scrollback crawl upward while a build
        // prints and is the single most annoying thing a mobile terminal can do.
        const int wanted = m_scrollOffset + lines;
        const int clamped = qBound(0, wanted, maxScrollOffset());
        const bool pinned = clamped == wanted;
        if (clamped != m_scrollOffset) {
            m_scrollOffset = clamped;
            emit scrollOffsetChanged();
        }
        emit maxScrollOffsetChanged();
        if (pinned)
            return;
        // History was evicted from under the view (the retained buffer is
        // bounded), so the window really did move: everything on screen changed.
        markAllDamaged();
        return;
    }
    // Following the live output: every row now shows the line that was below it.
    markAllDamaged();
    emit maxScrollOffsetChanged();
}

void MobileTerminalView::onCursorMoved()
{
    if (!m_screen)
        return;
    const int top = topAbsoluteRow();
    // The row the cursor LEFT has to be repainted too, or the old block stays on
    // screen and the pane appears to have two cursors.
    if (m_paintedCursorAbsoluteRow >= 0)
        markRowsDamaged(m_paintedCursorAbsoluteRow - top, m_paintedCursorAbsoluteRow - top);
    const int absolute = m_screen->firstScreenRow() + m_screen->cursorRow();
    markRowsDamaged(absolute - top, absolute - top);
}

void MobileTerminalView::onScreenReset()
{
    m_paintedCursorAbsoluteRow = -1;
    const bool wasScrolledBack = m_scrollOffset > 0;
    m_scrollOffset = 0;
    markAllDamaged();
    // Only on an actual change: QML binds a scrollbar to this, and a reset of a
    // pane that was already following the output is not a scroll event.
    if (wasScrolledBack)
        emit scrollOffsetChanged();
    emit maxScrollOffsetChanged();
}

QRect MobileTerminalView::pendingDamage() const
{
    return m_pendingDamage;
}

QList<int> MobileTerminalView::pendingDamagedRows() const
{
    QList<int> rows(m_damagedRows.cbegin(), m_damagedRows.cend());
    std::sort(rows.begin(), rows.end());
    return rows;
}

void MobileTerminalView::paint(QPainter *painter)
{
    // The dirty region Qt is asking for. An empty clip means "all of it", which
    // is what the very first paint and a resize produce.
    QRectF clip = painter->clipBoundingRect();
    if (clip.isEmpty())
        clip = QRectF(0.0, 0.0, width(), height());

    // Whatever was pending has now been asked for; anything that arrives during
    // this paint is damage for the NEXT one.
    m_damagedRows.clear();
    m_pendingDamage = QRect();

    painter->fillRect(clip, m_defaultBackground);
    if (!m_screen || m_rows <= 0 || m_columns <= 0 || m_lineHeight <= 0.0)
        return;

    // The two sentinels are the only values the VT stream cannot produce (every
    // explicit SGR colour goes through qRgb() and is fully opaque), so they are
    // safe to compare exactly rather than sniffed out of the alpha channel. Each
    // maps to ITS OWN theme colour: an inverse cell swaps the two afterwards, so
    // resolving a background sentinel to the foreground here would make SGR 7
    // paint the theme's foreground on both halves of the cell.
    const auto resolve = [this](QRgb value) {
        if (value == VtScreen::defaultForeground())
            return m_defaultForeground;
        if (value == VtScreen::defaultBackground())
            return m_defaultBackground;
        return QColor::fromRgb(value);
    };

    const int total = m_screen->totalLines();
    const int top = topAbsoluteRow();
    const int firstRow = qBound(0, static_cast<int>(std::floor(clip.top() / m_lineHeight)),
                                m_rows - 1);
    // The clip's bottom edge is exclusive: a band exactly one row high must not
    // pull the row below it into the loop.
    const int lastRow = qBound(firstRow,
                               static_cast<int>(std::ceil(clip.bottom() / m_lineHeight)) - 1,
                               m_rows - 1);

    // Rebuilt only when the attribute set changes, not per run: a QFont copy plus
    // four setters per cell run is measurable while a compiler scrolls.
    quint16 fontAttrs = 0xffff;
    QFont runFont = m_font;

    for (int row = firstRow; row <= lastRow; ++row) {
        const int absolute = top + row;
        if (absolute < 0 || absolute >= total)
            continue;
        const VtLine &line = m_screen->lineAt(absolute);
        const qreal y = row * m_lineHeight;
        const qreal baseline = y + m_baseline;
        const int cells = qMin<int>(line.size(), m_columns);

        int index = 0;
        while (index < cells) {
            const VtCell &first = line.at(index);
            // A zero-width cell is the covered half of a double-width character:
            // it carries no glyph of its own and is skipped outright. A run is cut
            // at any cell that is not a plain single-width one, so the single
            // drawText() per run below can rely on the font's advance matching the
            // grid; wide glyphs are drawn one at a time, at their exact column.
            if (first.width == 0) {
                ++index;
                continue;
            }

            int end = index + 1;
            if (first.width == 1) {
                while (end < cells) {
                    const VtCell &cell = line.at(end);
                    if (cell.width != 1 || cell.fg != first.fg || cell.bg != first.bg
                        || cell.attrs != first.attrs) {
                        break;
                    }
                    ++end;
                }
            }
            const int runCells = first.width == 1 ? end - index : first.width;

            QColor foreground = resolve(first.fg);
            QColor background = resolve(first.bg);
            if (first.attrs & VtInverse)
                std::swap(foreground, background);
            if (first.attrs & VtDim)
                foreground = blend(foreground, background, kDimBlendPercent);

            const QRectF runRect(index * m_cellWidth, y, runCells * m_cellWidth, m_lineHeight);
            if (background != m_defaultBackground)
                painter->fillRect(runRect, background);

            // VtHidden is the password case (SGR 8): the cells keep their
            // background and their width, and no glyph is drawn.
            if (!(first.attrs & VtHidden)) {
                if (first.attrs != fontAttrs) {
                    runFont = m_font;
                    runFont.setBold(first.attrs & VtBold);
                    runFont.setItalic(first.attrs & VtItalic);
                    runFont.setUnderline(first.attrs & VtUnderline);
                    runFont.setStrikeOut(first.attrs & VtStrike);
                    // VtBlink is deliberately NOT rendered, and that is a
                    // decision rather than an omission: blinking means a repaint
                    // timer running for as long as anything on screen carries
                    // SGR 5, which on a phone is a wakeup a second forever and a
                    // battery cost for an attribute most terminals have ignored
                    // since CRTs. The cell keeps its colours and its glyph, which
                    // is what xterm does with `-bl` off.
                    fontAttrs = first.attrs;
                }
                QVarLengthArray<char32_t, 256> text;
                for (int cell = index; cell < end; ++cell)
                    text.append(line.at(cell).text);
                painter->setFont(runFont);
                painter->setPen(foreground);
                painter->drawText(QPointF(index * m_cellWidth, baseline),
                                  QString::fromUcs4(text.data(), text.size()));
            }
            index = first.width == 1 ? end : index + first.width;
        }
    }

    // ---- cursor ----
    const int cursorAbsolute = m_screen->firstScreenRow() + m_screen->cursorRow();
    m_paintedCursorAbsoluteRow = cursorAbsolute;
    const int cursorRow = cursorAbsolute - top;
    if (!m_screen->cursorVisible() || cursorRow < firstRow || cursorRow > lastRow)
        return;
    const int cursorColumn = qBound(0, m_screen->cursorColumn(), m_columns - 1);
    const QRectF cell(cursorColumn * m_cellWidth, cursorRow * m_lineHeight,
                      m_cellWidth, m_lineHeight);
    if (m_cursorFocused) {
        // Focused: a filled block with the glyph knocked out of it, so the
        // character under the cursor stays readable.
        painter->fillRect(cell, m_defaultForeground);
        if (cursorAbsolute >= 0 && cursorAbsolute < total) {
            const VtLine &line = m_screen->lineAt(cursorAbsolute);
            if (cursorColumn < line.size() && line.at(cursorColumn).width != 0) {
                painter->setFont(m_font);
                painter->setPen(m_defaultBackground);
                const char32_t code = line.at(cursorColumn).text;
                painter->drawText(QPointF(cell.left(), cursorRow * m_lineHeight + m_baseline),
                                  QString::fromUcs4(&code, 1));
            }
        }
        return;
    }
    // Unfocused: hollow, which is how a terminal says the keyboard is elsewhere.
    painter->setPen(m_defaultForeground);
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(cell.adjusted(0.5, 0.5, -0.5, -0.5));
}

} // namespace ch
