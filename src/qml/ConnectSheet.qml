// Bound: the ListView delegate below reads this file's `root` id, and binding
// the component's context is what makes that resolvable statically (qmllint)
// instead of only at run time.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

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
// Inputs
//   profiles         array of {id, name, host, port, user, nodePath, repoRoot}
//   activeId         id of the profile the host considers current ("" = none)
//   connectionState  free-form status text; "connecting"/"authenticating"/
//                    "hostkeycheck"/"connected"/"error"/"notavailable" are
//                    additionally recognised for colouring (case-insensitive)
//   errorText        last failure; shown as a non-blocking banner while non-empty
//   pendingHostKey   null, or {host, keyType, fingerprint} to prompt about
//   pendingCredential null, or {user, host, prompt} to ask a secret for
//
// Outputs
//   connectRequested(profileId)  connect using that stored profile
//   profileSaved(fields)         create (fields.id === "") or update a profile
//   profileRemoved(id)           delete that profile
//   hostKeyDecision(accept)      answer for the pending host key
//   credentialSubmitted(secret)  answer for the pending credential; "" cancels
//   dismissed()                  user closed the sheet (Esc / Close / Cancel)
Rectangle {
    id: root

    // ---- public API -------------------------------------------------------
    property var profiles: []
    property string activeId: ""
    property string connectionState: ""
    property string errorText: ""
    property var pendingHostKey: null
    property var pendingCredential: null

    signal connectRequested(string profileId)
    signal profileSaved(var fields)
    signal profileRemoved(string id)
    signal hostKeyDecision(bool accept)
    signal credentialSubmitted(string secret)
    signal dismissed()

    // ---- internal state ---------------------------------------------------
    // Profile the form is editing; empty means "new, unsaved profile".
    property string editingId: ""
    // Set while a create round-trip is in flight so the appended profile can be
    // selected as soon as the host hands back a longer list.
    property bool awaitingNewProfile: false
    property int knownProfileCount: 0

    implicitWidth: 760
    implicitHeight: 480
    color: "#1e1e2e"
    radius: 6
    border.width: 1
    border.color: "#313244"
    focus: true

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
        nameField.text = root.textOf(entry, "name");
        hostField.text = root.textOf(entry, "host");
        portField.text = root.textOf(entry, "port") === "" ? "22" : root.textOf(entry, "port");
        userField.text = root.textOf(entry, "user");
        nodePathField.text = root.textOf(entry, "nodePath");
        repoRootField.text = root.textOf(entry, "repoRoot");
    }

    function selectIndex(index) {
        var list = root.profileList();
        if (index < 0 || index >= list.length)
            return;
        profileSelector.currentIndex = index;
        root.editingId = root.textOf(list[index], "id");
        root.loadForm(list[index]);
    }

    function beginNew() {
        profileSelector.currentIndex = -1;
        root.editingId = "";
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
        var parsed = parseInt(portField.text, 10);
        return isNaN(parsed) ? 0 : parsed;
    }

    function formValid() {
        return hostField.text.trim().length > 0
            && userField.text.trim().length > 0
            && root.portValue() >= 1 && root.portValue() <= 65535;
    }

    function save() {
        if (!root.formValid())
            return;
        if (root.editingId === "") {
            root.awaitingNewProfile = true;
            root.knownProfileCount = root.profileList().length;
        }
        root.profileSaved({
            "id": root.editingId,
            "name": nameField.text.trim(),
            "host": hostField.text.trim(),
            "port": root.portValue(),
            "user": userField.text.trim(),
            "nodePath": nodePathField.text.trim(),
            "repoRoot": repoRootField.text.trim()
        });
    }

    function connectNow() {
        if (root.editingId !== "")
            root.connectRequested(root.editingId);
    }

    function removeSelected() {
        if (root.editingId !== "")
            root.profileRemoved(root.editingId);
    }

    // Hand the typed secret up and wipe it here in the same turn. This file
    // keeps no copy of it and never routes it through the profile form, so it
    // cannot reach profileSaved() and therefore cannot reach QSettings.
    function submitSecret() {
        var secret = secretField.text;
        secretField.clear();
        root.credentialSubmitted(secret);
    }

    function cancelSecret() {
        secretField.clear();
        root.credentialSubmitted("");   // empty == cancel, nothing is retried
    }

    // ---- connection status vocabulary -------------------------------------
    //
    // `connectionState` is free-form text from the host. These map it onto the
    // six states ch::AppController::setConnectionState() actually publishes
    // (disconnected / connecting / hostkey / connected / reconnecting / failed),
    // keeping the older libssh-level words the pool can still surface.
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
        case "reconnecting": return "reconnecting";
        case "failed":
        case "error":
        case "notavailable": return "failed";
        default: return "disconnected";
        }
    }

    function stateColor(state) {
        switch (root.stateKey(state)) {
        case "connected": return "#a6e3a1";
        case "connecting": return "#f9e2af";
        case "hostkey": return "#f9e2af";
        case "credential": return "#f9e2af";
        case "reconnecting": return "#fab387";
        case "failed": return "#f38ba8";
        default: return "#6c7086";
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
            || key === "reconnecting";
    }

    // One sentence saying what the state means for the person looking at it;
    // `stateLabel` itself echoes the host's own word verbatim.
    function stateExplanation(state) {
        switch (root.stateKey(state)) {
        case "connected": return qsTr("Linked to the server.");
        case "connecting": return qsTr("Opening the SSH connection\u2026");
        case "hostkey": return qsTr("Waiting for you to accept this server's host key.");
        case "credential": return qsTr("Waiting for a password or key passphrase.");
        case "reconnecting": return qsTr("The link dropped; trying to restore it\u2026");
        case "failed": return qsTr("The last attempt failed. See the message below.");
        default: return qsTr("Not connected to any server.");
        }
    }

    onProfilesChanged: root.syncFromModel()
    onActiveIdChanged: {
        var index = root.indexOfId(root.activeId);
        if (index >= 0)
            root.selectIndex(index);
    }
    onPendingHostKeyChanged: {
        if (root.pendingHostKey)
            hostKeyReject.forceActiveFocus();
        else if (root.visible)
            profileSelector.forceActiveFocus();
    }
    onPendingCredentialChanged: {
        // Cleared on BOTH edges: on open so a previous attempt's keystrokes can
        // never be resubmitted, and on close so the secret is not left sitting
        // in a live QML item (and its undo stack) after it has been spent.
        secretField.clear();
        if (root.pendingCredential)
            secretField.forceActiveFocus();
        else if (root.visible)
            profileSelector.forceActiveFocus();
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
            color: "#a6adc8"
            font.pixelSize: 11
        }
        TextField {
            id: fieldInput
            width: field.width
            color: "#cdd6f4"
            placeholderTextColor: "#585b70"
            selectByMouse: true
            font.pixelSize: 13
            background: Rectangle {
                color: "#11111b"
                radius: 3
                border.width: 1
                border.color: fieldInput.activeFocus ? "#89b4fa" : "#313244"
            }
            onAccepted: field.accepted()
        }
        Label {
            id: fieldHint
            color: "#6c7086"
            font.pixelSize: 10
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
        property color accent: "#45475a"

        implicitHeight: 30
        leftPadding: 14
        rightPadding: 14
        focusPolicy: Qt.StrongFocus

        contentItem: Label {
            text: button.text
            color: button.enabled ? "#cdd6f4" : "#585b70"
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: 4
            color: !button.enabled ? "#232338"
                 : button.down ? "#45475a"
                 : button.hovered ? "#3a3a52" : "#313244"
            // Two pixels and a bright edge: the focus ring has to be legible at
            // a glance, not a one-pixel difference against #45475a.
            border.width: button.visualFocus ? 2 : 1
            border.color: button.visualFocus ? "#89b4fa"
                        : button.enabled ? button.accent : "#313244"
        }
    }

    // ---- header -----------------------------------------------------------
    Rectangle {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 44
        color: "#181825"
        radius: 6

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
            color: "#cdd6f4"
            font.bold: true
            font.pixelSize: 14
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
                color: "#11111b"
                border.width: 1
                border.color: root.stateColor(root.connectionState)

                HoverHandler { id: chipHover }
                ToolTip.visible: chipHover.hovered
                ToolTip.text: root.stateExplanation(root.connectionState)

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
                            color: "#11111b"
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }

                    Label {
                        objectName: "stateLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        // SECURITY: see errorLabel below — connectionState is
                        // free-form text from the host, not a literal.
                        textFormat: Text.PlainText
                        color: "#cdd6f4"
                        font.pixelSize: 12
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
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 1
        visible: root.errorText.length > 0 && !root.errorDismissed
        // Grows to fit: an ssh failure is a whole sentence naming a host, a
        // port and a reason, and one elided line of it tells nobody anything.
        height: visible ? Math.max(40, errorLabel.implicitHeight + 20) : 0
        color: "#3a1d28"

        // A red wash is the only thing separating this from the rest of the
        // sheet; the rule and the glyph say "error" without relying on hue.
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 3
            color: "#f38ba8"
        }

        Label {
            id: errorGlyph
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.top: parent.top
            anchors.topMargin: 10
            text: "\u2715"
            color: "#f38ba8"
            font.pixelSize: 12
            font.bold: true
        }

        Label {
            id: errorLabel
            objectName: "errorLabel"
            anchors.left: errorGlyph.right
            anchors.leftMargin: 8
            anchors.right: errorDismissButton.left
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
            color: "#f38ba8"
            font.pixelSize: 12
            text: root.errorText
        }

        SheetButton {
            id: errorDismissButton
            objectName: "errorDismissButton"
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            accent: "#f38ba8"
            text: qsTr("Dismiss")
            onClicked: root.errorDismissed = true
        }
    }

    // ---- body -------------------------------------------------------------
    Item {
        id: body
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
            color: "#181825"

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

                ScrollBar.vertical: ScrollBar {}

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

                    background: Rectangle {
                        color: root.textOf(profileDelegate.modelData, "id") === root.editingId
                               ? "#313244" : (profileDelegate.hovered ? "#232338" : "transparent")
                        radius: 3

                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 3
                            radius: 3
                            color: "#89b4fa"
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
                            border.color: "#89b4fa"
                            visible: profileSelector.activeFocus
                                     && profileDelegate.index === profileSelector.currentIndex
                        }
                    }

                    contentItem: Column {
                        spacing: 2

                        Row {
                            spacing: 6
                            Label {
                                objectName: "profileName" + profileDelegate.index
                                // Same rule as errorLabel: a stored profile is
                                // data (it can also arrive hand-edited from
                                // disk), never markup.
                                textFormat: Text.PlainText
                                text: root.textOf(profileDelegate.modelData, "name")
                                color: "#cdd6f4"
                                font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                            Label {
                                objectName: "activeBadge" + profileDelegate.index
                                text: qsTr("active")
                                color: "#89b4fa"
                                font.pixelSize: 10
                                anchors.verticalCenter: parent.verticalCenter
                                visible: root.textOf(profileDelegate.modelData, "id") === root.activeId
                            }
                        }
                        Label {
                            objectName: "profileEndpoint" + profileDelegate.index
                            textFormat: Text.PlainText
                            text: root.textOf(profileDelegate.modelData, "user") + "@"
                                  + root.textOf(profileDelegate.modelData, "host") + ":"
                                  + root.textOf(profileDelegate.modelData, "port")
                            color: "#6c7086"
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
                    color: "#45475a"
                    font.pixelSize: 28
                }
                Label {
                    objectName: "emptyHint"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: "#6c7086"
                    font.pixelSize: 12
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
                    accent: "#f38ba8"
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
                    color: "#a6adc8"
                    font.pixelSize: 12
                    text: qsTr("CodeHarbor edits a checkout that lives on another machine, over SSH. "
                               + "Describe that machine below — its address, your login, and the "
                               + "absolute path to node on it — then Save it and press Connect.")
                }

                Label {
                    objectName: "formTitle"
                    text: root.editingId === "" ? qsTr("New server") : qsTr("Edit server")
                    color: "#cdd6f4"
                    font.pixelSize: 13
                    font.bold: true
                }

                LabeledField {
                    id: nameField
                    objectName: "nameField"
                    width: form.width
                    label: qsTr("Name")
                    placeholder: qsTr("Defaults to the host name")
                    onAccepted: root.save()
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
                    }
                    LabeledField {
                        id: portField
                        objectName: "portField"
                        width: 108
                        label: qsTr("Port")
                        text: "22"
                        validator: IntValidator { bottom: 1; top: 65535 }
                        onAccepted: root.save()
                    }
                }

                LabeledField {
                    id: userField
                    objectName: "userField"
                    width: form.width
                    label: qsTr("User")
                    placeholder: qsTr("login name")
                    onAccepted: root.save()
                }

                LabeledField {
                    id: nodePathField
                    objectName: "nodePathField"
                    width: form.width
                    label: qsTr("Node path")
                    placeholder: qsTr("/usr/bin/node")
                    hint: qsTr("Absolute path to node on the server; it need not be on the login PATH.")
                    onAccepted: root.save()
                }

                LabeledField {
                    id: repoRootField
                    objectName: "repoRootField"
                    width: form.width
                    label: qsTr("Repository root")
                    placeholder: qsTr("/srv/codeharbor")
                    hint: qsTr("Remote CodeHarbor install providing codeharbord: an unpacked release tarball or a git checkout.")
                    onAccepted: root.save()
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
                    color: "#f9e2af"
                    font.pixelSize: 11
                    visible: !root.formValid()
                    text: qsTr("Host, user and a port in 1-65535 are required.")
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
                    objectName: "connectButton"
                    accent: "#89b4fa"
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
        radius: 6
        color: "#e61e1e2e"
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
            radius: 6
            color: "#181825"
            border.width: 1
            border.color: "#f9e2af"

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
                    color: "#f9e2af"
                    font.bold: true
                    font.pixelSize: 14
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
                    color: "#cdd6f4"
                    font.pixelSize: 12
                    text: qsTr("%1 presented a %2 key that is not in known_hosts.")
                          .arg(root.textOf(root.pendingHostKey, "host"))
                          .arg(root.textOf(root.pendingHostKey, "keyType"))
                }
                Label {
                    objectName: "hostKeyFingerprint"
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    color: "#a6e3a1"
                    font.pixelSize: 12
                    font.family: "Monospace"
                    text: root.textOf(root.pendingHostKey, "fingerprint")
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: "#6c7086"
                    font.pixelSize: 11
                    text: qsTr("Accept only if this fingerprint matches the server. Accepting stores the key in known_hosts.")
                }

                Row {
                    anchors.right: parent.right
                    spacing: 8

                    SheetButton {
                        id: hostKeyReject
                        objectName: "hostKeyRejectButton"
                        accent: "#89b4fa"
                        text: qsTr("Reject")
                        onClicked: root.hostKeyDecision(false)
                    }
                    SheetButton {
                        objectName: "hostKeyAcceptButton"
                        accent: "#f9e2af"
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
        radius: 6
        color: "#e61e1e2e"
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
            radius: 6
            color: "#181825"
            border.width: 1
            border.color: "#89b4fa"

            Column {
                id: credentialColumn
                anchors.centerIn: parent
                width: credentialPanel.width - 32
                spacing: 8

                Label {
                    text: qsTr("Authentication required")
                    color: "#89b4fa"
                    font.bold: true
                    font.pixelSize: 14
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
                    color: "#cdd6f4"
                    font.pixelSize: 12
                    text: qsTr("ssh-agent and the default keys could not authenticate %1@%2.")
                          .arg(root.textOf(root.pendingCredential, "user"))
                          .arg(root.textOf(root.pendingCredential, "host"))
                }
                TextField {
                    id: secretField
                    objectName: "credentialField"
                    width: parent.width
                    // The masked field this whole item exists for.
                    echoMode: TextInput.Password
                    passwordCharacter: "\u2022"
                    // A masked field must not offer to complete or correct what
                    // was typed into it, and must not be copyable by mouse.
                    selectByMouse: false
                    inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData
                                      | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    color: "#cdd6f4"
                    placeholderTextColor: "#585b70"
                    placeholderText: root.textOf(root.pendingCredential, "prompt") === ""
                                     ? qsTr("Password or key passphrase")
                                     : root.textOf(root.pendingCredential, "prompt")
                    font.pixelSize: 13
                    background: Rectangle {
                        color: "#11111b"
                        radius: 3
                        border.width: 1
                        border.color: secretField.activeFocus ? "#89b4fa" : "#313244"
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
                    color: "#6c7086"
                    font.pixelSize: 11
                    text: qsTr("Used for this one connection and then discarded. CodeHarbor never stores it.")
                }

                Row {
                    anchors.right: parent.right
                    spacing: 8

                    SheetButton {
                        objectName: "credentialCancelButton"
                        accent: "#89b4fa"
                        text: qsTr("Cancel")
                        onClicked: root.cancelSecret()
                    }
                    SheetButton {
                        objectName: "credentialSubmitButton"
                        accent: "#a6e3a1"
                        text: qsTr("Authenticate")
                        enabled: secretField.text.length > 0
                        onClicked: root.submitSecret()
                    }
                }
            }
        }
    }
}
