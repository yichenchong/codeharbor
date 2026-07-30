import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window

// The application's own title bar (SPEC 4.1), drawn inside the window because
// the window itself is frameless on the platforms where that is safe (see
// Main.qml's `customChrome`). It replaces the desktop environment's bar, which
// was the one light-coloured strip left on an otherwise fully dark window.
//
// Everything a window manager used to provide has to be provided here:
//
//   * MOVE — a drag on the bar hands the window to the compositor through
//     ApplicationWindow.startSystemMove(). Deliberately NOT a hand-rolled
//     "add the mouse delta to window.x": that fights the compositor's own
//     pointer grab and drifts, and on Wayland there are no global window
//     coordinates to add a delta to in the first place.
//   * MAXIMISE / RESTORE — the middle button and a double-click on the bar,
//     with the glyph reflecting the current state.
//   * MINIMISE and CLOSE — the outer two buttons.
//
// Resizing is NOT here: it needs the window's four edges and four corners, which
// live outside this bar, so Main.qml owns those handles (via startSystemResize).
//
// The bar drives `win`, which defaults to the window it is shown in. It is a
// plain `var` so a harness can hand it a stand-in window object.
Rectangle {
    id: bar

    // The window's own title, i.e. what the desktop bar used to show.
    property string title: ""
    // The Dev Session the window is currently pointed at, shown beside the
    // title. Empty when there is none. Server-derived text, so it is rendered as
    // plain text below and never as markup (SPEC 12).
    property string sessionLabel: ""
    property var win: Window.window

    implicitHeight: Theme.headerHeight
    color: Theme.surfaceDeep

    readonly property bool maximised: bar.win ? bar.win.visibility === Window.Maximized : false

    function toggleMaximised() {
        if (!bar.win)
            return;
        bar.win.visibility = bar.maximised ? Window.Windowed : Window.Maximized;
    }

    // The one-pixel line that separates the bar from the regions below it.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.borderSubtle
    }

    // Drag to move, double-click to maximise.
    //
    // startSystemMove() is called on the first real MOVEMENT, not on press: it
    // transfers the pointer grab to the compositor, which ends Qt's own click
    // sequence, so calling it from onPressed would consume the first half of
    // every double-click and the maximise gesture would never arrive. Waiting
    // for the platform's own drag threshold (Qt.styleHints.startDragDistance)
    // keeps both gestures on the same area.
    MouseArea {
        id: dragArea
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton

        property real pressX: 0
        property real pressY: 0
        property bool moving: false

        onPressed: (mouse) => {
            dragArea.pressX = mouse.x;
            dragArea.pressY = mouse.y;
            dragArea.moving = false;
        }
        onPositionChanged: (mouse) => {
            if (dragArea.moving || !dragArea.pressed || !bar.win)
                return;
            const travel = Math.abs(mouse.x - dragArea.pressX)
                         + Math.abs(mouse.y - dragArea.pressY);
            if (travel < Qt.styleHints.startDragDistance)
                return;
            dragArea.moving = true;
            // Dragging a maximised window restores it first, which is what the
            // gesture means: a maximised window cannot be moved anywhere.
            if (bar.maximised)
                bar.win.visibility = Window.Windowed;
            bar.win.startSystemMove();
        }
        onReleased: dragArea.moving = false
        onDoubleClicked: bar.toggleMaximised()
    }

    // A window button. Square, so the three read as one group, and hit-tested
    // above the drag area because it is declared after it.
    component ChromeButton: AbstractButton {
        id: button

        property string glyph: ""
        // Close is the destructive one and says so on hover; the other two only
        // lift off the surface.
        property color hoverFill: Theme.surfaceHover
        property color hoverText: Theme.text

        implicitWidth: Theme.headerHeight
        implicitHeight: Theme.headerHeight
        hoverEnabled: true

        Accessible.role: Accessible.Button
        Accessible.onPressAction: button.clicked()

        background: Rectangle {
            color: button.hovered || button.down ? button.hoverFill : "transparent"
            border.width: button.visualFocus ? 1 : 0
            border.color: Theme.accent

            Behavior on color {
                ColorAnimation { duration: 120; easing.type: Easing.OutQuart }
            }
        }

        contentItem: Label {
            text: button.glyph
            // A glyph, not remote content, but the whole application renders
            // every label as plain text so no string can ever be markup.
            textFormat: Text.PlainText
            color: button.hovered || button.down ? button.hoverText : Theme.textDim
            font.pixelSize: Theme.fontSizeLabel
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    Row {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom

        ChromeButton {
            objectName: "minimiseButton"
            glyph: "\u2212"
            Accessible.name: qsTr("Minimise window")
            onClicked: if (bar.win) bar.win.visibility = Window.Minimized
        }

        ChromeButton {
            objectName: "maximiseButton"
            // The glyph is the STATE, not the action: a maximised window offers
            // "shrink back", a windowed one "fill the screen".
            glyph: bar.maximised ? "\u25a3" : "\u25a1"
            Accessible.name: bar.maximised ? qsTr("Restore window") : qsTr("Maximise window")
            onClicked: bar.toggleMaximised()
        }

        ChromeButton {
            objectName: "closeButton"
            glyph: "\u2715"
            hoverFill: Theme.danger
            hoverText: Theme.textOnAccent
            Accessible.name: qsTr("Close window")
            onClicked: if (bar.win) bar.win.close()
        }
    }

    Row {
        id: titleRow
        anchors.left: parent.left
        anchors.leftMargin: Theme.radiusMedium
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.radiusMedium
        // Never run under the buttons: the whole row is elided instead.
        width: Math.max(0, parent.width - Theme.headerHeight * 3 - Theme.radiusMedium * 2)

        Label {
            objectName: "windowTitleLabel"
            text: bar.title
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            elide: Text.ElideRight
            width: Math.min(implicitWidth, titleRow.width)
            verticalAlignment: Text.AlignVCenter
        }

        Rectangle {
            visible: bar.sessionLabel.length > 0
            width: 1
            height: Theme.fontSizeBody
            color: Theme.borderSubtle
            anchors.verticalCenter: parent.verticalCenter
        }

        Label {
            objectName: "sessionLabel"
            visible: bar.sessionLabel.length > 0
            text: bar.sessionLabel
            textFormat: Text.PlainText
            color: Theme.textDim
            font.pixelSize: Theme.fontSizeBody
            elide: Text.ElideLeft
            width: Math.min(implicitWidth,
                            Math.max(0, titleRow.width - titleRow.spacing * 2 - 1))
            verticalAlignment: Text.AlignVCenter
        }
    }
}
