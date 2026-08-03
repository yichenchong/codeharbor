import QtQuick
import QtQuick.Controls.Basic

// Sidebar group header (SPEC 4.2): group name with a collapse chevron. Bound to
// a SessionsModel group row via role names (name, collapsed, itemId). Clicking
// the header moves the sidebar's keyboard cursor here and toggles the collapse;
// both that and a reorder drag go through the sidebar (`host`), which owns every
// app.* call.
ItemDelegate {
    id: header

    required property int index
    required property string name
    required property bool collapsed
    required property string itemId // group's ch id (SessionsModel itemId role)

    // The SessionsSidebar that instantiated this header; null-guarded so the
    // header stays usable standalone.
    property var host: null

    readonly property bool selected: host !== null && host.currentIsGroup
                                     && host.currentId === header.itemId
    readonly property bool dragging: host !== null && host.dragItem === header

    // A missing palette service has the same meaning as the "plain" palette:
    // this delegate remains usable in a headless harness and keeps today's
    // un-tinted text instead of treating a missing object as black.
    readonly property color groupTint:
        host !== null && host.groupColorFor !== undefined
            ? host.groupColorFor(header.name, host.groupPaletteName, host.groupPaletteSize)
            : Theme.text

    objectName: "groupHeader:" + itemId

    Component.onCompleted: if (host) host.registerHeader(header)
    Component.onDestruction: if (host) host.unregisterHeader(header)

    // Width is assigned by the instantiating parent (SessionsSidebar sets it to
    // the group block's width); fall back to the parent's width otherwise. This
    // is a Column child inside a DelegateModel delegate, never a ListView
    // delegate itself, so there is no ListView.view to size against — reading
    // one here only ever yielded null and the implicitWidth fallback. Same rule
    // as SessionRow.
    //
    // `implicitWidth` is a CONSTANT rather than the control's usual
    // content-derived one, because the name label below is sized FROM this
    // header's width: a content-derived implicit width would close that loop
    // into a binding cycle the moment a header was built without a parent.
    implicitWidth: 240
    width: parent ? parent.width : implicitWidth
    height: 32

    // Where this header's own text has to stop: the add-session button pinned
    // to the right edge, or that edge itself while the button is hidden. The
    // button is a child of the HEADER rather than of the content row, so it is
    // not part of what the row lays out around.
    readonly property real contentRightEdge:
        newSessionButton.visible ? newSessionButton.x : header.width - 8

    // Without this a screen reader announces an unnamed item: the delegate's own
    // `text` property is unused (the name arrives in `name`, a SessionsModel
    // role) and the collapse state is drawn as a chevron glyph.
    Accessible.role: Accessible.ListItem
    Accessible.name: header.name
    Accessible.description: header.collapsed ? qsTr("Collapsed group")
                                             : qsTr("Expanded group")

    background: Rectangle {
        // This is presentation-only rather than a public role, but it still
        // follows the active theme so light mode does not retain a dark wash.
        color: header.selected ? Theme.groupSelectedSurface() : Theme.surfaceDeep
        opacity: header.dragging ? 0.4 : 1.0

        // Keyboard focus ring. The selected wash alone is a 10% lightness step
        // against Theme.surfaceDeep — invisible on a dim screen, and the only
        // thing that told a keyboard user where the cursor was.
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
            visible: header.selected && header.host !== null && header.host.activeFocus
        }
    }

    contentItem: Row {
        spacing: 6
        leftPadding: 8
        opacity: (header.host !== null && header.host.stale === true) ? 0.55 : 1.0

        Label {
            id: chevron
            width: 16
            text: header.collapsed ? "\u25B8" : "\u25BE" // ▸ / ▾
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            verticalAlignment: Text.AlignVCenter
            anchors.verticalCenter: parent.verticalCenter
        }
        Label {
            text: header.name
            // The tint is derived from this group's own name. Plain/unknown
            // palettes return Theme.text, preserving the historical pixels.
            color: header.groupTint
            font.bold: true
            font.pixelSize: Theme.fontSizeLabel
            elide: Text.ElideRight
            anchors.verticalCenter: parent.verticalCenter
            // Text elides only against a WIDTH. Without one this label grew to
            // the group name's full natural width, so a long name ran under the
            // "collapsed" tag and under the add-session button pinned to the
            // right edge — the two things a header needs to keep legible.
            //
            // Measured against `contentRightEdge` in HEADER coordinates, since
            // the button is a child of the header and not of this Row; the
            // Row's own implicit width is derived from these children and would
            // be a cycle.
            width: Math.max(0, header.contentRightEdge - header.leftPadding
                            - parent.leftPadding - chevron.width - parent.spacing
                            - (collapsedTag.visible
                               ? collapsedTag.implicitWidth + parent.spacing : 0)
                            - 6)
        }
        // Collapsed groups hide their rows, so the header is the only thing
        // left saying there is anything in there at all.
        Label {
            id: collapsedTag
            text: qsTr("collapsed")
            visible: header.collapsed
            color: Theme.textDim
            font.pixelSize: Theme.fontSizeSmall
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    // Creating a Dev Session is the ONLY way into the product's actual work - a
    // terminal, an editor, a repository - so it needs a visible home. The group
    // header is that home: a session always belongs to a group, so the action
    // carries its group with it and needs no separate picker.
    //
    // Deliberately OUTSIDE contentItem and pinned to the right edge: inside the
    // Row it sat in the middle of the header and swallowed the press that starts
    // a group-reorder drag (tst_sidebar::reordersGroups caught exactly that).
    // The draggable expanse of the header stays draggable.
    Button {
        id: newSessionButton
        objectName: "newSessionButton:" + header.itemId
        text: "+"
        visible: !header.collapsed
        // A compact square instead of a "+ Session" word button: one of these
        // sits on every group header, and the words were wide enough to crowd
        // the group name they belong to. 24 logical pixels is the floor for a
        // pointer target on a high-density display, so the box stays 24 even
        // though the glyph in it is smaller.
        implicitWidth: 24
        implicitHeight: 24
        padding: 0
        // Reachable without a pointer: Tab lands here, and the accessible name
        // below is what a screen reader announces.
        focusPolicy: Qt.StrongFocus
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter

        // The label is a bare "+", so this sentence is the button's only real
        // name — and it has to name the GROUP, because there is one of these per
        // header and "+" alone cannot say which group the click would add to.
        // The tooltip is a pointer-only hint, so the SAME sentence is also the
        // accessible name; a tooltip is never the only label.
        readonly property string actionText:
            qsTr("Add a Dev Session to \u201c%1\u201d").arg(header.name)

        Accessible.role: Accessible.Button
        Accessible.name: newSessionButton.actionText

        // The module's one tooltip (AppToolTip.qml). The attached
        // `ToolTip.text` form is drawn by the Basic style in that style's own
        // light palette, so a hint about this dark panel arrived as a white
        // box. Named so tst_sidebar can read the hint back.
        AppToolTip {
            objectName: "newSessionButtonTip:" + header.itemId
            text: newSessionButton.actionText
            visible: newSessionButton.hovered
            // Long enough that crossing the row on the way somewhere else does
            // not flash it, matching the row tooltip in SessionRow.qml.
            delay: 600
        }

        contentItem: Label {
            text: newSessionButton.text
            color: newSessionButton.down ? Theme.textOnAccent : Theme.text
            font.pixelSize: Theme.fontSizeTitle
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: newSessionButton.down ? Theme.accent
                 : newSessionButton.hovered ? Theme.border : Theme.surfaceRaised
            border.width: newSessionButton.visualFocus ? 2 : 1
            border.color: newSessionButton.visualFocus ? Theme.accent : Theme.borderSubtle
        }

        // The Button consumes the press, so the parent ItemDelegate's onClicked
        // (which toggles collapse) never sees it - creating a session must not
        // also fold the group away. Asserted by
        // tst_sidebar::newSessionButtonTargetsItsGroupWithoutCollapsing.
        onClicked: if (header.host) header.host.requestNewSession(header.itemId)
    }

    // Selecting the header first is what makes the toggle unambiguous: the
    // sidebar's toggleCurrentGroup() acts on the keyboard cursor, which
    // selectGroup() has just moved here — so the click and the Space key take
    // exactly the same path.
    //
    // Deliberately routed through `host` rather than calling app.setGroupCollapsed
    // directly: the sidebar owns every app.* call (see its drag-and-drop
    // contract), and a header with no host is documented to stay usable
    // standalone, which a bare `app` lookup would break with a ReferenceError.
    onClicked: {
        if (!header.host)
            return;
        header.host.selectGroup(header);
        header.host.toggleCurrentGroup();
    }

    // Vertical drag = reorder groups. Same grab discipline as SessionRow: the
    // header stays put and the sidebar draws the insertion line.
    DragHandler {
        target: null
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.CanTakeOverFromItems
                         | PointerHandler.CanTakeOverFromHandlersOfDifferentType

        onActiveChanged: {
            if (!header.host)
                return;
            if (active) {
                header.host.beginDrag("group", header);
                header.host.updateDrag(centroid.scenePosition);
            } else {
                header.host.endDrag();
            }
        }
        onCentroidChanged: if (active && header.host) header.host.updateDrag(centroid.scenePosition)
    }
}
