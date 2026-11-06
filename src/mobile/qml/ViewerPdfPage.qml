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
// PdfDocument sees. The pane deletes its file on unload; peer retirement and
// service destruction remove only this service's private temporary directory.
// Crash leftovers can remain in app cache until OS eviction. No OS handler sees
// the URL; the in-memory route was preferred but is not exposed by QtQuick.Pdf.
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
    onDocumentSourceChanged: {
        if (root.service && !root.service.setPdfDocumentSource(pdfDocument, root.documentSource)
                && root.documentSource.toString().length > 0)
            root.errorText = qsTr("This document could not be opened.");
    }
    property string requestedPath: ""
    // The path whose spooled file is on disk and open in the PdfDocument below.
    // Tracked separately from `remotePath`, which is already the NEXT file by
    // the time onRemotePathChanged runs, and from `requestedPath`, which is
    // cleared the moment a reply lands.
    property string spooledPath: ""

    // A compressed PDF can describe a page far larger than a phone can raster.
    // Keep each page at <= 16 Mi pixels and <= 8192 pixels on either axis,
    // independently of zoom, device pixel ratio and the page's aspect ratio.
    function boundedRasterSize(width, height) {
        if (!isFinite(width) || !isFinite(height) || width <= 0 || height <= 0)
            return Qt.size(1, 1); // Zero would ask the image loader for its default size.
        const scale = Math.min(1, 8192 / width, 8192 / height,
                               Math.sqrt((16 * 1024 * 1024) / width / height));
        return Qt.size(Math.max(1, Math.floor(width * scale)),
                       Math.max(1, Math.floor(height * scale)));
    }

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

    function deactivate() {
        root.started = false;
        // Close the decoder before releasing the file, including early Loader
        // retirement when this QObject will not be destroyed until later.
        root.documentSource = "";
        root.loading = false;
        root.releaseSpool();
    }

    function reload() {
        // The service synchronously closes the native decoder BEFORE release;
        // the QtQuick wrapper's source="" setter does not perform that close.
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
    Component.onDestruction: root.deactivate()

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
        onStatusChanged: function(status) {
            // Native close also drives a status change. Only a currently
            // requested nonempty logical source can produce a visible error.
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
            readonly property bool validGeometry: view.width > 0 && view.height > 0
                && isFinite(view.width) && isFinite(view.height)
                && width > 0 && isFinite(width)
                && pointSize.width > 0 && pointSize.height > 0
                && isFinite(pointSize.width) && isFinite(pointSize.height)
                && isFinite(width * pointSize.height / pointSize.width)
            readonly property real rasterWidth: view.width * view.renderedZoom * Screen.devicePixelRatio
            readonly property size rasterSize: root.boundedRasterSize(
                rasterWidth, rasterWidth * pointSize.height / pointSize.width)
            readonly property real pageScale: validGeometry ? width / pointSize.width : 1
            width: view.contentWidth
            // A very wide page can otherwise have subpixel height. ListView
            // would instantiate hundreds of such pages (and their text/link
            // models) to fill one phone screen with only the 4dp spacing.
            // Each raster's independent pixel cap cannot bound that item fanout.
            height: Math.max(MobileTheme.touchTarget,
                             validGeometry ? pointSize.height * pageScale : view.height)

            // PDF page rasters can be transparent: their unpainted paper is
            // white, not the app's dark surface. Do not whiten the rest of a
            // wide page's minimum-height virtual slot.
            Rectangle {
                x: pageImage.x + (pageImage.width - pageImage.paintedWidth) / 2
                y: pageImage.y
                width: pageImage.paintedWidth
                height: pageImage.paintedHeight
                color: "white"
                visible: pdfPage.validGeometry && pdfDocument.status === PdfDocument.Ready
                    && pageImage.status === Image.Ready
            }

            PdfPageImage {
                id: pageImage
                objectName: "pdfPageImage"
                anchors.fill: parent
                visible: pdfPage.validGeometry
                document: pdfPage.validGeometry && pdfDocument.status === PdfDocument.Ready
                    ? pdfDocument : null
                // Null document alone leaves Qt's old Image source loadable by
                // the fallback file decoder when sourceSize changes. Clear both
                // inputs before layout; this absolute owned URL is resolvedSource.
                source: pdfPage.validGeometry && pdfDocument.status === PdfDocument.Ready
                    ? root.documentSource : ""
                currentFrame: pdfPage.index
                asynchronous: true
                sourceSize: pdfPage.rasterSize
                cache: false
                fillMode: Image.PreserveAspectFit
                verticalAlignment: Image.AlignTop
            }
            Text {
                anchors.centerIn: parent
                width: view.width - 2 * MobileTheme.spacingLarge
                visible: !pdfPage.validGeometry
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: MobileTheme.textDim
                text: qsTr("This page has invalid dimensions and cannot be drawn.")
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
                document: pdfPage.validGeometry ? pdfDocument : null
                page: pdfPage.index
                renderScale: pdfPage.pageScale
                from: selectionDrag.centroid.pressPosition
                to: selectionDrag.centroid.position
                hold: !selectionDrag.active && !selectionTap.pressed
                focus: true
            }
            Repeater {
                model: PdfLinkModel {
                    document: pdfPage.validGeometry ? pdfDocument : null
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
