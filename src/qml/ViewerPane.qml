import QtQuick
import QtQuick.Controls.Basic

// A single viewer pane (SPEC 3.3, 7.5) — a leaf of the viewer split tree. It
// asks the ViewerModel (`viewers`) to classify its URL and loads the matching
// read-only sub-view. Editing lands in workstream E.
Item {
    id: pane

    property url url
    property string paneId: ""

    // View kind for the current URL: "web" | "markdown" | "text" | "image" |
    // "pdf" | "directory" | "binary" (empty URL -> a neutral placeholder).
    property string kind: pane.url.toString().length === 0
                          ? "empty"
                          : viewers.viewKind(pane.url)

    Loader {
        anchors.fill: parent
        sourceComponent: {
            switch (pane.kind) {
            case "web": return webComponent;
            case "image": return imageComponent;
            case "pdf": return pdfComponent;
            case "directory": return directoryComponent;
            case "binary": return binaryComponent;
            case "empty": return emptyComponent;
            // Source/text/markdown/structured open in the Monaco editor
            // (SPEC 8.1/8.8). ViewerTextView remains a read-only fallback.
            case "text":
            case "markdown":
                return editorComponent;
            default:
                return textComponent;
            }
        }
    }

    Component { id: webComponent; ViewerWebView { url: pane.url } }
    Component { id: textComponent; ViewerTextView { url: pane.url } }
    Component { id: imageComponent; ViewerImageView { url: pane.url } }
    Component { id: pdfComponent; ViewerPdfView { url: pane.url } }
    Component { id: directoryComponent; ViewerDirectoryView { url: pane.url } }
    Component { id: binaryComponent; ViewerBinaryView { url: pane.url } }
    Component { id: editorComponent; EditorPaneView { fileUrl: pane.url } }

    // Nothing open. A pane a user can land on must say what it is and how to
    // fill it — the internal pane id is plumbing, and printing it as the
    // headline (which this used to do) tells nobody anything.
    Component {
        id: emptyComponent
        Rectangle {
            color: "#181825"

            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 340)
                spacing: 8

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "\u25a7"
                    color: "#45475a"
                    font.pixelSize: 30
                }
                Label {
                    objectName: "emptyTitle"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Nothing open in this pane")
                    color: "#cdd6f4"
                    font.pixelSize: 14
                }
                Label {
                    objectName: "emptyHint"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: qsTr("Open a file from the sessions sidebar, or split and close panes "
                               + "from the command palette (Ctrl+Shift+P).")
                    color: "#6c7086"
                    font.pixelSize: 12
                }
                Label {
                    objectName: "emptyPaneId"
                    anchors.horizontalCenter: parent.horizontalCenter
                    // The id still has to be reachable for a bug report; it is
                    // just no longer the message. Never markup: pane ids are
                    // built from server-supplied session ids.
                    textFormat: Text.PlainText
                    text: pane.paneId
                    visible: pane.paneId.length > 0
                    color: "#45475a"
                    font.pixelSize: 10
                    font.family: "Monospace"
                }
            }
        }
    }
}
