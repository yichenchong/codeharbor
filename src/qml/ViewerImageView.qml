import QtQuick
import QtQuick.Controls.Basic
import QtWebEngine
import "RemotePath.js" as RemotePath

// Remote image view (SPEC 7.4/7.5). The image is loaded through the privileged
// internal scheme (codeharbor-internal://file/<id>) on the INTERNAL profile,
// which the scheme handler resolves back to file.readFile. A QML Image cannot
// resolve the WebEngine-only internal scheme, so a WebEngineView renders it.
Item {
    id: root
    property url url

    // The `viewers` context property (ch::ViewerModel), resolved once and
    // guarded exactly as ViewerPane and EditorPaneView guard it, and for the
    // same reason: an unguarded lookup of a context property the host did not
    // install THROWS a ReferenceError, and that aborts the whole binding pass
    // building this view rather than producing an inert one.
    readonly property var viewerModel: (typeof viewers !== "undefined") ? viewers : null

    // The internal address this view is DISPLAYING. Held as a `url` so both
    // sides of every comparison below have been through QUrl's normalisation —
    // the failure signal carries a QUrl and internalUrlFor() hands back a
    // string, and only their normalized text is guaranteed to line up.
    property url internalUrl: ""

    // Whether that address is PINNED in the model's internal-URL table, and so
    // whether a release is owed. ch::ViewerModel's table is LRU-bounded and
    // refreshes recency only when a URL is minted or resolved, so a pane still
    // showing an image from hours ago has its address age out from under it and
    // the next reload fails on a URL that is visibly on screen. Retaining is
    // how the table learns "still displayed", and the contract is exactly one
    // release per retain that returned true — hence a flag rather than an
    // is-it-non-empty test, since a retain can legitimately fail.
    property bool retained: false

    // A refusal the embedded browser could not report: WebEngine's job
    // interface carries only Chromium's coarse error enum, so an image over the
    // inline cap arrived as a blank failed page with nothing to explain it.
    property string errorText: ""

    // The remote path this view is showing, as a person reads it (SPEC 8.3:
    // file:// always means the remote server). The internal address the
    // WebEngineView is actually pointed at is an opaque id, so it is this — and
    // only this — that can say WHICH image is on screen.
    readonly property string displayPath: root.url.toString().length > 0
        ? RemotePath.fileUrlToPath(root.url.toString()) : ""

    // The embedded page has started fetching and has not settled yet. Read by
    // the pane header (ViewerPane's `busy`), which without it drew nothing at
    // all while a large remote image was on its way and the pane was blank.
    property bool loading: false

    // Point this view at the current URL, pinning the address it will show and
    // letting go of the one it was showing.
    function retarget() {
        root.releaseInternalUrl();
        root.errorText = "";
        root.loading = false;
        if (root.url.toString().length === 0 || !root.viewerModel)
            return;
        root.internalUrl = root.viewerModel.internalUrlFor(root.url);
        root.retained = root.viewerModel.retainInternalUrl(root.internalUrl.toString());
    }

    // Give the pin back. Safe to call when nothing is pinned, which is what
    // makes it usable from both the navigation path and teardown.
    function releaseInternalUrl() {
        if (root.retained && root.viewerModel)
            root.viewerModel.releaseInternalUrl(root.internalUrl.toString());
        root.retained = false;
        root.internalUrl = "";
    }

    onUrlChanged: root.retarget()
    Component.onCompleted: root.retarget()
    // Closing the pane is the other way this view stops displaying its address.
    // Without it every image ever opened stays pinned for the life of the
    // process, which is the bound the table exists to enforce.
    Component.onDestruction: root.releaseInternalUrl()

    // A failure is this view's own only if it names the address this view is
    // showing: ONE ViewerModel is shared by every viewer pane and they all
    // listen here. Same matching rule ViewerPane applies to its probe replies —
    // compare against what THIS view asked for, not against a recomputed key.
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

    // A displayed image used to expose nothing at all: no name, no description,
    // no file name. Chromium's own document for an internal address has an
    // opaque URL and no alt text to offer, so everything a screen reader can
    // learn about this pane's content is said here.
    Accessible.role: Accessible.Graphic
    Accessible.name: root.displayPath.length > 0
                     ? qsTr("Image %1").arg(root.displayPath)
                     : qsTr("Image")
    Accessible.description: root.errorText.length > 0
        ? qsTr("This image could not be shown: %1").arg(root.errorText)
        : root.loading
          ? qsTr("Loading the image at %1.").arg(root.displayPath)
          : qsTr("The image at %1. Give it keyboard focus with Tab and scroll it with the "
                 + "arrow keys.").arg(root.displayPath)

    WebEngineView {
        anchors.fill: parent
        // An image larger than the pane SCROLLS, and until this the only way to
        // scroll it was the wheel or a drag: the view was reachable by clicking
        // and by nothing else. Chromium moves the viewport for the arrow and
        // page keys itself — that is browser behaviour, not script, so it works
        // with JavaScript disabled below.
        activeFocusOnTab: true
        // Hidden once the resource is known to have been refused: Chromium's
        // own blank failure page says nothing, and leaving it under the
        // explanation below only makes the pane look half-loaded.
        visible: root.errorText.length === 0
        profile: root.viewerModel ? root.viewerModel.internalProfile() : null
        // SECURITY (SPEC 7.2): these bytes are untrusted remote file content.
        // An .svg/.html served top-level could run JS and exfiltrate other
        // files, so disable scripting and any local/remote URL reach. The
        // scheme handler additionally sends a locked-down CSP for such types.
        settings.javascriptEnabled: false
        settings.localContentCanAccessFileUrls: false
        settings.localContentCanAccessRemoteUrls: false
        settings.javascriptCanOpenWindows: false
        onNavigationRequested: function(request) {
            if (root.pinnedDocument(request.url)
                    === root.pinnedDocument(root.internalUrl))
                return
            request.action = WebEngineNavigationRequest.IgnoreRequest
            console.warn("ViewerImageView: refused navigation to", request.url)
        }
        onNewWindowRequested: function(request) {
            console.warn("ViewerImageView: refused a new window for",
                         request.requestedUrl)
        }
        // A load that Chromium itself refuses (the scheme handler aborted the
        // job, the resource decoded to nothing) is otherwise a blank rectangle:
        // internalResourceError only fires for the refusals the HANDLER knows
        // about, and this view's whole job is to show one image. The handler's
        // own explanation is better wording, so it is never overwritten.
        onLoadingChanged: function(request) {
            root.loading = request.status === WebEngineView.LoadStartedStatus
            if (request.status === WebEngineView.LoadFailedStatus
                    && root.errorText.length === 0)
                root.errorText = request.errorString
        }
        url: root.internalUrl
    }

    // The refusal, said in words.
    Label {
        objectName: "imageStatus"
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
