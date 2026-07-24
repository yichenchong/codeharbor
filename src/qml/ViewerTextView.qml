import QtQuick
import QtQuick.Controls.Basic

// Read-only remote text / source view (SPEC 7.5). Content is fetched over the
// remote file service via ViewerModel.readTextFile (file.readFile). Editing is
// workstream E, so this view is intentionally read-only.
Rectangle {
    id: root
    property url url

    color: "#1e1e2e"

    property string content: ""
    property string errorText: ""

    // Remote path for file.readFile: strip the file:// scheme, leaving the
    // server-absolute path (a remote file:// URL never carries a host).
    function remotePath(u) {
        var s = u.toString();
        if (s.indexOf("file://") === 0)
            return decodeURIComponent(s.substring("file://".length));
        return s;
    }

    function reload() {
        root.content = "";
        root.errorText = "";
        if (root.url.toString().length > 0)
            viewers.readTextFile(root.remotePath(root.url));
    }

    onUrlChanged: reload()
    Component.onCompleted: reload()

    Connections {
        target: viewers
        function onTextFileRead(path, text) {
            if (path === root.remotePath(root.url))
                root.content = text;
        }
        function onTextFileError(message) {
            root.errorText = message;
        }
    }

    ScrollView {
        anchors.fill: parent
        clip: true

        TextArea {
            readOnly: true
            wrapMode: TextArea.NoWrap
            selectByMouse: true
            font.family: "monospace"
            font.pixelSize: 13
            color: "#cdd6f4"
            background: null
            text: root.errorText.length > 0
                  ? qsTr("Error: %1").arg(root.errorText)
                  : root.content
        }
    }
}
