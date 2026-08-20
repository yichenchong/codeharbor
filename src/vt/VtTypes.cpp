#include "VtTypes.h"

#include <QChar>

#include <algorithm>
#include <iterator>

namespace ch::vt {
namespace {

struct Range {
    char32_t first;
    char32_t last;
};

// East Asian Wide (W) and Fullwidth (F) code point ranges, sorted and merged.
//
// This table exists because QtCore/QtGui expose no East Asian Width property -
// QChar carries category, script, direction and decomposition, but not width -
// and a terminal that gets width wrong does not merely look wrong: the cursor
// column it computes diverges from the one the remote program computed, so
// every subsequent redraw lands in the wrong place. The alternative would be a
// new dependency (utf8proc/ICU), which the mobile client must not take on for
// one property.
//
// Derived from Unicode EastAsianWidth.txt: the W and F entries, with the
// contiguous CJK blocks merged into single spans. Ranges that are Wide only
// under emoji presentation (the Emoji_Presentation set) are included, because
// that is what every modern terminal and every terminal font does.
constexpr Range kWideRanges[] = {
    {0x1100, 0x115F},   // Hangul Jamo initial consonants
    {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},   {0x23F0, 0x23F0},
    {0x23F3, 0x23F3},   {0x25FD, 0x25FE},   {0x2614, 0x2615},   {0x2648, 0x2653},
    {0x267F, 0x267F},   {0x2693, 0x2693},   {0x26A1, 0x26A1},   {0x26AA, 0x26AB},
    {0x26BD, 0x26BE},   {0x26C4, 0x26C5},   {0x26CE, 0x26CE},   {0x26D4, 0x26D4},
    {0x26EA, 0x26EA},   {0x26F2, 0x26F3},   {0x26F5, 0x26F5},   {0x26FA, 0x26FA},
    {0x26FD, 0x26FD},   {0x2705, 0x2705},   {0x270A, 0x270B},   {0x2728, 0x2728},
    {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},   {0x2757, 0x2757},
    {0x2795, 0x2797},   {0x27B0, 0x27B0},   {0x27BF, 0x27BF},   {0x2B1B, 0x2B1C},
    {0x2B50, 0x2B50},   {0x2B55, 0x2B55},
    {0x2E80, 0x303E},   // CJK radicals .. CJK symbols (0x303F is Narrow)
    {0x3041, 0x33FF},   // Hiragana .. CJK compatibility
    {0x3400, 0x4DBF},   // CJK extension A
    {0x4E00, 0x9FFF},   // CJK unified ideographs
    {0xA000, 0xA4CF},   // Yi
    {0xA960, 0xA97F},   // Hangul Jamo extended A
    {0xAC00, 0xD7A3},   // Hangul syllables
    {0xF900, 0xFAFF},   // CJK compatibility ideographs
    {0xFE10, 0xFE19},   // vertical forms
    {0xFE30, 0xFE6F},   // CJK compatibility forms, small form variants
    {0xFF00, 0xFF60},   // fullwidth ASCII forms
    {0xFFE0, 0xFFE6},   // fullwidth signs
    {0x16FE0, 0x16FE4}, {0x16FF0, 0x16FF1}, {0x17000, 0x187F7}, {0x18800, 0x18CD5},
    {0x18D00, 0x18D08}, {0x1AFF0, 0x1AFFE}, {0x1B000, 0x1B152}, {0x1B164, 0x1B167},
    {0x1B170, 0x1B2FB}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F200, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C},
    {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0},
    {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC},
    {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A},
    {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1F978}, {0x1F97A, 0x1F9CB}, {0x1F9CD, 0x1F9FF}, {0x1FA70, 0x1FA74},
    {0x1FA78, 0x1FA7A}, {0x1FA80, 0x1FA86}, {0x1FA90, 0x1FAA8}, {0x1FAB0, 0x1FAB6},
    {0x1FAC0, 0x1FAC2}, {0x1FAD0, 0x1FAD6},
    {0x20000, 0x2FFFD}, // CJK extension B..F
    {0x30000, 0x3FFFD}, // CJK extension G
};

// The 16 ANSI colours, in the palette xterm ships with. They are the base of the
// 256-colour cube below and of SGR 30-37/90-97, so there is exactly one copy.
constexpr QRgb kAnsi16[16] = {
    qRgb(0x00, 0x00, 0x00), qRgb(0xcd, 0x00, 0x00), qRgb(0x00, 0xcd, 0x00), qRgb(0xcd, 0xcd, 0x00),
    qRgb(0x00, 0x00, 0xee), qRgb(0xcd, 0x00, 0xcd), qRgb(0x00, 0xcd, 0xcd), qRgb(0xe5, 0xe5, 0xe5),
    qRgb(0x7f, 0x7f, 0x7f), qRgb(0xff, 0x00, 0x00), qRgb(0x00, 0xff, 0x00), qRgb(0xff, 0xff, 0x00),
    qRgb(0x5c, 0x5c, 0xff), qRgb(0xff, 0x00, 0xff), qRgb(0x00, 0xff, 0xff), qRgb(0xff, 0xff, 0xff),
};

// The 6 levels of the 6x6x6 cube. Not a linear ramp - xterm's first step is 0
// and the rest are 95 + 40n - and getting it wrong makes every 256-colour
// program's greys visibly wrong.
constexpr int kCubeLevels[6] = {0, 95, 135, 175, 215, 255};

} // namespace

