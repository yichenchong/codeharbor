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
            // Markdown is shown as read-only source for now; rich HTML rendering
            // is a later increment.
            case "text":
            case "markdown":
            default: return textComponent;
            }
        }
    }

    Component { id: webComponent; ViewerWebView { url: pane.url } }
    Component { id: textComponent; ViewerTextView { url: pane.url } }
    Component { id: imageComponent; ViewerImageView { url: pane.url } }
    Component { id: pdfComponent; ViewerPdfView { url: pane.url } }
    Component { id: directoryComponent; ViewerDirectoryView { url: pane.url } }
    Component { id: binaryComponent; ViewerBinaryView { url: pane.url } }

    Component {
        id: emptyComponent
        Rectangle {
            color: "#181825"
            Label {
                anchors.centerIn: parent
                text: pane.paneId.length > 0 ? pane.paneId : qsTr("Empty pane")
                color: "#6c7086"
                font.pixelSize: 13
            }
        }
    }
}
