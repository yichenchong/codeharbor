// Bound: the ListView delegate below reads this file's `root` id, and binding
// the component's context is what makes that resolvable statically (qmllint)
// instead of only at run time.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import CodeHarbor
import "EndpointField.js" as EndpointField

// Server connection sheet (SPEC 4.1, 12.1): the one piece of UI that lets a
// user with a fresh config reach a server at all — list saved connection
// profiles, add/edit/remove one, connect, and answer the first-use host-key
// prompt.
//
// Deliberately self-contained: it reads NO application singleton and mutates
// NOTHING. Every input arrives as a property and every user intent leaves as a
// signal, so the host (Main.qml) owns ServerProfiles, the SSH pool and the
// known-hosts store, and this file can be instantiated and driven in isolation
// by a test.
//
//   profiles         array of {id, name, host, port, user, identityFile, nodePath, repoRoot}
//   activeId         id of the profile the host considers current ("" = none)
//   connectionState  free-form status text; the words ch::AppController
//                    publishes ("disconnected"/"connecting"/"hostkey"/
//                    "credential"/"connected"/"reconnecting"/"failed") and the
//                    older libssh-level ones ("authenticating"/"hostkeycheck"/
//                    "error"/"notavailable") are recognised for colouring
//                    (case-insensitive)
//   errorText        last failure; shown as a non-blocking banner while non-empty
//   pendingHostKey   null, or {host, keyType, fingerprint} to prompt about
//   pendingCredential null, or {user, host, prompt, kind} to ask a secret for;
//                    `kind` is "password" or "keyPassphrase"
//
// Outputs
//   connectRequested(profileId)  connect using that stored profile
//   upgradeRequested(profileId)  install this client's matching remote release
//   profileSaved(fields)         create (fields.id === "") or update a profile
//   profileRemoved(id)           delete that profile
//   credentialSubmitted(secret, kind) answer pending credential; "" cancels
//   dismissed()                  user closed the sheet (Esc / Close / Cancel)
Rectangle {
    id: root

    // ---- public API -------------------------------------------------------
    property var profiles: []
    property string activeId: ""
    property string connectionState: ""
    property string errorText: ""
    // In-memory libssh trace from the current/most recent handshake. It is
    // supplied by AppController and is never written to profile settings.
    property string diagnosticText: ""
    property var pendingHostKey: null
    property var pendingCredential: null

    signal connectRequested(string profileId)
    signal upgradeRequested(string profileId)
    signal profileSaved(var fields)
    signal profileRemoved(string id)
    signal hostKeyDecision(bool accept)
    signal credentialSubmitted(string secret, string kind)
    signal dismissed()

    // ---- internal state ---------------------------------------------------
    // Profile the form is editing; empty means "new, unsaved profile".
    property string editingId: ""
    // Set while a create round-trip is in flight so the appended profile can be
    // selected as soon as the host hands back a longer list.
    property bool awaitingNewProfile: false
    property int knownProfileCount: 0
    // Keep an in-progress form intact when a background profile refresh swaps
    // the model. `profilesChanged` is not an acknowledgement that this form
    // changed; it can be another profile being added or a reconnect update.
    property bool loadingForm: false
    property bool profileDirty: false
    property bool creatingNewProfile: false

    // A host-key or credential prompt is up, so the connection this sheet is
    // about is parked on that one answer.
    //
    // The prompt panels below cover the sheet and swallow every CLICK, but that
    // is only half of it: the buttons underneath stayed enabled, and therefore
    // TAB-REACHABLE. A keyboard user could reach Close/Cancel behind an unknown
    // host-key panel and dismiss the whole sheet, leaving ch::AppController
    // waiting for a resolveHostKey() that no longer has any UI to come from —
    // an attempt stuck "connecting" forever with nothing on screen to answer.
    // Disabling the sheet body is what makes the panels actually modal.
    // Same truthiness test the two panels' `visible` bindings use, so "a panel
    // is showing" and "the sheet is blocked" can never disagree.
    readonly property bool promptActive: (root.pendingHostKey ? true : false)
                                         || (root.pendingCredential ? true : false)

    implicitWidth: 760
    implicitHeight: 480
    color: Theme.surface
    radius: Theme.radiusMedium
    border.width: 1
    border.color: Theme.borderSubtle
    // Focus follows VISIBILITY, not construction. A hidden item can still hold
    // the window's active focus (Qt does not clear focus when an item is
    // hidden), and Main.qml builds this sheet at startup with `shown` false —
    // so a plain `focus: true` handed every keystroke to an invisible sheet
    // until the user happened to click something: the sidebar's arrow keys and
    // Escape both went nowhere. Same rule LogView and SettingsWindow follow.
    focus: root.visible

    // This sheet fills the whole window on top of the three regions, but a
    // Rectangle accepts no input of its own: Qt Quick hands an unaccepted press
    // to the next item DOWN, so a click that missed one of the controls below
    // went straight through to the terminal or editor behind the sheet. On the
    // cold-start path that is a click on a workspace the user has not connected
    // to yet. Declared FIRST so every real control still hit-tests above it;
    // `wheel` too, because a stray scroll is as wrong as a stray click.
    MouseArea {
        objectName: "sheetInputShield"
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: (wheel) => wheel.accepted = true
    }

    // ---- helpers ----------------------------------------------------------
    // Never let a missing key reach a string property: assigning `undefined`
    // to one is a QML warning, and profiles come from outside this file.
    function textOf(entry, key) {
        if (!entry)
            return "";
        var value = entry[key];
        return (value === undefined || value === null) ? "" : String(value);
    }

    function profileList() {
        return root.profiles ? root.profiles : [];
    }

    function indexOfId(id) {
        if (!id)
            return -1;
        var list = root.profileList();
        for (var i = 0; i < list.length; ++i) {
            if (root.textOf(list[i], "id") === id)
                return i;
        }
        return -1;
    }

    function loadForm(entry) {
        root.loadingForm = true;
        nameField.text = root.textOf(entry, "name");
        hostField.text = root.textOf(entry, "host");
        portField.text = root.textOf(entry, "port") === "" ? "22" : root.textOf(entry, "port");
        userField.text = root.textOf(entry, "user");
        identityFileField.text = root.textOf(entry, "identityFile");
        nodePathField.text = root.textOf(entry, "nodePath");
        repoRootField.text = root.textOf(entry, "repoRoot");
        root.loadingForm = false;
        root.profileDirty = false;
    }
    function formDiffersFrom(entry) {
        if (!entry || !nameField || !hostField || !portField || !userField
                || !identityFileField || !nodePathField || !repoRootField)
            return false;
        var storedPort = root.textOf(entry, "port");
        if (storedPort === "")
            storedPort = "22";
        return root.textOf(entry, "name") !== nameField.text
                || root.textOf(entry, "host") !== hostField.text
                || storedPort !== portField.text
                || root.textOf(entry, "user") !== userField.text
                || root.textOf(entry, "identityFile") !== identityFileField.text
                || root.textOf(entry, "nodePath") !== nodePathField.text
                || root.textOf(entry, "repoRoot") !== repoRootField.text;
    }

    function selectIndex(index) {
        var list = root.profileList();
        if (index < 0 || index >= list.length)
            return;
        root.awaitingNewProfile = false;
        root.creatingNewProfile = false;
        root.editingId = root.textOf(list[index], "id");
        root.loadForm(list[index]);
        profileSelector.currentIndex = index;
    }

    function beginNew() {
        root.awaitingNewProfile = false;
        root.knownProfileCount = root.profileList().length;
        profileSelector.currentIndex = -1;
        root.editingId = "";
        root.creatingNewProfile = true;
        root.loadForm(null);
        nameField.input.forceActiveFocus();
    }

    // The ListView owns currentIndex for key navigation, but `editingId` is the
    // selection: a model swap can clamp or reset currentIndex behind our back,
    // so the highlight follows editingId and currentIndex is re-applied once the
    // new model has actually been installed.
    function applyCurrentIndex() {
        profileSelector.currentIndex = root.indexOfId(root.editingId);
    }

    // Re-anchor the selection after the host swapped the list in. Keeps editing
    // the same profile when it survived, otherwise falls back to the active one.
    //
    // Gaining a selection out of nothing (first load with saved servers, or the
    // first profile just being saved) also moves focus to the list, so the very
    // next keystroke can pick a server and Enter connects to it. An edit already
    // in progress is never interrupted.
    function syncFromModel() {
        var list = root.profileList();
        var hadSelection = root.editingId !== "";
        if (root.awaitingNewProfile && list.length > root.knownProfileCount) {
            root.awaitingNewProfile = false;
            root.knownProfileCount = list.length;
            root.selectIndex(list.length - 1); // new profiles are appended
            Qt.callLater(root.applyCurrentIndex);
            if (!hadSelection)
                profileSelector.forceActiveFocus();
            return;
        }

        root.knownProfileCount = list.length;
        var index = root.indexOfId(root.editingId);
        var dirty = root.profileDirty
                     || (root.editingId !== "" && index >= 0
                         && root.formDiffersFrom(list[index]));

        // A model refresh is not permission to overwrite a form the user is
        // still editing. Keep the ListView highlight aligned, but do not call
        // selectIndex(), because that reloads every field from the old model.
        if (dirty && root.editingId !== "" && index >= 0) {
            profileSelector.currentIndex = index;
            Qt.callLater(root.applyCurrentIndex);
            return;
        }

        if (root.creatingNewProfile) {
            // A pristine first-run form is only a placeholder. Once the host
            // supplies saved profiles, select one and give the list keyboard
            // focus. A dirty new form, however, belongs to the user and must
            // remain untouched across the same background refresh.
            if (!root.profileDirty && list.length > 0) {
                if (index < 0)
                    index = root.indexOfId(root.activeId);
                if (index < 0)
                    index = 0;
                root.selectIndex(index);
                profileSelector.forceActiveFocus();
            } else {
                Qt.callLater(root.applyCurrentIndex);
            }
            return;
        }

        if (index < 0)
            index = root.indexOfId(root.activeId);
        if (index < 0 && list.length > 0)
            index = 0;
        if (index >= 0)
            root.selectIndex(index);
        else
            root.beginNew();
        Qt.callLater(root.applyCurrentIndex);
        if (!hadSelection && root.editingId !== "")
            profileSelector.forceActiveFocus();
    }

    function portValue() {
        const text = portField.text.trim();
        if (!/^[0-9]+$/.test(text))
            return 0;
        const parsed = parseInt(text, 10);
        return isFinite(parsed) ? parsed : 0;
    }

    // The Save gate. It has to be the STORE's rule, not a looser one: whatever
    // ServerProfiles::sanitize() refuses is dropped on the floor without a
    // word, so a Save button that is enabled for a value the store will reject
    // reports success and produces nothing — no new row in the list, no error.
    // Pasting a whole command line ("box.local -p 2222") into the host field is
    // exactly how a user gets there.
    function formValid() {
        return EndpointField.isUsable(hostField.text)
            && EndpointField.isUsable(userField.text)
            && root.portValue() >= 1 && root.portValue() <= 65535;
    }

    // Why the form cannot be saved, in the user's own terms; empty when it can.
    // "Fill these in" and "what you pasted cannot be a host" are different
    // problems and a single sentence covering both tells the second user
    // nothing about what to change.
    function validationMessage() {
        if (EndpointField.hasRejectedCharacters(hostField.text))
            return qsTr("The host cannot contain spaces, line breaks or control characters.");
        if (EndpointField.hasRejectedCharacters(userField.text))
            return qsTr("The user name cannot contain spaces, line breaks or control characters.");
        if (root.formValid())
            return "";
        return qsTr("Host, user and a port in 1-65535 are required.");
    }

    function save() {
        if (!root.formValid())
            return;

        // Normalise the form to the values the store will actually hold BEFORE
        // emitting them. ServerProfiles::sanitize() trims every field and
        // defaults an empty name to the host, so leaving the raw text on screen
        // left the form permanently disagreeing with storage: the very next
        // profilesChanged() ran formDiffersFrom(), saw a difference the user
        // never typed, concluded the form was an unsaved draft and stopped
        // reconciling it from then on. `loadingForm` keeps these writes from
        // being mistaken for edits of the user's own.
        root.loadingForm = true;
        // EndpointField.trim() for EVERY field, not JavaScript's String.trim():
        // it is the module's one copy of what QString::trimmed() strips, and the
        // store trims all seven with that. The two sets are not the same (they
        // disagree on U+0085 and U+FEFF), and a field trimmed by a different
        // rule than the store's is exactly the mismatch described above.
        hostField.text = EndpointField.trim(hostField.text);
        userField.text = EndpointField.trim(userField.text);
        portField.text = String(root.portValue());
        var name = EndpointField.trim(nameField.text);
        nameField.text = name === "" ? hostField.text : name;
        identityFileField.text = EndpointField.trim(identityFileField.text);
        nodePathField.text = EndpointField.trim(nodePathField.text);
        repoRootField.text = EndpointField.trim(repoRootField.text);
        root.loadingForm = false;

        if (root.editingId === "") {
            root.awaitingNewProfile = true;
            root.knownProfileCount = root.profileList().length;
        }
        root.profileSaved({
            "id": root.editingId,
            "name": nameField.text,
            "host": hostField.text,
            "port": root.portValue(),
            "user": userField.text,
            "identityFile": identityFileField.text,
            "nodePath": nodePathField.text,
            "repoRoot": repoRootField.text
        });
        root.profileDirty = false;
    }

    function connectNow() {
        if (root.editingId !== "")
            root.connectRequested(root.editingId);
    }

    function upgradeNow() {
        if (root.editingId !== "")
            root.upgradeRequested(root.editingId);
    }

    function removeSelected() {
        if (root.editingId !== "")
            root.profileRemoved(root.editingId);
    }

    // Which secret the pending prompt is asking for. Anything the host did not
    // label "password" is treated as a private-key passphrase, so a passphrase
    // is never offered to password authentication by accident.
    function credentialKind() {
        var kind = root.textOf(root.pendingCredential, "kind");
        return kind === "password" ? "password" : "keyPassphrase";
    }

    // One sentence saying why this secret is being asked for. A server may
    // require SEVERAL methods (OpenSSH's `AuthenticationMethods
    // publickey,password`), so a password request does NOT imply the key was
    // rejected — it is often the second half of an accepted key. Saying
    // otherwise sends the user off to debug a key that is working fine.
    function credentialExplanation() {
        var target = root.textOf(root.pendingCredential, "user") + "@"
                     + root.textOf(root.pendingCredential, "host");
        if (root.credentialKind() === "password") {
            return qsTr("%1 is asking for an account password. Servers that "
                        + "require more than one authentication method ask for "
                        + "this in addition to your key.").arg(target);
        }
        return qsTr("ssh-agent and the default keys could not authenticate %1. "
                    + "Unlock a private key to continue.").arg(target);
    }

    // Hand the typed secret up and wipe it here in the same turn. This file
    // keeps no copy of it and never routes it through the profile form, so it
    // cannot reach profileSaved() and therefore cannot reach QSettings.
    function submitSecret(kind) {
        var secret = secretField.text;
        secretField.clear();
        root.credentialSubmitted(secret, kind || root.credentialKind());
    }

    function cancelSecret() {
        secretField.clear();
        root.credentialSubmitted("", root.credentialKind());
    }

    // ---- connection status vocabulary -------------------------------------
    //
    // `connectionState` is free-form text from the host. These map it onto the
    // eight states ch::AppController::setConnectionState() actually publishes
    // (disconnected / connecting / provisioning / hostkey / credential /
    // connected / reconnecting / failed), keeping the older libssh-level words
    // the pool can still surface. Every one of them needs a case in all five
    // functions below; falling through to the default paints an attempt that is
    // making progress as a grey "nothing is running".
    //
    // Every state is encoded THREE ways — colour, glyph and word — because a
    // colour-only dot is unreadable to a colour-blind user and vanishes in a
    // greyscale screenshot.
    function stateKey(state) {
        switch (String(state).toLowerCase()) {
        case "connected": return "connected";
        case "connecting":
        case "authenticating": return "connecting";
        case "hostkey":
        case "hostkeycheck": return "hostkey";
        case "credential": return "credential";
        case "provisioning": return "provisioning";
        case "reconnecting": return "reconnecting";
        case "failed":
        case "error":
        case "notavailable": return "failed";
        default: return "disconnected";
        }
    }

    function stateColor(state) {
        switch (root.stateKey(state)) {
        case "connected": return Theme.success;
        case "connecting": return Theme.warning;
        case "hostkey": return Theme.warning;
        case "credential": return Theme.warning;
        case "provisioning": return Theme.warning;
        case "reconnecting": return Theme.statusReconnecting();
        case "failed": return Theme.danger;
        default: return Theme.textDim;
        }
    }

    // Drawn inside the dot. connecting and hostkey share a colour, so the glyph
    // is the only thing that separates "dialling" from "answer me".
    function stateGlyph(state) {
        switch (root.stateKey(state)) {
        case "connected": return "\u2713";    // check
        case "connecting": return "\u2219";   // bullet operator
        case "hostkey": return "?";
        case "credential": return "*";        // the password mask
        case "provisioning": return "\u2193"; // downloading onto the server
        case "reconnecting": return "\u21bb"; // clockwise open circle arrow
        case "failed": return "\u2715";       // multiplication x
        default: return "\u2013";             // en dash: nothing is running
        }
    }

    // Round while the link is fine or merely idle; squared off for the two
    // states that are waiting on the user. Silhouette, not hue.
    function stateRadius(state) {
        switch (root.stateKey(state)) {
        case "hostkey":
        case "credential":
        case "failed": return 3;
        default: return 7;
        }
    }

    function stateBusy(state) {
        const key = root.stateKey(state);
        return key === "connecting" || key === "hostkey" || key === "credential"
            || key === "reconnecting" || key === "provisioning";
    }

    // One sentence saying what the state means for the person looking at it;
    // `stateLabel` itself echoes the host's own word verbatim.
    function stateExplanation(state) {
        switch (root.stateKey(state)) {
        case "connected": return qsTr("Linked to the server.");
        case "connecting": return qsTr("Opening the SSH connection\u2026");
        case "hostkey": return qsTr("Waiting for you to accept this server's host key.");
        case "credential": return qsTr("Waiting for a password or key passphrase.");
        case "provisioning": return qsTr("Installing the CodeHarbor service on the server\u2026");
        case "reconnecting": return qsTr("The link dropped; trying to restore it\u2026");
        case "failed": return qsTr("The last attempt failed. See the message below.");
        default: return qsTr("Not connected to any server.");
        }
    }

    onProfilesChanged: root.syncFromModel()
    onActiveIdChanged: {
        // Follow the host's active profile, but never onto a form the user is
        // editing. Selecting it would reload every field from storage and
        // discard text that has not been saved yet.
        if (root.profileDirty || root.creatingNewProfile
                || root.activeId === root.editingId)
            return;
        var index = root.indexOfId(root.activeId);
        if (index >= 0)
            root.selectIndex(index);
    }
    onPendingHostKeyChanged: {
        if (root.pendingHostKey)
            Qt.callLater(() => hostKeyReject.forceActiveFocus());
        else if (root.visible)
            Qt.callLater(() => profileSelector.forceActiveFocus());
    }
    onPendingCredentialChanged: {
        // Cleared on BOTH edges: on open so a previous attempt's keystrokes can
        // never be resubmitted, and on close so the secret is not left sitting
        // in a live QML item (and its undo stack) after it has been spent.
        secretField.clear();
        if (root.pendingCredential)
            Qt.callLater(() => secretField.forceActiveFocus());
        else if (root.visible)
            Qt.callLater(() => profileSelector.forceActiveFocus());
    }
    onVisibleChanged: {
        if (!root.visible)
            return;
        Qt.callLater(() => {
            if (root.pendingCredential)
                secretField.forceActiveFocus();
            else if (root.pendingHostKey)
                hostKeyReject.forceActiveFocus();
            else if (root.profileList().length > 0)
                profileSelector.forceActiveFocus();
            else
                nameField.input.forceActiveFocus();
        });
    }
    Component.onCompleted: root.syncFromModel()

    Keys.onEscapePressed: (event) => {
        if (root.pendingCredential)
            root.cancelSecret();
        else if (root.pendingHostKey)
            root.hostKeyDecision(false);
        else
            root.dismissed();
        event.accepted = true;
    }

    // ---- one labelled text field -----------------------------------------
    component LabeledField: Column {
        id: field
        property alias label: fieldLabel.text
        property alias text: fieldInput.text
        property alias placeholder: fieldInput.placeholderText
        property alias validator: fieldInput.validator
        property alias hint: fieldHint.text
        property alias input: fieldInput // so the host can focus the editor itself
        signal accepted()

        spacing: 3

        Label {
            id: fieldLabel
            color: Theme.statusText()
            font.pixelSize: 11
        }
        TextField {
            id: fieldInput
            width: field.width
            // The visible Label above is a SEPARATE item, so nothing ties the
            // two together for assistive technology: without this every field
            // in the one surface a user meets before any server is reachable is
            // announced as an unnamed edit box.
            Accessible.name: fieldLabel.text
            Accessible.description: fieldHint.text
            color: Theme.text
            placeholderTextColor: Theme.textPlaceholder()
            selectByMouse: true
            font.pixelSize: Theme.fontSizeLabel
            background: Rectangle {
                color: Theme.surfaceSunken
                radius: 3
                border.width: 1
                border.color: fieldInput.activeFocus ? Theme.accent : Theme.borderSubtle
            }
            onAccepted: field.accepted()
        }
        Label {
            id: fieldHint
            color: Theme.textDim
            font.pixelSize: Theme.fontSizeSmall
            visible: text.length > 0
        }
    }

    // ---- one action button ------------------------------------------------
    // The Basic style ships a deliberately plain button: a 22px box with no
    // focus ring worth the name. Both are usability problems here — this sheet
    // is reachable before any pointer device is configured, so a keyboard user
    // must be able to SEE where they are, and Connect/Remove are consequential
    // enough to deserve a real hit target.
    component SheetButton: Button {
        id: button
        property color accent: Theme.border

        implicitHeight: 30
        leftPadding: 14
        rightPadding: 14
        focusPolicy: Qt.StrongFocus

        contentItem: Label {
            textFormat: Text.PlainText
            text: button.text
            color: button.enabled ? Theme.text : Theme.textPlaceholder()
            font.pixelSize: Theme.fontSizeBody
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: Theme.radiusSmall
            color: !button.enabled ? Theme.surfaceHover
                 : button.down ? Theme.border
                 : button.hovered ? Theme.controlHoverSurface() : Theme.surfaceRaised
            // Two pixels and a bright edge: the focus ring has to be legible at
            // a glance, not a one-pixel difference against #45475a.
            border.width: button.visualFocus ? 2 : 1
            border.color: button.visualFocus ? Theme.accent
                        : button.enabled ? button.accent : Theme.borderSubtle
        }
    }

    // ---- header -----------------------------------------------------------
    Rectangle {
        id: header
        // Untouchable — by pointer AND by keyboard — while a prompt is up; see
        // root.promptActive.
        enabled: !root.promptActive
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 44
        color: Theme.surfaceDeep
        radius: Theme.radiusMedium

        // Square off the bottom corners the rounded rectangle would leave.
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 8
            color: parent.color
        }

        Label {
            text: qsTr("Servers")
            color: Theme.text
            font.bold: true
            font.pixelSize: Theme.fontSizeTitle
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
        }

        // Connection status chip. Colour, glyph and word all carry the state,
        // so it survives a greyscale screenshot and a colour-blind reader; the
        // spinner only distinguishes "something is happening" from "settled".
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            Rectangle {
                id: statusChip
                objectName: "statusChip"
                anchors.verticalCenter: parent.verticalCenter
                width: chipRow.implicitWidth + 20
                height: 26
                radius: 13
                color: Theme.surfaceSunken
                border.width: 1
                border.color: root.stateColor(root.connectionState)

                HoverHandler { id: chipHover }
                // The module's one tooltip (AppToolTip.qml). The attached
                // `ToolTip.text` form is drawn by the Basic style in that
                // style's own light palette, so an explanation of this dark
                // sheet's status chip arrived as a white box.
                AppToolTip {
                    objectName: "statusChipTip"
                    visible: chipHover.hovered
                    text: root.stateExplanation(root.connectionState)
                }

                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: 6

                    // The Basic style draws its indicator as a 48px grid of
                    // dots; scaled into a 26px chip that is literally nothing on
                    // screen. Same semantics (`running` is still the property
                    // everything reads), drawn at a size that shows up.
                    BusyIndicator {
                        id: connectingIndicator
                        objectName: "connectingIndicator"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        height: 14
                        padding: 0
                        running: root.stateBusy(root.connectionState)
                        visible: running

                        contentItem: Item {
                            implicitWidth: 14
                            implicitHeight: 14

                            Rectangle { // track
                                anchors.fill: parent
                                radius: width / 2
                                color: "transparent"
                                border.width: 2
                                border.color: root.stateColor(root.connectionState)
                                opacity: 0.3
                            }

                            Item { // orbiting pip
                                anchors.fill: parent
                                transformOrigin: Item.Center

                                Rectangle {
                                    x: parent.width / 2 - 2
                                    y: 0
                                    width: 4
                                    height: 4
                                    radius: 2
                                    color: root.stateColor(root.connectionState)
                                }

                                RotationAnimator on rotation {
                                    running: connectingIndicator.running
                                    loops: Animation.Infinite
                                    from: 0
                                    to: 360
                                    duration: 900
                                }
                            }
                        }
                    }

                    Rectangle {
                        objectName: "stateDot"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        height: 14
                        radius: root.stateRadius(root.connectionState)
                        color: root.stateColor(root.connectionState)

                        Label {
                            anchors.centerIn: parent
                            text: root.stateGlyph(root.connectionState)
                            color: Theme.textOnAccent
                            font.pixelSize: Theme.fontSizeSmall
                            font.bold: true
                        }
                    }

                    Label {
                        objectName: "stateLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        // SECURITY: see errorLabel below — connectionState is
                        // free-form text from the host, not a literal.
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: Theme.fontSizeBody
                        text: root.connectionState.length > 0 ? root.connectionState
                                                              : qsTr("disconnected")
                    }
                }
            }

            SheetButton {
                objectName: "closeButton"
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Close")
                onClicked: root.dismissed()
            }
        }
    }

    // ---- error banner -----------------------------------------------------
    // Non-blocking: it never steals focus or input. Dismissing it is a per-
    // MESSAGE acknowledgement, not a permanent mute — the flag is cleared the
    // moment the host reports a different failure, so the next one is never
    // swallowed by an earlier dismissal.
    property bool errorDismissed: false
    onErrorTextChanged: root.errorDismissed = false

    Rectangle {
        id: errorBanner
        objectName: "errorBanner"
        enabled: !root.promptActive
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 1
        visible: root.errorText.length > 0 && !root.errorDismissed
        // Grows to fit: an ssh failure is a whole sentence naming a host, a
        // port and a reason, and one elided line of it tells nobody anything.
        height: visible ? Math.max(40, errorLabel.implicitHeight + 20) : 0
        color: Theme.errorSurface()

        // A red wash is the only thing separating this from the rest of the
        // sheet; the rule and the glyph say "error" without relying on hue.
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 3
            color: Theme.danger
        }

        Label {
            id: errorGlyph
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.top: parent.top
            anchors.topMargin: 10
            text: "\u2715"
            color: Theme.danger
            font.pixelSize: Theme.fontSizeBody
            font.bold: true
        }

        Label {
            id: errorLabel
            objectName: "errorLabel"
            anchors.left: errorGlyph.right
            anchors.leftMargin: 8
            anchors.right: errorDetailsButton.visible
                           ? errorDetailsButton.left : errorDismissButton.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            wrapMode: Text.WordWrap
            // SECURITY: a Label defaults to Text.AutoText, which silently
            // promotes anything that LOOKS like markup to StyledText — and
            // StyledText honours <img src="http://..."> by fetching the URL and
            // <a href> by making the text a live link. errorText is the last
            // failure string, which on this path comes from libssh and therefore
            // carries text the SERVER chose (a banner, a disconnect reason). A
            // hostile server must not be able to turn the error banner into a
            // network callback. It is data: draw it as data.
            textFormat: Text.PlainText
            color: Theme.danger
            font.pixelSize: Theme.fontSizeBody
            text: root.errorText
        }

        SheetButton {
            id: errorDismissButton
            objectName: "errorDismissButton"
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            accent: Theme.danger
            text: qsTr("Dismiss")
            onClicked: root.errorDismissed = true
        }

        SheetButton {
            id: errorDetailsButton
            objectName: "sshDetailsButton"
            anchors.right: errorDismissButton.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            visible: root.diagnosticText.length > 0
            width: visible ? implicitWidth : 0
            accent: Theme.warning
            text: qsTr("Details…")
            onClicked: sshDiagnosticsDialog.open()
        }
    }

    AppDialog {
        id: sshDiagnosticsDialog
        objectName: "sshDiagnosticsDialog"
        title: qsTr("SSH connection details")
        modal: true
        standardButtons: Dialog.Close
        anchors.centerIn: Overlay.overlay
        width: Math.min(root.width - 40, 900)
        height: Math.min(root.height - 40, 620)

        contentItem: ScrollView {
            clip: true
            TextArea {
                id: sshDiagnosticsText
                objectName: "sshDiagnosticsText"
                readOnly: true
                selectByMouse: true
                wrapMode: TextArea.NoWrap
                textFormat: Text.PlainText
                text: root.diagnosticText
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeBody
            }
        }
    }

    // ---- body -------------------------------------------------------------
    Item {
        id: body
        enabled: !root.promptActive
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: errorBanner.top

        // Saved profiles.
        Rectangle {
            id: listPane
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            width: 240
            color: Theme.surfaceDeep

            ListView {
                id: profileSelector
                objectName: "profileList"
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: listActions.top
                anchors.margins: 6
                clip: true
                focus: true
                currentIndex: -1
                keyNavigationEnabled: true
                model: root.profileList()

                ScrollBar.vertical: AppScrollBar {}

                Keys.onReturnPressed: (event) => {
                    root.connectNow();
                    event.accepted = true;
                }
                Keys.onEnterPressed: (event) => {
                    root.connectNow();
                    event.accepted = true;
                }
                // Only follow a genuine move to another profile: a model swap
                // can shuffle currentIndex, and reloading the form then would
                // throw away what the user is typing.
                onCurrentIndexChanged: {
                    var list = root.profileList();
                    if (profileSelector.currentIndex < 0
                            || profileSelector.currentIndex >= list.length)
                        return;
                    // A model replacement can move currentIndex while the
                    // user is typing. Keep the draft; an explicit row click
                    // calls selectIndex() directly and remains intentional.
                    if (root.profileDirty) {
                        Qt.callLater(root.applyCurrentIndex);
                        return;
                    }
                    if (root.textOf(list[profileSelector.currentIndex], "id") !== root.editingId)
                        root.selectIndex(profileSelector.currentIndex);
                }

                delegate: ItemDelegate {
                    id: profileDelegate
                    required property int index
                    required property var modelData

                    width: profileDelegate.ListView.view ? profileDelegate.ListView.view.width : 0
                    height: 46
                    objectName: "profileRow" + profileDelegate.index
                    onClicked: root.selectIndex(profileDelegate.index)

                    // An ItemDelegate normally lends its `text` to a screen
                    // reader, but this one draws a two-line contentItem of its
                    // own and leaves `text` empty — so without these the whole
                    // list, which is the keyboard surface of the sheet a user
                    // meets before any server is reachable, is announced as a
                    // stack of anonymous rows. Same rule as the sidebar's
                    // session rows and the command palette's results.
                    Accessible.role: Accessible.ListItem
                    Accessible.name: root.textOf(profileDelegate.modelData, "name")
                    Accessible.description: root.textOf(profileDelegate.modelData, "user") + "@"
                                            + root.textOf(profileDelegate.modelData, "host") + ":"
                                            + root.textOf(profileDelegate.modelData, "port")
                    Accessible.selected: root.textOf(profileDelegate.modelData, "id")
                                         === root.editingId

                    background: Rectangle {
                        color: root.textOf(profileDelegate.modelData, "id") === root.editingId
                               ? Theme.surfaceSelected : (profileDelegate.hovered ? Theme.surfaceHover : "transparent")
                        radius: 3

                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 3
                            radius: 3
                            color: Theme.accent
                            visible: root.textOf(profileDelegate.modelData, "id") === root.activeId
                        }

                        // Keyboard cursor. The selection wash alone cannot show
                        // it: moving the cursor with Up/Down also moves the
                        // selection, so without a ring a keyboard user has no
                        // idea the list is what their arrow keys are driving.
                        Rectangle {
                            anchors.fill: parent
                            radius: 3
                            color: "transparent"
                            border.width: 2
                            border.color: Theme.accent
                            visible: profileSelector.activeFocus
                                     && profileDelegate.index === profileSelector.currentIndex
                        }
                    }

                    contentItem: Column {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        spacing: 2

                        Row {
                            width: parent.width
                            spacing: 6
                            Label {
                                objectName: "profileName" + profileDelegate.index
                                // Same rule as errorLabel: a stored profile is
                                // data (it can also arrive hand-edited from
                                // disk), never markup.
                                width: Math.max(0, parent.width - parent.spacing
                                                   - (activeBadge.visible
                                                      ? activeBadge.implicitWidth : 0))
                                textFormat: Text.PlainText
                                text: root.textOf(profileDelegate.modelData, "name")
                                color: Theme.text
                                font.pixelSize: Theme.fontSizeLabel
                                elide: Text.ElideRight
                            }
                            Label {
                                id: activeBadge
                                objectName: "activeBadge" + profileDelegate.index
                                text: qsTr("active")
                                color: Theme.accent
                                font.pixelSize: Theme.fontSizeSmall
                                anchors.verticalCenter: parent.verticalCenter
                                visible: root.textOf(profileDelegate.modelData, "id") === root.activeId
                            }
                        }
                        Label {
                            objectName: "profileEndpoint" + profileDelegate.index
                            width: parent.width
                            textFormat: Text.PlainText
                            text: root.textOf(profileDelegate.modelData, "user") + "@"
                                  + root.textOf(profileDelegate.modelData, "host") + ":"
                                  + root.textOf(profileDelegate.modelData, "port")
                            color: Theme.textDim
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Column {
                id: listEmptyState
                anchors.centerIn: profileSelector
                width: profileSelector.width - 24
                spacing: 6
                visible: root.profileList().length === 0

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "\u2601"
                    color: Theme.textFaint
                    font.pixelSize: 28
                }
                Label {
                    objectName: "emptyHint"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
                    text: qsTr("No servers yet.\nFill in the form and press Save.")
                }
            }

            Row {
                id: listActions
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.margins: 6
                spacing: 6

                SheetButton {
                    objectName: "addButton"
                    text: qsTr("Add")
                    onClicked: root.beginNew()
                }
                SheetButton {
                    objectName: "removeButton"
                    accent: Theme.danger
                    text: qsTr("Remove")
                    enabled: root.editingId !== ""
                    onClicked: root.removeSelected()
                }
            }
        }

        // Edit form for the selected (or new) profile.
        Item {
            id: formPane
            anchors.top: parent.top
            anchors.left: listPane.right
            anchors.right: parent.right
            anchors.bottom: parent.bottom

            Column {
                id: form
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 14
                spacing: 8

                // First run: an empty list and an empty form say nothing about
                // what CodeHarbor wants from you. It goes away the moment there
                // is a server to pick, so it never becomes chrome to scroll past.
                Label {
                    objectName: "coldStartIntro"
                    width: form.width
                    visible: root.profileList().length === 0
                    wrapMode: Text.WordWrap
                    color: Theme.statusText()
                    font.pixelSize: Theme.fontSizeBody
                    text: qsTr("CodeHarbor edits a checkout that lives on another machine, over SSH. "
                               + "Describe that machine below — its address, your login, and the "
                               + "absolute path to node on it — then Save it and press Connect.")
                }

                Label {
                    objectName: "formTitle"
                    text: root.editingId === "" ? qsTr("New server") : qsTr("Edit server")
                    color: Theme.text
                    font.pixelSize: Theme.fontSizeLabel
                    font.bold: true
                }

                LabeledField {
                    id: nameField
                    objectName: "nameField"
                    width: form.width
                    label: qsTr("Name")
                    placeholder: qsTr("Defaults to the host name")
                    onAccepted: root.save()
                    onTextChanged: if (!root.loadingForm) root.profileDirty = true
                }

                Row {
                    width: form.width
                    spacing: 8

                    LabeledField {
                        id: hostField
                        objectName: "hostField"
                        width: form.width - 108 - parent.spacing
                        label: qsTr("Host")
                        placeholder: qsTr("hostname or address")
                        onAccepted: root.save()
                        onTextChanged: if (!root.loadingForm) root.profileDirty = true
                    }
                    LabeledField {
                        id: portField
                        objectName: "portField"
                        width: 108
                        label: qsTr("Port")
                        text: "22"
                        validator: IntValidator { bottom: 1; top: 65535 }
                        onAccepted: root.save()
                        onTextChanged: if (!root.loadingForm) root.profileDirty = true
                    }
                }

                LabeledField {
                    id: userField
                    objectName: "userField"
                    width: form.width
                    label: qsTr("User")
                    placeholder: qsTr("login name")
                    onAccepted: root.save()
                    onTextChanged: if (!root.loadingForm) root.profileDirty = true
                }

                LabeledField {
                    id: identityFileField
                    objectName: "identityFileField"
                    width: form.width
                    label: qsTr("Private key file")
                    placeholder: qsTr("Optional; ~/.ssh/config is also read")
                    hint: qsTr("Local path. Its passphrase is requested separately and never stored.")
                    onAccepted: root.save()
                    onTextChanged: if (!root.loadingForm) root.profileDirty = true
                }

                LabeledField {
                    id: nodePathField
                    objectName: "nodePathField"
                    width: form.width
                    label: qsTr("Node path")
                    placeholder: qsTr("/usr/bin/node")
                    hint: qsTr("Absolute path to node on the server; it need not be on the login PATH.")
                    onAccepted: root.save()
                    onTextChanged: if (!root.loadingForm) root.profileDirty = true
                }

                LabeledField {
                    id: repoRootField
                    objectName: "repoRootField"
                    width: form.width
                    label: qsTr("Repository root")
                    placeholder: qsTr("/srv/codeharbor")
                    hint: qsTr("Remote CodeHarbor install providing codeharbord: an unpacked release tarball or a git checkout.")
                    onAccepted: root.save()
                    onTextChanged: if (!root.loadingForm) root.profileDirty = true
                }
            }

            Row {
                id: formActions
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.margins: 14
                spacing: 8

                Label {
                    objectName: "validationHint"
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.warning
                    font.pixelSize: 11
                    // Naming the actual reason: the old sentence said only
                    // "these are required", which is no help at all to
                    // somebody who HAS filled the host in and whose value is
                    // refused for a different reason.
                    text: root.validationMessage()
                    visible: text.length > 0
                }
                SheetButton {
                    objectName: "cancelButton"
                    text: qsTr("Cancel")
                    onClicked: root.dismissed()
                }
                SheetButton {
                    objectName: "saveButton"
                    text: qsTr("Save")
                    enabled: root.formValid()
                    onClicked: root.save()
                }
                SheetButton {
                    objectName: "upgradeButton"
                    text: qsTr("Update server")
                    enabled: root.editingId !== ""
                    onClicked: root.upgradeNow()

                    HoverHandler { id: upgradeHover }
                    AppToolTip {
                        objectName: "upgradeButtonTip"
                        visible: upgradeHover.hovered
                        // Says what it WRITES and where, because this is the
                        // one button in the sheet that changes the server.
                        text: qsTr("Install the CodeHarbor service release that "
                                   + "matches this client into the repository "
                                   + "root on the server, then connect. A source "
                                   + "checkout there is left alone.")
                    }
                }
                SheetButton {
                    objectName: "connectButton"
                    accent: Theme.accent
                    text: qsTr("Connect")
                    enabled: root.editingId !== ""
                    onClicked: root.connectNow()
                }
            }
        }
    }

    // ---- first-use host key prompt ----------------------------------------
    // Covers the sheet because the answer gates the connection that everything
    // else here is about; the SSH pool is blocked on this decision (SPEC 12.1).
    Rectangle {
        id: hostKeyPrompt
        objectName: "hostKeyPrompt"
        anchors.fill: parent
        anchors.margins: 1
        radius: Theme.radiusMedium
        color: Theme.modalOverlaySurface()
        visible: root.pendingHostKey ? true : false

        // Swallow every click so the sheet underneath stays untouchable while
        // the prompt is up.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
        }

        Rectangle {
            id: hostKeyPanel
            anchors.centerIn: parent
            width: Math.min(520, hostKeyPrompt.width - 48)
            height: hostKeyColumn.implicitHeight + 32
            radius: Theme.radiusMedium
            color: Theme.surfaceDeep
            border.width: 1
            border.color: Theme.warning

            Keys.onEscapePressed: (event) => {
                root.hostKeyDecision(false);
                event.accepted = true;
            }

            Column {
                id: hostKeyColumn
                anchors.centerIn: parent
                width: hostKeyPanel.width - 32
                spacing: 8

                Label {
                    text: qsTr("Unknown host key")
                    color: Theme.warning
                    font.bold: true
                    font.pixelSize: Theme.fontSizeTitle
                }
                Label {
                    objectName: "hostKeyHost"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    // SECURITY: this is the ONE panel whose whole job is to show
                    // the user something they must read literally before they
                    // trust a server. keyType and fingerprint are what the
                    // UNVERIFIED peer presented. Markup here could restyle or
                    // hide part of the fingerprint (StyledText honours <font
                    // color>, and Text.AutoText opts into StyledText all by
                    // itself), so the decision must be made on the exact
                    // characters, never on a rendering of them.
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: Theme.fontSizeBody
                    text: qsTr("%1 presented a %2 key that is not in known_hosts.")
                          .arg(root.textOf(root.pendingHostKey, "host"))
                          .arg(root.textOf(root.pendingHostKey, "keyType"))
                }
                Label {
                    objectName: "hostKeyFingerprint"
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    color: Theme.success
                    font.pixelSize: Theme.fontSizeBody
                    font.family: Theme.monoFamily
                    text: root.textOf(root.pendingHostKey, "fingerprint")
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.textDim
                    font.pixelSize: 11
                    text: qsTr("Accept only if this fingerprint matches the server. Accepting stores the key in known_hosts.")
                }

                Row {
                    anchors.right: parent.right
                    spacing: 8

                    SheetButton {
                        id: hostKeyReject
                        objectName: "hostKeyRejectButton"
                        accent: Theme.accent
                        text: qsTr("Reject")
                        onClicked: root.hostKeyDecision(false)
                    }
                    SheetButton {
                        objectName: "hostKeyAcceptButton"
                        accent: Theme.warning
                        text: qsTr("Accept and remember")
                        onClicked: root.hostKeyDecision(true)
                    }
                }
            }
        }
    }

    // ---- password / key passphrase prompt ----------------------------------
    // Raised when the SSH pool asked for a credential and the attempt was
    // deliberately refused so the user could be asked (AppController's
    // credentialPrompt). Covers the sheet for the same reason the host-key
    // panel does: the connection everything else here is about is parked on
    // this one answer.
    //
    // The secret exists in `secretField` and nowhere else in this file.
    Rectangle {
        id: credentialPrompt
        objectName: "credentialPrompt"
        anchors.fill: parent
        anchors.margins: 1
        radius: Theme.radiusMedium
        color: Theme.modalOverlaySurface()
        visible: root.pendingCredential ? true : false

        // Swallow every click so the sheet underneath stays untouchable while
        // the prompt is up.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
        }

        Rectangle {
            id: credentialPanel
            anchors.centerIn: parent
            width: Math.min(520, credentialPrompt.width - 48)
            height: credentialColumn.implicitHeight + 32
            radius: Theme.radiusMedium
            color: Theme.surfaceDeep
            border.width: 1
            border.color: Theme.accent

            Column {
                id: credentialColumn
                anchors.centerIn: parent
                width: credentialPanel.width - 32
                spacing: 8

                Label {
                    text: root.credentialKind() === "password"
                          ? qsTr("Password required") : qsTr("Unlock your key")
                    color: Theme.accent
                    font.bold: true
                    font.pixelSize: Theme.fontSizeTitle
                }
                Label {
                    objectName: "credentialTarget"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    // PlainText for the same reason as the host-key panel: the
                    // user and host are echoed back from configuration the app
                    // does not control, and markup must not be able to restyle
                    // or hide which account is about to be authenticated.
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: Theme.fontSizeBody
                    text: root.credentialExplanation()
                }
                TextField {
                    id: secretField
                    objectName: "credentialField"
                    width: parent.width
                    // A masked field has no visible label of its own, and its
                    // placeholder is the server's own prompt string, so name it
                    // explicitly rather than leaving a screen reader to
                    // announce an unlabelled password box.
                    Accessible.name: root.credentialKind() === "password"
                                     ? qsTr("Password") : qsTr("Key passphrase")
                    // The masked field this whole item exists for.
                    echoMode: TextInput.Password
                    passwordCharacter: "\u2022"
                    // A masked field must not offer to complete or correct what
                    // was typed into it, and must not be copyable by mouse.
                    selectByMouse: false
                    inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData
                                      | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    color: Theme.text
                    placeholderTextColor: Theme.textPlaceholder()
                    placeholderText: root.textOf(root.pendingCredential, "prompt") === ""
                                     ? qsTr("Password or key passphrase")
                                     : root.textOf(root.pendingCredential, "prompt")
                    font.pixelSize: Theme.fontSizeLabel
                    background: Rectangle {
                        color: Theme.surfaceSunken
                        radius: 3
                        border.width: 1
                        border.color: secretField.activeFocus ? Theme.accent : Theme.borderSubtle
                    }
                    onAccepted: {
                        if (secretField.text.length > 0)
                            root.submitSecret();
                    }
                    Keys.onEscapePressed: (event) => {
                        root.cancelSecret();
                        event.accepted = true;
                    }
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.textDim
                    font.pixelSize: 11
                    text: qsTr("Used for this one connection and then discarded. CodeHarbor never stores it.")
                }

                Row {
                    anchors.right: parent.right
                    spacing: 8

                    SheetButton {
                        objectName: "credentialCancelButton"
                        accent: Theme.accent
                        text: qsTr("Cancel")
                        onClicked: root.cancelSecret()
                    }
                    SheetButton {
                        objectName: "credentialPasswordButton"
                        visible: root.credentialKind() === "keyPassphrase"
                        accent: Theme.warning
                        text: qsTr("Use password")
                        enabled: secretField.text.length > 0
                        onClicked: root.submitSecret("password")
                    }
                    SheetButton {
                        objectName: "credentialSubmitButton"
                        accent: Theme.success
                        text: root.credentialKind() === "keyPassphrase"
                              ? qsTr("Unlock key") : qsTr("Authenticate")
                        enabled: secretField.text.length > 0
                        onClicked: root.submitSecret()
                    }
                }
            }
        }
    }
}
