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
            logText.forceActiveFocus();
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
                text: root.logBuffer
                      ? qsTr("%1 of %2 entries").arg(root.logBuffer.count)
                                                    .arg(root.logBuffer.maxEntries)
                      : qsTr("No log source")
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
            border.color: Theme.border
            border.width: 1
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
                flickableDirection: Flickable.VerticalFlick

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
                    width: logFlick.width
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

                ScrollBar.vertical: AppScrollBar {}
            }
        }
    }

    Component.onCompleted: root.rebuild()
}
