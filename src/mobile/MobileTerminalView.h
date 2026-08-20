#pragma once

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QRect>
#include <QSet>
#include <QQmlEngine>

// Complete, not forward-declared: ch::VtScreen is the type of a Q_PROPERTY, and
// moc's metatype registration requires a pointer property to point at a fully
// defined type.
#include "VtScreen.h"

namespace ch {

// The mobile terminal renderer: it draws one ch::VtScreen with QPainter.
//
// WHY THIS EXISTS: the desktop pane renders a terminal with xterm.js inside a
// WebEngineView, and Qt WebEngine ships on neither Android nor iOS. There is no
// HTML, no canvas and no JS in this path at all — the grid is parsed by ch_vt in
// process and painted here.
//
// WHAT IT IS NOT: it is not a terminal. It holds no parser, no scrollback and no
// input handling; it reports metrics and paints cells. In particular, and exactly
// like the desktop pane (see the RESIZE note in src/qml/TerminalPaneView.qml),
// IT NEVER RESIZES ANYTHING. Only this class knows the cell metrics, so it
// publishes `columns`/`rows` derived from its own size and font and lets QML feed
// them to ch::MobileTerminalSession::resize(), which is what reflows the screen
// and pushes the SSH window-change. A view that resized the session itself would
// re-enter the layout it is being laid out by.
//
// DAMAGE TRACKING is the reason this is a QQuickPaintedItem rather than a naive
// repaint-everything item. A terminal's ordinary output touches one line: a
// full-item repaint per flush would re-rasterise the whole grid up to a hundred
// times a second while a build scrolls, which on a phone is both a dropped-frame
// source and a battery cost. Every damage signal from the screen is translated
// into a ROW BAND and passed to update(QRect), so Qt only asks this item to
// repaint the bands that changed, and paint() then clips its own work to the
// dirty region it is given. See markRowsDamaged().
class MobileTerminalView : public QQuickPaintedItem {
    Q_OBJECT
    // Instantiated by TerminalPage.qml, so this one IS creatable.
    QML_ELEMENT
    Q_PROPERTY(ch::VtScreen *screen READ screen WRITE setScreen NOTIFY screenChanged)
    // The monospace font the grid is drawn in. The cell size is derived from it,
    // so assigning it re-derives `columns`/`rows` and the QML page reports the
    // new grid to the session.
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged)
    // How many lines back from the live bottom the view is showing: 0 follows the
    // output, `maxScrollOffset` is the oldest retained history line.
    Q_PROPERTY(int scrollOffset READ scrollOffset WRITE setScrollOffset NOTIFY scrollOffsetChanged)
    Q_PROPERTY(int maxScrollOffset READ maxScrollOffset NOTIFY maxScrollOffsetChanged)
    // Cell grid this item can show, derived from its size and font.
    Q_PROPERTY(int columns READ columns NOTIFY gridChanged)
    Q_PROPERTY(int rows READ rows NOTIFY gridChanged)
    Q_PROPERTY(qreal lineHeight READ lineHeight NOTIFY gridChanged)
    Q_PROPERTY(qreal cellWidth READ cellWidth NOTIFY gridChanged)
    // Theme colours for the two SENTINEL values a cell carries when the stream
    // never set an explicit colour (ch::VtScreen::defaultForeground() /
    // defaultBackground()). The VT stream cannot choose them, which is the point:
    // "default" has to mean the app's own palette, so a light theme is not left
    // drawing white on white.
    Q_PROPERTY(QColor defaultForegroundColor READ defaultForegroundColor
                   WRITE setDefaultForegroundColor NOTIFY defaultColorsChanged)
    Q_PROPERTY(QColor defaultBackgroundColor READ defaultBackgroundColor
                   WRITE setDefaultBackgroundColor NOTIFY defaultColorsChanged)
    // Whether the pane currently owns the keyboard, which is what decides whether
    // the cursor is drawn as a filled block or as a hollow outline.
    //
    // A PROPERTY rather than this item's own hasActiveFocus(), and that is not a
    // style choice: the item that actually holds the focus is the invisible
    // TextInput the soft keyboard and the input method require (see
    // src/mobile/qml/TerminalPage.qml — Qt raises a keyboard only for a focused
    // item that accepts input methods, and a QQuickPaintedItem does not). This
    // item therefore NEVER has active focus, so reading hasActiveFocus() here
    // reported "the keyboard is elsewhere" for ever and the pane drew the hollow
    // cursor of an unfocused terminal even while the user was typing into it.
    Q_PROPERTY(bool cursorFocused READ cursorFocused WRITE setCursorFocused
                   NOTIFY cursorFocusedChanged)
public:
    explicit MobileTerminalView(QQuickItem *parent = nullptr);

