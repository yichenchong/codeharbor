import QtQuick
import QtQuick.Controls

// Remote directory browser, and the mobile client's IN-PANE navigator
// (mobile SPEC 7.5 / 9).
//
// SINGLE PANE. There is no split, no second column and no new window: tapping a
// directory re-lists inside this same page, and tapping a FILE swaps this page's
// content for the matching viewer, also inside this same page. Both cases also
// emit openRequested() so the host can persist the leaf's url — the host
// reassigns `remotePath` on this very item, which lands right back here. A file
// is left again through the button beside the address, which re-lists the
// directory it was tapped in — an in-pane navigator with no way back would strand
// the pane on the first file the user touched.
//
// The desktop's SPEC 9 marker discipline is kept: a path outside the Dev
// Session's repository root stays fully openable, and is simply MARKED. The
// marker is quiet and beside the address, never a block — being outside the
// project is a legitimate place to be, just an easy one to be in by accident.
Item {
    id: root

    property string remotePath: ""
    property url paneUrl
    property string repoRoot: ""
    property string paneId: ""
    signal openRequested(string path)
    signal titleRequested(string title)

    readonly property var service: (typeof viewerService !== "undefined") ? viewerService : null
    readonly property var capabilities: (typeof mobile !== "undefined" && mobile.capabilities)
                                        ? mobile.capabilities : null

    // What this pane is currently showing. Starts at `remotePath` and moves as
    // the user taps; the host may also reassign `remotePath`, which resets it.
    property string currentPath: ""
    property var entries: []
    property string errorText: ""
    property bool loading: false
    property string requestedPath: ""

    // "unknown" | "inside" | "outside" — the SPEC 9 marker state for currentPath.
    property string repoRootState: "unknown"
    property string repoCheckPath: ""

    // A host reassignment that names what this pane is ALREADY showing is
    // ignored: activate() below navigates first and then tells the host, so the
    // host's answering assignment would otherwise re-list the same directory —
    // clearing the rows and refetching them for nothing.
    // The `started` flag makes this handler inert until the object is fully
    // built. Initial properties are assigned BEFORE Component.onCompleted runs,
    // so without it the host's first `remotePath` listed the directory here and
    // the completion handler listed it a second time — two round trips, and a
    // visible flash as the second reply cleared and refilled the rows.
    property bool started: false
    onRemotePathChanged: {
        if (root.started && root.remotePath !== root.currentPath)
            root.navigate(root.remotePath);
    }
    Component.onCompleted: {
        root.started = true;
        root.navigate(root.remotePath);
    }

    // Every move of this pane's target has to reach the page inside it, whether
    // or not that page was rebuilt. See Loader.applyTarget() below.
    onCurrentPathChanged: content.applyTarget()

    // Is `path` a directory? A trailing slash is exactly what
    // ch::ViewerHandlerRegistry reads as "directory", and the same convention is
    // used here rather than a second rule.
    //
    // Plus the one directory a trailing slash cannot express: the session's
    // repository root as PaneHostPage hands it over. A viewer leaf with NO url
    // is "the session root, i.e. a directory listing" (ch::PaneListModel), and
    // the host resolves that to repoRoot — which has no trailing slash. Without
    // this clause the default viewer pane classified its own starting point as a
    // file and rendered "cannot show this Binary" instead of the listing.
    function isDirectoryPath(path) {
        return path.length === 0 || path.charAt(path.length - 1) === "/"
               || (root.repoRoot.length > 0 && path === root.repoRoot);
    }

    // The kind THIS page should render for `currentPath`. Delegated to the
    // registry through the service, so the mobile client and the desktop resolve
    // one path to one kind.
    readonly property string contentKind: {
        if (root.currentPath.length === 0 || root.isDirectoryPath(root.currentPath))
            return "directory";
        if (!root.service)
            return "binary";
        return root.service.viewKindFor(root.service.fileUrlFor(root.currentPath));
    }

    // The directory being listed, without the trailing slash that marked it as
    // one. "/" keeps its slash: it is the whole path, not a separator.
    readonly property string basePath: {
        const p = root.currentPath;
        if (p.length === 0)
            return "/";
        return p.length > 1 && p.charAt(p.length - 1) === "/"
               ? p.substring(0, p.length - 1) : p;
    }

    readonly property string parentPath: {
        const b = root.basePath;
        if (b.length === 0 || b === "/")
            return "";
        const i = b.lastIndexOf("/");
        if (i < 0)
            return "";
        return i === 0 ? "/" : b.substring(0, i);
    }

    // Server entries with a parent row in front when there is somewhere to go.
    // `entries` stays exactly the server's answer, so nothing reading it sees a
    // row the server never sent.
    readonly property var rows: {
        const out = [];
        if (root.parentPath.length > 0)
            out.push({ name: "..", kind: "parent" });
        for (let i = 0; i < root.entries.length; ++i)
            out.push(root.entries[i]);
        return out;
    }

    function childPath(row) {
        if (!row || row.kind === "parent")
            return "";
        const base = root.basePath === "/" ? "" : root.basePath;
        return base + "/" + row.name + (row.kind === "directory" ? "/" : "");
    }

    // The directory `path` lives in, trailing slash included so it reads as a
    // directory to isDirectoryPath() and to the viewer registry.
    function containingDirectory(path) {
        const i = path.lastIndexOf("/");
        return i <= 0 ? "/" : path.substring(0, i + 1);
    }

    // Back to the listing the current file was tapped in. Without this, tapping
    // a file was a ONE-WAY trip: the listing is hidden while a file is shown,
    // the pane's persisted url has already moved to that file, and the only exit
    // was leaving the pane altogether and reopening it — which reopened the file.
    function leaveFile() {
        const dir = root.containingDirectory(root.currentPath);
        root.navigate(dir);
        root.openRequested(dir);
    }

    // The label for a path, for the pane header: the last segment, which is the
    // name of the directory or file the pane is showing. "/" keeps its slash
    // because it has no segment to be named by.
    function displayTitle(path) {
        const trimmed = path.length > 1 && path.charAt(path.length - 1) === "/"
                      ? path.substring(0, path.length - 1) : path;
        const i = trimmed.lastIndexOf("/");
        const name = i >= 0 ? trimmed.substring(i + 1) : trimmed;
        return name.length > 0 ? name : path;
    }

    function navigate(path) {
        const target = path.length === 0 ? "/" : path;
        root.currentPath = target;
        root.entries = [];
        root.errorText = "";
        root.requestedPath = "";
        root.loading = false;
        root.repoRootState = "unknown";
        root.repoCheckPath = "";
        // PaneHostPage connects titleRequested precisely so an in-pane
        // navigation can move the header. Nothing was emitting it, so the header
        // kept naming the place the pane was opened at no matter how deep the
        // user had walked.
        root.titleRequested(root.displayTitle(target));
        if (!root.service)
            return;
        // The SPEC 9 question, asked for every path this pane shows — file or
        // directory — exactly as ViewerPane asks it on the desktop.
        root.repoCheckPath = root.isDirectoryPath(target) ? root.basePath : target;
        root.service.resolvePath(root.repoCheckPath, root.repoRoot);
        if (!root.isDirectoryPath(target))
            return;
        root.loading = true;
        root.requestedPath = root.basePath;
        root.service.listDirectory(root.basePath);
    }

    function activate(row) {
        if (!row)
            return;
        const target = row.kind === "parent"
                     ? (root.parentPath === "/" ? "/" : root.parentPath + "/")
                     : root.childPath(row);
        if (target.length === 0)
            return;
        // Applied locally FIRST, then told to the host so the leaf's url is
        // persisted. This order is what makes the host's answering assignment of
        // `remotePath` a no-op (see onRemotePathChanged): the other way round,
        // that assignment listed the directory once and this call listed it
        // again.
        root.navigate(target);
        root.openRequested(target);
    }

    Connections {
        target: root.service
        function onDirectoryListed(path, listing) {
            if (root.requestedPath.length === 0 || path !== root.requestedPath)
                return;
            root.requestedPath = "";
            root.loading = false;
            // Already ordered directories-first-then-name by
            // ch::MobileViewerService; re-sorting here would be a second, and
            // eventually different, opinion.
            root.entries = listing;
        }
        function onDirectoryError(path, message) {
            if (root.requestedPath.length === 0 || path !== root.requestedPath)
                return;
            root.requestedPath = "";
            root.loading = false;
            root.errorText = message;
        }
        function onPathResolved(path, resolvedPath, insideRepositoryRoot) {
            if (path !== root.repoCheckPath)
                return;
            root.repoRootState = insideRepositoryRoot ? "inside" : "outside";
        }
    }

    Rectangle {
        anchors.fill: parent
        color: MobileTheme.surface
    }

    Column {
        anchors.fill: parent
        spacing: 0

        // ---- address + SPEC 9 marker ---------------------------------------

        Item {
            width: parent.width
            height: MobileTheme.touchTarget

            // The way back to the listing, shown only while this pane is showing
            // a FILE. See leaveFile().
            AbstractButton {
                id: upButton
                objectName: "directoryUpButton"
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: MobileTheme.touchTarget
                implicitHeight: MobileTheme.touchTarget
                // Zero-width while hidden, so a listing's address starts exactly
                // where it did before.
                width: visible ? implicitWidth : 0
                visible: root.contentKind !== "directory"
                onClicked: root.leaveFile()

                contentItem: Text {
                    // The same glyph as the pane header's own back button, so
                    // one gesture reads one way in this shell.
                    text: "\u2039"
                    textFormat: Text.PlainText
                    color: MobileTheme.accent
                    font.pixelSize: MobileTheme.fontSizeTitle
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Text {
                id: address
                anchors.left: upButton.right
                anchors.right: outsideMarker.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: MobileTheme.spacing
                anchors.rightMargin: MobileTheme.spacing
                elide: Text.ElideLeft
                // ONE line, always. A remote path may legally contain a newline,
                // and elide alone does not stop Text from laying that out as two
                // lines and growing straight out of this 48-pixel row.
                maximumLineCount: 1
                // Server-controlled. PlainText.
                textFormat: Text.PlainText
                color: MobileTheme.textDim
                font.family: MobileTheme.monoFamily
                font.pixelSize: MobileTheme.fontSizeSmall
                text: root.currentPath
            }

            Text {
                id: outsideMarker
                objectName: "outsideRootMarker"
                anchors.right: parent.right
                anchors.rightMargin: MobileTheme.spacing
                anchors.verticalCenter: parent.verticalCenter
                // Zero-width while hidden, so an in-project pane's address is
                // exactly as wide as it was before.
                width: visible ? implicitWidth : 0
                visible: root.repoRootState === "outside"
                textFormat: Text.PlainText
                color: MobileTheme.warning
                font.pixelSize: MobileTheme.fontSizeSmall
                text: qsTr("outside project")
            }
        }

        Rectangle {
            width: parent.width
            height: 1
            color: MobileTheme.borderSubtle
        }

        Rectangle {
            width: parent.width
            height: visible ? Math.max(MobileTheme.touchTarget, listingError.implicitHeight + 2 * MobileTheme.spacing) : 0
            visible: root.errorText.length > 0
            color: MobileTheme.errorSurface()
            Text {
                id: listingError
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

        // ---- the listing, or the file the user tapped ----------------------

        Item {
            width: parent.width
            height: parent.height - y

            ListView {
                id: list
                objectName: "directoryList"
                anchors.fill: parent
                clip: true
                visible: root.contentKind === "directory"
                model: root.rows
                ScrollBar.vertical: ScrollBar {}

                delegate: ItemDelegate {
                    required property var modelData
                    width: list.width
                    // Every touch target is at least 48dp; a row that is exactly
                    // as tall as its text is unusable with a thumb.
                    height: MobileTheme.touchTarget
                    onClicked: root.activate(modelData)

                    contentItem: Row {
                        spacing: MobileTheme.spacing
                        Text {
                            id: rowIcon
                            anchors.verticalCenter: parent.verticalCenter
                            textFormat: Text.PlainText
                            color: MobileTheme.textFaint
                            font.pixelSize: MobileTheme.fontSizeBody
                            text: modelData.kind === "parent"
                                  ? "↑"
                                  : (modelData.kind === "directory" ? "▸" : "·")
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            // The icon's own width is subtracted rather than
                            // guessed at: a fixed allowance left the name column
                            // wider than the row and long names ran off the
                            // right edge instead of eliding.
                            width: list.width - rowIcon.width
                                   - 4 * MobileTheme.spacing
                            elide: Text.ElideMiddle
                            // One line for the same reason as the address above:
                            // a file name may contain a newline.
                            maximumLineCount: 1
                            // A remote file NAME is server-controlled data.
                            textFormat: Text.PlainText
                            color: MobileTheme.text
                            font.pixelSize: MobileTheme.fontSizeBody
                            text: modelData.kind === "directory"
                                  ? modelData.name + "/" : modelData.name
                        }
                    }
                }
            }

            // The pane's content when the user has tapped down to a file. Local
            // page state, one viewer at a time — the single-pane invariant holds
            // inside the pane as well as across the app.
            Loader {
                id: content
                anchors.fill: parent
                active: root.contentKind !== "directory"

                // The kind -> page table for the file the user tapped down to.
                // Total: an unrecognised kind lands on the unsupported page
                // rather than on nothing.
                //
                // There is deliberately NO "web" branch. Every path this page can
                // reach is a remote file:// path, and ch::ViewerHandlerRegistry
                // never resolves one of those to web navigation. If it somehow
                // did, loading it in a web view would be handing a remote file://
                // address to a browser engine — the exact SPEC 7.4 mistake the
                // internal scheme exists to prevent. It falls through to
                // "unsupported" instead.
                function pageFor(kind) {
                    switch (kind) {
                    case "markdown": return "ViewerMarkdownPage.qml";
                    case "text": return "ViewerTextPage.qml";
                    case "image": return "ViewerImagePage.qml";
                    case "pdf":
                        return (root.capabilities && root.capabilities.hasPdf)
                               ? "ViewerPdfPage.qml" : "ViewerUnsupportedPage.qml";
                    default: return "ViewerUnsupportedPage.qml";
                    }
                }

                source: content.pageFor(root.contentKind)

                // Pushed rather than bound (the loaded page owns these once it
                // has them) — and pushed AGAIN on every navigation, which is the
                // part that was missing: a Loader whose `source` string does not
                // change does not rebuild, so tapping one text file straight
                // after another left the FIRST file's content on screen under the
                // second file's address, with nothing to make it re-read.
                function applyTarget() {
                    if (!content.item)
                        return;
                    // Only the page this kind actually wants. When `source` has
                    // changed, the Loader is about to rebuild the item with these
                    // properties anyway, and pushing into the outgoing item would
                    // start a read for a page that is being destroyed.
                    if (!content.source.toString()
                             .endsWith(content.pageFor(root.contentKind)))
                        return;
                    content.item.remotePath = root.currentPath;
                    content.item.paneUrl = root.service
                                         ? root.service.fileUrlFor(root.currentPath) : "";
                    content.item.repoRoot = root.repoRoot;
                    content.item.paneId = root.paneId;
                    if (content.item.kind !== undefined)
                        content.item.kind = root.contentKind;
                }

                onLoaded: content.applyTarget()
            }

            // An empty directory and a directory that failed to list looked
            // identical: a blank area under an address. The parent row is still
            // there to tap, so this says what is missing rather than replacing
            // the listing.
            Text {
                anchors.centerIn: parent
                width: parent.width - 2 * MobileTheme.spacingLarge
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: MobileTheme.textDim
                font.pixelSize: MobileTheme.fontSizeBody
                // Only once an answer has actually arrived: with no service this
                // page never asked anything, and "empty" would be a claim it
                // cannot make.
                visible: root.contentKind === "directory" && root.service !== null
                         && !root.loading && root.errorText.length === 0
                         && root.entries.length === 0
                text: qsTr("This directory is empty.")
            }
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: root.loading
        visible: root.loading
    }
}
