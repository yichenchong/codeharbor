import QtQuick
import QtQuick.Controls.Basic

// The one draggable divider used by every SplitView in the application
// (SPEC 4.1). Set it as the view's `handle`:
//
//     SplitView { handle: AppSplitHandle {} ... }
//
// The Basic style's default handle is a filled 4-pixel plate in the style's own
// palette, which on a dark window reads as a light grey gutter between the
// regions — the "looks out of place" chrome this replaces. Here the grab area is
// invisible (Theme.splitHandleThickness wide, so it can be hit without aiming)
// and only a one-pixel line is painted inside it, brightening to the accent
// while the pointer is on it or a drag is under way.
Item {
    id: handle

    implicitWidth: Theme.splitHandleThickness
    implicitHeight: Theme.splitHandleThickness

    // The handle's own state comes from the SplitHandle attached type (NOT
    // SplitView, which attaches the sizing properties to the view's CHILDREN):
    // SplitHandle.pressed and SplitHandle.hovered are what a handle delegate is
    // given. Compared against `true` so that using this item outside a SplitView
    // — where both read undefined, which does not assign to a bool — leaves it
    // inert rather than warning.
    readonly property bool engaged: handle.SplitHandle.pressed === true
                                    || handle.SplitHandle.hovered === true

    // The line runs ALONG the divider, so its direction follows the handle's own
    // shape rather than the view's orientation: SplitView stretches the handle
    // across the split, making it wide-and-short between stacked children and
    // tall-and-thin between side-by-side ones. Reading the geometry keeps this
    // component independent of whether `SplitView.view` is resolvable from a
    // handle delegate.
    readonly property bool horizontalLine: handle.width >= handle.height

    Rectangle {
        anchors.centerIn: parent
        width: handle.horizontalLine ? handle.width : 1
        height: handle.horizontalLine ? 1 : handle.height
        color: handle.engaged ? Theme.accent : Theme.border

        Behavior on color {
            ColorAnimation { duration: 120; easing.type: Easing.OutQuart }
        }
    }
}