    VtScreen *screen() const;
    void setScreen(VtScreen *screen);

    QFont font() const;
    void setFont(const QFont &font);

    int scrollOffset() const;
    void setScrollOffset(int offset);
    int maxScrollOffset() const;

    int columns() const;
    int rows() const;
    qreal lineHeight() const;
    qreal cellWidth() const;

    QColor defaultForegroundColor() const;
    void setDefaultForegroundColor(const QColor &color);
    QColor defaultBackgroundColor() const;
    void setDefaultBackgroundColor(const QColor &color);

    bool cursorFocused() const;
    void setCursorFocused(bool focused);

    // Hit-testing for the pane's selection and long-press menu. QML has the touch
    // points; only this item knows where a cell is, and ch::VtScreen::textRange()
    // wants ABSOLUTE rows, so both conversions live here.
    Q_INVOKABLE int columnAt(qreal x) const;
    Q_INVOKABLE int absoluteRowAt(qreal y) const;
    // Absolute row currently drawn at the top of the item, i.e. the anchor every
    // row band is measured from.
    Q_INVOKABLE int topAbsoluteRow() const;

    void paint(QPainter *painter) override;

    // TEST SEAM: the union of the row bands handed to update() since the last
    // paint, in item coordinates, and empty when nothing is pending. It is what
    // makes "one changed line did not dirty the whole grid" an assertion rather
    // than a hope. Deliberately not invokable — QML has no business reading it.
    QRect pendingDamage() const;
    // TEST SEAM: item rows currently awaiting repaint, sorted.
    QList<int> pendingDamagedRows() const;

signals:
    void screenChanged();
    void fontChanged();
    void scrollOffsetChanged();
    void maxScrollOffsetChanged();
    void gridChanged();
    void defaultColorsChanged();
    void cursorFocusedChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void onLinesChanged(int firstAbsoluteRow, int lastAbsoluteRow);
    void onScreenScrolled(int lines);
    void onCursorMoved();
    void onScreenReset();
    // Recompute the cell metrics and the grid this item can show. Emits
    // gridChanged() only on an actual change, because the QML page answers it by
    // reporting the new geometry to the session, which resizes a remote PTY.
    void recomputeGrid();
    // Mark item rows [firstRow, lastRow] as needing repaint and ask Qt for that
    // band only. Out-of-range rows are clipped, an empty range costs nothing.
    void markRowsDamaged(int firstRow, int lastRow);
    void markAllDamaged();
    // Band, in item coordinates, covering item rows [firstRow, lastRow].
    QRect rowBand(int firstRow, int lastRow) const;
    void clampScrollOffset();

    // Not owned: the screen belongs to ch::MobileTerminalSession, which may
    // outlive or predecease this item depending on how the page is torn down.
    QPointer<VtScreen> m_screen;
    QFont m_font;
    QFontMetricsF m_metrics{QFont()};
    qreal m_cellWidth = 1.0;
    qreal m_lineHeight = 1.0;
    qreal m_baseline = 0.0;
    int m_columns = 0;
    int m_rows = 0;
    int m_scrollOffset = 0;
    QColor m_defaultForeground{Qt::white};
    QColor m_defaultBackground{Qt::black};
    // False until the page reports the keyboard is here, so a pane the user has
    // not touched yet draws the hollow cursor rather than claiming their input.
    bool m_cursorFocused = false;
    // Where the cursor was last painted, so a move repaints the row it LEFT as
    // well as the row it arrived on.
    int m_paintedCursorAbsoluteRow = -1;
    QSet<int> m_damagedRows;
    QRect m_pendingDamage;
};

} // namespace ch
