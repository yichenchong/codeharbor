import QtQuick
import QtQuick.Controls
import QtWebView
import CodeHarbor.Mobile.Core

// http(s) web view for the mobile client (mobile SPEC 7.5).
//
// GATED TWICE: by CH_HAVE_QTWEBVIEW at CMake level (this file is only added to the
// QML module when the optional Qt6::WebView package was found, or the
// `import QtWebView` above would fail the whole module), and by
// mobile.capabilities.hasWebView in QML, so nothing routes here on a build without
// it.
//
// The native backend is guarded before it sees an untrusted address. A unique
// app-created about:blank marker identifies this page's native view; the policy
// controller releases the http(s) address only after native navigation and
// new-window enforcement is installed. Missing/failed policy is a visible error,
// never permission to fall back to Qt's external-URL handling.
Item {
    id: root

    // Set by whoever routed here. A plain string, deliberately not a `url`
    // property: a url property normalises and would happily accept the very
    // schemes this page exists to refuse, which makes the assertion below read as
    // if it had already passed.
    // Direct pane hosts provide paneUrl; the Markdown overlay overrides webUrl
    // explicitly. Keep the default bound so both entry points load an address.
    property string webUrl: String(root.paneUrl)

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

    // Initial addresses are http(s); platform guards enforce every later
    // navigation, redirect and new-window request before Qt's default handler.
    function allowsScheme(candidate) {
        return /^https?:\/\//i.test(candidate);
    }
    readonly property bool schemeAllowed: root.allowsScheme(root.webUrl)

    property bool started: false
    onWebUrlChanged: {
        if (root.started && root.allowsScheme(root.webUrl))
            policy.attach(web);
    }
    Component.onCompleted: {
        root.started = true;
        if (root.schemeAllowed)
            policy.attach(web);
    }
    function deactivate() {
        root.started = false;
        // Keep the native policy installed until QObject teardown; never
        // restore an external-opening default while retiring this document.
        web.stop();
    }
    Component.onDestruction: root.deactivate()

    MobileWebViewPolicy {
        id: policy
        objectName: "mobileWebViewPolicy"
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
                text: root.schemeAllowed && policy.ready
                      && root.allowsScheme(web.url.toString())
                      ? web.url.toString() : root.webUrl
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
        Item {
            width: parent.width
            height: parent.height - y

            WebView {
                id: web
                objectName: "mobileWebView"
                anchors.fill: parent
                // Keep the bootstrap native view attached while the guard
                // locates it. On failure hide it so the explanation is visible.
                visible: root.schemeAllowed && policy.errorText.length === 0
                url: root.schemeAllowed && policy.ready ? root.webUrl : policy.marker
                settings.allowFileAccess: false
                settings.localContentCanAccessFileUrls: false
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

            Text {
                anchors.centerIn: parent
                width: parent.width - 2 * MobileTheme.spacingLarge
                visible: root.schemeAllowed && policy.errorText.length > 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: MobileTheme.textDim
                font.pixelSize: MobileTheme.fontSizeBody
                text: policy.errorText
            }

            BusyIndicator {
                anchors.centerIn: parent
                running: root.schemeAllowed
                         && (policy.installing || (policy.ready && web.loading))
                visible: running
            }
        }
    }
}
