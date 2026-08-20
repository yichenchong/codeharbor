import QtQuick
import QtQuick.Controls

// Native remote image viewer (mobile SPEC 7.5).
//
// The image NEVER comes from a URL the image pipeline could fetch itself. It
// comes from viewerService.imageUrl(path), an image://chremote/... address that
// ch::MobileImageProvider can answer only out of bytes ch::MobileViewerService
// already pulled over the SSH RPC channel. So: no file:// path handed to a
// renderer that would read the PHONE's filesystem, no http fetch, and no way for
// a remote name to become a local read.
//
// The handshake is deliberate and is documented in MobileImageProvider.h:
// requestImage() first, source only after imageReady. Setting `source` before the
// bytes are cached would make the provider answer a miss on its worker thread,
// which reports an error rather than waiting — the provider must never be the
// thread waiting on the network.
Item {
    id: root

    property string remotePath: ""
    property url paneUrl
    property string repoRoot: ""
    property string paneId: ""
    signal openRequested(string path)
    signal titleRequested(string title)

    readonly property var service: (typeof viewerService !== "undefined") ? viewerService : null

    property bool loading: false
    property string errorText: ""
    property url imageSource: ""
    property string requestedPath: ""
    // The path whose bytes the service is currently holding for this page. Kept
    // separately from `remotePath` because it is the only thing that can be
    // handed back: by the time onRemotePathChanged runs, `remotePath` is already
    // the NEXT file.
    property string cachedPath: ""

    // Hand back whatever bytes this page still owns. Not required for
    // correctness — the cache is LRU-bounded — but an image can be 8 MiB and a
    // phone has better uses for that.
    function releaseBytes() {
        if (!root.service)
            return;
        // Both the image on screen and a read still in flight: requestImage()
        // caches the bytes before it answers, so a request this page abandons
        // can still have filled the cache.
        const paths = [];
        if (root.cachedPath.length > 0)
            paths.push(root.cachedPath);
        if (root.requestedPath.length > 0 && root.requestedPath !== root.cachedPath)
            paths.push(root.requestedPath);
        root.cachedPath = "";
        root.requestedPath = "";
        for (let i = 0; i < paths.length; ++i)
            root.service.forgetImage(paths[i]);
    }

    function reload() {
        root.releaseBytes();
        root.errorText = "";
        root.imageSource = "";
        root.loading = false;
        flick.resetZoom();
        if (root.remotePath.length === 0 || !root.service)
            return;
        root.loading = true;
        root.requestedPath = root.remotePath;
        root.service.requestImage(root.remotePath);
    }

    // ONE request for the first target, not two: a page is built by assigning its
    // initial properties and only then running Component.onCompleted, so the
    // host's `remotePath` fires the change handler BEFORE completion and an
    // unconditional completion handler asked the server for the same thing twice
    // on every pane open. `started` keeps the change handler inert until the
    // object is fully built, so exactly one path issues the request.
    property bool started: false
    onRemotePathChanged: if (root.started) root.reload()
    Component.onCompleted: {
        root.started = true;
        root.reload();
    }

    Component.onDestruction: root.releaseBytes()

    Connections {
        target: root.service
        function onImageReady(path, url) {
            if (path !== root.requestedPath)
                return;
            root.requestedPath = "";
            // The service now holds these bytes on this page's behalf; remember
            // which path they belong to so releaseBytes() can give them back.
            root.cachedPath = path;
            root.loading = false;
            root.imageSource = url;
        }
        function onImageError(path, message) {
            if (path !== root.requestedPath)
                return;
            root.requestedPath = "";
            root.loading = false;
            root.errorText = message;
        }
    }

    Rectangle {
        anchors.fill: parent
        color: MobileTheme.surfaceDeep
    }

    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        contentWidth: Math.max(width, image.width * image.scale)
        contentHeight: Math.max(height, image.height * image.scale)
        boundsBehavior: Flickable.StopAtBounds

        function resetZoom() {
            image.scale = 1;
            flick.contentX = 0;
            flick.contentY = 0;
        }

        PinchArea {
            width: Math.max(flick.contentWidth, flick.width)
            height: Math.max(flick.contentHeight, flick.height)
            pinch.target: image
            pinch.minimumScale: 1
            pinch.maximumScale: 8
            // Zoom is a SCALE on the item, not a re-request at a new size: a
            // re-request would go back through the provider on every pinch
            // frame, decoding a multi-megabyte buffer per frame.
            onPinchFinished: flick.returnToBounds()

            Image {
                id: image
                anchors.centerIn: parent
                width: Math.min(flick.width, implicitWidth > 0 ? implicitWidth : flick.width)
                height: implicitWidth > 0
                        ? width * (implicitHeight / implicitWidth)
                        : flick.height
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                // Qt's own pixmap cache is bypassed so that re-setting the same
                // provider address after a re-read really re-requests the bytes.
                cache: false
                smooth: true
                source: root.imageSource
                transformOrigin: Item.Center
            }
        }

        // Double tap toggles between fit and 3x, which is what every photo
        // viewer on both platforms does.
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onDoubleTapped: {
                image.scale = image.scale > 1.01 ? 1 : 3;
                flick.returnToBounds();
            }
        }
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - 2 * MobileTheme.spacingLarge
        visible: root.errorText.length > 0
                 || (!root.loading && image.status === Image.Error)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: MobileTheme.textDim
        font.pixelSize: MobileTheme.fontSizeBody
        text: root.errorText.length > 0
              ? root.errorText
              : qsTr("This image could not be decoded.")
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: root.loading || image.status === Image.Loading
        visible: running
    }
}
