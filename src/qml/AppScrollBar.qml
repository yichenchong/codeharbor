import QtQuick
import QtQuick.Controls.Basic

// The one scrollbar used by every scrollable view in the application (SPEC 4.1).
// Drop it into any of the three standard slots:
//
//     ListView   { ScrollBar.vertical: AppScrollBar {} }
//     Flickable  { ScrollBar.vertical: AppScrollBar {} }
//     ScrollView { ScrollBar.horizontal: AppScrollBar {} }
//
// It is a ScrollBar, so every ScrollBar property (policy, interactive, size)
// still works and `policy` keeps its ScrollBar.AsNeeded default.
//
// The Basic style's own scrollbar is a light plate sized for a track with arrow
// buttons this application does not draw, which is why it stood out against a
// dark panel. This one has a transparent track, no buttons, and a thumb that
// fades in with the view's scroll activity and brightens under the pointer.
ScrollBar {
    id: control

    // Attached bars retain a live `size` ratio from their Flickable. A ratio of
    // one means the viewport already contains the whole content; zero is the
    // pre-layout/default value and likewise cannot prove that there is content
    // to scroll. Disabling and hiding the control together keeps that empty
    // track from becoming a hover target while still letting an overflowing
    // view use the normal transient scrollbar behavior.
    readonly property bool contentFits: control.size <= 0 || control.size >= 1
    enabled: !control.contentFits && control.policy !== ScrollBar.AlwaysOff
    visible: !control.contentFits && control.policy !== ScrollBar.AlwaysOff

    implicitWidth: Theme.scrollBarThickness
    implicitHeight: Theme.scrollBarThickness
    padding: 0

    // No track: the surface behind the bar is the panel's own, so a filled
    // track would draw a second, competing gutter beside every list.
    background: null

    contentItem: Rectangle {
        implicitWidth: Theme.scrollBarThickness
        implicitHeight: Theme.scrollBarThickness
        radius: Theme.radiusSmall
        color: control.pressed ? Theme.accent
                               : (control.hovered ? Theme.textDim : Theme.border)

        // `active` is true while the view is scrolling or the bar is hovered, so
        // an idle panel is not permanently outlined by a thumb. `enabled` is
        // also part of this guard: when content later fits, a running fade
        // cannot leave a handle visible or let a stale hover revive it.
        opacity: control.enabled
                 && (control.policy === ScrollBar.AlwaysOn || control.active) ? 1 : 0

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutQuart }
        }
        Behavior on color {
            ColorAnimation { duration: 120; easing.type: Easing.OutQuart }
        }
    }
}
