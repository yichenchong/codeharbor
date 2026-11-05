import QtQuick
import QtQuick.Controls
import QtQuick.Pdf
import QtQuick.Shapes

// Native PDF viewer (mobile SPEC 7.5).
//
// GATED TWICE, and both gates are load-bearing:
//   * at CMake level by CH_HAVE_QTPDF — this file is only added to the QML module
//     when Qt6::Pdf/PdfQuick and the QtQuick.Shapes plugin were found; otherwise
//     the imports above would fail the whole module;
//   * in QML by mobile.capabilities.hasPdf — PanePickerPage/PaneHostPage route a
//     "pdf" leaf to ViewerUnsupportedPage when the capability is absent, so this
//     page is never the thing a user is looking at on a build without QtPdf.
//
// BYTES. QtQuick.Pdf's PdfDocument takes a URL and nothing else — there is no
// QML-reachable in-memory load and no image-provider equivalent for PDF pages —
// so the document is spooled to app-private storage by
// ch::MobileViewerService::requestPdf() and the URL of THAT file is what
// PdfDocument sees. The file is deleted on unload (releasePdf below), on service
// destruction, and at app start; and it is never handed to an OS handler. The
// in-memory route was preferred and is not available; see requestPdf()'s note.
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

    property bool loading: false
    property string errorText: ""
    property url documentSource: ""
    property string requestedPath: ""
    // The path whose spooled file is on disk and open in the PdfDocument below.
    // Tracked separately from `remotePath`, which is already the NEXT file by
    // the time onRemotePathChanged runs, and from `requestedPath`, which is
    // cleared the moment a reply lands.
    property string spooledPath: ""

    // Delete every spool file this page still owns: the document on screen, and
    // any request whose reply this page will now ignore. requestPdf() writes the
    // file BEFORE it answers, so an abandoned request can leave one behind too —
    // that file would then survive until the service is destroyed.
    function releaseSpool() {
        if (!root.service)
            return;
        const paths = [];
        if (root.spooledPath.length > 0)
            paths.push(root.spooledPath);
        if (root.requestedPath.length > 0 && root.requestedPath !== root.spooledPath)
            paths.push(root.requestedPath);
        root.spooledPath = "";
        root.requestedPath = "";
        for (let i = 0; i < paths.length; ++i)
            root.service.releasePdf(paths[i]);
    }

    function reload() {
        // Close the document BEFORE the file under it is deleted: the
        // PdfDocument keeps the spool file open, and clearing `source` first
        // means the engine is never asked to page in from a file that is gone.
        root.errorText = "";
        root.documentSource = "";
        root.releaseSpool();
        view.zoom = 1;
        view.renderedZoom = 1;
        view.contentX = 0;
        view.contentY = 0;
        root.loading = false;
        if (root.remotePath.length === 0 || !root.service)
            return;
        if (root.capabilities && !root.capabilities.hasPdf) {
            // Belt and braces: the host routes around this page, and this page
            // refuses to pretend it can work if it is ever reached anyway.
            root.errorText = qsTr("This build of CodeHarbor has no PDF engine.");
            return;
        }
        root.loading = true;
        root.requestedPath = root.remotePath;
        root.service.requestPdf(root.remotePath);
    }

    // Release the PREVIOUS document before asking for the next one, so a pane
    // stepping through a directory of PDFs holds exactly one spooled file.
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
    Component.onDestruction: root.releaseSpool()

    Connections {
        target: root.service
        function onPdfReady(path, fileUrl) {
            if (path !== root.requestedPath)
                return;
            // The spool file for this path now exists and is this page's to
            // delete; the request itself is finished.
            root.requestedPath = "";
            root.spooledPath = path;
            root.loading = false;
            root.documentSource = fileUrl;
        }
        function onPdfError(path, message) {
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

    PdfDocument {
        id: pdfDocument
        source: root.documentSource
        onStatusChanged: function(status) {
            // Only while a document is actually loaded: clearing `source` on the
            // way to the next file also drives a status change, and reporting
            // THAT as a failure would print "could not be opened" over the
            // spinner of the file that is on its way.
            if (status === PdfDocument.Error
                    && root.documentSource.toString().length > 0)
                // Our own wording, not the engine's: QPdfDocument reports an
                // enum, and a number on screen explains nothing.
                root.errorText = qsTr("This document could not be opened. It may be encrypted or damaged.");
        }
    }

    PdfStyle { id: pdfStyle }

    // PdfMultiPageView unconditionally calls Qt.openUrlExternally for URI
    // annotations and exposes no link-policy hook. Compose its native building
    // blocks instead: page rasters, selection, and document-internal links only.
    ListView {
        id: view
        objectName: "pdfView"
        anchors.fill: parent
        anchors.bottomMargin: pageBar.height
        clip: true
        visible: root.errorText.length === 0
                 && root.documentSource.toString().length > 0
        model: pdfDocument.status === PdfDocument.Ready ? pdfDocument.pageCount : 0
        spacing: MobileTheme.spacingSmall
        contentWidth: width * zoom
        flickableDirection: Flickable.HorizontalAndVerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        property real zoom: 1
        // Keep the current raster during a pinch; decode at the new resolution
        // only after release rather than allocate a PDF raster on every frame.
        property real renderedZoom: 1
        readonly property int currentPage: count > 0
            ? Math.max(0, indexAt(contentX + width / 2, contentY + height / 2)) : -1
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar {}

        delegate: Item {
            id: pdfPage
            required property int index
            readonly property size pointSize: pdfDocument.pagePointSize(index)
            readonly property real pageScale: width > 0 && pointSize.width > 0
                                              ? width / pointSize.width : 1
            width: view.contentWidth
            height: pointSize.height * pageScale

            PdfPageImage {
                id: pageImage
                objectName: "pdfPageImage"
                anchors.fill: parent
                document: pdfDocument
                currentFrame: pdfPage.index
                asynchronous: true
                sourceSize.width: Math.ceil(view.width * view.renderedZoom * Screen.devicePixelRatio)
                fillMode: Image.PreserveAspectFit
            }
            Shape {
                anchors.fill: parent
                visible: pageImage.status === Image.Ready
                ShapePath {
                    strokeWidth: -1
                    fillColor: pdfStyle.selectionColor
                    scale: Qt.size(pdfPage.pageScale, pdfPage.pageScale)
                    PathMultiline {
                        paths: selection.geometry
                    }
                }
            }

            DragHandler {
                id: selectionDrag
                target: null
                acceptedDevices: PointerDevice.Mouse | PointerDevice.Stylus
            }
            TapHandler {
                id: selectionTap
                acceptedDevices: PointerDevice.Mouse | PointerDevice.Stylus
            }
            TapHandler {
                acceptedDevices: PointerDevice.TouchScreen
                onTapped: {
                    selection.clear();
                    selection.forceActiveFocus();
                }
            }
            PdfSelection {
                id: selection
                objectName: "pdfTextSelection"
                anchors.fill: parent
                document: pdfDocument
                page: pdfPage.index
                renderScale: pdfPage.pageScale
                from: selectionDrag.centroid.pressPosition
                to: selectionDrag.centroid.position
                hold: !selectionDrag.active && !selectionTap.pressed
                focus: true
            }
            Repeater {
                model: PdfLinkModel {
                    document: pdfDocument
                    page: pdfPage.index
                }
                delegate: Item {
                    required property rect rectangle
                    required property int page
                    required property point location
                    x: rectangle.x * parent.pageScale
                    y: rectangle.y * parent.pageScale
                    width: rectangle.width * parent.pageScale
                    height: rectangle.height * parent.pageScale
                    TapHandler {
                        // URI annotations have page == -1. They remain inert;
                        // no URL is ever handed to the OS or another renderer.
                        enabled: parent.page >= 0
                        onTapped: {
                            view.positionViewAtIndex(parent.page, ListView.Beginning);
                            const targetScale = view.contentWidth
                                / Math.max(1, pdfDocument.pagePointSize(parent.page).width);
                            view.contentX = parent.location.x * targetScale;
                            view.contentY += parent.location.y * targetScale;
                            view.returnToBounds();
                        }
                    }
                }
            }
        }

        PinchHandler {
            target: null
            property real initialZoom: 1
            onActiveChanged: {
                if (active)
                    initialZoom = view.zoom;
                else {
                    view.renderedZoom = view.zoom;
                    view.returnToBounds();
                }
            }
            onActiveScaleChanged: {
                const x = (view.contentX + centroid.position.x) / view.zoom;
                const y = (view.contentY - view.originY + centroid.position.y) / view.zoom;
                view.zoom = Math.max(1, Math.min(8, initialZoom * activeScale));
                view.forceLayout();
                view.contentX = x * view.zoom - centroid.position.x;
                view.contentY = view.originY + y * view.zoom - centroid.position.y;
            }
        }
    }

    // Page position and nothing else: a single-pane mobile surface has no room
    // for a toolbar, and search/print/annotation are not viewer features here.
    Rectangle {
        id: pageBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: MobileTheme.touchTarget
        color: MobileTheme.surfaceRaised
        visible: view.visible

        Text {
            anchors.centerIn: parent
            textFormat: Text.PlainText
            color: MobileTheme.textDim
            font.pixelSize: MobileTheme.fontSizeSmall
            text: pdfDocument.pageCount > 0
                  ? qsTr("Page %1 of %2").arg(view.currentPage + 1).arg(pdfDocument.pageCount)
                  : ""
        }
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - 2 * MobileTheme.spacingLarge
        visible: root.errorText.length > 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        // A server-supplied message. PlainText, like every other.
        textFormat: Text.PlainText
        color: MobileTheme.textDim
        font.pixelSize: MobileTheme.fontSizeBody
        text: root.errorText
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: root.loading
        visible: root.loading
    }
}
