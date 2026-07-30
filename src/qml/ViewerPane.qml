import QtQuick
import QtQuick.Controls.Basic

// A single viewer pane (SPEC 3.3, 7.5) — a leaf of the viewer split tree. It
// asks the ViewerModel (`viewers`) to classify its URL and loads the matching
// sub-view (source and markdown open in the Monaco editor, SPEC 8.1).
//
// THE PANE IS FILLED FROM HERE. Until now a viewer pane could only be filled by
// something else handing it a URL, and nothing in the application did: the
// sidebar lists Dev Sessions and has no file browser, and the command palette
// carries only split/close/kill commands. So a pane's own header now carries an
// ADDRESS BAR — a remote absolute path or a URL, Enter opens it in this pane —
// an empty pane falls back to LISTING the active Dev Session's repository root
// so there is always something to click, and the directory listing itself is
// navigable (ViewerDirectoryView.openRequested). None of that goes through the
// palette.
//
// file:// ALWAYS means a file on the remote SSH server, never a local one
// (SPEC 8.3), so remotePathOf()/fileUrlFor() below are the only conversion
// between the two spellings and are exact inverses of each other.
Item {
    id: pane

    property url url
    property string paneId: ""

    // This is the pane the user is working in. Written by the region that owns
    // the pane cache (ViewerRegion.applyFocusFlags) rather than derived here:
    // focus is a property of the REGION — exactly one of its panes has it — and
    // a pane cannot see its siblings.
    property bool paneActive: false

    // ---- focus reporting (SPEC 4.5) ----------------------------------------
    // The user is working in THIS pane. ViewerRegion connects this when it
    // mints the pane (takePane) and republishes it as the root region's
    // `focusedPaneId`; that is what the split/close commands target, instead of
    // guessing at the region's first leaf.
    signal paneActivated(string paneId)

    // ---- content reporting (SPEC 4.5) --------------------------------------
    // What this pane has open. The host records it in this pane's split-tree
    // leaf (SessionLayouts::setPaneUrl), which is what makes reopening a Dev
    // Session restore the FILES the user had open and not just the geometry.
    //
    // Also emitted on a paneId change, because a pane is born before it has a
    // name: ViewerRegion.takePane() creates the Item WITH its url and assigns
    // paneId immediately afterwards, so a url-only report would arrive
    // anonymous. That birth report is pure echo — the url came out of the very
    // leaf the host has stored — and the region deliberately subscribes AFTER
    // naming the pane, so nobody hears it; see the ordering comment in
    // ViewerRegion.takePane(). What the host needs is every later CHANGE, and
    // this signal is how those arrive.
    signal urlOpened(string paneId, string url)

    // The user asked to close this pane, from its own header. Closing a pane is
    // a LAYOUT change (SessionLayouts::closePane collapses the parent branch),
    // which only the host can make, so the pane asks rather than acts. The
    // region relays it as `closePaneRequested`.
    signal closeRequested(string paneId)

    function reportUrl() {
        // An unnamed pane is not addressable. The empty paneId is also a real
        // key (the placeholder leaf of an emptied region), but the two are
        // indistinguishable here, and reporting a url against a tree that has
        // no empty leaf would only raise a spurious host error.
        if (pane.paneId.length === 0)
            return;
        // `url`, deliberately NOT `effectiveUrl`: the session-root listing an
        // empty pane falls back to is a DEFAULT, not something the user opened,
        // and persisting it would turn every empty pane into a pinned directory
        // that no longer follows the Dev Session.
        pane.urlOpened(pane.paneId, pane.url.toString());
    }

    onUrlChanged: pane.reportUrl()
    onPaneIdChanged: pane.reportUrl()

    // ---- remote paths <-> file:// URLs -------------------------------------

    // The remote path inside a file:// URL; anything else (http/https) comes
    // back unchanged, because that IS its address.
    function remotePathOf(u) {
        const s = u.toString();
        if (s.indexOf("file://") === 0)
            return decodeURIComponent(s.substring("file://".length));
        return s;
    }

    // The exact inverse of remotePathOf(), and of ViewerDirectoryView's
    // remotePath(): each SEGMENT is percent-encoded and the separators are left
    // alone, so decodeURIComponent() on the result returns the original path
    // character for character. encodeURI() would not do — it leaves "#" and "?"
    // unescaped, and a remote file named "notes#1" would silently become a URL
    // with a fragment.
    function fileUrlFor(path) {
        const parts = String(path).split("/");
        for (let i = 0; i < parts.length; ++i)
            parts[i] = encodeURIComponent(parts[i]);
        return "file://" + parts.join("/");
    }

    // Open a remote path in THIS pane. A trailing slash means a directory, which
    // is what ViewerHandlerRegistry::resolve() reads to pick the listing view.
    function openRemotePath(path) {
        pane.url = pane.fileUrlFor(path);
    }

    // ---- the session's repository root, as an empty pane's default ---------

    // `app` is a root context property (AppController). Guarded with typeof
    // exactly as TerminalPaneView guards `terminalFactory`: an unguarded lookup
    // of a missing context property THROWS, which would abort the whole binding
    // pass that is building the pane, and a bare QML load (tst_uxshell) installs
    // no `app`.
    readonly property string sessionRoot:
        (typeof app !== "undefined" && app && app.activeSessionRepoRoot)
        ? String(app.activeSessionRepoRoot) : ""

    // The session root as a directory URL, or empty when there is no active Dev
    // Session — in which case the pane keeps a real empty state.
    readonly property url defaultUrl: {
        const root = pane.sessionRoot;
        if (root.length === 0)
            return "";
        return pane.fileUrlFor(root.charAt(root.length - 1) === "/" ? root : root + "/");
    }

    readonly property bool showingDefault: pane.url.toString().length === 0
                                           && pane.defaultUrl.toString().length > 0

    // What the sub-views actually render: what the user opened, else the session
    // root listing.
    readonly property url effectiveUrl: pane.url.toString().length > 0
                                        ? pane.url : pane.defaultUrl

    // The `viewers` context property (ch::ViewerModel), resolved once and
    // guarded for the same reason `app` is above: this pane's chrome — the
    // header, the address bar — exists whether or not there is anything open, so
    // the lookup is no longer confined to a branch that only runs once a URL has
    // arrived. An unguarded lookup of a missing context property throws, which
    // would abort the binding pass building the pane.
    readonly property var viewerModel: (typeof viewers !== "undefined") ? viewers : null

    // View kind for the current URL: "web" | "markdown" | "text" | "image" |
    // "pdf" | "directory" | "binary" (nothing to show -> a neutral placeholder).
    property string kind: (pane.effectiveUrl.toString().length === 0 || !pane.viewerModel)
                          ? "empty"
                          : pane.viewerModel.viewKind(pane.effectiveUrl)

    // The address as the user reads and edits it: a remote path for a file:// URL
    // and the address itself for anything else.
    readonly property string displayPath: pane.remotePathOf(pane.effectiveUrl)

    // Last segment of the address, which is what identifies a pane at a glance.
    // A directory keeps its trailing slash so a header cannot read as a file.
    readonly property string displayName: {
        const p = pane.displayPath;
        if (p.length === 0)
            return "";
        const isDir = p.charAt(p.length - 1) === "/";
        const trimmed = isDir ? p.substring(0, p.length - 1) : p;
        const i = trimmed.lastIndexOf("/");
        const name = i >= 0 ? trimmed.substring(i + 1) : trimmed;
        if (name.length === 0)
            return p;
        return isDir ? name + "/" : name;
    }

    // The directory a RELATIVE address is resolved against, so typing a bare
    // file name in a directory listing opens that file. Empty when the pane is
    // not showing a remote path at all (an http page has no remote directory).
    readonly property string currentDirectory: {
        const p = pane.displayPath;
        if (p.length === 0 || p.charAt(0) !== "/")
            return "";
        if (p.charAt(p.length - 1) === "/")
            return p.length > 1 ? p.substring(0, p.length - 1) : "";
        const i = p.lastIndexOf("/");
        return i <= 0 ? "" : p.substring(0, i);
    }

    // A translated word for what kind of thing is open, shown beside the name.
    readonly property string kindLabel: {
        switch (pane.kind) {
        case "directory": return qsTr("directory");
        case "web": return qsTr("web page");
        case "image": return qsTr("image");
        case "pdf": return qsTr("PDF");
        case "binary": return qsTr("binary");
        case "markdown":
        case "text": return qsTr("editor");
        default: return "";
        }
    }

    // ---- address entry -----------------------------------------------------

    // A path typed WITHOUT a trailing slash is ambiguous: only the server knows
    // whether /srv/repos/app is a directory or a file, and guessing wrong shows
    // a directory as an undownloadable "binary file". So the path is offered to
    // file.listDirectory first, and the answer decides: a listing means it was a
    // directory (re-opened with the trailing slash the handler registry needs),
    // an error means it was not and it is opened as a file — whose own view then
    // reports the real failure if there is one. This holds the path being asked
    // about; empty when no probe is outstanding.
    property string probePath: ""

    // Resolve whatever is in the address field and open it.
    function submitAddress() {
        const text = addressField.text.trim();
        if (text.length === 0) {
            // Emptying the address clears the pane, which is how a user gets
            // back to the session-root default.
            pane.probePath = "";
            pane.url = "";
            return;
        }
        // An explicit scheme is an address in its own right (https://…, and a
        // file:// URL a user pasted out of this very field).
        if (/^[a-zA-Z][a-zA-Z0-9+.\-]*:\/\//.test(text)) {
            pane.probePath = "";
            pane.url = text;
            return;
        }
        const path = text.charAt(0) === "/"
                     ? text
                     : pane.currentDirectory + "/" + text;
        if (path.charAt(path.length - 1) === "/" || !pane.viewerModel) {
            // Already spelled as a directory, or there is nobody to ask;
            // nothing to probe.
            pane.probePath = "";
            pane.openRemotePath(path);
            return;
        }
        pane.probePath = path;
        pane.viewerModel.listDirectory(path);
    }

    Connections {
        target: pane.viewerModel
        // Only ever the pane's OWN outstanding probe: `viewers` is shared by
        // every pane, and the directory views listen on this same pair.
        function onDirectoryListed(path, list) {
            if (path !== pane.probePath)
                return;
            pane.probePath = "";
            pane.openRemotePath(path + "/");
        }
        function onDirectoryError(path, message) {
            if (path !== pane.probePath)
                return;
            pane.probePath = "";
            pane.openRemotePath(path);
        }
    }

    // Put the cursor in the address bar. Reached from the header title, so
    // clicking the file name is a way in as well as the field itself.
    function focusAddress() {
        addressField.forceActiveFocus();
        addressField.selectAll();
    }

    // The field shows the pane's address, but it is EDITABLE, so a plain
    // binding cannot be used: the first keystroke would break it and the field
    // would then never follow a navigation again. It is pushed instead, and
    // never on top of what the user is in the middle of typing.
    onDisplayPathChanged: if (!addressField.activeFocus)
                              addressField.text = pane.displayPath

    Component.onCompleted: addressField.text = pane.displayPath

    // ---- chrome ------------------------------------------------------------

    AppPaneHeader {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top

        title: pane.displayName.length > 0 ? pane.displayName : qsTr("Empty pane")
        subtitle: pane.showingDefault ? qsTr("session root") : pane.kindLabel
        active: pane.paneActive
        // A listing is in flight, or a path is being probed. Only the directory
        // view publishes `loading`, so it is the only one asked — the others
        // (a web page, an image, the Monaco editor) report their own progress
        // inside themselves.
        busy: pane.probePath.length > 0
              || (pane.kind === "directory" && contentLoader.item
                  && contentLoader.item.loading === true)

        onTitleActivated: pane.focusAddress()

        actions: [
            AppPaneHeader.Action {
                text: qsTr("Close this pane")
                glyph: "\u00d7"
                onClicked: {
                    // Report focus first: the host closes the pane the user is
                    // in, so this pane has to BE that pane by the time the
                    // request arrives.
                    pane.paneActivated(pane.paneId);
                    pane.closeRequested(pane.paneId);
                }
            }
        ]
    }

    // The address bar. Deliberately always visible rather than hidden behind a
    // click on the title: it is the ONE affordance that makes a viewer pane
    // usable, and an affordance the user has to discover is what the command
    // palette already was.
    Rectangle {
        id: addressBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        height: Theme.paneHeaderHeight
        color: Theme.surfaceDeep

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.borderSubtle
        }

        TextField {
            id: addressField
            objectName: "viewerAddressField"

            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.right: goButton.left
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            height: parent.height - 6

            placeholderText: qsTr("Remote path (/srv/repos/app/README.md) or https:// address")
            placeholderTextColor: Theme.textDim
            color: Theme.text
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeBody
            selectByMouse: true
            leftPadding: 8
            rightPadding: 8
            topPadding: 0
            bottomPadding: 0

            background: Rectangle {
                color: Theme.surfaceSunken
                radius: Theme.radiusSmall
                border.width: addressField.activeFocus ? 2 : 1
                border.color: addressField.activeFocus ? Theme.accent : Theme.borderSubtle
            }

            onAccepted: pane.submitAddress()

            // Escape abandons the edit and puts the pane's real address back,
            // so a half-typed path cannot be left sitting in the field looking
            // like what the pane is showing.
            Keys.onEscapePressed: function (event) {
                addressField.text = pane.displayPath;
                addressField.deselect();
                pane.forceActiveFocus();
                event.accepted = true;
            }
        }

        AppPaneHeader.Action {
            id: goButton
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Open this address")
            glyph: "\u21b5"
            onClicked: pane.submitAddress()
        }
    }

    Loader {
        id: contentLoader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: addressBar.bottom
        anchors.bottom: parent.bottom
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

    // A CLICK is the only reliable evidence that the user is working here. Most
    // of the sub-views above are WebEngineViews, and focus inside a page is
    // Chromium's own state: it never surfaces as QML activeFocus, so watching
    // activeFocus would see nothing for exactly the panes that matter most (an
    // editor buffer being typed into). A click, by contrast, is what PUT the
    // focus in that page, and it is delivered as a real Qt press first.
    //
    // The press is observed and then DECLINED, so it goes on to whatever is
    // underneath — the web view, a header button, the address field, a text view
    // — and this stays a sniffer rather than an input-eating overlay. Declared
    // after the chrome and the content because delivery is topmost-first: a
    // sniffer underneath them would never be reached.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: function(mouse) {
            pane.paneActivated(pane.paneId);
            mouse.accepted = false;
        }
    }

    Component { id: webComponent; ViewerWebView { url: pane.effectiveUrl } }
    Component { id: textComponent; ViewerTextView { url: pane.effectiveUrl } }
    Component { id: imageComponent; ViewerImageView { url: pane.effectiveUrl } }
    Component { id: pdfComponent; ViewerPdfView { url: pane.effectiveUrl } }
    Component {
        id: directoryComponent
        ViewerDirectoryView {
            url: pane.effectiveUrl
            // Clicking a row navigates THIS pane: into a sub-directory, up to a
            // parent, or into a file's own viewer.
            onOpenRequested: (path) => pane.openRemotePath(path)
        }
    }
    Component { id: binaryComponent; ViewerBinaryView { url: pane.effectiveUrl } }
    Component { id: editorComponent; EditorPaneView { fileUrl: pane.effectiveUrl; recoveryPaneId: pane.paneId } }

    // Nothing open AND no Dev Session to fall back on. A pane a user can land on
    // must say what it is and how to fill it — the internal pane id is plumbing,
    // and printing it as the headline (which this used to do) tells nobody
    // anything.
    Component {
        id: emptyComponent
        Rectangle {
            color: Theme.surfaceDeep

            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 340)
                spacing: 8

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "\u25a7"
                    color: Theme.textFaint
                    font.pixelSize: 30
                }
                Label {
                    objectName: "emptyTitle"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Nothing open in this pane")
                    color: Theme.text
                    font.pixelSize: Theme.fontSizeTitle
                }
                Label {
                    objectName: "emptyHint"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    // What actually works, which the old text did not describe:
                    // it pointed at a file browser in the sidebar and at palette
                    // commands, and neither exists.
                    text: qsTr("Type a remote path or a https:// address in the bar above and "
                               + "press Enter. Open a Dev Session and this pane starts at its "
                               + "repository root instead.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
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
                    color: Theme.textFaint
                    font.pixelSize: Theme.fontSizeSmall
                    font.family: Theme.monoFamily
                }
            }
        }
    }
}
