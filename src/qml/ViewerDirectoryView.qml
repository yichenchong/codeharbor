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

    // Open-as requests carry the explicit handler choice and whether the user
    // asked for a new split. Keeping this as a signal leaves pane identity and
    // layout persistence in ViewerPane/Main rather than making the listing a
    // second pane manager.
    signal openAsRequested(string path, string kind, bool inNewPane)
    signal openWithRequested(string path, string scheme)
    signal messageRequested(string message)

    property var customSchemeRow: null

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
    function pathFor(row) {
        if (!row || row.kind === "parent")
            return "";
        // "/" is its own separator: "/" + "etc" must not become "//etc".
        const base = root.basePath === "/" ? "" : root.basePath;
        return base + "/" + row.name + (row.kind === "directory" ? "/" : "");
    }

    function activate(row) {
        if (!row)
            return;
        if (row.kind === "parent") {
            root.openRequested(root.parentPath === "/" ? "/" : root.parentPath + "/");
            return;
        }
        root.openRequested(root.pathFor(row));
    }

    function fallbackKinds(row) {
        if (!row || row.kind === "parent")
            return [];
        if (row.kind === "directory")
            return [ "directory" ];
        const lower = String(row.name).toLowerCase();
        const dot = lower.lastIndexOf(".");
        const ext = dot > 0 ? lower.substring(dot + 1) : "";
        if (ext === "png" || ext === "jpg" || ext === "jpeg" || ext === "gif"
                || ext === "svg" || ext === "webp" || ext === "bmp")
            return [ "image" ];
        if (ext === "pdf")
            return [ "pdf" ];
        if (ext === "txt" || ext === "md" || ext === "markdown"
                || ext === "js" || ext === "ts" || ext === "qml"
                || ext === "json" || ext === "yaml" || ext === "yml"
                || ext === "html" || ext === "htm" || ext === "css")
            return [ "editor", "text" ];
        return [ "binary" ];
    }

    function applicableKinds(row) {
        if (!row || row.kind === "parent")
            return [];
        const url = RemotePath.pathToFileUrl(root.pathFor(row));
        if (root.viewerModel
                && typeof root.viewerModel.applicableViewKinds === "function") {
            const kinds = root.viewerModel.applicableViewKinds(url);
            if (kinds && kinds.length > 0)
                return kinds;
        }
        return root.fallbackKinds(row);
    }

    function kindLabel(kind) {
        switch (kind) {
        case "editor": return qsTr("Editor");
        case "text": return qsTr("Text");
        case "image": return qsTr("Image");
        case "pdf": return qsTr("PDF");
        case "binary": return qsTr("Binary");
        case "directory": return qsTr("Directory");
        case "web": return qsTr("Web");
        default: return kind;
        }
    }

    function showCustomScheme(row) {
        root.customSchemeRow = row;
        customSchemeField.text = "";
        customSchemeDialog.open();
    }
    function validCustomScheme(scheme) {
        if (!/^[A-Za-z][A-Za-z0-9+.-]*$/.test(scheme))
            return false;
        const lower = scheme.toLowerCase();
        return lower !== "codeharbor-internal" && lower !== "http"
               && lower !== "https" && lower !== "file";
    }

    function submitCustomScheme() {
        const row = root.customSchemeRow;
        const scheme = customSchemeField.text.trim();
        if (!row || row.kind === "parent")
            return true;

        const valid = root.viewerModel
                      && typeof root.viewerModel.isValidApplicationScheme === "function"
                      ? root.viewerModel.isValidApplicationScheme(scheme)
                      : root.validCustomScheme(scheme);
        if (!valid) {
            root.messageRequested(qsTr("Invalid application scheme. Use a name "
                                       + "starting with a letter, followed by "
                                       + "letters, digits, '+', '-' or '.'."));
            return false;
        }
        if (!root.viewerModel
                || typeof root.viewerModel.openWithApplication !== "function"
                || !root.viewerModel.openWithApplication(scheme, root.pathFor(row))) {
            root.messageRequested(qsTr("No desktop application accepted %1://.")
                                  .arg(scheme));
            return false;
        }
        root.customSchemeRow = null;
        return true;
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
        objectName: "directoryList"
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
            readonly property var openAsKinds: root.applicableKinds(entry.modelData)
            readonly property string defaultKind:
                entry.openAsKinds.length > 0 ? String(entry.openAsKinds[0]) : ""
            readonly property string rowPath: root.pathFor(entry.modelData)

            width: entry.ListView.view ? entry.ListView.view.width : 0
            height: 26
            focusPolicy: Qt.StrongFocus
            text: entry.isParent
                  ? "\u2191 .."
                  : entry.isDirectory ? entry.modelData.name + "/"
                                      : entry.modelData.name

            onClicked: root.activate(entry.modelData)

            // Every row does something now, so it says so under the pointer.
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }

            // A context-menu key (or Shift+F10, which keyboards commonly use
            // when they omit a dedicated Menu key) is the non-pointer route.
            Keys.onPressed: function(event) {
                if (!entry.isParent
                        && (event.key === Qt.Key_Menu
                            || (event.key === Qt.Key_F10
                                && (event.modifiers & Qt.ShiftModifier)))) {
                    contextMenu.popup();
                    event.accepted = true;
                }
            }

            contentItem: Label {
                // Remote file names are data, never markup.
                textFormat: Text.PlainText
                text: entry.text
                color: entry.isParent || entry.isDirectory ? Theme.accent : Theme.text
                font.pixelSize: Theme.fontSizeLabel
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                // `openAsButton` is an id, reachable from here directly; it is
                // not a property of the delegate.
                rightPadding: openAsButton.visible ? openAsButton.width + 8 : 0
            }

            // A small, focusable affordance makes Open as reachable even on a
            // keyboard without a context-menu key.
            AppPaneHeader.Action {
                id: openAsButton
                objectName: "openAsButton"
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Open as")
                glyph: "\u2026"
                focusPolicy: Qt.TabFocus
                visible: !entry.isParent && entry.openAsKinds.length > 0
                onClicked: contextMenu.popup()
            }

            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: if (!entry.isParent) contextMenu.popup()
            }

            background: Rectangle {
                color: entry.hovered ? Theme.surfaceHover : "transparent"
            }

            Menu {
                id: contextMenu
                objectName: "directoryContextMenu"

                // Instantiator, not Repeater: a Menu keeps its entries in a
                // content MODEL, and a Repeater's items are never added to it -
                // they are created and then belong to nothing, so the menu
                // renders empty. Instantiator hands each created object over
                // explicitly, which is the supported way to build menu entries
                // from data.
                Menu {
                    id: openAsMenu
                    objectName: "openAsSubmenu"
                    title: qsTr("Open as")
                    enabled: entry.openAsKinds.length > 0
                    Instantiator {
                        model: entry.openAsKinds
                        onObjectAdded: (index, object) => openAsMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => openAsMenu.removeItem(object)
                        delegate: MenuItem {
                            required property var modelData
                            readonly property string viewerKind: String(modelData)
                            checkable: true
                            checked: viewerKind === entry.defaultKind
                            text: root.kindLabel(viewerKind)
                                   + (checked ? qsTr(" (default)") : "")
                            onTriggered: root.openAsRequested(entry.rowPath, viewerKind, false)
                        }
                    }
                }

                MenuSeparator {}

                // The short command uses the item's default handler. The
                // submenu below keeps alternate handler choices available in a
                // new pane too, rather than silently changing the current one.
                MenuItem {
                    text: qsTr("Open in new pane")
                    enabled: entry.defaultKind.length > 0
                    onTriggered: root.openAsRequested(entry.rowPath,
                                                       entry.defaultKind, true)
                }

                Menu {
                    id: openAsNewPaneMenu
                    objectName: "openAsNewPaneSubmenu"
                    title: qsTr("Open as in new pane")
                    enabled: entry.openAsKinds.length > 0
                    Instantiator {
                        model: entry.openAsKinds
                        onObjectAdded: (index, object) =>
                            openAsNewPaneMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) =>
                            openAsNewPaneMenu.removeItem(object)
                        delegate: MenuItem {
                            required property var modelData
                            readonly property string viewerKind: String(modelData)
                            text: root.kindLabel(viewerKind)
                            onTriggered: root.openAsRequested(entry.rowPath,
                                                               viewerKind, true)
                        }
                    }
                }

                MenuItem {
                    text: qsTr("Open with\u2026")
                    enabled: !entry.isParent
                    onTriggered: root.showCustomScheme(entry.modelData)
                }
            }
        }
    }
    AppDialog {
        id: customSchemeDialog
        objectName: "customApplicationSchemeDialog"
        title: qsTr("Open with\u2026")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        width: 380

        Column {
            width: parent.width
            spacing: 8

            Label {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Enter the application scheme to handle this remote path "
                           + "(for example, my-editor).")
                color: Theme.textDim
            }
            TextField {
                id: customSchemeField
                objectName: "customApplicationSchemeField"
                width: parent.width
                placeholderText: qsTr("application scheme")
                onAccepted: customSchemeDialog.accept()
            }
        }

        onOpened: {
            customSchemeField.text = "";
            customSchemeField.forceActiveFocus();
        }
        onAccepted: if (!root.submitCustomScheme()) customSchemeDialog.open()
        onRejected: root.customSchemeRow = null
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
