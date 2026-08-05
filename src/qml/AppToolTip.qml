import QtQuick
import QtQuick.Controls.Basic
import CodeHarbor

// The one tooltip used by every hint in the application (SPEC 4.1).
//
//     Button      { AppToolTip { text: qsTr("…"); visible: parent.hovered } }
//     Rectangle   { AppToolTip { text: hint; visible: someHover.hovered } }
//
// It is a ToolTip, so `text`, `delay`, `timeout` and `visible` behave exactly
// as they do on the attached `ToolTip.text` form.
//
// The attached form is what this replaces. That one is drawn by the Basic
// style's own tooltip, in the STYLE's light palette — near-black text on white —
// so a hint about this application's dark chrome arrived as a white box beside
// it. Two call sites had already been converted by hand to an inline themed
// ToolTip and four had not, which is how the application came to have two
// tooltip appearances; this is that inline block, said once.
ToolTip {
    id: control

    // Long enough that crossing a control on the way somewhere else does not
    // flash the hint. Call sites that want the slower sidebar timing set
    // `delay` themselves.
    delay: 400
    padding: 6

    contentItem: Label {
        // A hint can repeat a Dev Session name, a remote path or a server
        // message, all of which are data and never markup.
        textFormat: Text.PlainText
        text: control.text
        color: Theme.text
        font.pixelSize: Theme.fontSizeSmall
    }

    background: Rectangle {
        color: Theme.surfaceSunken
        border.color: Theme.border
        border.width: 1
        radius: Theme.radiusSmall
    }
}
