import QtQuick
import QtQuick.Controls.Basic

// Remote directory view (SPEC 7.5). Lists a remote directory's entries via
// ViewerModel.listDirectory (file.listDirectory); directories are suffixed with
// "/". Read-only: activating an entry is a later increment.
Rectangle {
    id: root
    property url url

    color: "#1e1e2e"

    property var entries: []
    property string errorText: ""
    // A listing is in flight. Without it an empty list means both "still asking
    // the server" and "this directory is empty", and the pane shows the same
    // blank rectangle for a slow link as for a finished answer.
    property bool loading: false

    // Remote path for file.listDirectory: strip the file:// scheme.
    function remotePath(u) {
        var s = u.toString();
        if (s.indexOf("file://") === 0)
            return decodeURIComponent(s.substring("file://".length));
        return s;
    }

    function reload() {
        root.entries = [];
        root.errorText = "";
        root.loading = root.url.toString().length > 0;
        if (root.loading)
            viewers.listDirectory(root.remotePath(root.url));
    }

    onUrlChanged: reload()
    Component.onCompleted: reload()

    Connections {
        target: viewers
        function onDirectoryListed(path, list) {
            if (path === root.remotePath(root.url)) {
                root.entries = list;
                root.loading = false;
            }
        }
        function onDirectoryError(path, message) {
            if (path === root.remotePath(root.url)) {
                root.errorText = message;
                root.loading = false;
            }
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: root.entries
        visible: root.errorText.length === 0

        // The Basic style draws an ItemDelegate's label in the SYSTEM palette's
        // text colour — near-black on this pane's #1e1e2e, i.e. an unreadable
        // listing. Every other row in this application states its colours, and
        // so does this one.
        delegate: ItemDelegate {
            id: entry
            required property var modelData
            width: entry.ListView.view ? entry.ListView.view.width : 0
            height: 26
            text: entry.modelData.kind === "directory"
                  ? entry.modelData.name + "/"
                  : entry.modelData.name

            contentItem: Label {
                // Remote file names are data, never markup.
                textFormat: Text.PlainText
                text: entry.text
                color: entry.modelData.kind === "directory" ? "#89b4fa" : "#cdd6f4"
                font.pixelSize: 13
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: entry.hovered ? "#232338" : "transparent"
            }
        }
    }

    // One place for every "there is nothing to list" answer, so the pane always
    // says which of them it is instead of showing an empty box.
    Label {
        objectName: "directoryStatus"
        anchors.centerIn: parent
        width: parent.width - 48
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        // Server-supplied failure text: data, not markup.
        textFormat: Text.PlainText
        visible: text.length > 0
        text: root.errorText.length > 0
              ? qsTr("Error: %1").arg(root.errorText)
              : root.loading ? qsTr("Listing\u2026")
              : root.entries.length === 0 && root.url.toString().length > 0
                ? qsTr("This directory is empty.") : ""
        color: root.errorText.length > 0 ? "#f38ba8" : "#6c7086"
        font.pixelSize: 13
    }
}
