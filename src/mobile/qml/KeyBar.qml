import QtQuick
import QtQuick.Controls
import CodeHarbor.Mobile

// The keys a phone's soft keyboard does not have, for the mobile terminal pane
// (SPEC 5.1 on a touch device).
//
// WHY THIS IS NOT OPTIONAL CHROME: a shell is unusable without Ctrl, Esc, Tab
// and arrows, and no mobile keyboard offers any of them. Ctrl-C to stop a build,
// Esc to leave insert mode, Tab to complete a path and the arrows for history
// are the four things a terminal user does most, so they are the four things
// nearest the thumb.
//
// LATCHED MODIFIERS: Ctrl and Alt cannot be HELD on a touch screen — there is no
// second finger convention and no chord. They therefore latch: tapping Ctrl arms
// it (and says so, visibly), the next key is sent with it applied, and it clears
// itself immediately afterwards. That is the standard phone-terminal behaviour
// and it makes Ctrl-C two taps rather than an impossibility. Tapping an armed
// modifier again disarms it, so an accidental tap costs one more tap and never
// silently modifies the next key.
//
// Every key goes out through ch::MobileTerminalSession::sendKey(), i.e. through
// ch::vt::encodeKey(), so a key from this bar is encoded by exactly the same code
// as a key from the soft keyboard — including the cursor-key mode, which decides
// whether an arrow is CSI A or SS3 A.
Rectangle {
    id: keyBar

    // ch::MobileTerminalSession. Null leaves the bar visible but inert, which is
    // what a page loaded without a terminal service shows.
    property var session: null

    // Armed modifiers, applied to the NEXT key and then cleared. Read by the
    // page's own key handling too, so a hardware keyboard and this bar cannot
    // disagree about what is armed.
    property bool ctrlLatched: false
    property bool altLatched: false

    // Overlap between the soft keyboard and the bottom of this bar, in scene
    // coordinates. The bar reserves it as bottom padding so its keys sit directly
    // above the keyboard instead of underneath it, and because that padding is
    // part of implicitHeight the terminal above shrinks by the same amount
    // automatically — the page never has to compute an inset of its own.
    //
    // Measured as an OVERLAP rather than taken as the keyboard's height: this bar
    // is not necessarily flush with the bottom of the window (a status bar, a
    // safe-area inset, a host page with padding), and reserving the full keyboard
    // height would then leave a dead band the size of the difference.
    readonly property real keyboardInset: {
        if (!Qt.inputMethod.visible)
            return 0
        const keyboard = Qt.inputMethod.keyboardRectangle
        if (keyboard.height <= 0)
            return 0
        // The bar's bottom edge is measured through its PARENT and never through
        // `keyBar.height`. This value feeds implicitHeight and implicitHeight
        // drives height, so reading height here would make the binding depend on
        // its own result: QML reports a binding loop, abandons the re-entrant
        // evaluation, and the inset can be left a frame behind the keyboard.
        // The bar is anchored to its parent's bottom, so the two bottom edges are
        // the same line however tall the inset makes the bar.
        const host = keyBar.parent
        if (!host)
            return 0
        const bottomInScene = host.mapToItem(null, 0, host.height).y
        return Math.max(0, Math.min(keyboard.height, bottomInScene - keyboard.y))
    }

    // 48dp is the platform minimum for a touch target on both Android and iOS,
    // and a terminal key bar is exactly where an undersized target costs the user
    // a wrong Ctrl-C.
    readonly property real keyHeight: MobileTheme.touchTarget
    readonly property real keyGap: MobileTheme.spacingSmall

    implicitHeight: keyHeight + keyGap * 2 + keyboardInset
    color: MobileTheme.surfaceRaised

    // Take the armed modifiers and disarm them, which IS the latch contract: an
    // armed modifier applies to exactly one following key and then releases,
    // including when nothing was armed at all — a latch that survived its key
    // would silently modify the one after it.
    //
    // Shared with TerminalPage.qml, which has to consume the same latch from two
    // other input paths (a key event and an input-method commit). Three private
    // copies of "read them, OR them, clear them" is how one of the three ends up
    // forgetting to clear.
    function takeLatchedModifiers() {
        let modifiers = Qt.NoModifier
        if (keyBar.ctrlLatched)
            modifiers |= Qt.ControlModifier
        if (keyBar.altLatched)
            modifiers |= Qt.AltModifier
        keyBar.ctrlLatched = false
        keyBar.altLatched = false
        return modifiers
    }

    // Send one key with whatever modifiers are armed, then disarm them.
    //
    // `text` is the character the key would produce unmodified; ch::vt::encodeKey
    // needs it to encode a plain punctuation key, and ignores it for a key with a
    // control encoding of its own (an arrow, Home, Delete).
    function sendKey(qtKey, text) {
        if (!keyBar.session)
            return
        keyBar.session.sendKey(qtKey, keyBar.takeLatchedModifiers(), text)
    }

    // One key. A Rectangle plus a TapHandler rather than a Controls Button
    // because the visual state a latched modifier needs (armed/not) is not a
    // Button state, and because a Button's own padding fights a fixed 48dp grid.
    component KeyButton: Rectangle {
        id: keyButton

        property string label: ""
        // Armed modifiers draw themselves as pressed-in so the user can see what
        // the next key will carry.
        property bool armed: false
        // Wider than a single glyph for the multi-letter keys, so "PgDn" is not
        // squeezed into a 48dp square.
        property real cells: 1

        signal activated()

        implicitWidth: Math.max(keyBar.keyHeight, keyBar.keyHeight * cells)
        implicitHeight: keyBar.keyHeight
        radius: MobileTheme.radiusSmall
        color: keyButton.armed
               ? MobileTheme.accent
               : (tap.pressed ? MobileTheme.surfaceHover : MobileTheme.surface)
        border.width: 1
        border.color: keyButton.armed ? MobileTheme.accent : MobileTheme.border

        Text {
            anchors.centerIn: parent
            text: keyButton.label
            // PlainText everywhere in mobile QML (SPEC 7.5), without exception:
            // the rule is enforced by inspection, so a literal label is written
            // the same way as a server-controlled string.
            textFormat: Text.PlainText
            color: keyButton.armed ? MobileTheme.textOnAccent : MobileTheme.text
            font.pixelSize: MobileTheme.fontSizeLabel
        }

        TapHandler {
            id: tap
            // Keys must not steal focus from the page's input item: the soft
            // keyboard has to stay up while the user taps Ctrl.
            onTapped: keyButton.activated()
        }
    }

    // Horizontally scrollable, because the full key set is wider than a phone at
    // a 48dp grid and dropping keys to make it fit is what makes a mobile
    // terminal useless. Vertical flicking is off so a swipe up over the bar is
    // never mistaken for scrolling the terminal.
    Flickable {
        anchors.fill: parent
        anchors.margins: keyBar.keyGap
        // The keyboard reservation is ON TOP OF the ordinary gap, not instead of
        // it. An explicitly assigned edge margin OVERRIDES anchors.margins for
        // that edge, so `bottomMargin: keyboardInset` alone silently dropped the
        // bottom gap and left the row of keys standing on the bar's bottom edge —
        // and, with the keyboard down, one keyGap taller than the space
        // implicitHeight reserves for it.
        anchors.bottomMargin: keyBar.keyGap + keyBar.keyboardInset
        contentWidth: keyRow.implicitWidth
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Row {
            id: keyRow
            spacing: keyBar.keyGap

            KeyButton {
                label: qsTr("Esc")
                cells: 1.2
                onActivated: keyBar.sendKey(Qt.Key_Escape, "")
            }
            KeyButton {
                label: qsTr("Tab")
                cells: 1.2
                onActivated: keyBar.sendKey(Qt.Key_Tab, "\t")
            }
            KeyButton {
                label: qsTr("Ctrl")
                cells: 1.3
                armed: keyBar.ctrlLatched
                // Toggles instead of sending: a modifier is not a key.
                onActivated: keyBar.ctrlLatched = !keyBar.ctrlLatched
            }
            KeyButton {
                label: qsTr("Alt")
                cells: 1.2
                armed: keyBar.altLatched
                onActivated: keyBar.altLatched = !keyBar.altLatched
            }
            KeyButton {
                label: "\u2190"
                onActivated: keyBar.sendKey(Qt.Key_Left, "")
            }
            KeyButton {
                label: "\u2193"
                onActivated: keyBar.sendKey(Qt.Key_Down, "")
            }
            KeyButton {
                label: "\u2191"
                onActivated: keyBar.sendKey(Qt.Key_Up, "")
            }
            KeyButton {
                label: "\u2192"
                onActivated: keyBar.sendKey(Qt.Key_Right, "")
            }
            KeyButton {
                label: qsTr("Home")
                cells: 1.5
                onActivated: keyBar.sendKey(Qt.Key_Home, "")
            }
            KeyButton {
                label: qsTr("End")
                cells: 1.2
                onActivated: keyBar.sendKey(Qt.Key_End, "")
            }
            KeyButton {
                label: qsTr("PgUp")
                cells: 1.5
                onActivated: keyBar.sendKey(Qt.Key_PageUp, "")
            }
            KeyButton {
                label: qsTr("PgDn")
                cells: 1.5
                onActivated: keyBar.sendKey(Qt.Key_PageDown, "")
            }
            KeyButton {
                label: qsTr("Del")
                cells: 1.2
                onActivated: keyBar.sendKey(Qt.Key_Delete, "")
            }
            // The punctuation a shell needs constantly and a phone keyboard hides
            // two layers deep: a path separator, an option dash, a pipe and $HOME.
            KeyButton {
                label: "/"
                onActivated: keyBar.sendKey(Qt.Key_Slash, "/")
            }
            KeyButton {
                label: "-"
                onActivated: keyBar.sendKey(Qt.Key_Minus, "-")
            }
            KeyButton {
                label: "|"
                onActivated: keyBar.sendKey(Qt.Key_Bar, "|")
            }
            KeyButton {
                label: "~"
                onActivated: keyBar.sendKey(Qt.Key_AsciiTilde, "~")
            }
        }
    }
}
