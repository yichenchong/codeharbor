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
// NOTHING HERE WRITES A PRIVATE KEY ANYWHERE. SPEC 11.2 (docs/SPEC.md:1710-1729)
// is an exhaustive allowlist of client-stored local state and private-key bytes
// are not on it; its only credential entry is "SSH-agent or credential-store
// references" (:1721) plus "an optional local identity-file path" (:1727). So the
// sheet offers exactly the two compliant shapes, and no third one:
//
//   PASTE  -> the bytes live in memory for this session and are gone when the app
//             closes. The cost is retyping/re-pasting per launch, which is the
//             same trade SPEC 12.1 (docs/SPEC.md:1798-1805) already accepts for
//             passphrases in exchange for persisting no secret at all.
//   CHOOSE -> the key stays in the USER's storage and the client remembers only a
//             REFERENCE to it (an Android persistable read grant, an iOS
//             security-scoped bookmark, or a plain path), stored in the server
//             profile's existing identity-file field. The bytes are read on
//             demand at connect time and never copied into app storage.
//
// There is deliberately no "remember this key on this device" option, because
// there is nowhere compliant to remember it.
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

    Connections {
        target: root.keyStoreRef
        ignoreUnknownSignals: true

        function onKeyImported(name, referenced) {
            // Cleared FIRST: the sheet must not hold the key text for a moment
            // longer than the import needed it, and it is never put back.
            root.clearInput()
            nameField.clear()
            root.pickedNeedsFallback = false
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

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            font.pixelSize: 12
            text: qsTr("There is no ssh-agent and no ~/.ssh on this device. "
                       + "CodeHarbor never stores a private key: paste one to use "
                       + "it for this session, or point it at a key file you keep "
                       + "yourself and only its location is remembered.")
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
            delegate: ItemDelegate {
                id: keyRow
                required property var modelData
                width: keyList.width
                height: 56
                // A row is not a button: dropping a key is the only action and it
                // has its own control, so tapping the row itself does nothing.
                enabled: false

                readonly property var info: root.keyStoreRef
                                            ? root.keyStoreRef.keyInfo(String(modelData)) : null

                contentItem: RowLayout {
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
                            text: {
                                if (!keyRow.info || !keyRow.info.name)
                                    return qsTr("unavailable")
                                var parts = []
                                parts.push(keyRow.info.referenced
                                           ? qsTr("your file, referenced")
                                           : qsTr("in memory, this session"))
                                if (keyRow.info.encrypted)
                                    parts.push(qsTr("encrypted"))
                                if (keyRow.info.fingerprintAvailable)
                                    parts.push(String(keyRow.info.fingerprint))
                                return parts.join("  \u00b7  ")
                            }
                        }
                    }
                    Button {
                        objectName: "deleteKeyButton"
                        Layout.minimumHeight: 48
                        Layout.minimumWidth: 48
                        enabled: true
                        flat: true
                        text: qsTr("Remove")
                        onClicked: root.pendingDeleteName = String(keyRow.modelData)
                    }
                }
            }
        }

        // Removal confirmation, inline so the row it refers to is still visible.
        // Never a one-tap action: a pasted key cannot be recovered from here at
        // all, and a reference has to be re-picked through the system dialog.
        Frame {
            Layout.fillWidth: true
            visible: root.pendingDeleteName.length > 0
            ColumnLayout {
                anchors.fill: parent
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: qsTr("Remove the key \"%1\"? Your own key file is not deleted, but CodeHarbor will forget it.")
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
                        onClicked: root.pendingDeleteName = ""
                    }
                    Button {
                        objectName: "deleteConfirmButton"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 48
                        text: qsTr("Remove")
                        onClicked: {
                            var name = root.pendingDeleteName
                            root.pendingDeleteName = ""
                            if (!root.keyStoreRef)
                                return
                            if (root.keyStoreRef.removeKey(name)) {
                                root.message = qsTr("Removed \"%1\".").arg(name)
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
