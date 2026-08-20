import QtQuick
import QtQuick.Controls

// A full-page "waiting on the server" veil.
//
// It is modal by construction rather than by a Popup: it covers its parent and
// swallows every press while it is shown. That is deliberate. The two things it
// covers - a connect handshake and a layout load - are exactly the moments when a
// second tap does damage: AppController refuses a second connect while one is in
// flight (so the tap is silently lost, which reads as a broken button), and a
// second Dev Session tap starts a load that discards the first (so the user waits
// twice). Blocking is the honest presentation of "this one is already happening".
Item {
    id: veil

    // The sentence explaining what is being waited for. Server-controlled text
    // can reach this - a connection error, an RPC message - so it is rendered as
    // plain text like every other string in this module.
    property string message: ""

    anchors.fill: parent
    visible: false
    // Above the page content it covers, but below nothing else: there is only
    // ever one of these on screen.
    z: 100

    // Swallow the whole gesture stream, including presses that would otherwise
    // reach a delegate underneath through the veil's transparent areas.
    MouseArea {
        anchors.fill: parent
        enabled: veil.visible
        acceptedButtons: Qt.AllButtons
        preventStealing: true
        onWheel: function(wheel) { wheel.accepted = true; }
    }

    Rectangle {
        anchors.fill: parent
        color: MobileTheme.surface
        opacity: 0.88
    }

    Column {
        anchors.centerIn: parent
        spacing: MobileTheme.spacingLarge
        width: Math.min(parent.width - 2 * MobileTheme.spacingLarge, 420)

        BusyIndicator {
            anchors.horizontalCenter: parent.horizontalCenter
            running: veil.visible
        }

        Text {
            width: parent.width
            visible: veil.message.length > 0
            text: veil.message
            // SPEC 7.5: every server-controlled string in this module is plain
            // text. Nothing here may interpret markup, and AutoText is not a
            // middle ground - it GUESSES, and a guess that lands on rich text is
            // the whole vulnerability.
            textFormat: Text.PlainText
            color: MobileTheme.textDim
            font.pixelSize: MobileTheme.fontSizeBody
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            maximumLineCount: 4
            elide: Text.ElideRight
        }
    }
}
