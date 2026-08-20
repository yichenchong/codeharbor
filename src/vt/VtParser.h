#pragma once

// The byte-level half of the engine: a UTF-8 decoder and an ANSI/DEC escape
// sequence state machine that turns a raw PTY stream into calls on ch::VtScreen.
//
// WHY it is a state machine and not a "find the sequence in the buffer" scan:
// the bytes arrive in whatever chunks an SSH channel produces, so a sequence can
// be split anywhere - between the ESC and the '[', in the middle of a parameter,
// in the middle of a UTF-8 character, in the middle of an OSC title. A scanner
// would have to buffer an unbounded tail and re-scan it; the state machine keeps
// only the few bytes of the sequence it is inside, and resuming is free.
//
// The structure follows Paul Williams' DEC-compatible parser: a small set of
// states, each with the "what does this byte class do here" table folded into a
// switch. Not shared with the desktop terminal because the desktop has no
// parser at all - xterm.js inside Qt WebEngine is the desktop's parser.

#include <QByteArray>
#include <QString>

namespace ch {

class VtScreen;

class VtParser {
public:
    // A sequence with more parameters than this is malformed; the extra ones are
    // dropped and the sequence is still dispatched, because refusing it wholesale
    // would lose the leading parameters that were valid. 32 covers every real
    // sequence - the longest in practice is an SGR with two truecolour specs.
    static constexpr int kMaxParams = 32;
    // Hard cap on an OSC/DCS/APC string payload. An unterminated string must not
    // grow without bound: the same bounded-failure reasoning as
    // ch::TerminalController::kMaxPendingEscapeBytes. Past the cap the payload is
    // discarded but the machine STAYS in the string state, so the eventual
    // terminator is still consumed and the stream does not desynchronise.
    static constexpr int kMaxStringBytes = 8 * 1024;

    explicit VtParser(VtScreen *screen);

    void feed(const QByteArray &bytes);
    // Drop any partially received sequence and any partial UTF-8 character.
    void reset();

private:
    enum class State {
        Ground,
        Escape,
        EscapeIntermediate,
        CsiEntry,
        CsiParam,
        CsiIntermediate,
        CsiIgnore,
        OscString,
        DcsEntry,
        DcsParam,
        DcsIntermediate,
        DcsPassthrough,
        DcsIgnore,
        // SOS/PM/APC: a string we never interpret but must still consume to its
        // terminator, or the payload would be executed as commands.
        StringIgnore
    };

    void handleByte(quint8 byte);
    void executeC0(quint8 byte);
    void printByte(quint8 byte);
    void decodeUtf8(quint8 byte);
    void emitReplacement();

    void clearSequence();
    void collectParamDigit(quint8 byte);
    void nextParam(bool subParameter);
    void collectIntermediate(quint8 byte);
    void appendString(quint8 byte);

    void dispatchEscape(quint8 finalByte);
    void dispatchCsi(quint8 finalByte);
    void dispatchOsc();
    void dispatchPrivateMode(quint8 finalByte);

    int param(int index, int fallback) const;

    VtScreen *m_screen;
    State m_state = State::Ground;

    // --- UTF-8 decoder -------------------------------------------------------
    // Retained across feed() calls. m_utf8Lower is the smallest code point the
    // pending sequence is allowed to produce, which is how overlong encodings
    // (the classic "\xC0\x80 is a NUL that slips past a filter" trick) are
    // rejected rather than decoded.
    char32_t m_utf8Accumulator = 0;
    int m_utf8Remaining = 0;
    char32_t m_utf8Lower = 0;

    // --- sequence state ------------------------------------------------------
    int m_params[kMaxParams] = {};
    // Whether parameter i was separated from parameter i-1 by ':' rather than
    // ';'. Needed because the two are NOT interchangeable: `38:2:r:g:b` is one
    // colour parameter with sub-parameters, while `4:3` is "underline, curly
    // style" and must not be read as "underline; italic".
    bool m_paramIsSub[kMaxParams] = {};
    int m_paramCount = 0;
    // Index of the parameter currently accumulating digits, -1 when none is (so
    // that ";;" can be told apart from ";0;"), or kMaxParams when the sequence
    // has overflowed and further digits must be swallowed rather than appended
    // to the last valid parameter.
    int m_paramSlot = -1;
    // Whether the NEXT parameter to be started is a sub-parameter, i.e. whether
    // the separator just seen was ':'.
    bool m_pendingIsSub = false;
    // Private-parameter prefix byte ('?', '<', '=', '>') or 0.
    quint8 m_privateMarker = 0;
    // Intermediate bytes 0x20-0x2f. Only the first is kept; no sequence this
    // engine implements uses two, and keeping one avoids a second buffer.
    quint8 m_intermediate = 0;
    bool m_intermediateOverflow = false;

    QByteArray m_string;
    bool m_stringOverflow = false;
    // Set when an ESC is seen inside a string: the next byte decides whether it
    // was the ST terminator (ESC \) or a stray ESC that aborts the string.
    bool m_stringEscape = false;
};

} // namespace ch
