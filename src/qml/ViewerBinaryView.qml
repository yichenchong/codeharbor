import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtWebEngine

// Binary / unpreviewable content view (SPEC 7.5). Shows file metadata and a
// Download affordance: the file is streamed through the internal scheme (served
// as an octet stream), which Chromium turns into a download the internal
// profile accepts to its download directory.
Rectangle {
    id: root
    property url url

    // The `viewers` context property (ch::ViewerModel), resolved once and
    // guarded exactly as ViewerPane and EditorPaneView guard it, and for the
    // same reason: an unguarded lookup of a context property the host did not
    // install THROWS a ReferenceError, and that aborts the whole binding pass
    // building this view rather than producing an inert one.
    readonly property var viewerModel: (typeof viewers !== "undefined") ? viewers : null

    // True only between an explicit Download click and the profile handing back
    // its download request. The internal profile is SHARED across panes, so the
    // handler must accept ONLY downloads this pane deliberately started.
    property bool downloadRequested: false
    // The internal URL that click navigated the hidden view to, remembered so
    // the handler below can recognise the download WITHOUT minting the URL a
    // second time. ViewerModel's id map is LRU-bounded: with enough files open,
    // the entry for this one can be evicted between the click and the callback,
    // and a re-mint then hands back a DIFFERENT id. The comparison would fail,
    // the download would never be accepted, and because downloadRequested is
    // only cleared on acceptance the pane's Download button would stay disabled
    // for good. Re-minting also bumps the map's usage order, evicting some other
    // pane's file for no reason.
    //
    // Remembering the id is not enough on its own: eviction is what would take
    // it away, and only a RETAIN tells the table this address is still in use.
    // `retained` records whether the pin was actually taken, because the
    // contract is exactly one release per retain that RETURNED TRUE and a
    // retain of an unknown address legitimately returns false.
    property string pendingDownloadUrl: ""
    property bool retained: false

    // A refusal the embedded browser could not report — an over-cap file, say.
    // WebEngine's job interface carries only Chromium's coarse error enum, so
    // without this a download that the scheme handler declined looked exactly
    // like one that silently never started.
    property string errorText: ""

    // Give the pin back and re-enable the button. Called from every route out
    // of a pending download: acceptance, a refusal, navigating this view
    // somewhere else, and teardown. Safe when nothing is pending.
    // Deliberately does NOT navigate the hidden view: this runs from
    // Component.onDestruction too, and reaching into a child while the tree is
    // being torn down is how a teardown path acquires a null dereference. The
    // click handler resets the view instead, where it is also load-bearing.
    function clearPendingDownload() {
        if (root.retained && root.viewerModel)
            root.viewerModel.releaseInternalUrl(root.pendingDownloadUrl);
        root.retained = false;
        root.pendingDownloadUrl = "";
        root.downloadRequested = false;
    }

    // Navigating this view away abandons any download it had in flight; so does
    // closing the pane. Without either, the address stays pinned for the life
    // of the process, which is the bound the table exists to enforce.
    onUrlChanged: {
        root.errorText = "";
        root.clearPendingDownload();
    }
    Component.onDestruction: root.clearPendingDownload()

    // A failure is this view's own only if it names the address this view
    // asked for: ONE ViewerModel is shared by every pane and they all listen
    // here. Same rule ViewerPane applies to its probe replies.
    function ownsFailure(candidate) {
        return root.pendingDownloadUrl.length > 0
               && String(candidate) === root.pendingDownloadUrl;
    }

    Connections {
        target: root.viewerModel
        function onInternalResourceError(internalUrl, message) {
            if (root.ownsFailure(internalUrl)) {
                root.errorText = message;
                root.clearPendingDownload();
            }
        }
    }

    color: Theme.surface

    function baseName(u) {
        var s = u.toString();
        var i = s.lastIndexOf("/");
        return i >= 0 ? s.substring(i + 1) : s;
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 8
        width: Math.min(parent.width - 48, 480)

        Label {
            Layout.alignment: Qt.AlignHCenter
            // SECURITY: a Label defaults to Text.AutoText, which promotes
            // anything that LOOKS like markup to StyledText — and StyledText
            // fetches <img src="http://...">. This is a file NAME chosen on the
            // remote server, so it is data and is drawn as data. Same rule as
            // ViewerDirectoryView's entries and TerminalPaneView's chrome.
            textFormat: Text.PlainText
            text: root.baseName(root.url)
            color: Theme.text
            font.pixelSize: 15
            font.bold: true
            elide: Text.ElideMiddle
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
        }
        Label {
            text: qsTr("Binary file — preview not available")
            color: Theme.textDim
            font.pixelSize: Theme.fontSizeBody
            Layout.alignment: Qt.AlignHCenter
        }
        Label {
            // Same rule: the remote path is data.
            textFormat: Text.PlainText
            text: root.url
            // #585b70 is a step between Theme.textDim and Theme.textFaint and
            // has no Theme role; kept so the path stays quieter than the caption
            // above it.
            color: "#585b70"
            font.pixelSize: 11
            elide: Text.ElideMiddle
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
        }
        Button {
            id: downloadButton
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Download")
            implicitHeight: 30
            leftPadding: 14
            rightPadding: 14
            // The Basic style draws a button in the STYLE's own light palette,
            // which on this dark pane is a white slab with near-black text — the
            // borrowed-chrome look. Same treatment as TerminalPaneView's Connect
            // button.
            contentItem: Label {
                text: downloadButton.text
                color: downloadButton.enabled ? Theme.text : Theme.textFaint
                font.pixelSize: Theme.fontSizeBody
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: Theme.radiusSmall
                color: downloadButton.down ? Theme.border
                     : downloadButton.hovered ? "#3a3a52" : Theme.surfaceRaised
                border.width: downloadButton.visualFocus ? 2 : 1
                border.color: downloadButton.visualFocus ? Theme.accent : Theme.border
            }
            // Disabled while a download this pane started is still pending, so a
            // single click cannot fan out into multiple accepted downloads.
            enabled: root.url.toString().length > 0 && !root.downloadRequested
                     && root.viewerModel !== null
            onClicked: {
                root.errorText = "";
                root.pendingDownloadUrl = root.viewerModel.internalUrlFor(root.url);
                // Pin it for as long as the download is in flight: the table is
                // LRU-bounded and a mint is the only thing that refreshed this
                // address's recency, so a busy session could evict it between
                // the click and the profile's callback.
                root.retained = root.viewerModel.retainInternalUrl(root.pendingDownloadUrl);
                root.downloadRequested = true;
                // Cleared first: a retry after a refusal would otherwise assign
                // the very same address the hidden view already holds, which is
                // a no-op and starts no request at all.
                downloader.url = "";
                downloader.url = root.pendingDownloadUrl;
            }
        }

        // The refusal, said in words, rather than a Download button that quietly
        // re-enables itself and nothing else.
        Label {
            objectName: "binaryStatus"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            // Handler-supplied failure text: data, not markup.
            textFormat: Text.PlainText
            visible: text.length > 0
            text: root.errorText.length > 0 ? qsTr("Error: %1").arg(root.errorText) : ""
            color: Theme.danger
            font.pixelSize: Theme.fontSizeLabel
        }
    }

    // Hidden view that drives the download through the internal scheme handler.
    WebEngineView {
        id: downloader
        visible: false
        width: 0
        height: 0
        profile: root.viewerModel ? root.viewerModel.internalProfile() : null
        // Untrusted bytes: never let this hidden view execute scripts.
        settings.javascriptEnabled: false
        settings.localContentCanAccessFileUrls: false
        settings.localContentCanAccessRemoteUrls: false
    }

    Connections {
        target: root.viewerModel ? root.viewerModel.internalProfile() : null
        function onDownloadRequested(download) {
            // Require an explicit user action from THIS pane: the profile is
            // shared, so without this guard every pane would auto-accept every
            // download triggered anywhere (including page-initiated ones).
            if (!root.downloadRequested)
                return;
            // Scope to this pane's own request and only handle the initial
            // state, guarding against accepting the same download twice.
            if (download.state !== WebEngineDownloadRequest.DownloadRequested
                || download.url.toString() !== root.pendingDownloadUrl)
                return;
            // Accept BEFORE letting the pin go: the download reads the resource
            // through the same internal address, and unpinning it first would
            // put it back at the front of the eviction queue while it is being
            // fetched. Settling here is the one release owed for the retain the
            // click took.
            download.accept();
            root.clearPendingDownload();
        }
    }
}
