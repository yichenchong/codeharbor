import QtQuick
import QtQuick.Controls

// Read-only remote text, natively (mobile SPEC 7.5 / 3.3).
//
// The desktop shows text through Monaco inside Qt WebEngine; neither exists on
// Android or iOS, so this is a plain ListView of lines. Every line is a Text with
// textFormat: Text.PlainText — the file's bytes are server-controlled data and
// this client never asks Qt to interpret them as markup, ever.
//
// Lines rather than one giant TextEdit: a file may be up to
// ch::MobileViewerService::kMaxInlineReadBytes (8 MiB), and a single text item
// holding that would lay the whole document out on the UI thread before the first
// frame. A ListView lays out what is on screen.
Item {
    id: root

    // ---- the page interface every mobile viewer page implements -------------
    property string remotePath: ""
    property url paneUrl
    property string repoRoot: ""
    property string paneId: ""
    signal openRequested(string path)
    signal titleRequested(string title)

    // The `viewerService` context property, guarded exactly as the desktop
    // guards its own (`viewers`), and for the same reason: an unguarded lookup
    // of a context property the host did not install throws a ReferenceError
    // that aborts the whole binding pass and leaves a blank pane instead of an
    // inert one.
    readonly property var service: (typeof viewerService !== "undefined") ? viewerService : null

    property bool loading: false
    property string errorText: ""
    property bool truncated: false
    property bool binary: false
    property var lines: []
    // The read THIS page has outstanding, and nothing else. One service instance
    // serves the visible pane, so a reply is matched against what was asked for
    // rather than accepted because it happens to fit.
    property string requestedPath: ""

    // Step/pinch font sizing, clamped so neither gesture can make the file
    // unreadable in either direction.
    property real fontSize: MobileTheme.fontSizeBody
    readonly property real minFontSize: 9
    readonly property real maxFontSize: 32
    function stepFont(delta) {
        root.fontSize = Math.max(root.minFontSize,
                                 Math.min(root.maxFontSize, root.fontSize + delta));
    }

    function reload() {
        root.requestedPath = "";
        root.lines = [];
        root.errorText = "";
        root.truncated = false;
        root.binary = false;
        root.loading = false;
        if (root.remotePath.length === 0 || !root.service)
            return;
        root.loading = true;
        root.requestedPath = root.remotePath;
        root.service.readFile(root.remotePath);
    }

    // ONE read for the first target, not two. A page is built by assigning its
    // initial properties and only then running Component.onCompleted, so
    // `remotePath` arriving from the host fires onRemotePathChanged BEFORE the
    // completion handler — and a completion handler that reloads unconditionally
    // therefore asked the server for the same file a second time on every single
    // pane open. `started` is what makes the change handler inert until the
    // object is fully built, so exactly one of the two paths issues the read.
    property bool started: false
    onRemotePathChanged: if (root.started) root.reload()
    Component.onCompleted: {
        root.started = true;
        root.reload();
    }

    Connections {
        target: root.service
        function onFileRead(path, text, binary, revision, isTruncated) {
            if (path !== root.requestedPath)
                return;
            root.requestedPath = "";
            root.loading = false;
            root.truncated = isTruncated;
            root.binary = binary;
            // A base64 (non-text) reply carries no text on purpose: showing a
            // wall of U+FFFD as though it were the file is exactly the
            // dishonesty this client avoids. See MobileViewerService::readFile.
            //
            // Split on every line terminator, not on "\n" alone: a CRLF file
            // would otherwise keep a carriage return at the end of every line,
            // which Text renders as a stray glyph (and widens the line for
            // wrapping) on a file the desktop shows cleanly.
            root.lines = binary ? [] : text.split(/\r\n|\r|\n/);
        }
        function onFileError(path, message) {
            if (path !== root.requestedPath)
                return;
            root.requestedPath = "";
            root.loading = false;
            root.errorText = message;
        }
    }

    Rectangle {
        anchors.fill: parent
        color: MobileTheme.surface
    }

    Column {
        anchors.fill: parent
        spacing: 0

        // ---- banners -------------------------------------------------------

        Rectangle {
            width: parent.width
            height: visible ? Math.max(MobileTheme.touchTarget, label.implicitHeight + 2 * MobileTheme.spacing) : 0
            visible: root.truncated
            color: MobileTheme.warningSurface()
            Text {
                id: label
                anchors.fill: parent
                anchors.margins: MobileTheme.spacing
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                color: MobileTheme.text
                font.pixelSize: MobileTheme.fontSizeSmall
                text: qsTr("Only the first 8 MiB of this file was read. What you see is a prefix, not the whole file.")
            }
        }

        Rectangle {
            width: parent.width
            height: visible ? Math.max(MobileTheme.touchTarget, errorLabel.implicitHeight + 2 * MobileTheme.spacing) : 0
            visible: root.errorText.length > 0
            color: MobileTheme.errorSurface()
            Text {
                id: errorLabel
                anchors.fill: parent
                anchors.margins: MobileTheme.spacing
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                color: MobileTheme.text
                font.pixelSize: MobileTheme.fontSizeSmall
                text: root.errorText
            }
        }

        Rectangle {
            width: parent.width
            height: visible ? Math.max(MobileTheme.touchTarget, binaryLabel.implicitHeight + 2 * MobileTheme.spacing) : 0
            visible: root.binary
            color: MobileTheme.surfaceRaised
            Text {
                id: binaryLabel
                anchors.fill: parent
                anchors.margins: MobileTheme.spacing
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                color: MobileTheme.textDim
                font.pixelSize: MobileTheme.fontSizeSmall
                text: qsTr("This file is not UTF-8 text, so there is nothing to show as text.")
            }
        }

        // ---- font controls -------------------------------------------------

        Row {
            width: parent.width
            height: MobileTheme.touchTarget
            spacing: MobileTheme.spacing

            Button {
                width: MobileTheme.touchTarget
                height: MobileTheme.touchTarget
                text: "A-"
                onClicked: root.stepFont(-1)
            }
            Button {
                width: MobileTheme.touchTarget
                height: MobileTheme.touchTarget
                text: "A+"
                onClicked: root.stepFont(1)
            }
            Text {
                height: MobileTheme.touchTarget
                verticalAlignment: Text.AlignVCenter
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                width: parent.width - 2 * MobileTheme.touchTarget - 3 * MobileTheme.spacing
                color: MobileTheme.textDim
                font.pixelSize: MobileTheme.fontSizeSmall
                // A server-controlled string. PlainText, like every other.
                text: root.loading ? qsTr("Reading…") : root.remotePath
            }
        }

        // ---- the file ------------------------------------------------------

        Item {
            width: parent.width
            height: parent.height - y

            PinchArea {
                anchors.fill: parent
                // Only the pinch is intercepted; the drag stays with the
                // ListView underneath so scrolling is unaffected.
                // Scaled INCREMENTALLY off the previous frame's scale rather
                // than off an anchor size: the gesture then behaves the same
                // whether it starts from the clamped minimum or the clamped
                // maximum, instead of snapping when the anchor is out of range.
                onPinchUpdated: function(pinch) {
                    const ratio = pinch.previousScale > 0
                                ? pinch.scale / pinch.previousScale : 1;
                    root.fontSize = Math.max(root.minFontSize,
                                             Math.min(root.maxFontSize,
                                                      root.fontSize * ratio));
                }

                ListView {
                    id: list
                    objectName: "textLines"
                    anchors.fill: parent
                    clip: true
                    model: root.lines
                    // Cached ahead by two screens: a phone scrolls fast and a
                    // fresh delegate per line is visibly stuttery otherwise.
                    cacheBuffer: height * 2
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Row {
                        required property int index
                        required property string modelData
                        width: list.width
                        spacing: MobileTheme.spacing

                        Text {
                            width: gutter.implicitWidth
                            textFormat: Text.PlainText
                            horizontalAlignment: Text.AlignRight
                            color: MobileTheme.textFaint
                            font.family: MobileTheme.monoFamily
                            font.pixelSize: root.fontSize
                            text: String(index + 1)
                        }
                        Text {
                            width: list.width - gutter.implicitWidth - MobileTheme.spacing
                            // Wrapped, not horizontally scrolled: there is no
                            // room on a phone for a second scroll axis, and a
                            // wrapped long line is still readable.
                            wrapMode: Text.WrapAnywhere
                            textFormat: Text.PlainText
                            color: MobileTheme.text
                            font.family: MobileTheme.monoFamily
                            font.pixelSize: root.fontSize
                            text: modelData
                        }
                    }
                }
            }

            // Width reference for the line-number gutter: as wide as the largest
            // line number in the file, so the text column does not jump as the
            // user scrolls past 999.
            Text {
                id: gutter
                visible: false
                textFormat: Text.PlainText
                font.family: MobileTheme.monoFamily
                font.pixelSize: root.fontSize
                text: String(Math.max(1, root.lines.length))
            }
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: root.loading
        visible: root.loading
    }
}
