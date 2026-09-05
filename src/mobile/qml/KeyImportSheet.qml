import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Load the private keys this client can authenticate with (SPEC 11.2, 12.1).
//
// WHY A SHEET AND NOT A PAGE. The mobile shell shows exactly one pane at a time,
// and loading a key is something the user does WHILE looking at the connect form
// they are about to use it in. A modal Dialog keeps that context on screen; a
// second page would lose it and would need its own way back.
//
// THREE SHAPES, AND SAVING IS THE ONLY ONE THAT WRITES BYTES. SPEC 11.2 is an
// exhaustive allowlist of client-stored local state; it used to exclude
// private-key bytes outright, and was amended to permit them under one
// explicitly opt-in shape. So the sheet offers exactly three, and no fourth:
//
//   PASTE  -> the bytes live in memory for this session and are gone when the app
//             closes. This stays the DEFAULT for a pasted key: importing never
//             writes anything. The cost is retyping/re-pasting per launch, which
//             is the same trade SPEC 12.1 (docs/SPEC.md:1798-1805) already
//             accepts for passphrases in exchange for persisting no secret.
//   CHOOSE -> the key stays in the USER's storage and the client remembers only a
//             REFERENCE to it (an Android persistable read grant, an iOS
//             security-scoped bookmark, or a plain path), stored in the server
//             profile's existing identity-file field. The bytes are read on
//             demand at connect time and never copied into app storage.
//   SAVE   -> after a paste has been imported, and only on a separate deliberate
//             tap, ch::MobileKeyStore::saveKeyOnDevice() copies it into this
//             app's private storage so it survives a relaunch. There is no
//             pre-ticked box and no implicit path into this: a user who never
//             taps the button is in exactly the pre-amendment situation.
//
// THE SAVE WARNING IS NOT SOFTENED. The file is NOT encrypted - the store has no
// passphrase of its own to encrypt it with, and inventing one would be a lock
// whose key sits next to it. Anyone holding the unlocked phone can read it. The
// sheet says that, in those words, next to the button, because a user cannot
// weigh "survives a relaunch" against a risk nobody named.
//
// THE PASTED TEXT IS NEVER ECHOED BACK. On a successful import the text area is
// cleared and the sheet shows the FINGERPRINT instead — a digest of the public
// half, which is safe to display and is the thing a user actually needs to
// compare against the server's authorized_keys. No branch of this file logs the
// key, assigns it to a property that outlives the import, or puts it back into a
// visible item.
//
// SECURITY (SPEC 7.5): every string that came from a file or from the store is
// rendered with textFormat: Text.PlainText. Nothing here is markup.
Dialog {
    id: root

    // ch::MobileKeyStore, or null when this sheet is loaded bare.
    property var keyStoreRef: null

    signal imported(string name)
    signal removed(string name)

    modal: true
    closePolicy: Popup.CloseOnEscape
    width: Math.min(parent ? parent.width - 32 : 360, 480)
    title: qsTr("Private keys")

    // ---- transient state ---------------------------------------------------
    //
    // `preview` is the DESCRIPTION of the text currently in the box: format, key
    // type, whether a passphrase will be needed, and the fingerprint. It is
    // derived; the key itself is never held in a property here.
    property var preview: null
    property string message: ""

    // The last picked file, kept only so a pick that could not be turned into a
    // durable reference can still be used for this session on a second tap. Not
    // key material: a location.
    property url pickedUrl: ""
    property bool pickedNeedsFallback: false

    // Confirmation target for a delete. Empty when no confirmation is pending —
    // dropping a credential is not undoable for a pasted key, so it never happens
    // on one tap.
    property string pendingDeleteName: ""
    // Which of the two deletions is pending, because they are not the same act
    // and must not share one sentence: forgetting a REFERENCE leaves the user's
    // own file alone, while forgetting a SAVED key erases the copy this app
    // wrote. The confirmation says which one it is about to do.
    property bool pendingDeleteFile: false

    // The just-imported in-memory key that can still be offered for saving, or
    // empty when there is nothing to offer. A NAME, never key material: the
    // bytes stay in the store, and saving asks the store to write its own copy.
    //
    // Only set for a paste. A referenced key has nothing to save - its bytes are
    // in the user's file by construction - and a key already on the device has
    // nothing left to do.
    property string savableName: ""

    // Bumped whenever a save or a forget changes what keyInfo() would answer
    // WITHOUT changing allKeyNames. The delegate reads keyInfo() imperatively,
    // so nothing else would invalidate that binding and a row would keep
    // describing the state it had before the tap.
    property int keyInfoRevision: 0

    // Armed while the save offer below still has to be brought on screen.
    //
    // WHY THE SHEET HAS TO SCROLL AT ALL, AND WHY contentY IS NOT SAFE TO
    // ASSUME. sheetFlick's HEIGHT is a function of its own CONTENT height: it
    // reports sheetColumn.implicitHeight as its implicitHeight, and the Dialog
    // takes height: Math.min(implicitHeight, maxSheetHeight). Both scroll
    // bounds therefore move whenever the column does.
    // QQuickFlickablePrivate::fixup() (qquickflickable.cpp) returns an
    // out-of-bounds Flickable to minYExtent - contentY 0 - as soon as the
    // content fits the viewport, and it is called from setContentHeight() and
    // from geometryChange(), i.e. on every content change and every resize.
    // Measured in tst_mobileshell: with the sheet scrolled to contentY 68,
    // growing the window from 412x892 to 412x1400 lifts the cap, makes
    // height == contentHeight == 780, and leaves contentY at 0. On Android that
    // resize is the soft keyboard opening and closing under adjustResize, which
    // is exactly when a user is typing in the paste box.
    //
    // That reset is Qt doing the right thing with the bounds it was handed - a
    // scroll position into content that now fits is meaningless - so nothing
    // here suppresses it. What was missing is anything that puts the part the
    // user is looking at back on screen afterwards, and that is what the two
    // reveals below are.
    property bool pendingSaveReveal: false

    // The ONE scrolling rule in this file: reveal [top, top + revealHeight) in
    // sheetFlick.contentItem coordinates, moving no further than it takes, and
    // not moving at all when it is already visible. Nothing else here assigns
    // contentY, so there is no second scroll for this one to fight.
    function revealInSheet(top, revealHeight) {
        if (sheetFlick.height <= 0 || revealHeight <= 0)
            return
        var y = sheetFlick.contentY
        if (top < y) {
            y = top
        } else if (top + revealHeight > y + sheetFlick.height) {
            // Math.min pins the TOP of anything taller than the viewport: the
            // save offer leads with the question it is asking, and scrolling to
            // its bottom edge would show two buttons and no question.
            y = Math.min(top, top + revealHeight - sheetFlick.height)
        } else {
            return
        }
        sheetFlick.contentY =
            Math.max(0, Math.min(y, Math.max(0, sheetFlick.contentHeight
                                                - sheetFlick.height)))
    }

    function revealItemInSheet(item) {
        if (!item || !item.visible || item.height <= 0)
            return
        root.revealInSheet(sheetFlick.contentItem.mapFromItem(item, 0, 0).y,
                           item.height)
    }

    // Bring the save offer on screen, and keep it there until the user has
    // answered it or moved on.
    //
    // It appears at the BOTTOM of a capped, scrolling column, which is where it
    // belongs: it is about the key that was just pasted, so it has to come after
    // the paste box and the import button. Off screen is where that put it, and
    // the report was that tapping import appeared to do nothing at all.
    //
    // Re-run on every layout pass rather than done once, because a ColumnLayout
    // settles over several passes and the geometry the Frame reports on the
    // first one is not its final one. Measured: the Frame reports its full
    // 253px height while still sitting at its pre-layout y, which is why an
    // earlier version of this that disarmed as soon as the Frame LOOKED visible
    // stopped one pass early and left the offer 77px below the fold.
    //
    // So arming is ended by the USER instead - a drag of the sheet, or putting
    // the cursor back in the paste box, or answering the offer - and never by a
    // geometry that a later pass can contradict. Once the offer really is on
    // screen every further pass is a no-op, because revealInSheet() does not
    // move a rectangle that is already visible.
    function revealSaveOffer() {
        if (!root.pendingSaveReveal)
            return
        root.revealItemInSheet(saveKeyOnDeviceFrame)
    }

    // Keep the text cursor on screen while the paste box is being typed into.
    //
    // cursorRectangle is in keyText's OWN coordinates, and keyText is as tall as
    // the key it holds - keyTextScroll is the 140px window onto it. So the
    // cursor is mapped into the sheet and then clamped to that window: an inner
    // view that has not caught up with the cursor yet must not be able to ask
    // the sheet to scroll to a place the box does not occupy.
    function revealKeyCursor() {
        if (!keyText.activeFocus || root.pendingSaveReveal)
            return
        var box = sheetFlick.contentItem.mapFromItem(keyTextScroll, 0, 0).y
        var rect = keyText.cursorRectangle
        var y = sheetFlick.contentItem.mapFromItem(keyText, 0, rect.y).y
        root.revealInSheet(Math.max(box, Math.min(y, box + keyTextScroll.height
                                                     - rect.height)),
                           rect.height)
    }

    // After any bounds change the position Qt left the Flickable at may show
    // neither of the two things that matter, so re-assert them in priority
    // order: the offer the user has just been asked about, otherwise the cursor
    // they are typing at.
    function revealWhatMatters() {
        if (root.pendingSaveReveal) {
            root.revealSaveOffer()
            return
        }
        root.revealKeyCursor()
    }

    // Empty the paste box WITHOUT leaving the key recoverable.
    //
    // TextArea.clear() reaches QQuickTextControl::clear(), which selects the whole
    // document and deletes the selection as an UNDOABLE edit: the key text stays
    // in the QTextDocument's undo stack and keyText.undo() puts it straight back
    // on screen, for as long as this sheet exists — which is as long as the app
    // runs. Assigning the text property instead goes through setPlainText(),
    // which disables and truncates the undo stack on the way. It only takes
    // effect when the value CHANGES, so an already-empty box is forced through a
    // change first, and a box the user emptied by hand is purged as well.
    function clearInput() {
        if (keyText.text.length === 0)
            keyText.text = " "
        keyText.text = ""
        root.preview = null
    }

    onOpened: {
        root.clearInput()
        root.message = ""
        root.pendingDeleteName = ""
        root.pendingDeleteFile = false
        root.savableName = ""
        root.pendingSaveReveal = false
        root.pickedUrl = ""
        root.pickedNeedsFallback = false
        nameField.clear()
    }
    onClosed: root.clearInput()

    // Validate what is in the box WITHOUT importing it. The store's
    // describeText() runs the same describeKeyText() the import runs, so a
    // preview that says yes cannot be followed by an import that says no.
    function refreshPreview() {
        if (!root.keyStoreRef || keyText.text.length === 0) {
            root.preview = null
            return
        }
        root.preview = root.keyStoreRef.describeText(keyText.text)
    }

    function suggestedName() {
        var typed = nameField.text.trim()
        if (typed.length > 0)
            return typed
        // A key type makes a better default than "key1": a user with an ed25519
        // and an RSA key can tell them apart in the picker.
        if (root.preview && root.preview.valid && String(root.preview.keyType).length > 0)
            return String(root.preview.keyType).replace(/[^A-Za-z0-9._-]/g, "-")
        return "pasted-key"
    }

    // Both import paths report only their FAILURE here. Success is reported by the
    // store's own keyImported(name, referenced) signal below, which is the only
    // place that knows the settled name — a file import may derive one when the
    // name field was left blank, and guessing it back from the sorted name list
    // would name the wrong key.
    function doPasteImport() {
        if (!root.keyStoreRef)
            return
        if (!root.keyStoreRef.importKeyFromText(root.suggestedName(), keyText.text))
            root.message = root.keyStoreRef.lastError
    }

    function doReference(fileUrl) {
        if (!root.keyStoreRef)
            return
        root.pickedUrl = fileUrl
        root.pickedNeedsFallback = false
        if (root.keyStoreRef.addReferenceFromFile(fileUrl, nameField.text.trim()).length > 0)
            return
        root.message = root.keyStoreRef.lastError
        // A file that is a valid key but cannot be REMEMBERED (a provider that
        // refuses a persistable grant, a platform that has no bookmark) is still
        // perfectly usable for this session. Offer that instead of a dead end —
        // but ONLY for that failure. Offering it after "this is a public key" or
        // "that name is taken" would put a button on screen whose only possible
        // outcome is the identical refusal, which the store answers by telling us
        // which kind of failure this was.
        root.pickedNeedsFallback = root.keyStoreRef.lastFailureAllowsSessionOnly()
    }

    function doSessionOnlyFile() {
        if (!root.keyStoreRef)
            return
        root.pickedNeedsFallback = false
        if (!root.keyStoreRef.importKeyFromFile(root.pickedUrl, nameField.text.trim()))
            root.message = root.keyStoreRef.lastError
    }

    // The one-line description of WHERE a key lives, and the only place that
    // decides it. Lifted out of the list delegate so the three states are named
    // once - a ListView with no model creates no delegate, so inlined branches
    // there could not be checked without a live store behind them.
    //
    // Ordered by how much of the key sits on this device: none of it (a
    // reference to the user's own file), this session only (a paste), or a file
    // this app wrote and will read again (a save).
    function keyStateLabel(info) {
        if (!info || !info.name)
            return qsTr("unavailable")
        var parts = []
        if (info.referenced)
            parts.push(qsTr("your file, referenced"))
        else if (info.saved)
            parts.push(qsTr("saved on this device"))
        else
            parts.push(qsTr("in memory, this session"))
        if (info.encrypted)
            parts.push(qsTr("encrypted"))
        if (info.fingerprintAvailable)
            parts.push(String(info.fingerprint))
        return parts.join("  \u00b7  ")
    }

    // Write the already-imported key to app-private storage. Reached only from
    // the button below, which is only on screen after an import the user asked
    // for - there is no code path that saves a key the user did not tap to save.
    function doSaveOnDevice(name) {
        if (!root.keyStoreRef)
            return
        if (!root.keyStoreRef.saveKeyOnDevice(name)) {
            root.message = root.keyStoreRef.lastError
            return
        }
        // The offer goes away rather than staying to be tapped twice: the row in
        // the list above now reads "saved on this device", which is the standing
        // answer, and the only remaining action on it is deleting the file.
        root.savableName = ""
        root.keyInfoRevision += 1
        root.message = qsTr("Saved \"%1\" on this device. The file is not encrypted; anyone who can unlock this phone can read it.").arg(name)
    }

    Connections {
        target: root.keyStoreRef
        ignoreUnknownSignals: true

        function onKeyImported(name, referenced) {
            // Cleared FIRST: the sheet must not hold the key text for a moment
            // longer than the import needed it, and it is never put back.
            root.clearInput()
            nameField.clear()
            root.pickedNeedsFallback = false
            // A paste, and only a paste, leaves something to offer. A referenced
            // key already lives in the user's own file, and an import that came
            // back already saved (a re-import of a name the store has on disk)
            // has nothing left to write.
            var info = root.keyStoreRef ? root.keyStoreRef.keyInfo(name) : null
            root.savableName = (referenced || (info && info.saved === true)) ? "" : name
            // Armed here rather than off the Frame's own visibility, so a
            // referenced import - which offers nothing - never scrolls anywhere.
            root.pendingSaveReveal = root.savableName.length > 0
            root.keyInfoRevision += 1
            root.message = referenced
                ? qsTr("Remembered the location of \"%1\". The key file stays where you keep it; CodeHarbor stored only a reference to it.").arg(name)
                : qsTr("Loaded \"%1\" for this session. Nothing was written to storage.").arg(name)
            root.imported(name)
        }
    }

    // QtQuick.Dialogs' FileDialog is the module Qt 6.10 ships for every platform
    // including Android and iOS, where it maps onto the system document picker.
    // It is the one file-choosing API available here at all: the mobile client
    // links no QtWidgets, so QFileDialog does not exist in this build. It hands
    // back a content: URI on Android and a security-scoped URL on iOS, which
    // ch::keyref::makeDurableReference() turns into a durable reference.
    FileDialog {
        id: filePicker
        objectName: "keyFilePicker"
        title: qsTr("Choose a private key file")
        // No name filter: OpenSSH private keys have no extension, so filtering by
        // one would hide exactly the files this picker exists to find.
        onAccepted: root.doReference(selectedFile)
    }

    // Scrollable, and CAPPED.
    //
    // Without a height cap the ColumnLayout's implicit height wins and the
    // Dialog grows past the window; the footer is pinned to the dialog, so the
    // Close button goes with it - off the bottom of the screen, and on Android
    // underneath the navigation bar where nothing can reach it. This sheet is
    // the longest surface in the client (explanation, key list, file pick, name,
    // paste box, preview, fingerprint), so it is the one that overflows first.
    //
    // The cap leaves the content taller than the space available, hence the
    // Flickable: the body scrolls while the footer stays put.
    //
    // The reserved space is the SYSTEM's, not a number picked here. This matters
    // on Android 15: the app targets SDK 35, where edge-to-edge is enforced, so
    // the window extends BEHIND the navigation bar and "inside the window" is
    // not the same as "reachable". A fixed margin would keep the footer inside
    // the window and still under the bar.
    //
    // Taken from the HOST, not from SafeArea here, and that is not a preference
    // - it is measured. With a bottom inset present, a plain Item in the
    // window's tree reports it while a Popup and a Popup's contentItem both
    // report zero, and `Window.window` is documented for Items only: on a
    // Dialog it warns and yields null. So the item that opens this sheet reads
    // the value and passes it in; ConnectPage does exactly that.
    property real safeTopInset: 0
    property real safeBottomInset: 0

    readonly property real safeTop: safeTopInset
    readonly property real safeBottom: safeBottomInset

    // Keep the popup's own box out of the insets, so the footer cannot be
    // positioned into the navigation bar even when the content is short.
    topMargin: safeTop
    bottomMargin: safeBottom

    // The 16-above-and-below breathing room is ON TOP of the insets, mirroring
    // the 32 this dialog already subtracts from the parent WIDTH. Literals
    // rather than MobileTheme because every other metric in this file is one.
    readonly property real maxSheetHeight:
        Math.max(240, (parent ? parent.height : 640) - safeTop - safeBottom - 32)
    height: Math.min(implicitHeight, maxSheetHeight)

    contentItem: Flickable {
        id: sheetFlick

        clip: true
        // Reported to the Dialog so implicitHeight still describes the content
        // it WANTS; the cap above is what decides the height it gets.
        implicitHeight: sheetColumn.implicitHeight
        contentWidth: width
        contentHeight: sheetColumn.implicitHeight
        boundsBehavior: Flickable.StopAtBounds

        // These two are the layout passes the save offer's geometry settles
        // over, and they are also the passes Qt's own fixup() zeroes contentY
        // in. Re-asserting here is what makes the offer arrive on screen
        // whichever pass lands last, and what puts the cursor back after a
        // keyboard-driven resize; both reveals no-op when nothing is pending.
        onContentHeightChanged: root.revealWhatMatters()
        onHeightChanged: root.revealWhatMatters()

        // The user taking the sheet somewhere else ends the offer's claim on the
        // scroll position. movementStarted covers a drag and a flick and NOT a
        // programmatic contentY, which is what the reveals themselves use.
        onMovementStarted: root.pendingSaveReveal = false

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: sheetColumn
            width: sheetFlick.width
            spacing: 12

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                font.pixelSize: 12
                text: qsTr("There is no ssh-agent and no ~/.ssh on this device. "
                           + "Three ways to give CodeHarbor a key: paste one and use "
                           + "it only until the app closes, point it at a key file you "
                           + "keep yourself so only the location is remembered, or - "
                           + "after pasting - save the key on this device, which "
                           + "writes it to storage unencrypted.")
            }

            // ---- keys currently available ------------------------------------
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: qsTr("Available keys")
                visible: keyList.count > 0
            }
            ListView {
                id: keyList
                objectName: "keyList"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(count * 56, 168)
                visible: count > 0
                clip: true
                model: root.keyStoreRef ? root.keyStoreRef.allKeyNames : []
                delegate: Item {
                    id: keyRow
                    required property var modelData
                    width: keyList.width
                    height: 56
                    // A row is not a button. It must NOT express that as an
                    // ItemDelegate with `enabled: false`, which is what it used
                    // to be: Qt propagates EFFECTIVE enabled down the item tree,
                    // so a disabled row disables the Remove and Delete file
                    // buttons inside it however they set their own `enabled`.
                    // That made every action in this list dead on a device while
                    // still rendering, and it hid a real bug behind a comment.
                    // A plain Item has no click of its own to suppress, so the
                    // only tappable things in a row are the two buttons.

                    // keyInfoRevision is read for its side effect on this
                    // binding: saving or deleting a file changes what keyInfo()
                    // answers while leaving allKeyNames alone, and nothing else
                    // here would make this re-evaluate.
                    readonly property var info: (root.keyInfoRevision,
                                                 root.keyStoreRef
                                                 ? root.keyStoreRef.keyInfo(String(modelData)) : null)

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                text: String(keyRow.modelData)
                            }
                            Label {
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                font.pixelSize: 11
                                font.family: "monospace"
                                text: root.keyStateLabel(keyRow.info)
                            }
                        }
                        // Two different acts, so two different buttons, and never
                        // both at once. "Remove" forgets what this app knows and
                        // leaves every file where it is; "Delete file" erases the
                        // copy this app wrote, which is the only case where a tap
                        // here destroys a file. Naming them the same would let a
                        // user reach for the familiar one and lose the key.
                        Button {
                            objectName: "deleteKeyButton"
                            Layout.minimumHeight: 48
                            Layout.minimumWidth: 48
                            flat: true
                            visible: !(keyRow.info && keyRow.info.saved === true)
                            text: qsTr("Remove")
                            onClicked: {
                                root.pendingDeleteFile = false
                                root.pendingDeleteName = String(keyRow.modelData)
                            }
                        }
                        Button {
                            objectName: "deleteSavedKeyFileButton"
                            Layout.minimumHeight: 48
                            Layout.minimumWidth: 48
                            flat: true
                            visible: keyRow.info && keyRow.info.saved === true
                            text: qsTr("Delete file")
                            onClicked: {
                                root.pendingDeleteFile = true
                                root.pendingDeleteName = String(keyRow.modelData)
                            }
                        }
                    }
                }
            }

            // Removal confirmation, inline so the row it refers to is still visible.
            // Never a one-tap action: a pasted key cannot be recovered from here at
            // all, a reference has to be re-picked through the system dialog, and a
            // saved key's file is gone for good once the file is unlinked.
            Frame {
                Layout.fillWidth: true
                visible: root.pendingDeleteName.length > 0
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8
                    Label {
                        objectName: "deleteConfirmLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text: root.pendingDeleteFile
                              ? qsTr("Delete the saved key file for \"%1\"? The copy CodeHarbor wrote to this device is erased and cannot be recovered here.")
                                .arg(root.pendingDeleteName)
                              : qsTr("Remove the key \"%1\"? Your own key file is not deleted, but CodeHarbor will forget it.")
                                .arg(root.pendingDeleteName)
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Button {
                            objectName: "deleteCancelButton"
                            Layout.fillWidth: true
                            Layout.minimumHeight: 48
                            flat: true
                            text: qsTr("Keep")
                            onClicked: {
                                root.pendingDeleteName = ""
                                root.pendingDeleteFile = false
                            }
                        }
                        Button {
                            objectName: "deleteConfirmButton"
                            Layout.fillWidth: true
                            Layout.minimumHeight: 48
                            text: root.pendingDeleteFile ? qsTr("Delete file")
                                                         : qsTr("Remove")
                            onClicked: {
                                var name = root.pendingDeleteName
                                var deleteFile = root.pendingDeleteFile
                                root.pendingDeleteName = ""
                                root.pendingDeleteFile = false
                                if (!root.keyStoreRef)
                                    return
                                if (name === root.savableName)
                                    root.savableName = ""
                                var ok = deleteFile ? root.keyStoreRef.forgetSavedKey(name)
                                                    : root.keyStoreRef.removeKey(name)
                                root.keyInfoRevision += 1
                                if (ok) {
                                    root.message = deleteFile
                                        ? qsTr("Deleted the saved key file for \"%1\".").arg(name)
                                        : qsTr("Removed \"%1\".").arg(name)
                                    root.removed(name)
                                } else {
                                    root.message = root.keyStoreRef.lastError
                                }
                            }
                        }
                    }
                }
            }

            MenuSeparator { Layout.fillWidth: true }

            // ---- add ---------------------------------------------------------
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: qsTr("Name")
            }
            TextField {
                id: nameField
                objectName: "keyNameField"
                Layout.fillWidth: true
                Layout.minimumHeight: 48
                placeholderText: qsTr("letters, digits, dots, dashes, underscores")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            Button {
                objectName: "pickKeyFileButton"
                Layout.fillWidth: true
                Layout.minimumHeight: 48
                text: qsTr("Choose a key file I keep\u2026")
                onClicked: filePicker.open()
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                font.pixelSize: 11
                text: qsTr("The file stays in your storage. CodeHarbor remembers only "
                           + "where it is and reads it when you connect.")
            }
            Button {
                objectName: "useFileOnceButton"
                Layout.fillWidth: true
                Layout.minimumHeight: 48
                flat: true
                visible: root.pickedNeedsFallback
                text: qsTr("Use that file just for this session")
                onClicked: root.doSessionOnlyFile()
            }

            MenuSeparator { Layout.fillWidth: true }

            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: qsTr("Or paste the private key")
            }
            ScrollView {
                id: keyTextScroll
                Layout.fillWidth: true
                Layout.preferredHeight: 140
                TextArea {
                    id: keyText
                    objectName: "keyTextArea"
                    wrapMode: TextEdit.WrapAnywhere
                    font.family: "monospace"
                    font.pixelSize: 11
                    // PlainText on an EDITOR too: TextEdit.AutoText would interpret a
                    // pasted "<" as markup and silently alter the very bytes being
                    // validated (SPEC 7.5 bans AutoText outright on mobile).
                    textFormat: TextEdit.PlainText
                    placeholderText: "-----BEGIN OPENSSH PRIVATE KEY-----"
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoAutoUppercase
                                      | Qt.ImhNoPredictiveText
                    onTextChanged: root.refreshPreview()
                    // Follow the cursor, and only while this box is the thing
                    // being typed into: the signal also fires when clearInput()
                    // empties the box after an import, and following it then
                    // would drag the view off the save offer that import just
                    // raised.
                    onCursorRectangleChanged: root.revealKeyCursor()
                    // Going back to the box is the other way the user ends the
                    // save offer's claim on the scroll position: from here on
                    // the cursor is the thing that has to stay visible.
                    onActiveFocusChanged: if (activeFocus)
                                              root.pendingSaveReveal = false
                }
            }

            // What the pasted text actually is, or why it was refused. Straight from
            // ch::MobileKeyStore::describeText().
            Label {
                objectName: "previewLabel"
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                font.pixelSize: 12
                text: {
                    if (!root.preview)
                        return ""
                    if (!root.preview.valid)
                        return String(root.preview.error)
                    var parts = [String(root.preview.format)]
                    if (String(root.preview.keyType).length > 0)
                        parts.push(String(root.preview.keyType))
                    parts.push(root.preview.encrypted
                               ? qsTr("passphrase required")
                               : qsTr("no passphrase"))
                    return parts.join("  \u00b7  ")
                }
            }
            Label {
                objectName: "previewFingerprintLabel"
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.WrapAnywhere
                textFormat: Text.PlainText
                font.family: "monospace"
                font.pixelSize: 11
                // The PUBLIC half's digest. Safe to show, and the only thing about the
                // key this sheet ever displays.
                text: (root.preview && root.preview.valid)
                      ? String(root.preview.fingerprint) : ""
            }

            // Deliberately NOT a DialogButtonBox AcceptRole button: that closes the
            // dialog, and a refused import would then report its reason to a sheet the
            // user can no longer see. Importing leaves the sheet open, so the
            // fingerprint of what just landed — or the reason nothing did — is on
            // screen next to the box it came from.
            Button {
                objectName: "importButton"
                Layout.fillWidth: true
                Layout.minimumHeight: 48
                text: qsTr("Use pasted key for this session")
                enabled: root.preview !== null && root.preview.valid === true
                onClicked: root.doPasteImport()
            }

            Label {
                objectName: "importMessageLabel"
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: root.message
            }

            // ---- keep it after this session ----------------------------------
            //
            // A SEPARATE, SECOND tap, after the import already succeeded. Not a
            // checkbox on the import button: a box next to "Use pasted key"
            // would be armed before the user has seen the fingerprint or read
            // what saving costs, and a box is the shape people tick without
            // reading. Not a default either - the section is not even on screen
            // until there is a session-only key to offer, and the store writes
            // nothing unless this button is pressed.
            //
            // It stays INSIDE sheetColumn so it scrolls with the rest, and it
            // stays AFTER the import button because it is about the key that
            // button just loaded; the sheet scrolls to it instead of moving it
            // somewhere it would read out of order. The Dialog's height cap and
            // its pinned footer are untouched.
            Frame {
                id: saveKeyOnDeviceFrame
                objectName: "saveKeyOnDeviceFrame"
                Layout.fillWidth: true
                visible: root.savableName.length > 0
                // The frame's own settling passes, which the Flickable's two
                // signals do not necessarily cover: a Frame that grows inside an
                // already-tall enough column changes no scroll bound at all.
                onHeightChanged: root.revealSaveOffer()
                onYChanged: root.revealSaveOffer()
                // Answered, or dropped with the key it referred to: nothing left
                // to scroll to, and the cursor may have the position back.
                onVisibleChanged: if (!visible) root.pendingSaveReveal = false
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text: qsTr("Keep \"%1\" after this session?").arg(root.savableName)
                    }
                    Label {
                        objectName: "saveKeyWarningLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        // Every clause here is a fact about what the store does,
                        // and none of it is hedged. The passphrase sentence is
                        // included because otherwise saving looks like it removes
                        // the per-launch prompt, and it does not.
                        text: qsTr("The key is written to this app's private storage. "
                                   + "It is not encrypted: anyone who can unlock this "
                                   + "phone can read it, and so can anyone who can read "
                                   + "this app's files. A passphrase, if the key has one, "
                                   + "is still never stored and is still asked for every "
                                   + "time you connect.")
                    }
                    Button {
                        objectName: "saveKeyOnDeviceButton"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 48
                        // Never pre-armed and never the default action: the
                        // enabled state comes from there being a session-only key
                        // and a live store, nothing else.
                        enabled: root.keyStoreRef !== null && root.savableName.length > 0
                        text: qsTr("Save this key on this device")
                        onClicked: root.doSaveOnDevice(root.savableName)
                    }
                    Button {
                        objectName: "keepSessionOnlyButton"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 48
                        flat: true
                        // Dismisses the OFFER, not the key: the import already
                        // happened and the key stays usable for this session.
                        text: qsTr("No, this session only")
                        onClicked: root.savableName = ""
                    }
                }
            }
        }
    }

    footer: DialogButtonBox {
        Button {
            objectName: "importCloseButton"
            text: qsTr("Close")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }
}
