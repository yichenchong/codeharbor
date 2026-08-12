import QtQuick
import QtQuick.Controls.Basic
import CodeHarbor

// The shell's non-blocking error toast (SPEC 10.3): the one place a shell-level
// failure — app.error, SessionLayouts.error, or a palette command that cannot
// run — is reported to the user. Hosted by Main.qml, which anchors it over the
// three regions and calls show() from its single notifyUser() funnel:
//
//     AppErrorBanner { id: errorBanner; z: 1000; anchors.top: parent.top }
//
// It is a file of its own for two reasons. It is a hundred lines of self-
// contained toast that had nothing to do with the window layout around it, and
// — the reason it moved — it could not be tested where it lived: Main.qml is an
// ApplicationWindow, which a QQuickView-based fixture cannot host, so the only
// gate that ever reached this toast was the one that loads the whole real
// application. On its own it is an ordinary component a test can show, dismiss
// and interrogate.
//
// The dismiss control is the accessibility fix that prompted the move. It used
// to be a bare MouseArea over the whole toast: no name, no focus, no key, so a
// keyboard or screen-reader user could not get rid of a shell error at all and
// simply had to wait out the six-second timer.
Rectangle {
    id: banner

    // Raise the toast with `message` and restart the auto-hide. Setting the
    // label imperatively rather than binding it is deliberate: this is the only
    // writer, and a binding here would be re-established on every show.
    function show(message) {
        errorLabel.text = message;
        banner.opacity = 0.97;
        hideTimer.restart();
    }

    // Take it back down. Both the timer and the dismiss button come through
    // here so that "hidden" always also means "not about to be hidden again a
    // moment later".
    //
    // The keyboard is handed back explicitly. Without that, a user who pressed
    // Space on the dismiss button would leave the focus sitting on a button
    // that is now invisible: Tab still works from there, but the next toast
    // would come up already focused and start its countdown under the focus it
    // is supposed to wait for.
    function dismiss() {
        hideTimer.stop();
        banner.opacity = 0;
        if (dismissButton.activeFocus)
            dismissButton.focus = false;
    }

    // True while the toast is on screen. What a host binds to if it wants to
    // know.
    readonly property bool raised: banner.opacity > 0

    objectName: "shellErrorBanner"
    visible: banner.opacity > 0
    opacity: 0
    width: Math.min(errorLabel.implicitWidth + dismissButton.width + 40, parent.width - 24)
    height: Math.max(errorLabel.implicitHeight + 20, dismissButton.height + 12)
    radius: Theme.radiusMedium
    color: Theme.danger

    // The toast is the ONLY place a shell-level failure is reported, and a bare
    // Rectangle carries no accessibility at all: without this a screen reader
    // never learns that anything went wrong. AlertMessage is the role for a
    // transient notification that is not a dialog.
    Accessible.role: Accessible.AlertMessage
    Accessible.name: errorLabel.text

    Behavior on opacity { NumberAnimation { duration: 200 } }

    Label {
        id: errorLabel
        objectName: "shellErrorLabel"
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.right: dismissButton.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        // SECURITY: a Label defaults to Text.AutoText, which promotes any
        // string that merely LOOKS like markup to StyledText — and StyledText
        // fetches <img src="http://..."> and turns <a href> into a live link.
        // What lands here is app.error / SessionLayouts.error, i.e. RPC and
        // libssh failure text forwarded VERBATIM from the server (SPEC 10.3),
        // so a hostile server must not be able to turn this toast into a
        // network callback. Same rule as every other server-fed Label.
        textFormat: Text.PlainText
        color: Theme.textOnAccent
        font.pixelSize: Theme.fontSizeLabel
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }

    // A real control, not a bare MouseArea: Tab reaches it, Space and Enter
    // press it, an assistive technology can name and invoke it, and Tab moves
    // straight back out again because nothing here consumes it.
    Rectangle {
        id: dismissButton
        objectName: "shellErrorDismiss"
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        width: 24
        height: 24
        radius: Theme.radiusSmall
        color: dismissHover.hovered ? Qt.lighter(Theme.danger, 1.25) : "transparent"
        border.color: dismissButton.activeFocus ? Theme.textOnAccent : "transparent"
        border.width: 2

        // Tab order needs no gate of its own: the banner is `visible: false`
        // whenever it is down, and focus traversal skips invisible items.
        // Binding this to `raised` instead would try to withdraw the tab stop
        // from under the focus the moment the toast is dismissed, which Qt
        // refuses out loud ("Cannot set activeFocusOnTab to false once item is
        // the active focus item").
        activeFocusOnTab: true

        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Dismiss this message")
        Accessible.focusable: true
        Accessible.focused: dismissButton.activeFocus
        Accessible.onPressAction: banner.dismiss()

        // The auto-hide is a race a keyboard user cannot win: reaching this
        // button by Tab takes longer than six seconds, and a toast that
        // vanishes out from under the focus leaves the focus nowhere. Holding
        // the countdown while the button has the keyboard, and starting it over
        // when the focus leaves, keeps the toast still for as long as it is
        // being operated without making it permanent.
        onActiveFocusChanged: {
            if (dismissButton.activeFocus)
                hideTimer.stop();
            else if (banner.raised)
                hideTimer.restart();
        }

        Keys.onReturnPressed: function (event) {
            banner.dismiss();
            event.accepted = true;
        }
        Keys.onEnterPressed: function (event) {
            banner.dismiss();
            event.accepted = true;
        }
        Keys.onSpacePressed: function (event) {
            banner.dismiss();
            event.accepted = true;
        }

        Label {
            anchors.centerIn: parent
            text: "\u2715"
            textFormat: Text.PlainText
            color: Theme.textOnAccent
            font.pixelSize: Theme.fontSizeSmall
        }

        HoverHandler {
            id: dismissHover
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            onTapped: banner.dismiss()
        }
    }

    Timer {
        id: hideTimer
        interval: 6000
        onTriggered: banner.dismiss()
    }
}
