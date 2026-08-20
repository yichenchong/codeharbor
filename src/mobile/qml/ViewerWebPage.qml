import QtQuick
import QtQuick.Controls
import QtWebView

// http(s) web view for the mobile client (mobile SPEC 7.5).
//
// GATED TWICE: by CH_HAVE_QTWEBVIEW at CMake level (this file is only added to the
// QML module when the optional Qt6::WebView package was found, or the
// `import QtWebView` above would fail the whole module), and by
// mobile.capabilities.hasWebView in QML, so nothing routes here on a build without
// it.
//
// QtWebView is the PLATFORM's web view — Android's WebView, iOS's WKWebView —
// rendered inside this app's own window. It is not Qt WebEngine and it is not a
// browser handoff: nothing here calls QDesktopServices, and no URL leaves this
// process.
//
// SCHEME ASSERTION. The ONLY thing this page will load is http or https, and the
// check is an assertion rather than a filter: anything else is refused, reported
// as text, and the web view is never given a URL at all. A remote file:// URL or a
// codeharbor-internal:// URL reaching a web view is the SPEC 7.4 mistake the whole
// internal-scheme design exists to prevent — on the desktop such a URL would be
// read as a CLIENT-machine path, and on a phone the platform web view would do
// exactly the same with the app's own sandbox.
Item {
    id: root

    // Set by whoever routed here. A plain string, deliberately not a `url`
    // property: a url property normalises and would happily accept the very
    // schemes this page exists to refuse, which makes the assertion below read as
    // if it had already passed.
    property string webUrl: ""

    // The page interface, for a host that loads this like any other viewer page.
    property string remotePath: ""
    property url paneUrl
    property string repoRoot: ""
    property string paneId: ""
    signal openRequested(string path)
    signal titleRequested(string title)
    // Emitted when the user closes the view (the markdown viewer loads this over
    // itself and needs the way back).
    signal closeRequested()

    // The one scheme test, used both for the address this page is handed and for
    // every navigation the page attempts afterwards.
    function allowsScheme(candidate) {
        return /^https?:\/\//i.test(candidate);
    }
    readonly property bool schemeAllowed: root.allowsScheme(root.webUrl)

    // The last navigation this page REFUSED, shown as text. Cleared whenever the
    // address changes, so a refusal never outlives the page it happened on.
    property string refusedNavigation: ""
    onWebUrlChanged: root.refusedNavigation = ""

    // The one navigation rule, stated once: a destination this page will not
    // load is STOPPED and reported, an allowed one clears the last refusal.
    // Returns whether `started` was allowed. The loadingChanged handler below is
    // its only production caller; keeping the rule out of the handler body is
    // what lets it be stated, read and exercised as one thing.
    function noteNavigation(started) {
        if (started.length === 0 || root.allowsScheme(started)) {
            root.refusedNavigation = "";
            return true;
        }
        web.stop();
        root.refusedNavigation = started;
        return false;
    }

    Rectangle {
        anchors.fill: parent
        color: MobileTheme.surface
    }

    Column {
        anchors.fill: parent
        spacing: 0

        Item {
            width: parent.width
            height: MobileTheme.touchTarget

            Text {
                anchors.left: parent.left
                anchors.leftMargin: MobileTheme.spacing
                anchors.right: closeButton.left
                anchors.rightMargin: MobileTheme.spacing
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideRight
                // The address as TEXT, always, whether or not it is loaded.
                textFormat: Text.PlainText
                color: MobileTheme.textDim
                font.family: MobileTheme.monoFamily
                font.pixelSize: MobileTheme.fontSizeSmall
                text: root.webUrl
            }

            Button {
                id: closeButton
                anchors.right: parent.right
                anchors.rightMargin: MobileTheme.spacing
                anchors.verticalCenter: parent.verticalCenter
                height: MobileTheme.touchTarget
                text: qsTr("Close")
                onClicked: root.closeRequested()
            }
        }

        Rectangle {
            width: parent.width
            height: 1
            color: MobileTheme.borderSubtle
        }

        // A refused NAVIGATION, as opposed to a refused starting address. See
        // the WebView below for why this can happen at all.
        Rectangle {
            width: parent.width
            height: visible ? Math.max(MobileTheme.touchTarget,
                                       refusal.implicitHeight + 2 * MobileTheme.spacing)
                            : 0
            visible: root.refusedNavigation.length > 0
            color: MobileTheme.warningSurface()
            Text {
                id: refusal
                anchors.fill: parent
                anchors.margins: MobileTheme.spacing
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                verticalAlignment: Text.AlignVCenter
                color: MobileTheme.text
                font.pixelSize: MobileTheme.fontSizeSmall
                // The destination is page-controlled text. PlainText, like every
                // other string on every mobile surface.
                text: qsTr("This page tried to open an address that is not http or https, and was stopped: %1")
                      .arg(root.refusedNavigation)
            }
        }

        Item {
            width: parent.width
            height: parent.height - y

            WebView {
                id: web
                objectName: "mobileWebView"
                anchors.fill: parent
                visible: root.schemeAllowed
                // The assertion, expressed where it cannot be bypassed: the URL
                // handed to the platform web view is EMPTY unless the scheme
                // passed. There is no branch in which a non-http(s) address is
                // loaded and then stopped.
                url: root.schemeAllowed ? root.webUrl : ""

                // THE SAME ASSERTION, APPLIED TO NAVIGATION. `url` above only
                // covers the address this page was handed; a loaded page can
                // navigate itself — a tapped link, a redirect, a meta refresh —
                // and QtWebView has no request interceptor, so the earliest
                // point this client can act is the start of the load.
                //
                // stop() is the cancellation API (re-assigning `url` starts
                // ANOTHER load instead of cancelling this one), and it maps
                // straight onto Android's WebView.stopLoading() and iOS's
                // [WKWebView stopLoading], so a file:// or intent:// destination
                // is aborted rather than merely reported. Without this the
                // platform web view would happily render the app's own sandbox.
                onLoadingChanged: function(loadRequest) {
                    if (loadRequest.status !== WebView.LoadStartedStatus)
                        return;
                    root.noteNavigation(loadRequest.url.toString());
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 2 * MobileTheme.spacingLarge
                visible: !root.schemeAllowed
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WrapAnywhere
                textFormat: Text.PlainText
                color: MobileTheme.textDim
                font.pixelSize: MobileTheme.fontSizeBody
                text: root.webUrl.length === 0
                      ? qsTr("There is no address to open.")
                      : qsTr("Only http and https addresses can be opened. This one was refused and nothing was loaded.")
            }

            BusyIndicator {
                anchors.centerIn: parent
                running: root.schemeAllowed && web.loading
                visible: running
            }
        }
    }
}
