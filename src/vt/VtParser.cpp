#include "VtParser.h"

#include "VtScreen.h"

#include <QChar>

namespace ch {
namespace {

// C0 controls this parser reacts to. Named rather than inlined as magic numbers
// because the same values appear in three states.
constexpr quint8 kNul = 0x00;
constexpr quint8 kBel = 0x07;
constexpr quint8 kBs = 0x08;
constexpr quint8 kHt = 0x09;
constexpr quint8 kLf = 0x0a;
constexpr quint8 kVt = 0x0b;
constexpr quint8 kFf = 0x0c;
constexpr quint8 kCr = 0x0d;
constexpr quint8 kSo = 0x0e;
constexpr quint8 kSi = 0x0f;
constexpr quint8 kCan = 0x18;
constexpr quint8 kSub = 0x1a;
constexpr quint8 kEsc = 0x1b;
constexpr quint8 kDel = 0x7f;

constexpr char32_t kReplacement = 0xFFFD;

// The longest window title worth keeping. Titles are server-controlled strings
// that end up in mobile QML (always as Text.PlainText - SPEC 7.5), and a
// megabyte-long "title" is an attack on the layout, not a title.
constexpr int kMaxTitleChars = 256;

bool isFinalByte(quint8 byte)
{
    return byte >= 0x40 && byte <= 0x7e;
}

bool isIntermediateByte(quint8 byte)
{
    return byte >= 0x20 && byte <= 0x2f;
}

// Characters that must never reach a window title.
//
// The title is a server-controlled string that ends up in the mobile QML chrome.
// It is always rendered as Text.PlainText (SPEC 7.5), so it cannot inject
// markup, but plain text is still enough to lie: a line or paragraph separator
// breaks the chrome's layout, and a bidi override or isolate reorders the text
// AROUND the title, which is how "release-notes.txt" is made to read as
// "release-notes.exe". Control characters go for the same reason.
//
// Zero-width joiners are deliberately NOT stripped even though they are also
// Other_Format: they are ordinary content inside an emoji sequence, and removing
// them would corrupt legitimate titles. Only the reordering controls are
// dangerous, so only those are named.
bool isForbiddenTitleChar(QChar c)
{
    switch (c.category()) {
    case QChar::Other_Control:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
        return true;
    default:
        break;
    }
    const char16_t code = c.unicode();
    return code == 0x200E || code == 0x200F        // LRM, RLM
        || (code >= 0x202A && code <= 0x202E)      // LRE, RLE, PDF, LRO, RLO
        || (code >= 0x2066 && code <= 0x2069);     // LRI, RLI, FSI, PDI
}

} // namespace

VtParser::VtParser(VtScreen *screen)
    : m_screen(screen)
{
}

void VtParser::feed(const QByteArray &bytes)
{
    // Iterated over raw bytes: QByteArray::at() would bounds-check every byte of
    // every screen redraw for no benefit, and there is nothing in this loop that
    // can reallocate the buffer.
    const auto *data = reinterpret_cast<const quint8 *>(bytes.constData());
    const qsizetype size = bytes.size();
    for (qsizetype i = 0; i < size; ++i)
        handleByte(data[i]);
}

void VtParser::reset()
{
    m_state = State::Ground;
    m_utf8Accumulator = 0;
    m_utf8Remaining = 0;
    m_utf8Lower = 0;
    m_string.clear();
    m_stringOverflow = false;
    m_stringEscape = false;
    clearSequence();
}

void VtParser::clearSequence()
{
    m_paramCount = 0;
    m_paramSlot = -1;
    m_pendingIsSub = false;
    m_privateMarker = 0;
    m_intermediate = 0;
    m_intermediateOverflow = false;
}

int VtParser::param(int index, int fallback) const
{
    if (index < 0 || index >= m_paramCount)
        return fallback;
    const int value = m_params[index];
    // A negative slot is an OMITTED parameter ("CSI ;5H"), which means "use the
    // sequence's default", not zero.
    return value < 0 ? fallback : value;
}

void VtParser::collectParamDigit(quint8 byte)
{
    if (m_paramSlot < 0) {
        if (m_paramCount < kMaxParams) {
            m_params[m_paramCount] = 0;
            m_paramIsSub[m_paramCount] = m_pendingIsSub;
            m_paramSlot = m_paramCount;
            ++m_paramCount;
        } else {
            m_paramSlot = kMaxParams; // sink
        }
    }
    if (m_paramSlot < kMaxParams) {
        int &value = m_params[m_paramSlot];
        // Clamped rather than wrapped: a parameter of 99999999999 is malformed,
        // and every consumer clamps to the grid anyway, but signed overflow
        // would be undefined behaviour.
        if (value <= 65535)
            value = value * 10 + (byte - '0');
    }
}

void VtParser::nextParam(bool subParameter)
{
    if (m_paramSlot < 0 && m_paramCount < kMaxParams) {
        // Nothing was accumulating, so this separator closes an EMPTY slot.
        m_params[m_paramCount] = -1;
        m_paramIsSub[m_paramCount] = m_pendingIsSub;
        ++m_paramCount;
    }
    m_paramSlot = -1;
    m_pendingIsSub = subParameter;
}

void VtParser::collectIntermediate(quint8 byte)
{
    if (m_intermediate == 0)
        m_intermediate = byte;
    else
        m_intermediateOverflow = true;
}

void VtParser::appendString(quint8 byte)
{
    if (m_string.size() >= kMaxStringBytes) {
        m_stringOverflow = true;
        return;
    }
    m_string.append(static_cast<char>(byte));
}

void VtParser::emitReplacement()
{
    m_utf8Remaining = 0;
    m_utf8Accumulator = 0;
    m_utf8Lower = 0;
    m_screen->printCodePoint(kReplacement);
}

void VtParser::decodeUtf8(quint8 byte)
{
    if (m_utf8Remaining > 0) {
        if ((byte & 0xc0) == 0x80) {
            m_utf8Accumulator = (m_utf8Accumulator << 6) | static_cast<char32_t>(byte & 0x3f);
            if (--m_utf8Remaining == 0) {
                const char32_t codePoint = m_utf8Accumulator;
                const char32_t lower = m_utf8Lower;
                m_utf8Accumulator = 0;
                m_utf8Lower = 0;
                // Overlong encodings, surrogates and out-of-range code points are
                // all reported as U+FFFD. Decoding them instead would let a
                // stream smuggle an ASCII byte past anything that inspected the
                // raw bytes, and would put an unpaired surrogate into a
                // char32_t that QString cannot represent.
                const bool valid = codePoint >= lower && codePoint <= 0x10FFFF
                                   && !(codePoint >= 0xD800 && codePoint <= 0xDFFF);
                m_screen->printCodePoint(valid ? codePoint : kReplacement);
            }
            return;
        }
        // An invalid continuation byte. Report the broken character, then
        // REPROCESS this byte as a fresh start below: dropping it instead is what
        // makes a decoder desynchronise, because the byte may itself be a valid
        // lead byte or an ASCII character.
        emitReplacement();
    }

    if (byte < 0x80) {
        m_screen->printCodePoint(byte);
        return;
    }
    if ((byte & 0xe0) == 0xc0) {
        m_utf8Accumulator = byte & 0x1f;
        m_utf8Remaining = 1;
        m_utf8Lower = 0x80;
        return;
    }
    if ((byte & 0xf0) == 0xe0) {
        m_utf8Accumulator = byte & 0x0f;
        m_utf8Remaining = 2;
        m_utf8Lower = 0x800;
        return;
    }
    if ((byte & 0xf8) == 0xf0) {
        m_utf8Accumulator = byte & 0x07;
        m_utf8Remaining = 3;
        m_utf8Lower = 0x10000;
        return;
    }
    // A stray continuation byte (0x80-0xbf) or a 5/6-byte lead that UTF-8 has not
    // permitted since 2003.
    m_screen->printCodePoint(kReplacement);
}

void VtParser::printByte(quint8 byte)
{
    if (m_utf8Remaining > 0 || byte >= 0x80)
        decodeUtf8(byte);
    else
        m_screen->printCodePoint(byte);
}

void VtParser::executeC0(quint8 byte)
{
    switch (byte) {
    case kBel:
        m_screen->requestBell();
        break;
    case kBs:
        m_screen->backspace();
        break;
    case kHt:
        m_screen->horizontalTab(1);
        break;
    case kLf:
    case kVt:
    case kFf:
        // VT and FF are line feeds on every terminal in use; they honour the
        // scroll region exactly as LF does.
        m_screen->lineFeed();
        break;
    case kCr:
        m_screen->carriageReturn();
        break;
    case kSo:
    case kSi:
        // Charset shift-out/shift-in. This engine is UTF-8 only, so the G1
        // charset can never be anything but the G0 one; ignoring them is safe,
        // whereas printing them would put spurious glyphs on screen.
        break;
    case kNul:
        // A pad byte. Never printed: it has zero width and would otherwise
        // overwrite a cell with an invisible character.
        break;
    default:
        break;
    }
}

void VtParser::handleByte(quint8 byte)
{
    switch (m_state) {
    case State::Ground:
        if (byte == kEsc) {
            if (m_utf8Remaining > 0)
                emitReplacement();
            clearSequence();
            m_state = State::Escape;
            return;
        }
        if (byte < 0x20 || byte == kDel) {
            if (m_utf8Remaining > 0)
                emitReplacement();
            if (byte != kDel)
                executeC0(byte);
            return;
        }
        printByte(byte);
        return;

    case State::Escape:
        if (byte == kEsc) {
            clearSequence();
            return; // ESC ESC: the second one starts over
        }
        if (byte == kCan || byte == kSub) {
            m_state = State::Ground;
            return;
        }
        if (byte < 0x20) {
            executeC0(byte);
            return;
        }
        if (byte == kDel)
            return;
        if (isIntermediateByte(byte)) {
            collectIntermediate(byte);
            m_state = State::EscapeIntermediate;
            return;
        }
        switch (byte) {
        case '[':
            m_state = State::CsiEntry;
            return;
        case ']':
            m_string.clear();
            m_stringOverflow = false;
            m_stringEscape = false;
            m_state = State::OscString;
            return;
        case 'P':
            m_state = State::DcsEntry;
            return;
        case 'X': // SOS
        case '^': // PM
        case '_': // APC
            m_stringEscape = false;
            m_state = State::StringIgnore;
            return;
        default:
            // Ground BEFORE the dispatch, never after. A dispatched action can
            // re-enter this parser: ch::VtScreen::deviceStatusReport() emits a
            // reply, and a listener that answers by feeding those bytes back into
            // the same screen (a loopback, an echo test, or a session whose input
            // and output are wired together) calls feed() from inside this call.
            // With the state still set to the sequence being dispatched, those
            // bytes were consumed as a continuation of it and silently vanished.
            //
            // Safe because no dispatch reads parser state after its action runs:
            // every case evaluates its arguments first, and nothing follows the
            // switch in dispatchEscape()/dispatchCsi().
            m_state = State::Ground;
            dispatchEscape(byte);
            return;
        }

    case State::EscapeIntermediate:
        if (byte == kEsc) {
            clearSequence();
            m_state = State::Escape;
            return;
        }
        if (byte == kCan || byte == kSub) {
            // CAN and SUB abort a sequence in EVERY state that can be inside
            // one, not just in Escape and CsiEntry. Falling through to
            // executeC0() instead would leave the machine collecting
            // intermediates, so the bytes after the abort would be swallowed as
            // part of a sequence the sender has already cancelled.
            m_state = State::Ground;
            return;
        }
        if (byte < 0x20) {
            executeC0(byte);
            return;
        }
        if (isIntermediateByte(byte)) {
            collectIntermediate(byte);
            return;
        }
        if (byte == kDel)
            return;
        // Ground first, for the re-entrancy reason given above.
        m_state = State::Ground;
        dispatchEscape(byte);
        return;

    case State::CsiEntry:
    case State::CsiParam:
        if (byte == kEsc) {
            clearSequence();
            m_state = State::Escape;
            return;
        }
        if (byte == kCan || byte == kSub) {
            m_state = State::Ground;
            return;
        }
        if (byte < 0x20) {
            // A C0 control INSIDE a CSI is executed and the sequence continues.
            // Real programs do this: a shell prompt redraw can interleave a CR
            // with a sequence it is still emitting.
            executeC0(byte);
            return;
        }
        if (byte == kDel)
            return;
        if (byte >= '0' && byte <= '9') {
            collectParamDigit(byte);
            m_state = State::CsiParam;
            return;
        }
        if (byte == ';') {
            nextParam(false);
            m_state = State::CsiParam;
            return;
        }
        if (byte == ':') {
            nextParam(true);
            m_state = State::CsiParam;
            return;
        }
        if (byte >= 0x3c && byte <= 0x3f) {
            // '<' '=' '>' '?' are only legal immediately after the CSI.
            if (m_state == State::CsiEntry && m_paramCount == 0) {
                m_privateMarker = byte;
                m_state = State::CsiParam;
            } else {
                m_state = State::CsiIgnore;
            }
            return;
        }
        if (isIntermediateByte(byte)) {
            collectIntermediate(byte);
            m_state = State::CsiIntermediate;
            return;
        }
        if (isFinalByte(byte)) {
            // Ground first: a CSI action can re-enter feed() (DSR emits a reply
            // whose handler may write back), and with the state left in CsiParam
            // those bytes were swallowed as more of this sequence.
            m_state = State::Ground;
            dispatchCsi(byte);
            return;
        }
        m_state = State::CsiIgnore;
        return;

    case State::CsiIntermediate:
        if (byte == kEsc) {
            clearSequence();
            m_state = State::Escape;
            return;
        }
        if (byte == kCan || byte == kSub) {
            m_state = State::Ground;
            return;
        }
        if (byte < 0x20) {
            executeC0(byte);
            return;
        }
        if (byte == kDel)
            return;
        if (isIntermediateByte(byte)) {
            collectIntermediate(byte);
            return;
        }
        if (isFinalByte(byte)) {
            // Ground first, same re-entrancy reason as the CsiParam case.
            m_state = State::Ground;
            dispatchCsi(byte);
            return;
        }
        m_state = State::CsiIgnore;
        return;

    case State::CsiIgnore:
        if (byte == kEsc) {
            clearSequence();
            m_state = State::Escape;
            return;
        }
        if (byte == kCan || byte == kSub) {
            m_state = State::Ground;
            return;
        }
        if (byte < 0x20) {
            executeC0(byte);
            return;
        }
        if (isFinalByte(byte))
            m_state = State::Ground;
        return;

    case State::DcsEntry:
    case State::DcsParam:
        if (byte == kEsc) {
            clearSequence();
            m_state = State::Escape;
            return;
        }
        if (byte == kCan || byte == kSub) {
            m_state = State::Ground;
            return;
        }
        if (byte >= '0' && byte <= '9') {
            collectParamDigit(byte);
            m_state = State::DcsParam;
            return;
        }
        if (byte == ';') {
            nextParam(false);
            m_state = State::DcsParam;
            return;
        }
        if (byte == ':') {
            nextParam(true);
            m_state = State::DcsParam;
            return;
        }
        if (isIntermediateByte(byte)) {
            collectIntermediate(byte);
            m_state = State::DcsIntermediate;
            return;
        }
        if (isFinalByte(byte)) {
            // No DCS command is implemented (DECRQSS, Sixel and ReGIS are the
            // real users, and none of them is meaningful on a phone), but the
            // PAYLOAD still has to be consumed to its ST, or its bytes would be
            // executed as terminal commands - the exact hazard a passthrough
            // state exists to prevent.
            m_string.clear();
            m_stringOverflow = false;
            m_stringEscape = false;
            m_state = State::DcsPassthrough;
            return;
        }
        m_state = State::DcsIgnore;
        return;

    case State::DcsIntermediate:
        if (byte == kEsc) {
            clearSequence();
            m_state = State::Escape;
            return;
        }
        if (byte == kCan || byte == kSub) {
            m_state = State::Ground;
            return;
        }
        if (isIntermediateByte(byte)) {
            collectIntermediate(byte);
            return;
        }
        if (isFinalByte(byte)) {
            m_string.clear();
            // Reset alongside m_string, exactly as the DcsEntry/DcsParam path
            // does: the two DCS entry points share one buffer, and leaving their
            // bookkeeping to differ by which route was taken is how a stale flag
            // eventually leaks into the OSC path that reads it.
            m_stringOverflow = false;
            m_stringEscape = false;
            m_state = State::DcsPassthrough;
            return;
        }
        m_state = State::DcsIgnore;
        return;

    case State::DcsIgnore:
    case State::DcsPassthrough:
    case State::StringIgnore:
    case State::OscString: {
        const bool dispatches = (m_state == State::OscString);
        if (m_stringEscape) {
            m_stringEscape = false;
            if (byte == '\\') { // ST
                // Ground BEFORE the dispatch, for the same re-entrancy reason as
                // the CSI cases. The string buffer is cleared AFTER, because
                // dispatchOsc() reads it — so the two cannot simply be swapped.
                m_state = State::Ground;
                if (dispatches)
                    dispatchOsc();
                m_string.clear();
                return;
            }
            // A stray ESC aborts the string. The byte after it belongs to a NEW
            // escape sequence and must be reprocessed, not swallowed.
            m_string.clear();
            clearSequence();
            m_state = State::Escape;
            handleByte(byte);
            return;
        }
        if (byte == kEsc) {
            m_stringEscape = true;
            return;
        }
        if (byte == kBel) {
            // xterm's BEL terminator, which is what almost every program still
            // uses for OSC 0/2 window titles. Ground first, string cleared after
            // the dispatch that reads it, as above.
            m_state = State::Ground;
            if (dispatches)
                dispatchOsc();
            m_string.clear();
            return;
        }
        if (byte == kCan || byte == kSub) {
            m_string.clear();
            m_state = State::Ground;
            return;
        }
        if (byte < 0x20)
            return; // other C0 bytes are not part of the payload
        if (dispatches)
            appendString(byte);
        return;
    }
    }
}

void VtParser::dispatchEscape(quint8 finalByte)
{
    if (m_intermediateOverflow)
        return;

    if (m_intermediate == '#') {
        // DECALN. The only ESC # sequence worth implementing: it is what every
        // conformance test and several TUI probes use to check the grid.
        if (finalByte == '8')
            m_screen->alignmentTest();
        return;
    }
    if (m_intermediate == '(' || m_intermediate == ')' || m_intermediate == '*'
        || m_intermediate == '+' || m_intermediate == '-' || m_intermediate == '.'
        || m_intermediate == '/') {
        // Charset designation (ESC ( B and friends). Consumed and ignored: this
        // engine is UTF-8 only, and the alternative - honouring the DEC line
        // drawing charset - is unnecessary because programs that draw boxes over
        // a UTF-8 terminal emit the Unicode box characters directly.
        return;
    }
    if (m_intermediate != 0)
        return;

    switch (finalByte) {
    case 'D': // IND
        m_screen->lineFeed();
        break;
    case 'E': // NEL
        m_screen->nextLine();
        break;
    case 'M': // RI
        m_screen->reverseIndex();
        break;
    case 'H': // HTS
        m_screen->setTabStopAtCursor();
        break;
    case '7': // DECSC
        m_screen->saveCursor();
        break;
    case '8': // DECRC
        m_screen->restoreCursor();
        break;
    case 'c': // RIS
        // hardReset() rather than VtScreen::reset(): the parser is mid-feed, so
        // it must not reset ITSELF here (there is nothing pending to drop, and
        // clobbering m_state would fight the caller that is about to set it).
        m_screen->hardReset();
        break;
    case '=': // DECKPAM
    case '>': // DECKPNM
        // Keypad application mode. Ignored deliberately: a phone has no numeric
        // keypad, so there is no key whose encoding could differ.
        break;
    case '\\': // stray ST
    default:
        break;
    }
}

void VtParser::dispatchPrivateMode(quint8 finalByte)
{
    const bool enable = (finalByte == 'h');
    for (int i = 0; i < m_paramCount; ++i) {
        const int mode = param(i, -1);
        if (mode >= 0)
            m_screen->setMode(mode, enable, true);
    }
}

void VtParser::dispatchCsi(quint8 finalByte)
{
    if (m_intermediateOverflow)
        return;

    if (m_privateMarker == '?') {
        if (finalByte == 'h' || finalByte == 'l')
            dispatchPrivateMode(finalByte);
        // CSI ? Ps n (DECDSR) and the rest are not answered: unlike DSR proper,
        // no program blocks on them.
        return;
    }
    if (m_privateMarker != 0) {
        // CSI > c (secondary device attributes), CSI < ... - queries and mouse
        // reports this engine does not answer. Consumed silently.
        return;
    }

    if (m_intermediate == '!') {
        if (finalByte == 'p') // DECSTR, soft terminal reset
            m_screen->softReset();
        return;
    }
    if (m_intermediate != 0) {
        // CSI Ps SP q (cursor style), CSI Ps $ ... (DECRQM and friends). The
        // cursor style is a renderer concern the mobile view does not vary, and
        // the request forms are not answered for the same reason as above.
        return;
    }

    // Movement and edit counts default to 1 and treat an explicit 0 as 1, which
    // is what ECMA-48 means by "Pn with a default of 1".
    const auto count = [this](int index) { return qMax(1, param(index, 1)); };

    switch (finalByte) {
    case '@': // ICH
        m_screen->insertCharacters(count(0));
        break;
    case 'A': // CUU
        m_screen->cursorUp(count(0));
        break;
    case 'B': // CUD
    case 'e': // VPR
        m_screen->cursorDown(count(0));
        break;
    case 'C': // CUF
    case 'a': // HPR
        m_screen->cursorForward(count(0));
        break;
    case 'D': // CUB
        m_screen->cursorBack(count(0));
        break;
    case 'E': // CNL
        m_screen->cursorNextLine(count(0));
        break;
    case 'F': // CPL
        m_screen->cursorPreviousLine(count(0));
        break;
    case 'G': // CHA
    case '`': // HPA
        m_screen->setCursorColumn(count(0));
        break;
    case 'H': // CUP
    case 'f': // HVP
        m_screen->setCursorPosition(count(0), count(1));
        break;
    case 'I': // CHT
        m_screen->horizontalTab(count(0));
        break;
    case 'J': // ED
        m_screen->eraseInDisplay(param(0, 0));
        break;
    case 'K': // EL
        m_screen->eraseInLine(param(0, 0));
        break;
    case 'L': // IL
        m_screen->insertLines(count(0));
        break;
    case 'M': // DL
        m_screen->deleteLines(count(0));
        break;
    case 'P': // DCH
        m_screen->deleteCharacters(count(0));
        break;
    case 'S': // SU
        m_screen->scrollUpLines(count(0));
        break;
    case 'T': // SD
        m_screen->scrollDownLines(count(0));
        break;
    case 'X': // ECH
        m_screen->eraseCharacters(count(0));
        break;
    case 'Z': // CBT
        m_screen->reverseTab(count(0));
        break;
    case 'd': // VPA
        m_screen->setCursorRow(count(0));
        break;
    case 'g': // TBC
        m_screen->clearTabStop(param(0, 0));
        break;
    case 'm': // SGR
        m_screen->applySgr(m_params, m_paramIsSub, m_paramCount);
        break;
    case 'n': // DSR
        m_screen->deviceStatusReport(param(0, 0));
        break;
    case 'r': // DECSTBM
        m_screen->setScrollRegion(param(0, 1), param(1, m_screen->rows()));
        break;
    case 's': // SCOSC (ANSI.SYS save cursor)
        m_screen->saveCursor();
        break;
    case 'u': // SCORC
        m_screen->restoreCursor();
        break;
    case 'h': // SM
    case 'l': // RM
        // The only non-private ANSI modes are IRM (4) and LNM (20). Neither is
        // implemented: insert mode is used by no program written this century,
        // and LNM would make LF also do a CR, which breaks every program that
        // does not expect it.
        break;
    case 'c': // DA
    case 't': // window manipulation
    default:
        break;
    }
}

void VtParser::dispatchOsc()
{
    if (m_stringOverflow || m_string.isEmpty())
        return;

    // "Ps ; Pt". A missing ';' means a bare numeric command with no payload.
    int cursor = 0;
    int code = 0;
    bool haveCode = false;
    while (cursor < m_string.size() && m_string.at(cursor) >= '0' && m_string.at(cursor) <= '9') {
        code = code * 10 + (m_string.at(cursor) - '0');
        if (code > 99999)
            return; // malformed
        haveCode = true;
        ++cursor;
    }
    if (!haveCode)
        return;
    if (cursor < m_string.size() && m_string.at(cursor) != ';')
        return; // not a code, e.g. "2abc" - discarded rather than guessed at
    if (cursor < m_string.size())
        ++cursor;

    switch (code) {
    case 0: // set icon name and window title
    case 1: // set icon name
    case 2: // set window title
        break;
    default:
        // OSC 4 (palette), 7 (cwd), 8 (hyperlink), 52 (clipboard), 133 (prompt
        // marks) and everything else: parsed to their terminator above and
        // discarded here, which is the whole point of parsing them at all - an
        // unrecognised OSC must not leak its payload onto the screen.
        return;
    }

    QString title = QString::fromUtf8(m_string.constData() + cursor,
                                      m_string.size() - cursor);
    // Strip the characters that can misrepresent the title, then bound its
    // length: a control character, a line separator or a bidi override in a
    // server-controlled string reaches the QML chrome intact otherwise. See
    // isForbiddenTitleChar for what each class does there.
    title.removeIf(isForbiddenTitleChar);
    if (title.size() > kMaxTitleChars)
        title.truncate(kMaxTitleChars);
    m_screen->setWindowTitle(title);
}

} // namespace ch
