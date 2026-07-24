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
            enabled: root.url.toString().length > 0
            onClicked: downloader.url = viewers.internalUrlFor(root.url)
        }
    }

    // Hidden view that drives the download through the internal scheme handler.
    WebEngineView {
        id: downloader
        visible: false
        width: 0
        height: 0
        profile: viewers.internalProfile()
    }

    Connections {
        target: viewers.internalProfile()
        function onDownloadRequested(download) {
            download.accept();
        }
    }
}
