#pragma once

// The cell vocabulary shared by the VT parser, the screen model and whatever
// renders it. Deliberately dependency-free apart from QtCore/QtGui value types:
// ch_vt exists because Qt WebEngine (and therefore xterm.js, which every
// desktop terminal pane is built on - see src/qml/TerminalPaneView.qml) does not
// exist on Android or iOS, so the mobile client has to model the terminal grid
// itself. Nothing here may reach for a transport, a QML type or a font.

#include <QRgb>
#include <QtGlobal>
#include <QVector>

namespace ch {

// SGR rendition bits. A bitfield rather than one bool per attribute because a
// cell is copied for every scroll of every line: 2 bytes of flags keeps VtCell
// at 16 bytes, which is what makes scrolling a memmove of contiguous cells.
enum VtAttr : quint16 {
    VtBold = 1 << 0,
    VtDim = 1 << 1,
    VtItalic = 1 << 2,
    VtUnderline = 1 << 3,
    VtBlink = 1 << 4,
    VtInverse = 1 << 5,
    VtHidden = 1 << 6,
    VtStrike = 1 << 7
};

// Sentinel colours meaning "whatever the renderer's theme calls default".
//
// They MUST be distinguishable from an explicitly selected colour, otherwise a
// theme cannot honour its own background: a program that says SGR 40 (black
// background) and a program that says nothing at all would arrive as the same
// QRgb, and the renderer would either lose the theme or lose the program's
// choice. Every colour this engine produces from an SGR sequence goes through
// qRgb(), which sets alpha to 0xff; these two carry alpha 0x00, a value no real
// palette entry can take. See ch::VtScreen::defaultForeground().
inline constexpr QRgb VtDefaultForeground = 0x00000001u;
inline constexpr QRgb VtDefaultBackground = 0x00000002u;

// One character cell.
//
// `text` is a single code point, never a QString: the parser writes a cell for
// every printable byte of terminal output, and a per-glyph QString would put an
// allocation in the hottest path in the engine. Combining marks are folded into
// the base code point instead of being stored separately (see
// VtScreen::printCodePoint).
//
// `width` is 1 for an ordinary cell, 2 for the LEADING cell of a double-width
// character, and 0 for the cell that leading cell covers. A renderer therefore
// draws only cells with width != 0, and a text extractor skips width == 0.
//
// The colour members carry initialisers even though the struct is otherwise a
// plain aggregate: VtLine is resized by QVector, which value-initialises new
// elements, and a zero QRgb is neither a valid palette colour nor one of the
// sentinels above. Without the initialisers every grow of the grid would
// produce cells that render as transparent black.
//
// The member ORDER is load-bearing, not cosmetic. The two one-and-two-byte
// members come last so that they share the tail padding of the four-byte ones:
// with `width` between `text` and `fg` the compiler inserts three bytes of
// padding and the cell becomes 20 bytes, a 25% larger grid and scrollback for
// nothing. The static_assert keeps the 16-byte figure that
// VtScreen::kMaxHistoryLines' memory budget is derived from honest.
struct VtCell {
    char32_t text = U' ';
    QRgb fg = VtDefaultForeground;
    QRgb bg = VtDefaultBackground;
    quint16 attrs = 0;
    quint8 width = 1;
};

static_assert(sizeof(VtCell) == 16, "VtCell must stay 16 bytes: the scrollback "
                                    "memory bound in VtScreen assumes it");

// A screen or history row. Always exactly `columns` cells long; the engine never
// stores ragged lines, because a renderer that draws a viewport by row index
// must not have to bounds-check every cell.
using VtLine = QVector<VtCell>;

namespace vt {

// Display width of a code point in terminal cells: 0 for a combining mark or a
// format character, 2 for an East Asian Wide/Fullwidth character, 1 otherwise.
//
// The zero-width answer comes from Qt's own Unicode tables (QChar::category),
// so no data is duplicated and no dependency is added. The double-width answer
// cannot: QtCore exposes no East Asian Width property, so the W/F ranges live
// in a sorted table in VtTypes.cpp.
int charWidth(char32_t codePoint);

// The xterm 256-colour palette: 0-7 standard, 8-15 bright, 16-231 the 6x6x6
// cube, 232-255 the greyscale ramp. Out-of-range indices answer the default
// foreground sentinel rather than a wrong colour.
QRgb xterm256Color(int index);

} // namespace vt

} // namespace ch
