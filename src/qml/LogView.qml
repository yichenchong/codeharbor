pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CodeHarbor

// Transient, full-surface diagnostics sheet. The host supplies the one bounded
// LogBuffer; this component keeps no second history, so messages emitted while
// it is closed remain available on the next open. Credential fields never enter
// LogBuffer, and this view only displays its already-sanitized entries.
Rectangle {
    id: root

    property var logBuffer: null
    property bool shown: false
    property string severityFilter: ""
    property string textFilter: ""
    property bool followTail: true
    property var filteredEntries: []
    property bool _movingTail: false
    readonly property string visibleText: {
        var lines = [];
        // NOT Array.isArray(): the buffer hands back a C++ list, which QML
        // exposes as a list-like wrapper rather than a JavaScript Array, so the
        // stricter test rejected every real value and left the view blank.
        var rows = root.filteredEntries;
        var count = (rows && rows.length !== undefined) ? rows.length : 0;
        for (var i = 0; i < count; ++i) {
            if (rows[i] && rows[i].text !== undefined && rows[i].text !== null)
                lines.push(String(rows[i].text));
        }
        return lines.join("\n");
    }

    signal dismissed()

    color: Theme.surface
    border.color: Theme.borderSubtle
    border.width: 1
    radius: Theme.radiusMedium
    focus: root.shown
    enabled: root.shown

    // Same rule as SettingsWindow: a full-surface sheet that takes the
    // keyboard and blocks the window behind it is a dialog to assistive
    // technology, and needs to say so and name itself.
    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("Application log")

    // The three buttons, the severity ComboBox and the filter field below are
    // plain QtQuick.Controls controls, so they draw from the STYLE's palette —
    // which is light. The filter field was the worst of it: its `color` is
    // Theme.text (a pale grey) while its well stayed the style's white, i.e.
    // near-invisible text in the one place a user types to find a diagnostic.
    // An Item's palette propagates down the visual parent chain, so one mapping
    // here reaches all of them. Same role-by-role mapping as AppDialog.qml,
    // plus `light`, which is what a ComboBox popup ROW is painted with.
    palette.window: Theme.surface
    palette.windowText: Theme.text
    palette.dark: Theme.border
    palette.base: Theme.surfaceSunken
    palette.text: Theme.text
    palette.placeholderText: Theme.textDim
    palette.button: Theme.surfaceRaised
    palette.buttonText: Theme.text
    palette.mid: Theme.border
    palette.light: Theme.surfaceDeep
    palette.midlight: Theme.surfaceRaised
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.textOnAccent
    palette.brightText: Theme.textOnAccent

    function rebuild() {
        if (!root.logBuffer) {
            root.filteredEntries = [];
            return;
        }
        root.filteredEntries = root.logBuffer.filteredEntries(root.severityFilter,
                                                               root.textFilter);
        root.scrollToTail();
    }

    function scrollToTail() {
        if (!root.followTail)
            return;
        // Text layout settles on the next event-loop turn. Otherwise a live
        // message can arrive between measuring and painting the text area.
        Qt.callLater(function () {
            if (!root.followTail || !logFlick)
                return;
            root._movingTail = true;
            logFlick.contentY = Math.max(0, logFlick.contentHeight - logFlick.height);
            root._movingTail = false;
        });
    }

    function copyVisible() {
        if (logText.selectedText.length > 0) {
            logText.copy();
            return;
        }
        logText.selectAll();
        logText.copy();
        logText.deselect();
    }

    onLogBufferChanged: root.rebuild()
    onSeverityFilterChanged: root.rebuild()
    onTextFilterChanged: root.rebuild()
    onShownChanged: {
        if (root.shown) {
            root.followTail = true;
            root.scrollToTail();
            // DEFERRED. `shown` is what the host binds its own `visible` to,
            // and this handler is connected before that binding, so at this
            // instant the sheet is still hidden and forceActiveFocus() on a
            // hidden item is dropped: the log text never took the keyboard and
            // the arrow keys did nothing until the user clicked in the well.
            // One turn later the sheet is on screen and the focus sticks.
            Qt.callLater(function () {
                if (root.shown)
                    logText.forceActiveFocus();
            });
        }
    }

    Keys.onEscapePressed: (event) => {
        root.shown = false;
        root.dismissed();
        event.accepted = true;
    }

    Connections {
        target: root.logBuffer
        enabled: root.logBuffer !== null
        function onEntriesChanged() { root.rebuild(); }
    }

    // This sheet fills the whole window on top of the three regions, but a
    // Rectangle accepts no input of its own: Qt Quick hands an unaccepted press
    // to the next item DOWN, so every click that did not happen to land on one
    // of the controls below went straight through to the terminal or editor
    // behind the sheet — focusing a pane, or scrolling a shell the user could
    // not even see. Declared FIRST so every real control still hit-tests above
    // it; `wheel` is handled too, because a scroll is just as much a stray
    // input as a click.
    MouseArea {
        objectName: "sheetInputShield"
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: (wheel) => wheel.accepted = true
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                objectName: "logTitle"
                text: qsTr("Application log")
                color: Theme.text
                font.pixelSize: Theme.fontSizeTitle
                font.bold: true
                Layout.fillWidth: true
            }

            Label {
                objectName: "logCount"
                // With a filter on, the buffer's own occupancy describes
                // something the user is NOT looking at: the well can be empty
                // while this said "37 of 500 entries", which reads as a broken
                // view rather than a filter that matched nothing. Say how many
                // of the held entries are on screen instead, and keep the
                // capacity reading for the unfiltered case.
                readonly property bool filtering: root.severityFilter.length > 0
                                                  || root.textFilter.length > 0
                readonly property int shownCount: root.filteredEntries
                                                  && root.filteredEntries.length !== undefined
                                                  ? root.filteredEntries.length : 0
                text: !root.logBuffer
                      ? qsTr("No log source")
                      : filtering
                        ? qsTr("%1 of %2 entries match").arg(shownCount)
                                                        .arg(root.logBuffer.count)
                        : qsTr("%1 of %2 entries").arg(root.logBuffer.count)
                                                  .arg(root.logBuffer.maxEntries)
                color: Theme.textDim
                font.pixelSize: Theme.fontSizeSmall
            }

            Button {
                objectName: "logCloseButton"
                text: qsTr("Close")
                onClicked: {
                    root.shown = false;
                    root.dismissed();
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ComboBox {
                id: severityBox
                objectName: "severityFilter"
                // The control has no visible label of its own, so without this
                // a screen reader announces the severity filter as an unnamed
                // combo box.
                Accessible.name: qsTr("Severity filter")
                Layout.preferredWidth: 130
                model: [qsTr("All"), qsTr("Debug"), qsTr("Info"),
                        qsTr("Warning"), qsTr("Critical"), qsTr("Fatal")]
                onCurrentIndexChanged: {
                    // The visible labels are translated, so deriving the
                    // filter key from currentText breaks every non-English
                    // locale. LogBuffer stores canonical severity names.
                    var keys = ["", "debug", "info", "warning", "critical", "fatal"];
                    root.severityFilter = currentIndex >= 0 && currentIndex < keys.length
                                           ? keys[currentIndex] : "";
                }
            }

            TextField {
                id: textFilterField
                objectName: "textFilter"
                Layout.fillWidth: true
                Accessible.name: qsTr("Message filter")
                placeholderText: qsTr("Filter messages")
                color: Theme.text
                selectByMouse: true
                onTextChanged: root.textFilter = text
            }

            Button {
                objectName: "copyLogButton"
                text: qsTr("Copy selected / visible")
                onClicked: root.copyVisible()
            }

            Button {
                objectName: "clearLogButton"
                text: qsTr("Clear")
                onClicked: if (root.logBuffer) root.logBuffer.clear()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.surfaceSunken
            // The well IS the focus indicator for the text area inside it: the
            // sheet focuses that area for itself as it opens (see onShownChanged
            // above), and with a null background and no caret to watch — the
            // text is read-only — there was nothing on screen saying which
            // control the arrow keys and Ctrl+A were about to act on.
            border.color: logText.activeFocus ? Theme.accent : Theme.border
            border.width: logText.activeFocus ? 2 : 1
            radius: Theme.radiusSmall

            Flickable {
                id: logFlick
                objectName: "logScroll"
                anchors.fill: parent
                anchors.margins: 8
                clip: true
                contentWidth: logText.width
                contentHeight: logText.height
                boundsBehavior: Flickable.StopAtBounds
                // Entries are NOT wrapped (a log line is easier to scan when
                // one entry is one line), so the widest line routinely runs
                // past the sheet. Without a horizontal axis the overflow was
                // simply clipped and unreachable — a remote stack trace could
                // be in the buffer and still be unreadable.
                flickableDirection: Flickable.HorizontalAndVerticalFlick

                onContentYChanged: {
                    if (root._movingTail)
                        return;
                    var bottom = Math.max(0, contentHeight - height);
                    if (bottom - contentY <= 3)
                        root.followTail = true;
                    else if (contentY < bottom)
                        root.followTail = false;
                }

                TextArea {
                    id: logText
                    objectName: "logText"
                    // Read-only, so it never gets the name a text field gets
                    // from its placeholder or its label: without this a screen
                    // reader lands on the log and announces an unnamed edit box.
                    Accessible.name: qsTr("Log output")
                    // Never narrower than the viewport, so a short log still
                    // fills the well and the click target for `selectAll` is
                    // the whole area; wider when a line demands it, which is
                    // what gives the Flickable something to scroll.
                    width: Math.max(implicitWidth, logFlick.width)
                    height: Math.max(implicitHeight, logFlick.height)
                    text: root.visibleText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextArea.NoWrap
                    textFormat: TextEdit.PlainText
                    color: Theme.text
                    selectionColor: Theme.surfaceSelected
                    selectedTextColor: Theme.text
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeBody
                    padding: 4
                    background: null
                    placeholderText: qsTr("No diagnostics yet.")
                }

                ScrollBar.vertical: AppScrollBar { objectName: "logVerticalScroll" }
                ScrollBar.horizontal: AppScrollBar { objectName: "logHorizontalScroll" }
            }
        }
    }

    Component.onCompleted: root.rebuild()
}
