import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "EndpointField.js" as EndpointField

// The mobile client's FIRST screen and the only way to reach a server at all
// (SPEC 4.1, 12.1). One pane, one column, no split and no window chrome: pick or
// type an endpoint, choose a credential, connect.
//
// WHY THIS IS NOT ConnectSheet.qml. The desktop sheet is a CONNECTOR only —
// profiles are created and edited in the settings window's Server pane, and the
// sheet displays what is stored. A phone has no settings window and no second
// surface to send the user to, so this page does both jobs: it edits the one
// profile it is about and connects with it. What it deliberately does NOT do is
// keep its own profile store — every field goes into the existing
// ch::ServerProfiles through mobile.connectToServer(), whose seven-field
// whitelist is the reason a passphrase typed on this page cannot reach disk even
// by accident (ServerProfiles.h, "NO SECRET IS EVER STORED").
//
// VALIDATION IS NOT RE-DERIVED HERE either. Host and user go through
// EndpointField.js — the module's single copy of
// ServerProfiles::isUsableEndpointField(), shared verbatim with the desktop
// module rather than forked — so a value this page accepts is a value the store
// will accept. A second, drifting copy is exactly how "saved" turns into a
// profile that silently never appears.
//
// SECURITY (SPEC 7.5): every string that came from a server or from a stored
// file is rendered with textFormat: Text.PlainText. Nothing on this page is ever
// markup.
Page {
    id: root

    // The Dev Session picker to advance to. MobileMain owns navigation; this page
    // only reports that a connection was asked for.
    signal connected()

    // ---- context-property guards ------------------------------------------
    //
    // Same rule the desktop QML follows: a page loaded bare — by a test, or by a
    // tool that renders one file — must degrade to inert chrome instead of
    // throwing on a missing context property.
    readonly property var mobileController: (typeof mobile !== "undefined") ? mobile : null
    readonly property var keys: (typeof keyStore !== "undefined") ? keyStore : null
    readonly property var appController: (typeof app !== "undefined") ? app : null
    readonly property var profileStore: root.appController && root.appController.serverProfiles
                                        ? root.appController.serverProfiles : null

    // ---- form state --------------------------------------------------------
    property string profileId: ""
    property string hostText: ""
    property string portText: "22"
    property string userText: ""
    property string nodePathText: ""
    property string repoRootText: ""
    // "" means "no key: let the ladder reach password / keyboard-interactive".
    property string selectedKeyName: ""

    readonly property bool portValid: {
        var trimmed = root.portText.trim()
        if (trimmed.length === 0)
            return true // blank means the default, exactly as the store reads it
        var value = Number(trimmed)
        return Number.isInteger(value) && value >= 1 && value <= 65535
    }
    readonly property int portNumber: {
        var trimmed = root.portText.trim()
        return trimmed.length === 0 ? 22 : Number(trimmed)
    }
    readonly property bool formValid: EndpointField.isUsable(root.hostText)
                                      && EndpointField.isUsable(root.userText)
                                      && root.portValid

    readonly property string validationMessage: {
        if (root.formValid)
            return ""
        if (EndpointField.hasRejectedCharacters(root.hostText))
            return qsTr("The host cannot contain spaces, line breaks or control characters.")
        if (EndpointField.hasRejectedCharacters(root.userText))
            return qsTr("The user name cannot contain spaces, line breaks or control characters.")
        if (!root.portValid)
            return qsTr("The port must be a whole number between 1 and 65535.")
        return qsTr("Fill in a host and a login name to connect.")
    }

    readonly property string connectionState: root.mobileController
                                              ? root.mobileController.connectionState : ""
    readonly property bool busy: root.connectionState === "connecting"
                                 || root.connectionState === "provisioning"
                                 || root.connectionState === "hostkey"
                                 || root.connectionState === "credential"

    // ---- profile plumbing --------------------------------------------------

    function textOf(entry, key) {
        if (!entry)
            return ""
        var value = entry[key]
        return (value === undefined || value === null) ? "" : String(value)
    }

    function profileList() {
        return root.profileStore ? root.profileStore.profiles : []
    }

    // Load a remembered profile into the form. The credential is NOT part of the
    // profile: identityFile is a path, and the name it belongs to is recovered by
    // matching that path against the store, so a profile saved with a key still
    // selects that key on the next run.
    function loadProfile(id) {
        var list = root.profileList()
        for (var i = 0; i < list.length; ++i) {
            if (root.textOf(list[i], "id") !== id)
                continue
            var entry = list[i]
            root.profileId = id
            root.hostText = root.textOf(entry, "host")
            root.portText = root.textOf(entry, "port")
            root.userText = root.textOf(entry, "user")
            root.nodePathText = root.textOf(entry, "nodePath")
            root.repoRootText = root.textOf(entry, "repoRoot")
            // The profile's identityFile is a REFERENCE to a key file the user
            // manages, never key bytes (SPEC 11.2). Re-adopting it makes the
            // remembered credential selectable again after a relaunch;
            // registerReference() is idempotent, so re-loading the same profile
            // does not accumulate duplicates.
            //
            // Only when there IS one: a password-only profile stores an empty
            // identityFile, and handing that to registerReference() makes it
            // report "There is no key reference to load" — an error the user then
            // reads on the connect page for doing nothing wrong.
            var reference = root.textOf(entry, "identityFile")
            root.selectedKeyName = (root.keys && reference.length > 0)
                ? root.keys.registerReference(reference, "")
                : ""
            return
        }
    }

    // (No path lookup: a key is identified by the reference the store hands back,
    // and registerReference() above is what turns a stored reference into a name.)

    // Everything the UI needs about the SELECTED key, read once.
    //
    // keyInfo() is a call, not a property, and for a REFERENCED key it opens the
    // user's key file and parses it. Three separate bindings used to call it — the
    // fingerprint label, the passphrase field's visibility, and the closed combo
    // box — so every re-evaluation read the private key off the user's storage
    // three times and left three copies of it in memory. This binding is the one
    // read they share. It depends on allKeyNames as well as on the selection, so
    // removing or re-importing a key refreshes it.
    readonly property var selectedKeyInfo: {
        if (!root.keys || root.selectedKeyName.length === 0)
            return null
        if (root.keys.allKeyNames.indexOf(root.selectedKeyName) < 0)
            return null
        return root.keys.keyInfo(root.selectedKeyName)
    }

    // `info` is the already-read description when the caller has one (the closed
    // combo box, which shares root.selectedKeyInfo); null makes this read it,
    // which is what the per-row delegates need.
    function keyLabel(name, info) {
        if (name.length === 0)
            return qsTr("No key (password only)")
        var details = info ? info : (root.keys ? root.keys.keyInfo(name) : null)
        if (!details || !details.name)
            return name
        var suffix = details.referenced ? qsTr("your file, referenced")
                                        : qsTr("in memory, this session")
        if (details.encrypted)
            suffix += qsTr(", passphrase required")
        return name + " \u2014 " + suffix
    }

    // Empty a field that held a secret WITHOUT leaving the secret recoverable.
    //
    // TextField.clear() routes through QQuickTextInputPrivate::clear(), which
    // deletes the text as an undoable edit: every character is pushed onto the
    // item's undo history and undo() puts them all back on screen. Assigning the
    // text property instead goes through internalSetText(), which drops that
    // history — but only when the value actually CHANGES, so a field that is
    // already empty (the user deleted the characters by hand) is forced through a
    // change first. Assigning also cancels any pending input-method preedit, so
    // nothing can commit the old text back afterwards.
    function wipeSecretField(field) {
        if (field.text.length === 0)
            field.text = " "
        field.text = ""
    }

    function keyChoices() {
        var choices = [""]
        if (root.keys)
            choices = choices.concat(root.keys.allKeyNames)
        return choices
    }

    // A disconnect wipes every in-memory key (MobileAppController::disconnect ->
    // MobileKeyStore::forgetSession), so a name selected before it can name
    // nothing afterwards. Drop the stale selection rather than leaving the field
    // showing a credential that no longer exists and that Connect would silently
    // fail to install. Durable REFERENCES survive a disconnect, so a referenced
    // key stays selected across one.
    Connections {
        target: root.keys
        function onKeyNamesChanged() {
            if (root.selectedKeyName.length === 0)
                return
            if (root.keys.allKeyNames.indexOf(root.selectedKeyName) < 0)
                root.selectedKeyName = ""
        }
    }

    // A passphrase belongs to ONE key. Changing the selection makes whatever is
    // still sitting in the field a secret for a credential this page is no longer
    // going to use, so it goes — and so does anything already armed on the store
    // under the old name, which armPassphrase() would otherwise keep parked until
    // some later attempt spent it on the wrong key.
    onSelectedKeyNameChanged: {
        root.wipeSecretField(passphraseField)
        if (root.keys)
            root.keys.forgetPassphrase()
    }

    function connectNow() {
        if (!root.formValid || !root.mobileController)
            return
        // Resolve the credential and install it on the SSH pool for THIS attempt.
        // Both credential paths end up as in-memory key material — a pasted key
        // already is, and a reference is read on demand — because nothing on this
        // client writes a key file for libssh to open.
        if (root.keys && !root.keys.applyIdentityForConnect(root.selectedKeyName))
            return
        var profile = {
            "name": EndpointField.trim(root.hostText),
            "host": EndpointField.trim(root.hostText),
            "port": root.portNumber,
            "user": EndpointField.trim(root.userText),
            // A REFERENCE to the user's own key file, never key bytes, and empty
            // for a pasted session-only key, which has nothing durable to record
            // (SPEC 11.2, docs/SPEC.md:1721 and :1727).
            "identityFile": root.keys ? root.keys.referenceFor(root.selectedKeyName) : "",
            "nodePath": root.nodePathText.trim(),
            "repoRoot": root.repoRootText.trim()
        }
        if (root.profileId.length > 0)
            profile.id = root.profileId
        root.mobileController.connectToServer(profile)
        root.connected()
    }

    // ---- credential prompts ------------------------------------------------
    //
    // MobileAppController forwards ch::AppController's credential chain
    // verbatim, so an encrypted key and a password-only server are both reachable
    // rather than dead ends. A passphrase the user typed up front is spent HERE,
    // once: takePassphrase() erases it as it hands it over (SPEC 12.1), so a
    // failed attempt asks again instead of replaying a secret that did not work.
    property string credentialKind: ""
    property string credentialPrompt: ""
    readonly property bool credentialActive: root.credentialKind.length > 0

    Connections {
        target: root.mobileController
        ignoreUnknownSignals: true

        function onCredentialPrompt(user, host, prompt, kind) {
            if (kind === "keyPassphrase" && root.keys
                && root.keys.hasArmedPassphrase(root.selectedKeyName)) {
                root.mobileController.submitCredential(
                    root.keys.takePassphrase(root.selectedKeyName), kind)
                return
            }
            root.credentialKind = kind
            root.credentialPrompt = prompt
            root.wipeSecretField(secretField)
            secretDialog.open()
        }
    }

    function submitSecret() {
        var secret = secretField.text
        var kind = root.credentialKind
        // Emptied in the same turn it is read, and never assigned to a property:
        // a QML property holding a passphrase would outlive this dialog and show
        // up in any object dump. wipeSecretField() rather than clear() because
        // clear() would leave the characters in the field's undo history.
        root.wipeSecretField(secretField)
        root.credentialKind = ""
        root.credentialPrompt = ""
        if (root.mobileController)
            root.mobileController.submitCredential(secret, kind)
    }

    function cancelSecret() {
        root.wipeSecretField(secretField)
        var kind = root.credentialKind
        root.credentialKind = ""
        root.credentialPrompt = ""
        if (root.keys)
            root.keys.forgetPassphrase()
        if (root.mobileController)
            root.mobileController.submitCredential("", kind)
    }

    // ---- host trust --------------------------------------------------------

    HostTrustSheet {
        id: hostTrust
        anchors.centerIn: Overlay.overlay
        controller: root.mobileController
        keyStoreRef: root.keys
        port: root.portNumber
    }

    // ---- key import --------------------------------------------------------

    KeyImportSheet {
        id: keyImport
        anchors.centerIn: Overlay.overlay
        keyStoreRef: root.keys
        onImported: (name) => root.selectedKeyName = name
        onRemoved: (name) => {
            if (root.selectedKeyName === name)
                root.selectedKeyName = ""
        }
    }

    // ---- layout ------------------------------------------------------------

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 12
                text: qsTr("Connect to a server")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.bold: true
            }
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: 12

            // Remembered servers, straight out of ch::ServerProfiles. No second
            // profile store exists on mobile.
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.topMargin: 12
                text: qsTr("Remembered servers")
                textFormat: Text.PlainText
                visible: root.profileList().length > 0
            }
            ComboBox {
                id: profileSelector
                objectName: "profileSelector"
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.minimumHeight: 48
                visible: root.profileList().length > 0
                model: root.profileList()
                textRole: "name"
                // Server-supplied? No — a profile name is the user's own text.
                // Plain text regardless: this page never renders markup.
                delegate: ItemDelegate {
                    required property var modelData
                    required property int index
                    width: profileSelector.width
                    height: 48
                    contentItem: Label {
                        text: root.textOf(modelData, "name")
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    // currentIndex is left to the ComboBox: this page's state is
                    // `profileId`, and loadProfile() is what publishes the
                    // choice. Assigning the index here as well would put the two
                    // out of step the moment the profile list is replaced.
                    onClicked: {
                        profileSelector.popup.close()
                        root.loadProfile(root.textOf(modelData, "id"))
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                text: qsTr("Host")
                textFormat: Text.PlainText
            }
            TextField {
                id: hostField
                objectName: "hostField"
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.minimumHeight: 48
                text: root.hostText
                placeholderText: qsTr("box.local or 10.0.0.4")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                onTextEdited: root.hostText = text
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("User")
                        textFormat: Text.PlainText
                    }
                    TextField {
                        id: userField
                        objectName: "userField"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 48
                        text: root.userText
                        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                        onTextEdited: root.userText = text
                    }
                }
                ColumnLayout {
                    Layout.preferredWidth: 96
                    Label {
                        text: qsTr("Port")
                        textFormat: Text.PlainText
                    }
                    TextField {
                        id: portField
                        objectName: "portField"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 48
                        text: root.portText
                        inputMethodHints: Qt.ImhDigitsOnly
                        onTextEdited: root.portText = text
                    }
                }
            }

            // ---- credential ----------------------------------------------
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                text: qsTr("Private key")
                textFormat: Text.PlainText
            }
            ComboBox {
                id: keySelector
                objectName: "keySelector"
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.minimumHeight: 48
                model: root.keyChoices()
                currentIndex: Math.max(0, root.keyChoices().indexOf(root.selectedKeyName))
                delegate: ItemDelegate {
                    required property var modelData
                    required property int index
                    width: keySelector.width
                    height: 48
                    contentItem: Label {
                        text: root.keyLabel(String(modelData), null)
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    // Same rule as the profile delegate: `selectedKeyName` is the
                    // selection and currentIndex follows it through its binding.
                    onClicked: {
                        keySelector.popup.close()
                        root.selectedKeyName = String(modelData)
                    }
                }
                contentItem: Label {
                    leftPadding: 12
                    text: root.keyLabel(root.selectedKeyName, root.selectedKeyInfo)
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
            }

            // The fingerprint of the chosen key, in OpenSSH's own spelling so it
            // can be compared with `ssh-keygen -lf` or with the server's
            // authorized_keys. Hidden rather than blank for a legacy PEM, whose
            // public half is not readable without the passphrase.
            Label {
                objectName: "keyFingerprintLabel"
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                visible: text.length > 0
                wrapMode: Text.WrapAnywhere
                font.family: "monospace"
                font.pixelSize: 12
                textFormat: Text.PlainText
                text: (root.selectedKeyInfo
                       && root.selectedKeyInfo.fingerprintAvailable === true)
                      ? String(root.selectedKeyInfo.fingerprint) : ""
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                spacing: 12
                Button {
                    objectName: "manageKeysButton"
                    Layout.fillWidth: true
                    Layout.minimumHeight: 48
                    text: qsTr("Add or manage keys")
                    onClicked: keyImport.open()
                }
            }

            // An optional passphrase typed up front, so an encrypted key does
            // not always cost a round trip through the prompt. Armed for ONE
            // attempt and erased when it is used or when the attempt is
            // abandoned; there is no property here that keeps it.
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                text: qsTr("Key passphrase (this attempt only, never saved)")
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                visible: passphraseField.visible
            }
            TextField {
                id: passphraseField
                objectName: "passphraseField"
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.minimumHeight: 48
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoAutoUppercase
                                  | Qt.ImhNoPredictiveText
                visible: root.selectedKeyInfo !== null
                         && root.selectedKeyInfo.encrypted === true
                // Straight into the store's one-shot slot and out of the field, so
                // the characters do not sit in a visible item's text property
                // while the handshake runs. wipeSecretField() rather than clear():
                // clear() would leave every character in this item's undo history.
                onEditingFinished: {
                    if (root.keys)
                        root.keys.armPassphrase(root.selectedKeyName, text)
                    root.wipeSecretField(passphraseField)
                }
            }

            // ---- optional server details ---------------------------------
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                text: qsTr("Remote node path (optional)")
                textFormat: Text.PlainText
            }
            TextField {
                objectName: "nodePathField"
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.minimumHeight: 48
                text: root.nodePathText
                placeholderText: qsTr("/usr/bin/node")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                onTextEdited: root.nodePathText = text
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                text: qsTr("Repository root (optional)")
                textFormat: Text.PlainText
            }
            TextField {
                objectName: "repoRootField"
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.minimumHeight: 48
                text: root.repoRootText
                placeholderText: qsTr("/srv/codeharbor")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                onTextEdited: root.repoRootText = text
            }

            // ---- status and action ---------------------------------------
            Label {
                objectName: "validationLabel"
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                visible: text.length > 0
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: root.validationMessage
            }
            // Everything the controller says about the attempt, including a
            // changed host key. PLAIN TEXT: it can carry a server-supplied host
            // name and a libssh error string (SPEC 7.5).
            Label {
                objectName: "statusLabel"
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                visible: text.length > 0
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: root.mobileController ? root.mobileController.statusText : ""
            }
            Label {
                objectName: "keyStoreErrorLabel"
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                visible: text.length > 0
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: root.keys ? root.keys.lastError : ""
            }

            Button {
                objectName: "connectButton"
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 24
                Layout.minimumHeight: 48
                enabled: root.formValid && !root.busy && root.mobileController !== null
                text: root.busy ? qsTr("Connecting\u2026") : qsTr("Connect")
                onClicked: root.connectNow()
            }
        }
    }

    // The credential prompt. A Dialog rather than a page so the attempt it is
    // parked on stays visible behind it, and modal so no control underneath can
    // be reached while ch::AppController waits for exactly this answer.
    Dialog {
        id: secretDialog
        objectName: "secretDialog"
        anchors.centerIn: Overlay.overlay
        modal: true
        closePolicy: Popup.NoAutoClose
        width: Math.min(parent ? parent.width - 32 : 320, 420)
        title: root.credentialKind === "password" ? qsTr("Account password")
                                                  : qsTr("Key passphrase")

        contentItem: ColumnLayout {
            spacing: 12
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                // Server-authored: keyboard-interactive prompts come from the
                // remote PAM stack (SPEC 7.5).
                textFormat: Text.PlainText
                text: root.credentialPrompt.length > 0
                      ? root.credentialPrompt
                      : (root.credentialKind === "password"
                         ? qsTr("The server is asking for an account password.")
                         : qsTr("Unlock the private key to continue."))
            }
            TextField {
                id: secretField
                objectName: "secretField"
                Layout.fillWidth: true
                Layout.minimumHeight: 48
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoAutoUppercase
                                  | Qt.ImhNoPredictiveText
                onAccepted: {
                    secretDialog.close()
                    root.submitSecret()
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                font.pixelSize: 12
                text: qsTr("Used for this one attempt and then discarded. "
                           + "CodeHarbor never stores passwords or passphrases.")
            }
        }

        footer: DialogButtonBox {
            Button {
                objectName: "secretCancelButton"
                text: qsTr("Cancel")
                flat: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Button {
                objectName: "secretSubmitButton"
                text: qsTr("Continue")
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }

        onAccepted: root.submitSecret()
        onRejected: root.cancelSecret()
    }
}
