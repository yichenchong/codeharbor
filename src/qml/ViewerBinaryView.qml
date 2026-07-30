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
    property string pendingDownloadUrl: ""

    color: "#1e1e2e"

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
            color: "#cdd6f4"
            font.pixelSize: 15
            font.bold: true
            elide: Text.ElideMiddle
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
        }
        Label {
            text: qsTr("Binary file — preview not available")
            color: "#6c7086"
            font.pixelSize: 12
            Layout.alignment: Qt.AlignHCenter
        }
        Label {
            // Same rule: the remote path is data.
            textFormat: Text.PlainText
            text: root.url
            color: "#585b70"
            font.pixelSize: 11
            elide: Text.ElideMiddle
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
        }
        Button {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Download")
            // Disabled while a download this pane started is still pending, so a
            // single click cannot fan out into multiple accepted downloads.
            enabled: root.url.toString().length > 0 && !root.downloadRequested
            onClicked: {
                root.pendingDownloadUrl = viewers.internalUrlFor(root.url);
                root.downloadRequested = true;
                downloader.url = root.pendingDownloadUrl;
            }
        }
    }

    // Hidden view that drives the download through the internal scheme handler.
    WebEngineView {
        id: downloader
        visible: false
        width: 0
        height: 0
        profile: viewers.internalProfile()
        // Untrusted bytes: never let this hidden view execute scripts.
        settings.javascriptEnabled: false
        settings.localContentCanAccessFileUrls: false
        settings.localContentCanAccessRemoteUrls: false
    }

    Connections {
        target: viewers.internalProfile()
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
            root.downloadRequested = false;
            root.pendingDownloadUrl = "";
            download.accept();
        }
    }
}
