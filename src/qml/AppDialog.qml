import QtQuick
import QtQuick.Controls.Basic
import CodeHarbor

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
// It adds one behaviour of its own: Enter presses the dialog's DEFAULT button,
// and that button is drawn in the accent colour so the keyboard's answer is
// visible before it is used. See `defaultButton` below.
//
// The values come from Theme (SPEC 4.1), so the dialogs cannot drift away from
// the rest of the window. The mapping is not one-to-one — several palette roles
// paint the same surface as each other — so each assignment below names the part
// of the dialog it is responsible for.
Dialog {
    id: control

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

    // The standard button Enter presses, and the one drawn in the accent colour.
    //
    // A dialog with nothing typed in it is answered with the keyboard or not at
    // all, so Enter has to mean something. It means the affirmative button by
    // default — OK in a rename, which is the common case — resolved from the
    // buttons the dialog actually has.
    //
    // A dialog whose affirmative answer DESTROYS something sets this to
    // `Dialog.Cancel`, so that a stray Enter closes the dialog rather than
    // deleting the thing it is asking about; `Dialog.NoButton` switches the
    // behaviour off entirely. Either way the marked button and the button Enter
    // presses are the same button, so what is drawn is what happens.
    property int defaultButton: control._affirmativeButton()

    // The first of these the dialog actually has. Ordered by how affirmative the
    // answer is, with Close last because a dialog that only reports something has
    // no other answer to give.
    function _affirmativeButton() {
        const preference = [Dialog.Ok, Dialog.Yes, Dialog.Save, Dialog.Apply,
                            Dialog.Retry, Dialog.Close];
        for (const button of preference) {
            if (control.standardButtons & button)
                return button;
        }
        return Dialog.NoButton;
    }

    // `standardButton()` builds its buttons lazily, so this is only reliable
    // once the dialog has been opened at least once — which is exactly when it
    // is needed.
    function _defaultButtonItem() {
        if (control.defaultButton === Dialog.NoButton)
            return null;
        return control.standardButton(control.defaultButton);
    }

    // Pressing the button rather than calling accept() keeps the button's ROLE
    // in charge: OK accepts, Close and Cancel reject, Discard reports a discard.
    // A disabled button does nothing, the same as it would under the pointer.
    function _activateDefaultButton() {
        const button = _defaultButtonItem();
        if (button && button.enabled)
            button.clicked();
    }

    // The button Enter presses, once the dialog has been opened. Exposed because
    // "what does Enter do here" is a question a caller and a test both ask, and
    // reading it back is the only way to be sure the marking, the announcement
    // and the key all name the same button.
    readonly property alias defaultButtonItem: control._markedButton
    property var _markedButton: null
    // Marked two ways, because "which button does Enter press" has to be
    // answerable by looking and by listening: the accent colour for the eye, and
    // a spoken description for a screen reader.
    function _markDefaultButton() {
        const button = _defaultButtonItem();
        if (button === _markedButton)
            return;
        if (_markedButton) {
            _markedButton.palette.button = undefined;
            _markedButton.palette.buttonText = undefined;
            _markedButton.Accessible.description = "";
        }
        _markedButton = button;
        if (button) {
            button.palette.button = Theme.accent;
            button.palette.buttonText = Theme.textOnAccent;
            button.Accessible.description =
                qsTr("Default button. Press Enter to choose it.");
        }
    }

    // `Connections` rather than an `onOpened:` handler here, because a call site
    // that declares its own `onOpened:` would REPLACE a handler declared in this
    // file and silently lose the marking.
    Connections {
        target: control
        function onOpened() { control._markDefaultButton(); }
        function onStandardButtonsChanged() { control._markDefaultButton(); }
        function onDefaultButtonChanged() { control._markDefaultButton(); }
    }

    // Enter and the keypad's Enter, only while this dialog is on screen.
    //
    // A `Shortcut` rather than a key handler because a Dialog is a Popup, not an
    // Item, so `Keys` cannot attach to it at all. This also leaves a focused
    // multi-line text area alone: the editor consumes Return first, and only an
    // otherwise-unhandled Return reaches the shortcut.
    Shortcut {
        sequences: [StandardKey.InsertParagraphSeparator]
        enabled: control.visible && control.defaultButton !== Dialog.NoButton
        onActivated: control._activateDefaultButton()
    }
}