int charWidth(char32_t codePoint)
{
    // NUL and the C0/C1 control ranges never occupy a cell. The parser executes
    // them rather than printing them, but a caller extracting text from a cell
    // that some other path wrote must still get a sane answer.
    if (codePoint == 0 || codePoint < 0x20 || (codePoint >= 0x7f && codePoint < 0xa0))
        return 0;
    if (codePoint > 0x10FFFF)
        return 1;

    // U+00AD SOFT HYPHEN is the one exception that has to come before the
    // category test. Its category is Other_Format (Cf), so the switch below
    // would call it zero width and VtScreen would try to fold it into the
    // preceding cell as if it were a combining mark - which drops it, because no
    // precomposed form exists. Every wcwidth() and every terminal gives it one
    // cell instead: groff and `man` emit it in justified text, and a page whose
    // hyphens silently vanish is wrong in a way the user cannot explain.
    if (codePoint == 0x00AD)
        return 1;

    // Combining marks and format characters (ZWJ, ZWNJ, the bidi controls)
    // advance nothing: they modify the cell that precedes them. Non-spacing (Mn)
    // and enclosing (Me) marks are zero width by definition; spacing combining
    // marks (Mc) are treated the same way here because a terminal grid has no
    // way to advance "half a cell" for the Indic and Thai vowel signs that
    // dominate that category, and every widely used terminal makes the same
    // choice.
    switch (QChar::category(codePoint)) {
    case QChar::Mark_NonSpacing:
    case QChar::Mark_Enclosing:
    case QChar::Mark_SpacingCombining:
    case QChar::Other_Format:
        return 0;
    default:
        break;
    }

    const auto *end = kWideRanges + std::size(kWideRanges);
    const auto *hit = std::upper_bound(kWideRanges, end, codePoint,
                                       [](char32_t value, const Range &range) {
                                           return value < range.first;
                                       });
    if (hit != kWideRanges) {
        --hit;
        if (codePoint <= hit->last)
            return 2;
    }
    return 1;
}

QRgb xterm256Color(int index)
{
    if (index < 0 || index > 255)
        return VtDefaultForeground;
    if (index < 16)
        return kAnsi16[index];
    if (index < 232) {
        const int offset = index - 16;
        return qRgb(kCubeLevels[(offset / 36) % 6],
                    kCubeLevels[(offset / 6) % 6],
                    kCubeLevels[offset % 6]);
    }
    // 232-255: 24 greys from 8 to 238 in steps of 10. Pure black and pure white
    // are deliberately absent from the ramp; they live at indices 0 and 15.
    const int level = 8 + (index - 232) * 10;
    return qRgb(level, level, level);
}

} // namespace ch::vt
