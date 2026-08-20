#pragma once

// Key and paste encoding: Qt key events -> the bytes a PTY expects.
//
// Separate from VtScreen because it is pure, stateless translation, and because
// the two halves of a terminal are genuinely independent: the screen never needs
// to know what a key produced, and the encoder needs exactly one bit of screen
// state (whether the remote program asked for application cursor keys).
//
// On the desktop this job belongs to xterm.js inside Qt WebEngine, which does
// not exist on Android or iOS - hence this.

#include <QByteArray>
#include <QString>
#include <Qt>

namespace ch::vt {

// Encode one key press.
//
// `qtKey` is a Qt::Key value, `text` is QKeyEvent::text() (which already carries
// the shifted/composed character for ordinary keys, so this function must not
// try to re-derive it), and `applicationCursorKeys` is VtScreen's DECCKM state:
// it selects SS3 (ESC O A) instead of CSI (ESC [ A) for the cursor keys, which
// is what makes the arrow keys work inside readline and vim.
//
// Returns an empty array for a key that produces nothing at all (a bare modifier
// press, or an unhandled key with no text), which the caller must treat as "send
// nothing" rather than as an error.
QByteArray encodeKey(int qtKey, Qt::KeyboardModifiers mods, const QString &text,
                     bool applicationCursorKeys);

// Encode pasted text.
//
// With `bracketedPaste` on (DECSET 2004), the payload is wrapped in
// ESC [ 200 ~ ... ESC [ 201 ~ so the remote program can tell pasted text from
// typed text - which is what stops a shell from EXECUTING every line of a
// multi-line paste as it arrives.
//
// Newlines are normalised to CR in both modes, because a PTY in canonical mode
// treats LF as a literal line feed rather than as "the user pressed Return", and
// control characters other than TAB and CR are removed. The removal is not
// cosmetic: without it, a paste containing ESC [ 201 ~ could close the bracket
// early and have the rest of itself executed, and a paste containing a bare ESC
// could inject any escape sequence at all.
QByteArray encodePaste(const QString &text, bool bracketedPaste);

} // namespace ch::vt
