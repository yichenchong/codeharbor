import QtQuick.Controls.Basic

// A modal dialog wearing the application's own colour scheme (SPEC 4.1).
//
// Every other surface in this module paints itself explicitly — a Rectangle with
// a Catppuccin Mocha hex colour and a Label with a matching text colour — while
// the four dialogs (rename a session, new group, new Dev Session, SSH
// connection details) were the only things left drawing in the QtQuick.Controls
// Basic style's DEFAULT palette. That palette is light: a white sheet with black
// text and grey buttons, opening on top of a #1e1e2e window. It looked like a
// foreign window, and the text fields inside it inherited light-on-light
// colours from it too.
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
// The hex values are the same ones the hand-painted surfaces use, so the dialogs
// cannot drift away from the rest of the window:
//   #1e1e2e base surface   #11111b sunken/inset   #313244 raised control
//   #45475a border/pressed #cdd6f4 primary text   #6c7086 dimmed text
//   #89b4fa focus accent
Dialog {
    palette.window: "#1e1e2e"
    palette.windowText: "#cdd6f4"
    // Dialog border and the "checked/highlighted button" fill.
    palette.dark: "#45475a"
    // Text-field wells and anything drawn ON the accent colour.
    palette.base: "#11111b"
    palette.text: "#cdd6f4"
    palette.placeholderText: "#6c7086"
    palette.button: "#313244"
    palette.buttonText: "#cdd6f4"
    // Pressed-button blend target and the resting text-field border.
    palette.mid: "#45475a"
    palette.midlight: "#313244"
    palette.highlight: "#89b4fa"
    palette.highlightedText: "#11111b"
    palette.brightText: "#11111b"
    // The dim behind a modal dialog is this colour at 50% alpha.
    palette.shadow: "#11111b"
}
