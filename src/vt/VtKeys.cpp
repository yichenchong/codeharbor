#include "VtKeys.h"

namespace ch::vt {
namespace {

// xterm's modifier parameter: 1 plus a bitmask. Shared by the CSI and SS3 forms,
// and the reason a modified arrow key is "CSI 1 ; 5 A" rather than a different
// sequence per combination.
int modifierParameter(Qt::KeyboardModifiers mods)
{
    int value = 1;
    if (mods.testFlag(Qt::ShiftModifier))
        value += 1;
    if (mods.testFlag(Qt::AltModifier))
        value += 2;
    if (mods.testFlag(Qt::ControlModifier))
        value += 4;
    // Meta (Command on macOS, Super elsewhere) is deliberately NOT folded in: it
    // is the platform's own modifier, and a terminal that reported it would make
    // Cmd-C send bytes instead of copying.
    return value;
}

// A cursor-style key: CSI final, SS3 final, or the modified CSI 1;m final form.
QByteArray cursorKey(char finalByte, bool applicationCursorKeys, int modParam)
{
    QByteArray out;
    out.reserve(8);
    if (modParam > 1) {
        // The modified form is ALWAYS CSI, even in application mode: SS3 has no
        // parameter slot to put the modifier in.
        out += "\x1b[1;";
        out += QByteArray::number(modParam);
        out += finalByte;
        return out;
    }
    out += applicationCursorKeys ? "\x1bO" : "\x1b[";
    out += finalByte;
    return out;
}

// A "tilde" key: CSI code ~ , or CSI code ; m ~ when modified.
QByteArray tildeKey(int code, int modParam)
{
    QByteArray out;
    out.reserve(10);
    out += "\x1b[";
    out += QByteArray::number(code);
    if (modParam > 1) {
        out += ';';
        out += QByteArray::number(modParam);
    }
    out += '~';
    return out;
}

// Control codes for Ctrl + a printable key. Returns -1 when this key has no
// control code, so the caller can fall back to the event's text.
int controlCode(int qtKey)
{
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z)
        return qtKey - Qt::Key_A + 1; // Ctrl-A == 0x01 .. Ctrl-Z == 0x1a

    switch (qtKey) {
    case Qt::Key_Space:
    case Qt::Key_At:
    case Qt::Key_2:
        return 0x00; // NUL
    case Qt::Key_BracketLeft:
    case Qt::Key_3:
        return 0x1b; // ESC
    case Qt::Key_Backslash:
    case Qt::Key_4:
        return 0x1c; // FS
    case Qt::Key_BracketRight:
    case Qt::Key_5:
        return 0x1d; // GS
    case Qt::Key_AsciiCircum:
    case Qt::Key_6:
        return 0x1e; // RS
    case Qt::Key_Underscore:
    case Qt::Key_Minus:
    case Qt::Key_7:
        return 0x1f; // US
    case Qt::Key_Question:
    case Qt::Key_Slash:
    case Qt::Key_8:
        return 0x7f; // DEL
    default:
        // The digit spellings above (Ctrl-2 .. Ctrl-8) exist because that is how
        // these codes are reachable on a keyboard - and on a phone's software
        // keyboard the shifted symbols may not be reachable at all.
        return -1;
    }
}

} // namespace

