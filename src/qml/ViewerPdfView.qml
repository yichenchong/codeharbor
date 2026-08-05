import QtQuick
import QtQuick.Controls.Basic
import QtWebEngine

// Remote PDF view (SPEC 7.5). The PDF is streamed through the internal scheme
// on the INTERNAL profile and rendered by Chromium's built-in PDF viewer.
Item {
    id: root
    property url url

    // The `viewers` context property (ch::ViewerModel), resolved once and
    // guarded exactly as ViewerPane and EditorPaneView guard it, and for the
    // same reason: an unguarded lookup of a context property the host did not
    // install THROWS a ReferenceError, and that aborts the whole binding pass
    // building this view rather than producing an inert one.
    readonly property var viewerModel: (typeof viewers !== "undefined") ? viewers : null

    // The internal address this view is DISPLAYING, whether it is PINNED in the
    // model's LRU-bounded internal-URL table, and any refusal reported for it.
    // See ViewerImageView.qml for why each of the three is needed; this view is
    // its twin and deliberately keeps the same shape.
    property url internalUrl: ""
    property bool retained: false
    property string errorText: ""

    function retarget() {
        root.releaseInternalUrl();
        root.errorText = "";
        if (root.url.toString().length === 0 || !root.viewerModel)
            return;
        root.internalUrl = root.viewerModel.internalUrlFor(root.url);
        root.retained = root.viewerModel.retainInternalUrl(root.internalUrl.toString());
    }

    function releaseInternalUrl() {
        if (root.retained && root.viewerModel)
            root.viewerModel.releaseInternalUrl(root.internalUrl.toString());
        root.retained = false;
        root.internalUrl = "";
    }

    onUrlChanged: root.retarget()
    Component.onCompleted: root.retarget()
    Component.onDestruction: root.releaseInternalUrl()

    function ownsFailure(candidate) {
        return root.internalUrl.toString().length > 0
               && String(candidate) === root.internalUrl.toString();
    }

    Connections {
        target: root.viewerModel
        function onInternalResourceError(internalUrl, message) {
            if (root.ownsFailure(internalUrl))
                root.errorText = message;
        }
    }

    function pinnedDocument(candidate) {
        let text = String(candidate)
        const fragment = text.indexOf("#")
        if (fragment >= 0)
            text = text.substring(0, fragment)
        return text
    }

    WebEngineView {
        anchors.fill: parent
        visible: root.errorText.length === 0
        profile: root.viewerModel ? root.viewerModel.internalProfile() : null
        // SECURITY (SPEC 7.2): untrusted remote bytes. Disable page scripting
        // and any local/remote URL reach so a malicious document cannot run JS
        // to exfiltrate other files. The built-in PDF viewer renders in its own
        // internal context and is unaffected by disabling page JavaScript.
        settings.javascriptEnabled: false
        settings.localContentCanAccessFileUrls: false
        settings.localContentCanAccessRemoteUrls: false
        settings.javascriptCanOpenWindows: false
        settings.pdfViewerEnabled: true
        settings.pluginsEnabled: true
        onNavigationRequested: function(request) {
            if (root.pinnedDocument(request.url)
                    === root.pinnedDocument(root.internalUrl))
                return
            request.action = WebEngineNavigationRequest.IgnoreRequest
            console.warn("ViewerPdfView: refused navigation to", request.url)
        }
        onNewWindowRequested: function(request) {
            console.warn("ViewerPdfView: refused a new window for",
                         request.requestedUrl)
        }
        // Same rule as ViewerImageView: a load Chromium itself refused would
        // otherwise be a blank pane, because internalResourceError only covers
        // the refusals the scheme handler knows about. The handler's own
        // explanation is better wording, so it is never overwritten.
        onLoadingChanged: function(request) {
            if (request.status === WebEngineView.LoadFailedStatus
                    && root.errorText.length === 0)
                root.errorText = request.errorString
        }
        url: root.internalUrl
    }

    // The refusal, said in words.
    Label {
        objectName: "pdfStatus"
        anchors.centerIn: parent
        width: parent.width - 48
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        // Server- and handler-supplied failure text: data, not markup.
        textFormat: Text.PlainText
        visible: text.length > 0
        text: root.errorText.length > 0 ? qsTr("Error: %1").arg(root.errorText) : ""
        color: Theme.danger
        font.pixelSize: Theme.fontSizeLabel
    }
}
