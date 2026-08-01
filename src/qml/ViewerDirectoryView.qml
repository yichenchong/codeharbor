import QtQuick
import QtQuick.Controls.Basic
import "RemotePath.js" as RemotePath

// Remote directory view (SPEC 7.5). Lists a remote directory's entries via
// ViewerModel.listDirectory (file.listDirectory); directories are suffixed with
// "/".
//
// NAVIGABLE. This used to be a dead list — the only way to reach a file was for
// something else to hand the pane a URL, and nothing in the application did, so
// a viewer pane could not be filled at all. Activating a row now navigates:
// `openRequested` carries the remote path the pane should open, and the pane
// (ViewerPane) turns it into the file:// URL the rest of the code expects. A
// directory's path is emitted WITH its trailing slash, because that trailing
// slash is exactly what ViewerHandlerRegistry::resolve() reads as "this is a
// directory" — so navigating into a sub-directory lands back in this view, and
// activating a file opens the matching viewer, both in the same pane.
Rectangle {
    id: root
    property url url

    // The pane should open `path` (absolute, on the remote server; a directory
    // ends in "/"). Deliberately a request rather than a self-assignment: the
    // pane owns what it is showing and has to report it to the host so the Dev
    // Session reopens on the same file (SessionLayouts::setPaneUrl).
    signal openRequested(string path)

    color: Theme.surface

    property var entries: []
    property string errorText: ""
    // A listing is in flight. Without it an empty list means both "still asking
    // the server" and "this directory is empty", and the pane shows the same
    // blank rectangle for a slow link as for a finished answer.
    property bool loading: false

    // The `viewers` context property (ch::ViewerModel), resolved once and
    // guarded exactly as ViewerPane and EditorPaneView guard it, and for the
    // same reason: an unguarded lookup of a context property the host did not
    // install THROWS a ReferenceError, and that aborts the whole binding pass
    // building this view rather than producing an inert one.
    readonly property var viewerModel: (typeof viewers !== "undefined") ? viewers : null

    // Remote path for file.listDirectory: strip the file:// scheme.
    function remotePath(u) {
        return RemotePath.fileUrlToPath(u.toString());
    }

    // The listing this view has outstanding, and nothing else. Empty when there
    // is none.
    //
    // Held as STATE, set when the request goes out and cleared the moment a
    // reply settles it, rather than recomputed from `url` at reply time and
    // matched against anything that happens to fit. ONE ViewerModel is shared
    // by every viewer pane and they all listen on this one signal pair, so the
    // loose form let ANOTHER pane's listing of this same directory land here:
    // its answer silently replaced the rows under the user, and a
    // directoryError raised for its request painted an error over a listing
    // that had loaded perfectly well.
    //
    // The key is the path because ch::ViewerModel documents listDirectory as
    // pane-INDEPENDENT (ViewerModel.h) — the answer for a path is the same for
    // whoever asked — but a reply is still matched against what THIS view asked
    // for, once.
    property string requestedPath: ""

    // The directory being listed, WITHOUT the trailing slash that marked the URL
    // as a directory — the form child paths are built from. "/" keeps its slash
    // because it is the whole path, not a separator.
    readonly property string basePath: {
        const p = root.remotePath(root.url);
        return p.length > 1 && p.charAt(p.length - 1) === "/"
               ? p.substring(0, p.length - 1) : p;
    }

    // The directory above this one, or "" when there is none (the filesystem
    // root, or no URL at all) — which is what hides the ".." row.
    readonly property string parentPath: {
        const b = root.basePath;
        if (b.length === 0 || b === "/")
            return "";
        const i = b.lastIndexOf("/");
        if (i < 0)
            return "";
        return i === 0 ? "/" : b.substring(0, i);
    }

    // What the list actually shows: the server's entries, with a parent-directory
    // row in front of them when there is somewhere to go back to. `entries` stays
    // exactly the server's answer, so nothing reading it sees a row the server
    // never sent.
    readonly property var rows: {
        const out = [];
        if (root.parentPath.length > 0)
            out.push({ name: "..", kind: "parent" });
        for (let i = 0; i < root.entries.length; ++i)
            out.push(root.entries[i]);
        return out;
    }

    // Turn an activated row into the path the pane should open.
    function activate(row) {
        if (!row)
            return;
        if (row.kind === "parent") {
            root.openRequested(root.parentPath === "/" ? "/" : root.parentPath + "/");
            return;
        }
        // "/" is its own separator: "/" + "etc" must not become "//etc".
        const base = root.basePath === "/" ? "" : root.basePath;
        const child = base + "/" + row.name;
        root.openRequested(row.kind === "directory" ? child + "/" : child);
    }

    // Start listing the current URL, abandoning any listing still outstanding
    // for the previous one. There is nothing to cancel model-side — unlike a
    // text read, ch::ViewerModel keeps no per-listing bookkeeping to retire —
    // so dropping the key is exactly what abandoning one means here.
    function reload() {
        root.requestedPath = "";
        root.entries = [];
        root.errorText = "";
        root.loading = false;
        if (root.url.toString().length === 0 || !root.viewerModel)
            return;
        root.loading = true;
        root.requestedPath = root.remotePath(root.url);
        root.viewerModel.listDirectory(root.requestedPath);
    }

    onUrlChanged: reload()
    Component.onCompleted: reload()

    // This reply belongs to the listing THIS view issued. Settling clears the
    // key, so a later answer about the same directory — another pane's request,
    // or this pane's own probe in ViewerPane — cannot disturb what is already
    // on screen.
    function ownsReply(path) {
        return root.requestedPath.length > 0 && path === root.requestedPath;
    }

    Connections {
        target: root.viewerModel
        function onDirectoryListed(path, list) {
            if (root.ownsReply(path)) {
                root.requestedPath = "";
                root.entries = list;
                root.loading = false;
            }
        }
        function onDirectoryError(path, message) {
            if (root.ownsReply(path)) {
                root.requestedPath = "";
                root.errorText = message;
                root.loading = false;
            }
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: root.rows
        visible: root.errorText.length === 0
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: AppScrollBar {}

        // The Basic style draws an ItemDelegate's label in the SYSTEM palette's
        // text colour — near-black on this pane's own surface, i.e. an unreadable
        // listing. Every other row in this application states its colours, and
        // so does this one.
        delegate: ItemDelegate {
            id: entry
            required property var modelData

            readonly property bool isParent: entry.modelData.kind === "parent"
            readonly property bool isDirectory: entry.modelData.kind === "directory"

            width: entry.ListView.view ? entry.ListView.view.width : 0
            height: 26
            text: entry.isParent
                  ? "\u2191 .."
                  : entry.isDirectory ? entry.modelData.name + "/"
                                      : entry.modelData.name

            onClicked: root.activate(entry.modelData)

            // Every row does something now, so it says so under the pointer.
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }

            contentItem: Label {
                // Remote file names are data, never markup.
                textFormat: Text.PlainText
                text: entry.text
                color: entry.isParent || entry.isDirectory ? Theme.accent : Theme.text
                font.pixelSize: Theme.fontSizeLabel
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: entry.hovered ? Theme.surfaceHover : "transparent"
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
        color: root.errorText.length > 0 ? Theme.danger : Theme.textDim
        font.pixelSize: Theme.fontSizeLabel
    }
}
