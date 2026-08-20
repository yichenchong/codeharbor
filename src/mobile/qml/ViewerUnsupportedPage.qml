import QtQuick
import QtQuick.Controls

// The honest end of the mobile viewer table (mobile SPEC 7.5).
//
// A viewer kind reaches this page for exactly one of a small, KNOWN set of
// reasons, and the page states which one plus the path. It is not a placeholder
// and it is not a "coming soon": there are resources a phone client genuinely
// cannot show, and saying so precisely is the correct behaviour. What it must
// never do is offer to hand the file to another application — that would export a
// remote path out of the app sandbox (SPEC 7.4 / 2.4), which is why the desktop's
// "Open as / Open with" affordances have no counterpart here.
//
// It also deliberately offers no DOWNLOAD. The desktop's binary view offers
// metadata and a download because a desktop has somewhere to download to; on a
// phone the only destinations are shared storage or another app, both of which are
// the same leak.
Item {
    id: root

    property string remotePath: ""
    property url paneUrl
    property string repoRoot: ""
    property string paneId: ""
    signal openRequested(string path)
    signal titleRequested(string title)

    // The kind that could not be shown. Set by whoever routed here: the
    // ch::ViewerKinds vocabulary for a file the pane list DID classify
    // ("binary" is the common case), plus "unsupported" — PaneListModel's own
    // word for a url the viewer registry claimed nothing for.
    property string kind: "binary"

    // Why, in one sentence, for the kind at hand. Concrete reasons only — no
    // "unsupported file type" without saying what made it so.
    readonly property string reason: {
        switch (root.kind) {
        case "pdf":
            return qsTr("This build of CodeHarbor has no PDF engine, so PDF documents cannot be drawn.");
        case "web":
            return qsTr("This build of CodeHarbor has no web view, so web pages cannot be opened. The address is shown below as text.");
        case "binary":
            return qsTr("This file is not UTF-8 text, so there is nothing to show as text, and it is not an image or PDF this client can draw.");
        case "directory":
            return qsTr("This path is no longer a directory the server will list.");
        case "unsupported":
            return qsTr("CodeHarbor could not tell what kind of resource this is, so it has no viewer for it. The address is shown below as text.");
        default:
            return qsTr("There is no mobile viewer for this kind of resource.");
        }
    }

    readonly property string kindLabel: {
        switch (root.kind) {
        case "markdown": return qsTr("Markdown");
        case "text": return qsTr("Text");
        case "image": return qsTr("Image");
        case "pdf": return qsTr("PDF");
        case "binary": return qsTr("Binary");
        case "directory": return qsTr("Directory");
        case "web": return qsTr("Web page");
        // A NOUN, never the kind word itself: the title below reads
        // "Cannot show this %1", and "unsupported" — or any kind this table has
        // not been taught — turned that into "Cannot show this unsupported".
        case "unsupported":
        default: return qsTr("resource");
        }
    }

    Rectangle {
        anchors.fill: parent
        color: MobileTheme.surface
    }

    Column {
        anchors.centerIn: parent
        width: parent.width - 2 * MobileTheme.spacingLarge
        spacing: MobileTheme.spacing

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            textFormat: Text.PlainText
            color: MobileTheme.text
            font.pixelSize: MobileTheme.fontSizeTitle
            text: qsTr("Cannot show this %1").arg(root.kindLabel)
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: MobileTheme.textDim
            font.pixelSize: MobileTheme.fontSizeBody
            text: root.reason
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WrapAnywhere
            // Server-controlled. PlainText, like every other string on every
            // mobile surface.
            textFormat: Text.PlainText
            color: MobileTheme.textFaint
            font.family: MobileTheme.monoFamily
            font.pixelSize: MobileTheme.fontSizeSmall
            text: root.remotePath.length > 0
                  ? root.remotePath : root.paneUrl.toString()
        }
    }
}
