import QtQuick
import QtQuick.Controls

// The one place a message from the server, the transport or the shell is shown.
//
// A phone has no status bar, no log pane and no room for a toast stack, so this
// is a single strip pinned above the page content, collapsed to zero height when
// there is nothing to say. It is DISMISSIBLE and does not auto-hide: the messages
// that land here are refusals and failures ("that Dev Session is no longer in the
// workspace", a host-key change, an RPC error forwarded verbatim), and a strip
// that faded after three seconds would hide the only explanation the user gets on
// a surface where they cannot scroll back through a log.
Rectangle {
    id: bar

    // The message to show. Empty collapses the strip.
    property string message: ""
    // Whether this message is a failure rather than a progress note. It only
    // changes the colour: a red strip for "connect failed", a quiet one for
    // "opening session…".
    property bool failure: false

    // Dismissal is LOCAL and lasts exactly as long as the message it was aimed
    // at: the next CHANGE of `message` brings the strip back. Clearing `message`
    // upstream is not required and must not be, because the status text is a
    // property of the controller, not of this view.
    //
    // Remembering the dismissed TEXT instead would have been wrong, and was: a
    // second failure with the same wording (two wrong passwords in a row, the
    // same pane refused twice) stayed hidden forever, leaving the user with a
    // button that appeared to do nothing and no explanation on screen. A repeat
    // of identical text is not a case this has to solve — assigning a string
    // property the value it already holds emits no change at all, so a genuine
    // re-notification is always a change and a genuine duplicate is never one.
    property bool _dismissed: false

    readonly property bool showing: message.length > 0 && !_dismissed

    onMessageChanged: bar._dismissed = false

    height: showing ? Math.max(implicitHeight, MobileTheme.touchTarget) : 0
    implicitHeight: row.implicitHeight + 2 * MobileTheme.spacing
    visible: height > 0
    clip: true
    color: failure ? MobileTheme.errorSurface() : MobileTheme.surfaceRaised

    Behavior on height {
        NumberAnimation { duration: 120; easing.type: Easing.OutQuad }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: MobileTheme.borderSubtle
    }

    Row {
        id: row
        anchors.fill: parent
        anchors.margins: MobileTheme.spacing
        spacing: MobileTheme.spacing

        Text {
            width: Math.max(0, row.width - dismiss.width - row.spacing)
            anchors.verticalCenter: parent.verticalCenter
            text: bar.message
            // SPEC 7.5. This strip exists to display strings the SERVER chose,
            // which is precisely why it may never interpret them as markup.
            textFormat: Text.PlainText
            color: bar.failure ? MobileTheme.danger : MobileTheme.text
            font.pixelSize: MobileTheme.fontSizeBody
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
        }

        // A full touch target, not a 16-pixel glyph: this is the only control on
        // the strip and it must be hittable without aiming.
        AbstractButton {
            id: dismiss
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: MobileTheme.touchTarget
            implicitHeight: MobileTheme.touchTarget
            onClicked: bar._dismissed = true

            contentItem: Text {
                text: "\u00d7"
                textFormat: Text.PlainText
                color: MobileTheme.textDim
                font.pixelSize: MobileTheme.fontSizeTitle
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
}
