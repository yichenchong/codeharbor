import QtQuick
import QtQuick.Controls
import CodeHarbor.Mobile

// Step two of the two-step selection: which pane of the chosen Dev Session.
//
// The list is ch::PaneListModel — both server-authoritative region trees
// flattened depth-first, viewer region first — and it is SECTIONED by region, so
// the two regions the desktop shows side by side read here as two headed groups
// in one list. The order inside each section is the order the panes appear in the
// stored tree, which is what keeps a row from moving under the user's thumb when
// the layout is republished.
//
// This page is only ever reached once BOTH regions have resolved (see
// ch::MobileAppController::onLayoutsLoaded), so an empty list here means the Dev
// Session genuinely has no panes rather than "not loaded yet".
Page {
    id: page

    readonly property var ctl: (typeof mobile !== "undefined") ? mobile : null
    readonly property var host: (typeof app !== "undefined") ? app : null

    background: Rectangle { color: MobileTheme.surface }

    header: Rectangle {
        implicitHeight: MobileTheme.touchTarget
        color: MobileTheme.surfaceRaised

        Row {
            anchors.fill: parent
            spacing: 0

            AbstractButton {
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: MobileTheme.touchTarget
                implicitHeight: MobileTheme.touchTarget
                enabled: page.ctl !== null
                onClicked: page.ctl.back()

                contentItem: Text {
                    text: "\u2039"
                    textFormat: Text.PlainText
                    color: MobileTheme.accent
                    font.pixelSize: MobileTheme.fontSizeTitle
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - MobileTheme.touchTarget
                       - MobileTheme.spacingLarge
                // A fixed label, not the Dev Session's name: the name is not in
                // the pane list and ch::AppController exposes only the active
                // session's ID and repository root, so there is nothing
                // server-controlled rendered here at all.
                text: qsTr("Panes")
                textFormat: Text.PlainText
                color: MobileTheme.text
                font.pixelSize: MobileTheme.fontSizeTitle
                elide: Text.ElideRight
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: MobileTheme.border
        }
    }

    // The region word as a heading. The model's `region` role holds the wire
    // words ("viewer" / "terminal"), which are keys and not labels, so they are
    // translated once here rather than shown raw.
    function regionHeading(region) {
        return region === "terminal" ? qsTr("Terminals") : qsTr("Viewers");
    }

    // A one-word description of what the pane will show, derived from the kind
    // ch::ViewerHandlerRegistry resolved. It is deliberately the kind and not the
    // file extension: the kind is what decides which page opens, so this is the
    // row telling the truth about what the tap will do.
    //
    // The words are the closed vocabulary ch::PaneListModel produces, one for
    // one with PaneHostPage's routing table — see the note there for why
    // "editor" is not among them.
    function kindWord(kind) {
        switch (kind) {
        case "terminal": return qsTr("Terminal");
        case "markdown": return qsTr("Markdown");
        case "text": return qsTr("Text");
        case "image": return qsTr("Image");
        case "pdf": return page.ctl && page.ctl.capabilities.hasPdf
                           ? qsTr("PDF") : qsTr("PDF (not supported by this build)");
        case "directory": return qsTr("Files");
        case "web": return page.ctl && page.ctl.capabilities.hasWebView
                           ? qsTr("Web page") : qsTr("Web page (not supported by this build)");
        case "binary": return qsTr("Binary");
        default: return qsTr("Not supported");
        }
    }

    ListView {
        id: paneList
        anchors.fill: parent
        clip: true
        model: page.ctl ? page.ctl.panes : null
        boundsBehavior: Flickable.DragAndOvershootBounds
        ScrollBar.vertical: ScrollBar {}

        // Sectioning by region is what preserves the desktop's two-region model
        // on a surface that can only show one pane: the user still sees which
        // region a pane belongs to, they just visit it instead of glancing at it.
        section.property: "region"
        section.criteria: ViewSection.FullString
        section.delegate: Rectangle {
            // `section` is a context property the view injects into the section
            // delegate's scope, NOT a model role, so it is read directly rather
            // than declared `required`: a required property is only satisfied
            // from the model, and declaring one here fails to initialise.
            width: paneList.width
            implicitHeight: MobileTheme.touchTarget * 0.75
            color: MobileTheme.surfaceDeep

            Text {
                anchors.fill: parent
                anchors.leftMargin: MobileTheme.spacingLarge
                anchors.rightMargin: MobileTheme.spacingLarge
                text: page.regionHeading(section)
                textFormat: Text.PlainText
                color: MobileTheme.textDim
                font.pixelSize: MobileTheme.fontSizeSmall
                font.bold: true
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }

        delegate: AbstractButton {
            id: paneRow
            required property string paneKey
            required property string title
            required property string kind

            width: paneList.width
            implicitHeight: MobileTheme.touchTarget
            enabled: page.ctl !== null
            onClicked: page.ctl.selectPane(paneRow.paneKey)

            background: Rectangle {
                color: paneRow.pressed ? MobileTheme.surfaceHover
                                       : (page.ctl
                                          && page.ctl.selectedPaneKey === paneRow.paneKey
                                          ? MobileTheme.surfaceSelected
                                          : "transparent")

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: MobileTheme.borderSubtle
                }
            }

            // A plain child Column with margins rather than `contentItem` with
            // padding: Column is a positioner and has no padding properties, so
            // assigning one is a QML error, not merely a wrong margin.
            Column {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: MobileTheme.spacingLarge
                anchors.rightMargin: MobileTheme.spacingLarge

                Text {
                    width: parent.width
                    // A file name, a user-chosen pane title, or a slot label —
                    // all of them server-controlled strings. SPEC 7.5.
                    text: paneRow.title
                    textFormat: Text.PlainText
                    color: MobileTheme.text
                    font.pixelSize: MobileTheme.fontSizeBody
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    text: page.kindWord(paneRow.kind)
                    // Translated from a fixed vocabulary rather than taken from
                    // the wire, but rendered under the same rule as everything
                    // else so no page in this module has an exception.
                    textFormat: Text.PlainText
                    color: MobileTheme.textDim
                    font.pixelSize: MobileTheme.fontSizeSmall
                    elide: Text.ElideRight
                }
            }
        }
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - 2 * MobileTheme.spacingLarge
        visible: paneList.count === 0
        // One literal, not a concatenation: lupdate extracts the argument of
        // qsTr() literally and cannot see through a `+`.
        text: qsTr("This Dev Session has no panes yet.")
        textFormat: Text.PlainText
        color: MobileTheme.textDim
        font.pixelSize: MobileTheme.fontSizeBody
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }
}
