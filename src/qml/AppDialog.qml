import QtQuick.Controls.Basic

// A modal dialog wearing the application's own colour scheme (SPEC 4.1).
//
// Every other surface in this module paints itself explicitly — a Rectangle in a
// Theme surface colour and a Label in a Theme text colour — while the four
// dialogs (rename a session, new group, new Dev Session, SSH connection details)
// were the only things left drawing in the QtQuick.Controls Basic style's DEFAULT
// palette. That palette is light: a white sheet with black text and grey buttons,
// opening on top of a dark window. It looked like a foreign window, and the text
// fields inside it inherited light-on-light colours from it too.
//
// The fix is a palette rather than replaced background/header/footer items,
// because every part of a Basic-style dialog already reads its colours from the
// palette: the sheet (palette.window), its border (palette.dark), the bold title
// Label (palette.windowText), the button box (palette.window) and its buttons
// (palette.button / palette.buttonText / palette.mid when pressed), and the
// TextField / TextArea / ScrollBar inside the content (palette.base,
// palette.text, palette.placeholderText, palette.highlight, palette.mid). A
// palette set here therefore reaches the children too — it propagates down the
// visual parent chain — so a dialog's contents need no per-control colours.
//
// Only colours are set. Everything a dialog does (modality, position, standard
// buttons, content) stays at the call site, so this is a drop-in replacement for
// `Dialog`.
//
// The values come from Theme (SPEC 4.1), so the dialogs cannot drift away from
// the rest of the window. The mapping is not one-to-one — several palette roles
// paint the same surface as each other — so each assignment below names the part
// of the dialog it is responsible for.
Dialog {
    palette.window: Theme.surface
    palette.windowText: Theme.text
    // Dialog border and the "checked/highlighted button" fill.
    palette.dark: Theme.border
    // Text-field wells and anything drawn ON the accent colour.
    palette.base: Theme.surfaceSunken
    palette.text: Theme.text
    palette.placeholderText: Theme.textDim
    palette.button: Theme.surfaceRaised
    palette.buttonText: Theme.text
    // Pressed-button blend target and the resting text-field border.
    palette.mid: Theme.border
    palette.midlight: Theme.surfaceRaised
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.textOnAccent
    palette.brightText: Theme.textOnAccent
    // The dim behind a modal dialog is this colour at 50% alpha.
    palette.shadow: Theme.surfaceSunken
}
