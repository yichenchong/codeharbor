import QtQuick
import QtQuick.Controls.Basic
import CodeHarbor

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
//
// It is also the ONLY way to resize a region, which is why it takes keyboard
// focus and answers the arrow keys: without that, region sizing was a
// pointer-only feature and assistive technology could not see the divider at
// all. Tab and Shift+Tab move the focus straight back out again — nothing here
// consumes them.
Item {
    id: handle

    // Emitted after a key has moved the divider, so a host that persists its
    // region sizes can write them back. A pointer drag is already covered by
    // SplitView.resizing; a key resize never sets that flag, so without this
    // signal a keyboard-only resize would be forgotten on the next launch.
    signal resized()

    // One arrow press, and one Page press. Sixteen pixels is small enough to
    // land on an exact width and large enough that crossing a 260-pixel sidebar
    // is not a hundred keystrokes; Page covers the rest in a handful.
    readonly property real keyStep: 16
    readonly property real pageStep: 96

    implicitWidth: Theme.splitHandleThickness
    implicitHeight: Theme.splitHandleThickness

    activeFocusOnTab: true

    // The handle's own state comes from the SplitHandle attached type (NOT
    // SplitView, which attaches the sizing properties to the view's CHILDREN):
    // SplitHandle.pressed and SplitHandle.hovered are what a handle delegate is
    // given. Compared against `true` so that using this item outside a SplitView
    // — where both read undefined, which does not assign to a bool — leaves it
    // inert rather than warning.
    readonly property bool engaged: handle.SplitHandle.pressed === true
                                    || handle.SplitHandle.hovered === true

    // The view this handle divides. SplitView.view is NOT usable here: the
    // attached object resolves its view as parentItem().parentItem(), which is
    // right for a CONTENT child (they live under the view's contentItem) but
    // wrong for a handle, which SplitView parents directly to itself — so the
    // attached property would resolve to whatever contains the view and warn.
    // The handle's own `parent` IS the view. Duck-typed rather than cast, so
    // this component still works standing on its own outside a SplitView.
    readonly property var view: handle.parent
                                && handle.parent.orientation !== undefined
                                && handle.parent.contentChildren !== undefined
                                ? handle.parent : null

    // Which way the divider MOVES. Inside a view that is the view's own
    // orientation; on its own, fall back to the shape SplitView would have
    // given it — a divider between side-by-side items is tall and thin.
    readonly property bool horizontal: handle.view ? handle.view.orientation === Qt.Horizontal
                                                   : handle.height >= handle.width

    // The line runs ALONG the divider, so its direction follows the handle's own
    // shape rather than the view's orientation: SplitView stretches the handle
    // across the split, making it wide-and-short between stacked children and
    // tall-and-thin between side-by-side ones. Reading the geometry keeps this
    // component independent of whether `SplitView.view` is resolvable from a
    // handle delegate.
    readonly property bool horizontalLine: handle.width >= handle.height

    Accessible.role: Accessible.Splitter
    Accessible.name: handle.horizontal
                     ? qsTr("Divider between the panels to its left and right")
                     : qsTr("Divider between the panels above and below it")
    Accessible.description: handle.horizontal
        ? qsTr("Left and Right move it, Page Up and Page Down move it further, Tab leaves it.")
        : qsTr("Up and Down move it, Page Up and Page Down move it further, Tab leaves it.")
    Accessible.focusable: true
    Accessible.focused: handle.activeFocus

    // Every visible item the view lays out, in declaration order. Hidden ones
    // are skipped for the same reason SplitView skips them: they occupy no
    // space, so no handle sits beside them.
    function visibleItems() {
        var out = [];
        if (!handle.view)
            return out;
        var kids = handle.view.contentChildren;
        for (var i = 0; i < kids.length; ++i) {
            if (kids[i] && kids[i].visible)
                out.push(kids[i]);
        }
        return out;
    }

    // SplitView's own rule (QQuickSplitViewPrivate::updateFillIndex): the first
    // visible item that asks to fill, else the last visible one. That item's
    // preferred size is ignored — it gets whatever the others leave — so
    // writing a preferred size onto it would silently do nothing.
    function fillItem(items) {
        for (var i = 0; i < items.length; ++i) {
            var attached = items[i].SplitView;
            if (handle.horizontal ? attached.fillWidth : attached.fillHeight)
                return items[i];
        }
        return items.length > 0 ? items[items.length - 1] : null;
    }

    function setPreferred(item, size) {
        var value = Math.max(0, size);
        if (handle.horizontal)
            item.SplitView.preferredWidth = value;
        else
            item.SplitView.preferredHeight = value;
    }

    // Move the divider by `delta` pixels the way a drag would: the item before
    // it grows and the item after it shrinks.
    //
    // The new sizes are computed from the items' CURRENT width/height, never
    // from the preferred size last written. That is what keeps a run of key
    // presses against a minimumWidth from banking an ever-growing preferred
    // value that then has to be paid back before the divider moves again —
    // SplitView clamps the laid-out size, so reading it back self-corrects.
    function moveBy(delta) {
        var items = handle.visibleItems();
        if (items.length < 2)
            return;

        var at = handle.horizontal ? handle.x : handle.y;
        var before = null;
        var after = null;
        var beforeAt = 0;
        var afterAt = 0;
        for (var i = 0; i < items.length; ++i) {
            var itemAt = handle.horizontal ? items[i].x : items[i].y;
            if (itemAt < at) {
                if (!before || itemAt > beforeAt) {
                    before = items[i];
                    beforeAt = itemAt;
                }
            } else if (!after || itemAt < afterAt) {
                after = items[i];
                afterAt = itemAt;
            }
        }
        if (!before || !after)
            return;

        var fill = handle.fillItem(items);
        if (before !== fill)
            handle.setPreferred(before, (handle.horizontal ? before.width : before.height) + delta);
        if (after !== fill)
            handle.setPreferred(after, (handle.horizontal ? after.width : after.height) - delta);
        handle.resized();
    }

    // Only the keys that move the divider are accepted; everything else — Tab
    // above all — is left to propagate, so the focus is never held here.
    Keys.onLeftPressed: function (event) {
        if (handle.horizontal) {
            handle.moveBy(-handle.keyStep);
            event.accepted = true;
        }
    }
    Keys.onRightPressed: function (event) {
        if (handle.horizontal) {
            handle.moveBy(handle.keyStep);
            event.accepted = true;
        }
    }
    Keys.onUpPressed: function (event) {
        if (!handle.horizontal) {
            handle.moveBy(-handle.keyStep);
            event.accepted = true;
        }
    }
    Keys.onDownPressed: function (event) {
        if (!handle.horizontal) {
            handle.moveBy(handle.keyStep);
            event.accepted = true;
        }
    }
    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_PageUp) {
            handle.moveBy(-handle.pageStep);
            event.accepted = true;
        } else if (event.key === Qt.Key_PageDown) {
            handle.moveBy(handle.pageStep);
            event.accepted = true;
        }
    }

    Rectangle {
        anchors.centerIn: parent
        // A one-pixel hairline is not a focus indicator, and this item has no
        // other paint of its own: while it holds the keyboard the whole grab
        // area is filled with the accent, so "the arrow keys resize THIS
        // divider" is visible from across the window.
        width: handle.horizontalLine ? handle.width : (handle.activeFocus ? handle.width : 1)
        height: handle.horizontalLine ? (handle.activeFocus ? handle.height : 1) : handle.height
        color: handle.activeFocus || handle.engaged ? Theme.accent : Theme.border

        Behavior on color {
            ColorAnimation { duration: 120; easing.type: Easing.OutQuart }
        }
    }
}
