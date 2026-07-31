import QtQuick
import QtQuick.Controls.Basic
import "RemotePath.js" as RemotePath

// Read-only remote text / source view (SPEC 7.5). Content is fetched over the
// remote file service via ViewerModel.readTextFile (file.readFile). Editing is
// workstream E, so this view is intentionally read-only.
Rectangle {
    id: root
    property url url

    color: Theme.surface

    property string content: ""
    property string errorText: ""
    // A read is in flight. Without it an empty buffer means both "still asking
    // the server" and "this file is empty", and the pane looks identical to a
    // pane that failed silently.
    property bool loading: false

    // Remote path for file.readFile: strip the file:// scheme, leaving the
    // server-absolute path (a remote file:// URL never carries a host).
    function remotePath(u) {
        return RemotePath.fileUrlToPath(u.toString());
    }

    // The read this view has outstanding: the token ViewerModel.readTextFile()
    // handed back, plus the path it was for. Both empty when nothing is
    // outstanding.
    property string requestedToken: ""
    property string requestedPath: ""

    // Start reading the current URL, dropping any read still in flight for the
    // PREVIOUS one.
    //
    // ch::ViewerModel::cancelTextFile() takes the TOKEN of the read to drop
    // (src/viewers/ViewerModel.h), never a path. ONE ViewerModel is shared by
    // every viewer pane, and two panes may be showing the SAME file, so a
    // path-shaped cancel could not tell this view's read from the other pane's
    // read of that same file and would leave that pane loading forever.
    //
    // The path comparison in the Connections block below is kept as defence in
    // depth; the token is what actually decides.
    function reload() {
        if (root.requestedToken.length > 0)
            viewers.cancelTextFile(root.requestedToken);
        root.requestedToken = "";
        root.requestedPath = "";
        root.content = "";
        root.errorText = "";
        root.loading = root.url.toString().length > 0;
        if (root.loading) {
            root.requestedPath = root.remotePath(root.url);
            root.requestedToken = viewers.readTextFile(root.requestedPath);
        }
    }

    onUrlChanged: reload()
    Component.onCompleted: reload()

    // This reply belongs to the read this view issued, and to no other pane's.
    // Settling clears the token: there is nothing left to cancel, and the next
    // reload must not ask the model to drop a read that already finished.
    function ownsReply(token, path) {
        return token.length > 0 && token === root.requestedToken
               && path === root.requestedPath;
    }

    Connections {
        target: viewers
        function onTextFileRead(token, path, text) {
            if (root.ownsReply(token, path)) {
                root.requestedToken = "";
                root.content = text;
                root.loading = false;
            }
        }
        function onTextFileError(token, path, message) {
            if (root.ownsReply(token, path)) {
                root.requestedToken = "";
                root.errorText = message;
                root.loading = false;
            }
        }
    }

    ScrollView {
        anchors.fill: parent
        clip: true

        // The application's own scrollbars. The Basic style's are a light plate
        // sized for arrow buttons this application does not draw, which is what
        // made a dark text pane look like it had borrowed someone else's chrome.
        // Both axes: the text is deliberately NOT wrapped, so a long line is
        // reached by scrolling sideways.
        ScrollBar.vertical: AppScrollBar {}
        ScrollBar.horizontal: AppScrollBar {}

        TextArea {
            readOnly: true
            wrapMode: TextArea.NoWrap
            selectByMouse: true
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeLabel
            color: Theme.text
            background: null
            // Content ONLY. A failure used to be rendered in here, in the same
            // monospace face as the file, where it is indistinguishable from a
            // file whose first line happens to read "Error: ...".
            text: root.content
        }
    }

    // Every "there is nothing to read" answer in one place, so the pane says
    // which one it is instead of showing an empty buffer. Declared after the
    // ScrollView so it draws on top of it.
    Label {
        objectName: "textStatus"
        anchors.centerIn: parent
        width: parent.width - 48
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        // Server-supplied failure text: data, not markup.
        textFormat: Text.PlainText
        visible: text.length > 0
        text: root.errorText.length > 0
              ? qsTr("Error: %1").arg(root.errorText)
              : root.loading ? qsTr("Loading\u2026")
              : root.content.length === 0 && root.url.toString().length > 0
                ? qsTr("This file is empty.") : ""
        color: root.errorText.length > 0 ? Theme.danger : Theme.textDim
        font.pixelSize: Theme.fontSizeLabel
    }
}
