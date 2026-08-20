import QtQuick
import QtQuick.Controls
import CodeHarbor.Mobile

// The one pane. Step three of the walk, and the only page in this module that
// instantiates anything expensive.
//
// THE SINGLE-LIVE-PANE INVARIANT lives here, in its QML half (the C++ half is
// ch::MobileAppController::setNavStage, which releases the selection on every
// exit from the Pane stage; see the comment on that class). This page holds
// exactly ONE Loader, it is the only thing in the whole module that loads a pane
// component, and it is driven through Loader.setSource() rather than a `source`
// binding. That is not a style preference:
//
//   * setSource(url, props) destroys the current item and constructs the next one
//     with its properties already set. A `source` binding plus an onLoaded
//     assignment builds the item first and configures it afterwards, so a
//     terminal page would run one frame with no devSessionId and could attach to
//     nothing — or, worse, to a stale binding's value.
//   * Passing "" first is an explicit UNLOAD, and it is what keeps the previous
//     pane from being ATTACHED alongside the new one. Be precise about the
//     timing, because the obvious reading is wrong: QQuickLoader's unload
//     unparents the old item, hides it and then deleteLater()s it, so the object
//     itself outlives setSource() by one turn of the event loop. What is true at
//     every instant is that at most one pane is in the scene and reachable; the
//     released one is detached, drawing nothing and receiving nothing, and its
//     PTY channel or editor buffer goes with it on that next turn. Setting a new
//     source WITHOUT the unload would instead build the second pane while the
//     first is still parented and live, which is two attached panes for the
//     duration of the construction — that is the case this ordering rules out.
//
// The mapping from a pane's KIND to its page is fixed and total: every kind lands
// on exactly one page, and anything unrecognised lands on ViewerUnsupportedPage
// rather than on nothing. Two kinds depend on an optional Qt module and fall back
// to that same page when the build has not got it, because a QML `import
// QtQuick.Pdf` against a kit without Qt Pdf is a load error that shows the user a
// blank pane and puts the reason in a log they will never read.
Page {
    id: page

    readonly property var ctl: (typeof mobile !== "undefined") ? mobile : null
    readonly property var host: (typeof app !== "undefined") ? app : null
    readonly property var layoutStore: (typeof layouts !== "undefined") ? layouts : null

    readonly property var pane: page.ctl ? page.ctl.selectedPane : ({})
    readonly property string paneKind: pane && pane.kind ? String(pane.kind) : ""
    readonly property string paneRegion: pane && pane.region ? String(pane.region) : ""
    readonly property string paneId: pane && pane.paneId ? String(pane.paneId) : ""
    readonly property string repoRoot: page.host ? page.host.activeSessionRepoRoot : ""
    readonly property string devSessionId: page.host ? page.host.activeSessionId : ""

    // What the pane currently has open. Seeded from the layout leaf's stored url
    // and then reassigned in place by an in-pane navigation (a directory listing
    // the user taps down through), which is why it is a plain property rather
    // than a binding onto the model: rebinding it from the leaf on every
    // republish would yank the user back to where they started.
    property url currentUrl: ""

    // What the LOADED page has asked the header to say, if anything — an editor
    // naming the file it actually opened, a directory browser naming where it
    // navigated to. Held here, as a property the header binds to, rather than
    // assigned onto the header's `text`: a plain assignment would destroy that
    // binding permanently, and every pane opened afterwards would keep showing
    // the title of the first page that ever reported one. Cleared by reload().
    property string titleOverride: ""

    background: Rectangle { color: MobileTheme.surface }

    // A remote absolute path out of a remote file:// url. Deliberately narrow:
    // only file:// is unwrapped, so an http(s) or unknown-scheme url yields an
    // empty path and the page that receives it has nothing to open on the local
    // filesystem. Nothing in this module ever hands a url to QDesktopServices or
    // to an OS handler (SPEC 7.4).
    function remotePathOf(u) {
        const text = String(u ? u : "");
        if (text.length === 0)
            return page.repoRoot;
        if (text.indexOf("file://") !== 0)
            return "";
        const encoded = text.substring(7);
        // decodeURIComponent THROWS on a stray '%' ("/srv/repo/100%.txt" is a
        // perfectly legal remote file name, and the layout stores the url the
        // server gave us verbatim). An exception here would escape reload() and
        // leave the Loader unloaded — a blank pane with no message, for a file
        // whose only sin is its name. Undecodable text is passed through as it
        // stands, which is what it already was.
        try {
            return decodeURIComponent(encoded);
        } catch (e) {
            return encoded;
        }
    }

    // The fixed kind -> page table. The vocabulary is closed and comes from ONE
    // place, ch::PaneListModel: "terminal" for a terminal-region leaf,
    // "directory" for a viewer leaf with no url, "unsupported" for a url no
    // handler claims, and otherwise the first word of
    // ch::ViewerHandlerRegistry::applicableViewKinds() — "markdown", "text",
    // "image", "pdf", "web" or "binary". Every one of those has an entry below,
    // and the default catches the rest so the table stays total.
    //
    // "editor" is deliberately NOT a case. It is not in ch::ViewerKinds::all()
    // and the registry stopped offering it (see the note on applicableViewKinds:
    // it was a second spelling of "text" that showed the user a choice they
    // could never save), so a branch for it here would be a dead entry keeping a
    // retired word alive in the mobile vocabulary.
    function componentFor(kind) {
        const hasPdf = page.ctl ? page.ctl.capabilities.hasPdf : false;
        const hasWebView = page.ctl ? page.ctl.capabilities.hasWebView : false;
        switch (kind) {
        case "terminal":  return "TerminalPage.qml";
        case "markdown":  return "ViewerMarkdownPage.qml";
        case "text":      return "ViewerEditorPage.qml";
        case "image":     return "ViewerImagePage.qml";
        case "pdf":       return hasPdf ? "ViewerPdfPage.qml"
                                        : "ViewerUnsupportedPage.qml";
        case "directory": return "ViewerDirectoryPage.qml";
        case "web":       return hasWebView ? "ViewerWebPage.qml"
                                            : "ViewerUnsupportedPage.qml";
        default:          return "ViewerUnsupportedPage.qml";
        }
    }

    // The initial property set for the page about to be built. A terminal pane is
    // addressed by its server-minted terminal_panes row id (SPEC 5.2); a viewer
    // pane by what it has open. Both get the repository root and the layout
    // paneId, because both need somewhere to resolve a relative path from and a
    // key to record per-pane state under.
    //
    // `file` is passed in rather than recomputed, because one property belongs to
    // the PAGE and not to the kind: ViewerUnsupportedPage explains WHY it cannot
    // show the resource, and it can only do that if it is told which kind it was
    // handed. It is set for that page alone — Loader.setSource() refuses a
    // property the component does not declare, and the other viewer pages do not
    // declare `kind`.
    function propertiesFor(kind, file) {
        if (kind === "terminal") {
            return {
                "devSessionId": page.devSessionId,
                "paneId": page.paneId,
                "terminalPaneId": page.pane && page.pane.terminalPaneId
                                  ? String(page.pane.terminalPaneId) : "",
                "repoRoot": page.repoRoot,
                // "paneTitle", not "title": Page.title is FINAL, so the page
                // carries its label under its own name.
                "paneTitle": page.pane && page.pane.title
                             ? String(page.pane.title) : ""
            };
        }
        const properties = {
            "remotePath": page.remotePathOf(page.currentUrl),
            "paneUrl": page.currentUrl,
            "repoRoot": page.repoRoot,
            "paneId": page.paneId
        };
        if (file === "ViewerUnsupportedPage.qml")
            properties["kind"] = kind;
        return properties;
    }

    function reload() {
        // The explicit unload. See the class comment: this is what bounds the
        // number of live panes to one at every instant.
        paneLoader.setSource("");
        // The header label belongs to the pane that is going away, so it goes
        // with it. Without this a page that reports no title of its own would
        // inherit the last one that did.
        page.titleOverride = "";
        if (!page.ctl || page.paneKind.length === 0)
            return;
        const file = page.componentFor(page.paneKind);
        paneLoader.setSource(file, page.propertiesFor(page.paneKind, file));
    }

    // An in-pane navigation: the directory browser moved to another path. The
    // SAME item is reconfigured rather than reloaded, so the page keeps its scroll
    // position and, for a directory browser, its own history.
    function navigateTo(path) {
        if (!paneLoader.item || String(path).length === 0)
            return;
        const target = "file://" + path;
        page.currentUrl = target;
        if (paneLoader.item.hasOwnProperty("remotePath"))
            paneLoader.item.remotePath = path;
        if (paneLoader.item.hasOwnProperty("paneUrl"))
            paneLoader.item.paneUrl = target;

        // Record what the pane has open so reopening this Dev Session restores
        // the pane's CONTENT and not just its place in the list. Through the
        // STAMPED entry point, which is the rule for every delayed gesture: the
        // pane that started this may be gone by the time the write lands, and the
        // stamp is what proves which session and generation it belonged to.
        if (page.layoutStore && page.devSessionId.length > 0
                && page.paneRegion.length > 0 && page.paneId.length > 0) {
            page.layoutStore.setPaneUrlForSession(page.devSessionId,
                                                  page.layoutStore.generation,
                                                  page.paneRegion, page.paneId,
                                                  target);
        }
    }

    header: Rectangle {
        implicitHeight: MobileTheme.touchTarget
        color: MobileTheme.surfaceRaised

        Row {
            anchors.fill: parent
            spacing: 0

            AbstractButton {
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: MobileTheme.touchTarget
                implicitHeight: MobileTheme.touchTarget
                enabled: page.ctl !== null
                onClicked: page.ctl.back()

                contentItem: Text {
                    text: "\u2039"
                    textFormat: Text.PlainText
                    color: MobileTheme.accent
                    font.pixelSize: MobileTheme.fontSizeTitle
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Text {
                id: headerTitle
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - MobileTheme.touchTarget
                       - MobileTheme.spacingLarge
                // The pane's own title, replaced by whatever the loaded page
                // reports through titleRequested (an editor showing the file it
                // actually opened, a directory browser showing where it has
                // navigated to). Server-controlled in every case. SPEC 7.5.
                text: page.titleOverride.length > 0
                      ? page.titleOverride
                      : (page.pane && page.pane.title ? String(page.pane.title) : "")
                textFormat: Text.PlainText
                color: MobileTheme.text
                font.pixelSize: MobileTheme.fontSizeLabel
                elide: Text.ElideMiddle
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: MobileTheme.border
        }
    }

    Loader {
        id: paneLoader
        anchors.fill: parent
        // Asynchronous loading is deliberately OFF. The pane is the entire
        // screen and there is nothing to show behind it, so an async load buys a
        // blank page instead of a brief stall — and it would also loosen the
        // unload-then-load ordering above: an incubating pane is built over
        // several frames, so a second selection arriving mid-build would have
        // two panes under construction rather than one finished and one
        // starting.
        asynchronous: false

        onLoaded: {
            if (!item)
                return;
            // Both signals are optional: a page that has nothing to navigate to
            // and no title to report simply does not declare them.
            if (item.openRequested)
                item.openRequested.connect(page.navigateTo);
            if (item.titleRequested)
                item.titleRequested.connect(function(title) {
                    page.titleOverride = String(title);
                });
        }
    }

    // A pane whose kind has no page at all cannot happen (componentFor is total),
    // but a pane whose COMPONENT failed to load can — a missing optional module, a
    // QML error in a page. Saying so beats an empty screen.
    Text {
        anchors.centerIn: parent
        width: parent.width - 2 * MobileTheme.spacingLarge
        visible: paneLoader.status === Loader.Error
        text: qsTr("This pane could not be opened on this device.")
        textFormat: Text.PlainText
        color: MobileTheme.textDim
        font.pixelSize: MobileTheme.fontSizeBody
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }

    // Seeding and reloading. `currentUrl` is taken from the leaf ONCE per
    // selection, which is what lets an in-pane navigation move it afterwards
    // without a binding pulling it back.
    Component.onCompleted: {
        page.currentUrl = page.pane && page.pane.url ? String(page.pane.url) : "";
        page.reload();
    }

    Connections {
        target: page.ctl
        function onSelectedPaneChanged() {
            // An empty selection is the controller releasing the pane on its way
            // out of this stage. Unload and stop: the page itself is about to be
            // popped, and building a pane for a selection that no longer exists
            // is precisely the second live pane this design forbids.
            if (!page.ctl || !page.ctl.selectedPaneKey
                    || page.ctl.selectedPaneKey.length === 0) {
                paneLoader.setSource("");
                return;
            }
            page.currentUrl = page.pane && page.pane.url
                              ? String(page.pane.url) : "";
            page.reload();
        }
    }
}
