import QtQuick
import QtQuick.Controls

// Native remote text editor (mobile SPEC 8.2 / 8.4 / 8.6).
//
// NO MONACO, NO WEBCHANNEL. The desktop's editor is Monaco in a Qt WebEngine page
// talking to ch::EditorController over Qt WebChannel; none of those three exist on
// Android or iOS. What is reused is the part that matters: the very same
// ch::EditorController, minted per pane by `editorFactory`, driving the very same
// SPEC 8.2 state machine, the same file.watch subscription and the same
// revision-guarded save. Only the text surface is different — a native TextArea
// instead of a browser.
//
// The controller HOLDS its first contentLoaded until ready() is called, because on
// the desktop WebChannel connects long after the controller exists. That is a
// contract, not an implementation detail: this page calls ready() once, after its
// Connections are live, or the buffer would never arrive.
Item {
    id: root

    property string remotePath: ""
    property url paneUrl
    property string repoRoot: ""
    property string paneId: ""
    property string acceptedServerId: ""
    property string devSessionId: ""
    signal openRequested(string path)
    signal titleRequested(string title)
    signal closeRequested()

    readonly property var factory: (typeof editorFactory !== "undefined") ? editorFactory : null

    // The per-pane controller. Parented to this page, so its server-side
    // file.watch subscription is released when the page goes away (SPEC 8.7)
    // rather than leaked.
    property var controller: null

    function clearRecoveryOffer() {
        recovery.content = "";
        recovery.offeredPath = "";
        recovery.close();
    }

    function applyRecoveryScope() {
        if (root.controller)
            root.controller.setRecoveryScope(root.acceptedServerId, root.devSessionId, root.paneId);
    }

    function retireRecoveryScope() {
        if (!root.controller)
            return;
        root.clearLeaveConsent();
        root.clearRecoveryOffer();
        // Retire old replies immediately; apply only the final bound context,
        // never a transient new-server/old-session combination.
        root.controller.setRecoveryScope("", "", "");
        Qt.callLater(root.applyRecoveryScope);
    }
    onAcceptedServerIdChanged: root.retireRecoveryScope()
    onDevSessionIdChanged: root.retireRecoveryScope()
    onPaneIdChanged: root.retireRecoveryScope()

    property string fileState: "disconnected"
    property string revision: ""
    property string statusText: ""
    property bool conflict: false
    // The buffer differs from what was loaded/saved. Tracked here rather than
    // read off the controller because the controller does not own the text.
    property bool dirty: false
    // The path whose bytes are actually IN the buffer (or on their way into it).
    // Not the same thing as `remotePath`: the host can reassign that at any time,
    // and while the unsaved-changes guard below is open the buffer still holds
    // the previous file. Everything the user is told about "this file" is said
    // about THIS path, so the header can never name a file the user is not
    // editing.
    property string openedPath: ""
    // The text handed to the last save, so a reply can tell whether the buffer
    // still matches what was written.
    property string savedText: ""
    // Set while the page itself is writing into the TextArea, so onTextChanged
    // does not mistake a load for a user edit.
    property bool applying: false

    // The pane header follows the file the buffer holds, through the
    // titleRequested seam PaneHostPage connects.
    function fileNameOf(path) {
        const i = path.lastIndexOf("/");
        return i >= 0 ? path.substring(i + 1) : path;
    }
    onOpenedPathChanged: {
        root.clearLeaveConsent();
        if (root.openedPath.length > 0)
            root.titleRequested(root.fileNameOf(root.openedPath));
    }

    // Open `path` and record it as the buffer's identity in one step, so the two
    // can never disagree.
    function openPath(path) {
        if (!root.controller || path.length === 0)
            return;
        root.clearRecoveryOffer();
        root.clearLeaveConsent();
        root.openedPath = path;
        root.controller.open(path);
    }

    readonly property bool writable: root.controller !== null
                                     && !root.controller.readOnly
                                     && root.fileState !== "loading"
                                     && root.fileState !== "disconnected"
                                     && root.fileState !== "error"
                                     && root.fileState !== "read_only"

    Component.onCompleted: {
        if (!root.factory)
            return;
        // Recovery is scoped to the accepted peer, Dev Session, stable pane
        // and actual opened path. An incomplete context never uses a raw key.
        root.controller = root.factory.create(root, root.acceptedServerId,
                                              root.devSessionId, root.paneId);
        if (!root.controller)
            return;
        root.fileState = root.controller.fileState;
        // ready() AFTER the Connections below exist, which they do by the time
        // Component.onCompleted runs. See the note at the top of this file.
        root.controller.ready();
        if (root.remotePath.length > 0)
            root.openPath(root.remotePath);
    }

    // Pane/header/system Back all arrive here before the host releases this
    // controller. Recovery is not permission to throw away dirty RAM.
    function requestClose() {
        if (!root.dirty) {
            root.closeRequested();
            return;
        }
        root.requestLeave("", true);
    }

    onRemotePathChanged: {
        if (!root.controller || root.remotePath.length === 0)
            return;
        if (root.dirty) {
            // The guard is not advisory: a pane that silently threw away edits
            // when the host reassigned its path would lose work with no trace.
            root.requestLeave(root.remotePath, false);
            return;
        }
        root.openPath(root.remotePath);
    }

    property string pendingPath: ""
    property var leaveContext: null

    function requestLeave(path, closing) {
        root.pendingPath = path;
        root.leaveContext = {
            "controller": root.controller,
            "path": root.openedPath,
            "server": root.acceptedServerId,
            "session": root.devSessionId,
            "pane": root.paneId,
            "target": path,
            "closing": closing
        };
        leaveGuard.open();
    }

    function clearLeaveConsent() {
        root.leaveContext = null;
        root.pendingPath = "";
        leaveGuard.close();
    }

    function leaveConsentIsCurrent(consent) {
        return consent !== null && consent.controller === root.controller
            && consent.path === root.openedPath
            && consent.server === root.acceptedServerId
            && consent.session === root.devSessionId
            && consent.pane === root.paneId;
    }

    Connections {
        target: root.controller

        function onContentLoaded(content, rev) {
            if (root.dirty) {
                // A load landing on a buffer with UNSAVED edits does not win.
                // The buffer stays exactly as the user left it and the revision
                // stays the one it was loaded against, so the next save reports a
                // conflict and the user decides; adopting `rev` here would let
                // that save silently overwrite the very change this content came
                // from. Reachable only if the controller reloads a buffer it
                // believes is clean (see onTextChanged, which tells it on the
                // first keystroke rather than after the debounce).
                root.conflict = true;
                root.statusText = qsTr("This file changed on the server and this buffer has unsaved edits, so the edits were kept. Saving reports a conflict; Reload takes the server's version and discards them.");
                return;
            }
            root.applying = true;
            editor.text = content;
            root.applying = false;
            root.revision = rev;
            root.dirty = false;
            root.conflict = false;
            root.statusText = "";
        }
        function onFileStateChanged(state) {
            root.fileState = state;
        }
        function onSaved(rev) {
            root.revision = rev;
            // NOT unconditionally clean: a keystroke that landed while the write
            // was in flight is not in the file, and clearing the flag would
            // disable the Save button over an edit that was never written.
            root.dirty = editor.text !== root.savedText;
            // A debounced observation may have arrived after Save queued these
            // same bytes. Reconcile the confirmed baseline explicitly, and do
            // not let a pending snapshot mark an already saved buffer dirty.
            snapshot.stop();
            if (root.controller) {
                if (root.dirty)
                    root.controller.reportContent(editor.text);
                else
                    root.controller.reportDirty(false, rev);
            }
            root.conflict = false;
            root.statusText = root.dirty ? qsTr("Saved. There are newer edits.")
                                         : qsTr("Saved.");
        }
        function onSaveConflict(currentRevision) {
            // NEVER resolved silently. The buffer stays exactly as the user left
            // it and the only ways forward are explicit: reload (losing the
            // edits) or keep editing and save against the new revision.
            root.conflict = true;
            root.revision = currentRevision;
            root.statusText = qsTr("This file changed on the server since it was opened. Saving now would overwrite that change.");
        }
        function onSaveError(message) {
            // The server's own wording, shown as plain text.
            root.statusText = message;
        }
        function onRecoveryAvailable(recoveredContent) {
            if (!root.controller || !root.controller.recoveryEnabled
                    || root.openedPath.length === 0)
                return;
            recovery.offeredPath = root.openedPath;
            recovery.content = recoveredContent;
            recovery.open();
        }
    }

    Rectangle {
        anchors.fill: parent
        color: MobileTheme.surface
    }

    Column {
        anchors.fill: parent
        spacing: 0

        // ---- status + actions ----------------------------------------------

        Item {
            width: parent.width
            height: MobileTheme.touchTarget

            Text {
                anchors.left: parent.left
                anchors.leftMargin: MobileTheme.spacing
                anchors.right: reloadButton.left
                anchors.rightMargin: MobileTheme.spacing
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideLeft
                textFormat: Text.PlainText
                color: MobileTheme.textDim
                font.pixelSize: MobileTheme.fontSizeSmall
                text: {
                    const marker = root.dirty ? " •" : "";
                    if (!root.controller)
                        return qsTr("No editor is available in this build.");
                    // openedPath, never remotePath: while the unsaved-changes
                    // guard is open the host has already moved remotePath on, and
                    // naming that file over the previous file's buffer is a lie
                    // about what the Save button would write.
                    if (root.controller.readOnly)
                        return root.openedPath + qsTr(" (read-only)") + marker;
                    return root.openedPath + marker;
                }
            }

            Button {
                id: reloadButton
                anchors.right: saveButton.left
                anchors.rightMargin: MobileTheme.spacing
                anchors.verticalCenter: parent.verticalCenter
                height: MobileTheme.touchTarget
                text: qsTr("Reload")
                enabled: root.controller !== null
                onClicked: {
                    if (root.dirty) {
                        root.requestLeave("", false);
                        return;
                    }
                    root.controller.requestReload();
                }
            }

            Button {
                id: saveButton
                anchors.right: parent.right
                anchors.rightMargin: MobileTheme.spacing
                anchors.verticalCenter: parent.verticalCenter
                height: MobileTheme.touchTarget
                text: qsTr("Save")
                enabled: root.writable && root.dirty
                onClicked: {
                    root.statusText = "";
                    // The revision the buffer was LOADED at, echoed verbatim
                    // (SPEC 8.4): opaque server data, never parsed or minted here.
                    root.savedText = editor.text;
                    root.controller.save(root.savedText, root.revision);
                }
            }
        }

        Rectangle {
            width: parent.width
            height: visible ? Math.max(MobileTheme.touchTarget, status.implicitHeight + 2 * MobileTheme.spacing) : 0
            visible: root.statusText.length > 0
            color: root.conflict ? MobileTheme.warningSurface() : MobileTheme.surfaceRaised
            // Anchored on three edges, NOT filled. `anchors.fill` makes this
            // item's height follow the rectangle, while the rectangle's height
            // above is derived from this item's implicit height — a loop Qt
            // detects and then abandons, leaving the strip sized from whichever
            // half of it happened to be evaluated first. Left/right/top plus a
            // natural height keeps the dependency one-way.
            Text {
                id: status
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: MobileTheme.spacing
                anchors.rightMargin: MobileTheme.spacing
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                color: MobileTheme.text
                font.pixelSize: MobileTheme.fontSizeSmall
                text: root.statusText
            }
        }

        Rectangle {
            width: parent.width
            height: visible ? Math.max(MobileTheme.touchTarget,
                recoveryUnavailableText.implicitHeight + 2 * MobileTheme.spacing) : 0
            visible: root.controller !== null && root.openedPath.length > 0
                     && !root.controller.recoveryEnabled
            color: MobileTheme.warningSurface()
            Text {
                id: recoveryUnavailableText
                objectName: "editorRecoveryStatus"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: MobileTheme.spacing
                anchors.rightMargin: MobileTheme.spacing
                anchors.verticalCenter: parent.verticalCenter
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: MobileTheme.text
                font.pixelSize: MobileTheme.fontSizeSmall
                text: qsTr("Crash recovery is unavailable until this document is opened in its accepted server and Dev Session with a stable pane and remote recovery directory.")
            }
        }

        Rectangle {
            width: parent.width
            height: visible ? MobileTheme.touchTarget : 0
            visible: root.fileState === "read_only"
            color: MobileTheme.surfaceRaised
            Text {
                anchors.fill: parent
                anchors.margins: MobileTheme.spacing
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                color: MobileTheme.textDim
                font.pixelSize: MobileTheme.fontSizeSmall
                // FileState::ReadOnly means the buffer is a PREFIX of an
                // over-size file. Saving it would delete everything past the
                // prefix, so the surface is read-only and says why.
                text: qsTr("Only the first 8 MiB of this file was read, so it cannot be saved.")
            }
        }

        // ---- the buffer ----------------------------------------------------

        ScrollView {
            width: parent.width
            height: parent.height - y
            clip: true

            TextArea {
                id: editor
                objectName: "editorText"
                readOnly: !root.writable
                // Wrapped: a phone has no room for horizontal scrolling, and a
                // code line that runs off the right edge cannot be read at all.
                wrapMode: TextEdit.WrapAnywhere
                selectByMouse: true
                font.family: MobileTheme.monoFamily
                font.pixelSize: MobileTheme.fontSizeBody
                color: MobileTheme.text
                // A native TextEdit. Its textFormat is PlainText — the default —
                // and it is pinned here rather than left implicit, because this is
                // the one surface where a server-controlled string is put into an
                // editable item.
                textFormat: TextEdit.PlainText

                onTextChanged: {
                    if (root.applying || !root.controller)
                        return;
                    if (!root.dirty) {
                        root.dirty = true;
                        // The FIRST keystroke is reported immediately, not after
                        // the debounce: reportContent() is also what marks the
                        // controller's buffer dirty, and an external-change reload
                        // arriving inside the debounce window would find a buffer
                        // it believes is clean and re-read the file over what was
                        // just typed (EditorController only auto-reloads a clean
                        // buffer).
                        root.controller.reportContent(editor.text);
                        return;
                    }
                    // Debounced snapshot for crash recovery (SPEC 11.3), exactly
                    // as the desktop's page debounces its own reportContent.
                    snapshot.restart();
                }
            }
        }
    }

    Timer {
        id: snapshot
        objectName: "recoverySnapshotTimer"
        interval: 500
        repeat: false
        onTriggered: {
            if (root.controller)
                root.controller.reportContent(editor.text);
        }
    }

    // One width for both sheets, computed from the PAGE and nothing else. Every
    // dialog geometry (width, availableWidth, implicitWidth) is derived from the
    // popup's own layout, so measuring a sheet's text against one of those makes
    // the text's wrapped height feed back into the height the popup is deriving —
    // the implicitHeight binding loop Qt reports and then abandons, which leaves
    // the sheet sized from whichever half was evaluated first.
    readonly property real sheetWidth: Math.min(root.width - 2 * MobileTheme.spacingLarge, 480)
    readonly property real sheetTextWidth: root.sheetWidth - 2 * MobileTheme.spacingLarge

    // ---- unsaved-changes guard ---------------------------------------------

    Dialog {
        id: leaveGuard
        objectName: "unsavedChangesDialog"
        anchors.centerIn: parent
        width: root.sheetWidth
        modal: true
        title: qsTr("Discard unsaved changes?")
        standardButtons: Dialog.Discard | Dialog.Cancel

        // No explicit width here: a Popup already resizes its contentItem to its
        // own availableWidth, so binding width to that same value made the item
        // and the popup's layout assign the same property, and the wrapped text's
        // implicit height then fed back into the dialog's implicit height — the
        // "Binding loop detected for property implicitHeight" the shell test log
        // was full of. Dropping the binding leaves the popup in sole charge of
        // the width, which is what it was already doing.
        // The message sits inside a plain Item rather than being the contentItem
        // itself. A popup assigns its content item's width and height from its own
        // geometry, and its implicit height is read back OFF that same item — so a
        // wrapping Text used directly as the content item is on both sides of the
        // calculation, which is the "Binding loop detected for property
        // implicitHeight" Qt reports and then abandons, leaving the sheet sized
        // from whichever half ran first. The wrapper's implicit height comes from
        // a Text whose width is fixed by the page, so the dependency is one-way.
        contentItem: Item {
            implicitHeight: guardMessage.implicitHeight
            Text {
                id: guardMessage
                width: root.sheetTextWidth
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: MobileTheme.text
                font.pixelSize: MobileTheme.fontSizeSmall
                text: qsTr("This file has edits that were never saved. Leaving now loses them.")
            }
        }

        onDiscarded: {
            const consent = root.leaveContext;
            if (!root.leaveConsentIsCurrent(consent)) {
                root.clearLeaveConsent();
                return;
            }
            // Consume before emitting any signal: duplicate/late confirmation
            // must never act on another document, context or replacement pane.
            root.clearLeaveConsent();
            snapshot.stop();
            root.dirty = false;
            if (consent.closing)
                root.closeRequested();
            else if (consent.target.length > 0)
                root.openPath(consent.target);
            else
                root.controller.requestReload();
        }
        onRejected: {
            // The page keeps editing the file it already had open: `openedPath`
            // was never moved, so the header and the Save button still speak
            // about the buffer on screen even though the host has moved
            // `remotePath` on.
            root.clearLeaveConsent();
        }
    }

    // ---- crash-recovery offer ----------------------------------------------

    Dialog {
        id: recovery
        objectName: "recoveryDialog"
        anchors.centerIn: parent
        width: root.sheetWidth
        modal: true
        property string content: ""
        property string offeredPath: ""
        title: qsTr("Unsaved changes were recovered")
        standardButtons: Dialog.Yes | Dialog.No

        // Preserve the standard roles, but distinguish postponing from deletion.
        onOpened: {
            const restore = recovery.standardButton(Dialog.Yes);
            if (restore)
                restore.text = qsTr("Restore");
            const notNow = recovery.standardButton(Dialog.No);
            if (notNow)
                notNow.text = qsTr("Not now");
        }

        // Same reason as the guard dialog above: the popup sizes its contentItem.
        // The message sits inside a plain Item rather than being the contentItem
        // itself. A popup assigns its content item's width and height from its own
        // geometry, and its implicit height is read back OFF that same item — so a
        // wrapping Text used directly as the content item is on both sides of the
        // calculation, which is the "Binding loop detected for property
        // implicitHeight" Qt reports and then abandons, leaving the sheet sized
        // from whichever half ran first. The wrapper's implicit height comes from
        // a Text whose width is fixed by the page, so the dependency is one-way.
        contentItem: Item {
            implicitHeight: recoveryMessage.implicitHeight
            Text {
                id: recoveryMessage
                width: root.sheetTextWidth
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: MobileTheme.text
                font.pixelSize: MobileTheme.fontSizeSmall
                text: qsTr("Restore puts this copy back in the editor, unsaved. "
                           + "Not now keeps the saved file on screen. The recovery copy "
                           + "may be offered again. New edits can replace it; saving can clear it.");
            }
        }

        onAccepted: {
            if (!root.controller || !root.controller.recoveryEnabled
                    || recovery.offeredPath.length === 0
                    || recovery.offeredPath !== root.openedPath) {
                root.clearRecoveryOffer();
                return;
            }
            root.applying = true;
            editor.text = recovery.content;
            root.applying = false;
            root.dirty = true;
            // `applying` suppressed onTextChanged, so the controller has not been
            // told. Tell it here: reportContent() is what marks ITS buffer dirty,
            // and a controller that still believes the buffer is clean will
            // re-read the file over the restored snapshot the first time the file
            // changes on the server.
            if (root.controller)
                root.controller.reportContent(editor.text);
            root.clearRecoveryOffer();
        }
        onRejected: root.clearRecoveryOffer()
    }
}