QByteArray encodeKey(int qtKey, Qt::KeyboardModifiers mods, const QString &text,
                     bool applicationCursorKeys)
{
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool alt = mods.testFlag(Qt::AltModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const int modParam = modifierParameter(mods);

    // Navigation, editing and function keys carry their modifiers INSIDE the
    // sequence, so they return directly and never get the Alt-as-ESC treatment
    // below: "ESC ESC [ A" is not something any program understands.
    switch (qtKey) {
    case Qt::Key_Up:
        return cursorKey('A', applicationCursorKeys, modParam);
    case Qt::Key_Down:
        return cursorKey('B', applicationCursorKeys, modParam);
    case Qt::Key_Right:
        return cursorKey('C', applicationCursorKeys, modParam);
    case Qt::Key_Left:
        return cursorKey('D', applicationCursorKeys, modParam);
    case Qt::Key_Home:
        return cursorKey('H', applicationCursorKeys, modParam);
    case Qt::Key_End:
        return cursorKey('F', applicationCursorKeys, modParam);
    case Qt::Key_Insert:
        return tildeKey(2, modParam);
    case Qt::Key_Delete:
        return tildeKey(3, modParam);
    case Qt::Key_PageUp:
        return tildeKey(5, modParam);
    case Qt::Key_PageDown:
        return tildeKey(6, modParam);
    case Qt::Key_F1:
        return cursorKey('P', true, modParam);
    case Qt::Key_F2:
        return cursorKey('Q', true, modParam);
    case Qt::Key_F3:
        return cursorKey('R', true, modParam);
    case Qt::Key_F4:
        return cursorKey('S', true, modParam);
    case Qt::Key_F5:
        return tildeKey(15, modParam);
    case Qt::Key_F6:
        return tildeKey(17, modParam);
    case Qt::Key_F7:
        return tildeKey(18, modParam);
    case Qt::Key_F8:
        return tildeKey(19, modParam);
    case Qt::Key_F9:
        return tildeKey(20, modParam);
    case Qt::Key_F10:
        return tildeKey(21, modParam);
    case Qt::Key_F11:
        return tildeKey(23, modParam);
    case Qt::Key_F12:
        return tildeKey(24, modParam);
    case Qt::Key_Backtab:
        return QByteArrayLiteral("\x1b[Z");
    default:
        break;
    }
    // Some platforms deliver Shift+Tab as Key_Tab with ShiftModifier rather than
    // as Key_Backtab.
    if (qtKey == Qt::Key_Tab && shift)
        return QByteArrayLiteral("\x1b[Z");

    QByteArray base;
    switch (qtKey) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        // CR, not LF: the PTY's line discipline turns CR into "the user pressed
        // Return", and a bare LF would insert a literal line feed instead.
        base = QByteArrayLiteral("\r");
        break;
    case Qt::Key_Backspace:
        // DEL (0x7f). Every modern stty has erase=^?, and sending BS instead is
        // the classic "backspace prints ^H in bash" bug. Ctrl-Backspace sends BS,
        // which is what readline binds to backward-kill-word.
        base = ctrl ? QByteArrayLiteral("\x08") : QByteArrayLiteral("\x7f");
        break;
    case Qt::Key_Tab:
        base = QByteArrayLiteral("\t");
        break;
    case Qt::Key_Escape:
        base = QByteArrayLiteral("\x1b");
        break;
    default:
        break;
    }

    if (base.isEmpty() && ctrl) {
        const int code = controlCode(qtKey);
        if (code >= 0)
            base = QByteArray(1, static_cast<char>(code));
    }

    if (base.isEmpty()) {
        if (text.isEmpty())
            return {}; // a bare modifier press, or a key with no representation
        base = text.toUtf8();
        // A control character already produced by the platform (some keyboards
        // deliver Ctrl-C as text "\x03") is passed through untouched.
    }

    if (alt && !base.isEmpty()) {
        // Alt/Meta is an ESC prefix, the convention every Unix terminal and
        // readline expects ("Meta-b" is ESC b, not a byte with the high bit set).
        QByteArray prefixed;
        prefixed.reserve(base.size() + 1);
        prefixed += '\x1b';
        prefixed += base;
        return prefixed;
    }
    return base;
}

QByteArray encodePaste(const QString &text, bool bracketedPaste)
{
    const QByteArray utf8 = text.toUtf8();

    QByteArray body;
    body.reserve(utf8.size());
    // qsizetype, not int: a clipboard payload is caller-supplied and QByteArray
    // is indexed by qsizetype, so an int counter would be the one place in this
    // file where a large paste could overflow the index rather than the buffer.
    for (qsizetype i = 0; i < utf8.size(); ++i) {
        const char byte = utf8.at(i);
        if (byte == '\r') {
            // Collapse CRLF to a single CR so a paste from a Windows editor does
            // not submit every line twice.
            if (i + 1 < utf8.size() && utf8.at(i + 1) == '\n')
                ++i;
            body += '\r';
            continue;
        }
        if (byte == '\n') {
            body += '\r';
            continue;
        }
        if (byte == '\t') {
            body += byte;
            continue;
        }
        const auto value = static_cast<unsigned char>(byte);
        if (value < 0x20 || value == 0x7f)
            continue; // see VtKeys.h: an ESC in a paste is an injection vector
        body += byte;
    }

    if (!bracketedPaste)
        return body;

    QByteArray out;
    out.reserve(body.size() + 12);
    out += "\x1b[200~";
    out += body;
    out += "\x1b[201~";
    return out;
}

} // namespace ch::vt
