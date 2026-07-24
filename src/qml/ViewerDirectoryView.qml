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
        if (root.url.toString().length > 0)
            viewers.listDirectory(root.remotePath(root.url));
    }

    onUrlChanged: reload()
    Component.onCompleted: reload()

    Connections {
        target: viewers
        function onDirectoryListed(path, list) {
            if (path === root.remotePath(root.url))
                root.entries = list;
        }
        function onDirectoryError(message) {
            root.errorText = message;
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: root.entries
        visible: root.errorText.length === 0

        delegate: ItemDelegate {
            required property var modelData
            width: ListView.view.width
            text: modelData.kind === "directory"
                  ? modelData.name + "/"
                  : modelData.name
        }
    }

    Label {
        anchors.centerIn: parent
        visible: root.errorText.length > 0
        text: qsTr("Error: %1").arg(root.errorText)
        color: "#f38ba8"
        font.pixelSize: 13
    }
}
