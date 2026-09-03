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

#include "VtTypes.h"  // VtMouseEncoding

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

// Encode ONE mouse-wheel notch as a button report.
//
// A wheel notch is reported as a button press with no matching release: xterm
// assigns 64 to wheel-up and 65 to wheel-down, and nothing sends a release for
// them. `column` and `row` are 1-based cell coordinates.
//
// Why this exists at all: tmux runs on the ALTERNATE screen, which has no
// scrollback of its own, so a client cannot scroll it by moving a local view
// offset - there is nothing above the screen to move to. tmux owns that history
// and only reveals it when it receives a wheel event, which is why the session
// turns `mouse on`. So scrolling an attached tmux means SENDING these bytes.
//
// `encoding` must come from VtScreen::mouseEncoding(), not from a "mouse is on"
// boolean:
//   None   -> returns empty. The program asked for no events; the caller should
//             fall back to whatever local scrollback it has.
//   Legacy -> CSI M followed by three bytes, each a coordinate or button offset
//             by 32. Coordinates are CLAMPED to 223, the largest value that
//             survives that packing, because a byte cannot carry more and a
//             wrapped value would report a wheel somewhere else entirely.
//   Sgr    -> CSI < b ; col ; row M, which has no coordinate ceiling.
QByteArray encodeMouseWheel(bool up, int column, int row,
                            VtMouseEncoding encoding);

} // namespace ch::vt
