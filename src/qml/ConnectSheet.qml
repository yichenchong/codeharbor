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
//
// Outputs
//   connectRequested(profileId)  connect using that stored profile
//   profileSaved(fields)         create (fields.id === "") or update a profile
//   profileRemoved(id)           delete that profile
//   hostKeyDecision(accept)      answer for the pending host key
//   dismissed()                  user closed the sheet (Esc / Close / Cancel)
Rectangle {
    id: root

    // ---- public API -------------------------------------------------------
    property var profiles: []
    property string activeId: ""
    property string connectionState: ""
    property string errorText: ""
    property var pendingHostKey: null

    signal connectRequested(string profileId)
    signal profileSaved(var fields)
    signal profileRemoved(string id)
    signal hostKeyDecision(bool accept)
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

    function stateColor(state) {
        switch (String(state).toLowerCase()) {
        case "connected": return "#a6e3a1";
        case "connecting":
        case "hostkeycheck":
        case "authenticating": return "#f9e2af";
        case "error":
        case "notavailable": return "#f38ba8";
        default: return "#6c7086";
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
    Component.onCompleted: root.syncFromModel()

    Keys.onEscapePressed: (event) => {
        if (root.pendingHostKey)
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

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            BusyIndicator {
                objectName: "connectingIndicator"
                width: 18
                height: 18
                anchors.verticalCenter: parent.verticalCenter
                running: String(root.connectionState).toLowerCase() === "connecting"
                         || String(root.connectionState).toLowerCase() === "authenticating"
                         || String(root.connectionState).toLowerCase() === "hostkeycheck"
                visible: running
            }

            Rectangle {
                width: 8
                height: 8
                radius: 4
                anchors.verticalCenter: parent.verticalCenter
                color: root.stateColor(root.connectionState)
            }

            Label {
                objectName: "stateLabel"
                anchors.verticalCenter: parent.verticalCenter
                color: "#a6adc8"
                font.pixelSize: 12
                text: root.connectionState.length > 0 ? root.connectionState
                                                      : qsTr("disconnected")
            }

            Button {
                objectName: "closeButton"
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Close")
                onClicked: root.dismissed()
            }
        }
    }

    // ---- error banner (non-blocking: it never steals focus or input) ------
    Rectangle {
        id: errorBanner
        objectName: "errorBanner"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 1
        height: root.errorText.length > 0 ? 40 : 0
        visible: height > 0
        color: "#3a1d28"

        Label {
            objectName: "errorLabel"
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            color: "#f38ba8"
            font.pixelSize: 12
            text: root.errorText
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
                    }

                    contentItem: Column {
                        spacing: 2

                        Row {
                            spacing: 6
                            Label {
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

            Label {
                objectName: "emptyHint"
                anchors.centerIn: profileSelector
                width: profileSelector.width - 24
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: "#6c7086"
                font.pixelSize: 12
                text: qsTr("No servers yet.\nFill in the form and press Save.")
                visible: root.profileList().length === 0
            }

            Row {
                id: listActions
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.margins: 6
                spacing: 6

                Button {
                    objectName: "addButton"
                    text: qsTr("Add")
                    onClicked: root.beginNew()
                }
                Button {
                    objectName: "removeButton"
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
                    hint: qsTr("Remote CodeHarbor checkout that provides codeharbord.")
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
                Button {
                    objectName: "cancelButton"
                    text: qsTr("Cancel")
                    onClicked: root.dismissed()
                }
                Button {
                    objectName: "saveButton"
                    text: qsTr("Save")
                    enabled: root.formValid()
                    onClicked: root.save()
                }
                Button {
                    objectName: "connectButton"
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

                    Button {
                        id: hostKeyReject
                        objectName: "hostKeyRejectButton"
                        text: qsTr("Reject")
                        onClicked: root.hostKeyDecision(false)
                    }
                    Button {
                        objectName: "hostKeyAcceptButton"
                        text: qsTr("Accept")
                        onClicked: root.hostKeyDecision(true)
                    }
                }
            }
        }
    }
}
