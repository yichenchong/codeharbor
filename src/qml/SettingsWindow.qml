pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import CodeHarbor
import "EndpointField.js" as EndpointField

// Full-surface settings sheet. Main owns the `shown` flag; this component owns
// focus, Escape and the preference controls so each value is written directly
// to the client-local AppSettings object.
Rectangle {
    id: root
    objectName: "settingsWindow"
    property bool shown: false
    // Source-loaded tests can choose the initial pane before Loader creation;
    // the application leaves this unset and therefore keeps Appearance first.
    property string selectedGroup: typeof initialSettingsGroup === "string"
                                   ? initialSettingsGroup : "appearance"
    property var toolbarItems: []
    property var viewerDefaultEntries: []
    property var profileEntries: []
    property string selectedProfileId: ""
    property string profileName: ""
    property string profileHost: ""
    property string profilePort: "22"
    property string profileUser: ""
    property string profileIdentityFile: ""
    property string profileNodePath: ""
    property string profileRepoRoot: ""
    property bool loadingProfile: false
    property bool profileDirty: false
    // Captured when the delete confirmation opens, so the question names the
    // profile the user picked even if the list changes underneath the dialog.
    property string pendingDeleteId: ""
    property string pendingDeleteName: ""
    signal dismissed()
    // The Name field lives inside the server pane's Component and cannot be
    // reached from here by id; the pane listens for this instead.
    signal profileNameFocusRequested()

    // The host owns visibility; this component only manages its sheet content.
    color: Theme.surface
    border.width: 1
    border.color: Theme.borderSubtle
    focus: visible

    // A full-surface sheet that takes the keyboard and swallows every click
    // behind it is a dialog in everything but type; without a role and a name
    // a screen reader announces it as an unlabelled rectangle and never tells
    // the user the window changed.
    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("Settings")

    // The parts of a QtQuick.Controls control this file does NOT replace still
    // draw from the STYLE's palette, which is light: a ComboBox drops a white
    // popup with white rows on top of this dark sheet, and a SpinBox's two step
    // buttons are light grey plates. An Item's palette propagates down the
    // visual parent chain, so one mapping here reaches every control inside —
    // the same trick, and the same role-by-role mapping, as AppDialog.qml.
    // `light` is the extra role a ComboBox popup ROW is drawn with.
    palette.window: Theme.surface
    palette.windowText: Theme.text
    palette.dark: Theme.border
    palette.base: Theme.surfaceSunken
    palette.text: Theme.text
    palette.placeholderText: Theme.textDim
    palette.button: Theme.surfaceRaised
    palette.buttonText: Theme.text
    palette.mid: Theme.border
    palette.light: Theme.surfaceDeep
    palette.midlight: Theme.surfaceRaised
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.textOnAccent
    palette.brightText: Theme.textOnAccent

    // This sheet fills the whole window on top of the three regions, but a
    // Rectangle accepts no input of its own: Qt Quick hands an unaccepted press
    // to the next item DOWN, so a click that missed one of the controls below
    // went straight through to the terminal or editor behind the sheet —
    // focusing a pane, or scrolling a shell the user could not even see.
    // Declared FIRST so every real control still hit-tests above it; `wheel`
    // too, because a stray scroll is as wrong as a stray click.
    MouseArea {
        objectName: "sheetInputShield"
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: (wheel) => wheel.accepted = true
    }

    component SheetButton: Button {
        id: button
        property color accent: Theme.border
        // A SheetButton is used both as a stand-alone command button, whose
        // label is centred over it, and as a row of the server list, where a
        // centred name reads as a heading rather than as an entry to pick. The
        // row form sets this to Text.AlignLeft; nothing else changes.
        property int textAlignment: Text.AlignHCenter
        implicitHeight: 30
        leftPadding: 12
        rightPadding: 12
        focusPolicy: Qt.StrongFocus
        contentItem: Label {
            text: button.text
            textFormat: Text.PlainText
            color: button.enabled ? Theme.text : Theme.textFaint
            font.pixelSize: Theme.fontSizeBody
            horizontalAlignment: button.textAlignment
            verticalAlignment: Text.AlignVCenter
            // A command button is as wide as its own label, so this never
            // fires for one; a list row is as wide as the list, and without it
            // a profile name longer than the row spilled over both of its
            // borders and was then cut mid-glyph by the view's clip.
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: !button.enabled ? Theme.surfaceHover
                 : button.down ? Theme.border
                 : button.hovered ? Theme.surfaceHover : Theme.surfaceRaised
            // A checked SheetButton is the SELECTED row of a list (the server
            // profiles). Selection has to be visible: the fill cannot carry it
            // because Theme.surfaceSelected and Theme.surfaceRaised are the
            // same colour in the dark palette, so it is drawn as the accent
            // outline instead — the same weight the focus ring uses.
            border.width: button.visualFocus || button.checked ? 2 : 1
            border.color: button.visualFocus || button.checked ? Theme.accent
                        : button.enabled ? button.accent : Theme.borderSubtle
        }
    }

    component ChoiceBox: ComboBox {
        id: choice
        implicitWidth: 190
        implicitHeight: 30
        focusPolicy: Qt.StrongFocus
        contentItem: Label {
            leftPadding: 9
            rightPadding: 9
            text: choice.displayText
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: Theme.surfaceSunken
            border.width: choice.visualFocus ? 2 : 1
            border.color: choice.visualFocus ? Theme.accent : Theme.borderSubtle
        }
    }

    component NumberBox: SpinBox {
        id: numberBox
        implicitWidth: 110
        implicitHeight: 30
        focusPolicy: Qt.StrongFocus
        editable: true
        contentItem: TextInput {
            text: numberBox.displayText
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            selectByMouse: true
            validator: numberBox.validator
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: Theme.surfaceSunken
            border.width: numberBox.visualFocus ? 2 : 1
            border.color: numberBox.visualFocus ? Theme.accent : Theme.borderSubtle
        }
    }

    component Field: Column {
        id: field
        property alias label: fieldLabel.text
        property alias text: fieldInput.text
        property alias input: fieldInput
        signal editingFinished()
        spacing: 3
        Label {
            id: fieldLabel
            color: Theme.textDim
            font.pixelSize: Theme.fontSizeSmall
        }
        TextField {
            id: fieldInput
            width: field.width
            // The visible Label above is a separate item; without this the
            // server fields are announced as unnamed edit boxes. Same rule as
            // ConnectSheet's LabeledField, which edits the same records.
            Accessible.name: fieldLabel.text
            implicitHeight: 30
            color: Theme.text
            placeholderTextColor: Theme.textDim
            selectByMouse: true
            font.pixelSize: Theme.fontSizeBody
            background: Rectangle {
                color: Theme.surfaceSunken
                radius: Theme.radiusSmall
                border.width: fieldInput.activeFocus ? 2 : 1
                border.color: fieldInput.activeFocus ? Theme.accent : Theme.borderSubtle
            }
            onEditingFinished: field.editingFinished()
        }
    }

    function settingsObject() {
        if (typeof app === "undefined" || !app)
            return null;
        return app.settings || null;
    }

    function validViewerKindsForExtension(extension) {
        if (typeof viewers === "undefined" || !viewers
                || typeof viewers.validViewKindsForExtension !== "function")
            return [];
        return viewers.validViewKindsForExtension(String(extension || ""));
    }

    function syncViewerDefaults() {
        var settings = root.settingsObject();
        var stored = settings ? settings.viewerDefaults : {};
        var keys = Object.keys(stored || {});
        keys.sort();
        var rows = [];
        for (var i = 0; i < keys.length; ++i) {
            rows.push({ extension: keys[i], kind: String(stored[keys[i]]) });
        }
        root.viewerDefaultEntries = rows;
    }

    function viewerKindLabel(kind) {
        switch (String(kind)) {
        case "web": return qsTr("Web page");
        case "markdown": return qsTr("Rendered Markdown");
        case "text": return qsTr("Source editor");
        case "image": return qsTr("Image");
        case "pdf": return qsTr("PDF");
        case "directory": return qsTr("Directory");
        case "binary": return qsTr("Binary");
        default: return String(kind);
        }
    }

    function removeViewerDefault(extension) {
        var settings = root.settingsObject();
        if (settings)
            settings.clearViewerDefault(String(extension || ""));
    }

    // Reconciliation is a VIEW of the stored order against the buttons this
    // build has; it is not a user choice and must not be written back. Saving
    // it here meant merely opening the app (or this pane) rewrote the settings
    // file with an order nobody chose - visible as a modified config after a
    // launch that changed nothing. Only moveToolbar() below persists, because
    // only it is the user reordering something.
    function reconcileToolbar() {
        var settings = root.settingsObject();
        var stored = settings ? settings.toolbarOrder : [];
        root.toolbarItems = ToolbarRegistry.reconcile(stored,
                                                      ToolbarRegistry.knownIds());
    }

    function moveToolbar(index, delta) {
        var target = index + delta;
        if (index < 0 || target < 0 || target >= root.toolbarItems.length)
            return;
        var next = root.toolbarItems.slice(0);
        var item = next[index];
        next[index] = next[target];
        next[target] = item;
        root.toolbarItems = next;
        var settings = root.settingsObject();
        if (settings)
            settings.toolbarOrder = next;
    }

    function toolbarLabel(id) {
        switch (String(id)) {
        case "nav.back": return qsTr("Back");
        case "nav.forward": return qsTr("Forward");
        case "nav.reload": return qsTr("Reload");
        case "nav.home": return qsTr("Home");
        case "pane.split.horizontal": return qsTr("Split side by side");
        case "pane.split.vertical": return qsTr("Split top and bottom");
        case "pane.close": return qsTr("Close pane");
        default: return String(id);
        }
    }

    function profileText(entry, key) {
        if (!entry || entry[key] === undefined || entry[key] === null)
            return "";
        return String(entry[key]);
    }

    function profileAt(id) {
        for (var i = 0; i < root.profileEntries.length; ++i) {
            if (root.profileText(root.profileEntries[i], "id") === id)
                return root.profileEntries[i];
        }
        return null;
    }
    function loadSelectedProfile() {
        var entry = root.profileAt(root.selectedProfileId);
        root.loadingProfile = true;
        root.profileName = root.profileText(entry, "name");
        root.profileHost = root.profileText(entry, "host");
        root.profilePort = root.profileText(entry, "port") || "22";
        root.profileUser = root.profileText(entry, "user");
        root.profileIdentityFile = root.profileText(entry, "identityFile");
        root.profileNodePath = root.profileText(entry, "nodePath");
        root.profileRepoRoot = root.profileText(entry, "repoRoot");
        root.loadingProfile = false;
        root.profileDirty = false;
    }

    function syncProfiles() {
        var profiles = [];
        if (typeof app !== "undefined" && app && app.serverProfiles)
            profiles = app.serverProfiles.profiles || [];
        root.profileEntries = profiles;
        if (root.profileAt(root.selectedProfileId)) {
            // A settings refresh can arrive while a field is being edited.
            // Re-reading the stored profile here would erase that draft.
            if (!root.profileDirty)
                root.loadSelectedProfile();
            return;
        }
        root.profileDirty = false;
        var preferred = (typeof app !== "undefined" && app && app.serverProfiles
                         && app.serverProfiles.activeId)
                        ? app.serverProfiles.activeId : "";
        if (!root.profileAt(preferred) && profiles.length > 0)
            preferred = root.profileText(profiles[0], "id");
        root.selectedProfileId = preferred;
        root.loadSelectedProfile();
    }

    // The same requirements ServerProfiles::sanitize() enforces, checked here
    // so the draft can SAY it will not be stored instead of the write being
    // dropped in silence. Host and user go through the module's one copy of
    // the store's rule (EndpointField.js), which the Connect sheet uses too:
    // this pane edits exactly the same records, so a value it accepts and the
    // sheet refuses would be the same silent-drop bug wearing a different hat.
    // Port follows the store's rule as well: blank means "use the default",
    // anything else has to be a number in range.
    function profileValid() {
        var port = String(root.profilePort).trim();
        if (port.length > 0) {
            if (!/^[0-9]+$/.test(port))
                return false;
            var parsed = parseInt(port, 10);
            if (parsed < 1 || parsed > 65535)
                return false;
        }
        return EndpointField.isUsable(root.profileHost)
            && EndpointField.isUsable(root.profileUser);
    }

    function saveProfile() {
        // `selectedProfileId` naming a profile is not the same as that profile
        // still existing: a delete (ours or another window's, arriving through
        // profilesChanged) leaves the id behind for one turn, and updateProfile()
        // on a dead id would silently resurrect nothing while the user watched
        // their typing disappear. hasSelection() checks the LIST, not the id.
        if (root.loadingProfile || !root.hasSelection())
            return;
        // An unsaveable draft stays DIRTY: clearing the flag would tell the
        // rest of this file the fields match the stored profile when the store
        // just refused them, and the next background refresh would then wipe
        // the user's half-finished edit.
        if (!root.profileValid())
            return;
        if (typeof app === "undefined" || !app || !app.serverProfiles)
            return;
        app.serverProfiles.updateProfile(root.selectedProfileId, {
            name: root.profileName,
            host: root.profileHost,
            port: root.profilePort,
            user: root.profileUser,
            identityFile: root.profileIdentityFile,
            nodePath: root.profileNodePath,
            repoRoot: root.profileRepoRoot
        });
        root.profileDirty = false;
    }

    // The seven fields, the validation hint and the delete button all act on
    // "the profile currently selected", which is only meaningful while that
    // profile is in the list the store last published.
    function hasSelection() {
        return root.selectedProfileId.length > 0
            && root.profileAt(root.selectedProfileId) !== null;
    }

    function profileIndexOf(id) {
        for (var i = 0; i < root.profileEntries.length; ++i) {
            if (root.profileText(root.profileEntries[i], "id") === id)
                return i;
        }
        return -1;
    }

    // A new profile is written to the store IMMEDIATELY rather than being held
    // on screen as a draft, because this pane has no Save button: every field
    // writes through on editingFinished, and a field can only write through to
    // a record that exists. That means seeding host and user, since
    // ServerProfiles refuses a profile that could never connect (see
    // sanitize()) and would hand back an empty id for a blank one. The seeds
    // are deliberately not plausible addresses — they are there to be typed
    // over, and the row shows the name, not them.
    //
    // Nothing here touches the network: this is the first-run path, where no
    // server is reachable by definition.
    function addProfile() {
        if (typeof app === "undefined" || !app || !app.serverProfiles)
            return;
        var id = app.serverProfiles.addProfile({
            name: qsTr("New server"),
            host: "new-server",
            port: 22,
            user: "user",
            identityFile: "",
            nodePath: "",
            repoRoot: ""
        });
        if (!id)
            return;
        // addProfile() has already emitted profilesChanged, so syncProfiles()
        // ran with the OLD selection and profileEntries is current: selecting
        // the new id and reloading is all that is left. The dirty flag belongs
        // to the profile we are leaving, and that one is saved or refused
        // already; carrying it over would make the next refresh protect this
        // pristine form from itself.
        root.profileDirty = false;
        root.selectedProfileId = id;
        root.loadSelectedProfile();
        // The name is the one field with no useful seed, and typing over the
        // seeded host/user is what the user does next, so the caret starts
        // where the first keystroke belongs.
        root.profileNameFocusRequested();
    }

    // Deleting is destructive and the store keeps no undo, so the button only
    // gets as far as the confirmation; this is the other half.
    function confirmDeleteProfile() {
        if (!root.hasSelection())
            return;
        root.pendingDeleteId = root.selectedProfileId;
        root.pendingDeleteName = root.profileText(root.profileAt(root.selectedProfileId), "name")
                                 || root.profileText(root.profileAt(root.selectedProfileId), "host")
                                 || qsTr("Unnamed profile");
        deleteProfileDialog.open();
    }

    function deletePendingProfile() {
        var removedId = root.pendingDeleteId;
        root.pendingDeleteId = "";
        if (removedId.length === 0 || typeof app === "undefined" || !app || !app.serverProfiles)
            return;
        // Pick the neighbour BEFORE the store rewrites the list: afterwards the
        // removed profile's position is gone and there is nothing left to be a
        // neighbour OF. The one BELOW takes the deleted row's place on screen,
        // which is what the eye expects; deleting the last row falls back to
        // the one above, and deleting the only row leaves no selection at all.
        var index = root.profileIndexOf(removedId);
        var neighbour = "";
        if (index >= 0) {
            if (index + 1 < root.profileEntries.length)
                neighbour = root.profileText(root.profileEntries[index + 1], "id");
            else if (index > 0)
                neighbour = root.profileText(root.profileEntries[index - 1], "id");
        }
        // The draft in the fields belongs to the profile being destroyed, so it
        // dies with it: leaving the flag set would stop syncProfiles() from
        // loading the neighbour and strand the old values in the form.
        root.profileDirty = false;
        root.selectedProfileId = neighbour;
        app.serverProfiles.removeProfile(removedId);
        // removeProfile() emits profilesChanged, and syncProfiles() has already
        // reloaded the form from the neighbour by now. Run it again anyway: a
        // store that refused the removal must leave the pane agreeing with what
        // is actually stored rather than pointing at a neighbour it never
        // moved to.
        root.syncProfiles();
    }

    function closeSheet() {
        // Drop the flag here as well as telling the host. `shown` is this
        // component's own state, and a host that only WATCHES it (rather than
        // wiring onDismissed back into it) would otherwise be left with a
        // sheet that says it is open after Escape or Close. LogView.qml does
        // the same, so the two sheets behave alike.
        root.shown = false;
        root.dismissed();
    }

    onVisibleChanged: {
        if (root.visible) {
            root.reconcileToolbar();
            root.syncViewerDefaults();
            root.syncProfiles();
            root.forceActiveFocus();
        }
    }
    Component.onCompleted: {
        root.reconcileToolbar();
        root.syncViewerDefaults();
        root.syncProfiles();
    }

    Connections {
        target: root.settingsObject()
        function onToolbarOrderChanged() { root.reconcileToolbar(); }
        function onViewerDefaultsChanged() { root.syncViewerDefaults(); }
    }
    Connections {
        target: ToolbarRegistry
        function onOrderChanged() { root.reconcileToolbar(); }
    }
    Connections {
        target: (typeof app !== "undefined" && app && app.serverProfiles)
                ? app.serverProfiles : null
        function onProfilesChanged() { root.syncProfiles(); }
        function onActiveIdChanged() { root.syncProfiles(); }
    }

    Keys.onEscapePressed: function(event) {
        root.closeSheet();
        event.accepted = true;
    }

    // Declared here rather than inside the server pane's Component so it is not
    // destroyed the instant the group selector switches panes, which is exactly
    // what happens if the dialog is left open and something else takes the
    // selection. Same shape as the sidebar's delete-group confirmation.
    AppDialog {
        id: deleteProfileDialog
        objectName: "serverDeleteDialog"
        title: qsTr("Delete server profile")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        width: 400

        Label {
            objectName: "serverDeleteMessage"
            // The dialog has an explicit width, so the message wraps to the
            // content area it is given rather than sizing the frame itself;
            // a Label's implicitWidth is read-only and cannot ask for one.
            width: parent ? parent.width : 360
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            text: qsTr("Delete the server profile \"%1\"? This permanently removes its address, user and paths from this computer. This cannot be undone.")
                      .arg(root.pendingDeleteName)
        }

        onAccepted: root.deletePendingProfile()
        onRejected: root.pendingDeleteId = ""
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.surface

        Rectangle {
            id: header
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 46
            color: Theme.surfaceDeep
            border.color: Theme.borderSubtle
            border.width: 1
            Label {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Settings")
                color: Theme.text
                font.bold: true
                font.pixelSize: Theme.fontSizeTitle
            }
            SheetButton {
                objectName: "settingsCloseButton"
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Close")
                onClicked: root.closeSheet()
            }
        }

        Rectangle {
            id: sidebar
            objectName: "settingsGroupSidebar"
            anchors.top: header.bottom
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            width: 190
            color: Theme.surfaceDeep
            // Rectangle's border is uniform, so the divider between the group
            // list and the pane is drawn as its own hairline rather than as a
            // one-sided border, which Rectangle cannot express.
            Column {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 4
                Label {
                    leftPadding: 8
                    text: qsTr("Groups")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeSmall
                }
                Repeater {
                    model: [
                        { key: "appearance", label: qsTr("Appearance") },
                        { key: "viewerDefaults", label: qsTr("File viewers") },
                        { key: "server", label: qsTr("Server") },
                        { key: "tmux", label: qsTr("Tmux") }
                    ]
                    delegate: Button {
                        id: groupButton
                        required property var modelData
                        objectName: "settingsGroup:" + modelData.key
                        text: modelData.label
                        width: parent ? parent.width : 170
                        implicitHeight: 34
                        focusPolicy: Qt.StrongFocus
                        checkable: true
                        // These four are a radio set: exactly one group is
                        // open. Without autoExclusive, clicking the group that
                        // is ALREADY selected toggles `checked` to false from
                        // C++, which does not re-run the binding below (its
                        // dependency, selectedGroup, did not change), so the
                        // row that is still showing its pane stops looking
                        // selected until another group is picked.
                        autoExclusive: true
                        checked: root.selectedGroup === modelData.key
                        onClicked: {
                            root.selectedGroup = modelData.key;
                            root.forceActiveFocus();
                        }
                        contentItem: Label {
                            leftPadding: 10
                            text: groupButton.text
                            textFormat: Text.PlainText
                            color: groupButton.checked ? Theme.text : Theme.textDim
                            font.pixelSize: Theme.fontSizeBody
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: Theme.radiusSmall
                            color: groupButton.checked ? Theme.surfaceSelected
                                 : groupButton.hovered ? Theme.surfaceHover : "transparent"
                            border.width: groupButton.visualFocus ? 2 : 0
                            border.color: Theme.accent
                        }
                    }
                }
            }
            Rectangle {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: Theme.borderSubtle
            }
        }

        Loader {
            id: paneLoader
            objectName: "settingsGroupLoader"
            // Group panes are small and switching them must finish before the
            // old pane is destroyed; asynchronous incubation can otherwise
            // leave a delegate attached to a disappearing settings window.
            asynchronous: false
            anchors.top: header.bottom
            anchors.left: sidebar.right
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 18
            sourceComponent: root.selectedGroup === "appearance"
                             ? appearancePane
                             : root.selectedGroup === "viewerDefaults"
                               ? viewerDefaultsPane
                               : root.selectedGroup === "server"
                                 ? serverPane : tmuxPane
        }
    }
    Component {
        id: appearancePane

        Flickable {
            objectName: "appearancePane"
            clip: true
            contentWidth: width
            contentHeight: appearanceColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            // Every scrollable view in this module uses the one shared bar
            // (AppScrollBar.qml); it hides itself while the content fits, so a
            // short pane is unchanged and a long one stops silently clipping.
            ScrollBar.vertical: AppScrollBar {}

            Column {
                id: appearanceColumn
                width: parent.width
                spacing: 16

                Label {
                    text: qsTr("Appearance")
                    color: Theme.text
                    font.bold: true
                    font.pixelSize: Theme.fontSizeTitle
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Changes apply immediately. Theme and group colours are applied by the appearance layer; terminal controls are applied by the terminal renderer.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
                }

                Row {
                    spacing: 14
                    Label {
                        width: 170
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Theme")
                        color: Theme.text
                        font.pixelSize: Theme.fontSizeBody
                    }
                    ChoiceBox {
                        objectName: "themeChoice"
                        model: [qsTr("Dark"), qsTr("Light")]
                        currentIndex: root.settingsObject()
                                     && root.settingsObject().theme === "light" ? 1 : 0
                        onActivated: {
                            var settings = root.settingsObject();
                            if (settings)
                                settings.theme = currentIndex === 1 ? "light" : "dark";
                        }
                    }
                }

                Row {
                    spacing: 14
                    Label {
                        width: 170
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Group colour palette")
                        color: Theme.text
                        font.pixelSize: Theme.fontSizeBody
                    }
                    ChoiceBox {
                        objectName: "groupPaletteChoice"
                        model: [qsTr("Plain"), qsTr("Tokyo Night")]
                        currentIndex: root.settingsObject()
                                     && root.settingsObject().groupPalette === "tokyonight" ? 1 : 0
                        onActivated: {
                            var settings = root.settingsObject();
                            if (settings)
                                settings.groupPalette = currentIndex === 1 ? "tokyonight" : "plain";
                        }
                    }
                }

                Row {
                    spacing: 14
                    Label {
                        width: 170
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Palette size")
                        color: Theme.text
                        font.pixelSize: Theme.fontSizeBody
                    }
                    NumberBox {
                        objectName: "groupPaletteSizeSpin"
                        from: 5
                        to: 64
                        value: root.settingsObject()
                               ? root.settingsObject().groupPaletteSize : 8
                        onValueModified: {
                            var settings = root.settingsObject();
                            if (settings)
                                settings.groupPaletteSize = value;
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.borderSubtle
                }

                Label {
                    text: qsTr("Toolbar button ordering")
                    color: Theme.text
                    font.bold: true
                    font.pixelSize: Theme.fontSizeLabel
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Use the keyboard-friendly up/down actions. Unknown saved ids are dropped and buttons added by a later build are appended.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
                }
                Rectangle {
                    objectName: "toolbarOrderList"
                    width: Math.min(parent.width, 540)
                    height: Math.max(36, Math.min(4 * 36, root.toolbarItems.length * 36))
                    color: Theme.surfaceSunken
                    border.width: 1
                    border.color: Theme.borderSubtle
                    ListView {
                        id: toolbarList
                        anchors.fill: parent
                        anchors.margins: 2
                        clip: true
                        // This list is capped at four rows tall, so it scrolls
                        // as soon as a fifth button exists.
                        ScrollBar.vertical: AppScrollBar {}
                        model: root.toolbarItems
                        delegate: Rectangle {
                            id: toolbarRow
                            required property string modelData
                            required property int index
                            width: toolbarList.width - 4
                            height: 32
                            color: toolbarRow.index % 2 === 0 ? Theme.surfaceSunken
                                                              : Theme.surfaceDeep
                            Label {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: moveButtons.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.toolbarLabel(toolbarRow.modelData)
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.fontSizeBody
                            }
                            Row {
                                id: moveButtons
                                anchors.right: parent.right
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2
                                SheetButton {
                                    objectName: "toolbarUp:" + toolbarRow.modelData
                                    text: qsTr("Up")
                                    enabled: toolbarRow.index > 0
                                    implicitWidth: 42
                                    implicitHeight: 26
                                    onClicked: root.moveToolbar(toolbarRow.index, -1)
                                }
                                SheetButton {
                                    objectName: "toolbarDown:" + toolbarRow.modelData
                                    text: qsTr("Down")
                                    enabled: toolbarRow.index + 1 < root.toolbarItems.length
                                    implicitWidth: 52
                                    implicitHeight: 26
                                    onClicked: root.moveToolbar(toolbarRow.index, 1)
                                }
                            }
                        }
                    }
                }

                Row {
                    spacing: 14
                    Label {
                        width: 170
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Terminal text size")
                        color: Theme.text
                        font.pixelSize: Theme.fontSizeBody
                    }
                    NumberBox {
                        objectName: "terminalFontSizeSpin"
                        from: 6
                        to: 48
                        value: root.settingsObject()
                               ? root.settingsObject().terminalFontSize : 13
                        onValueModified: {
                            var settings = root.settingsObject();
                            if (settings)
                                settings.terminalFontSize = value;
                        }
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("pt")
                        color: Theme.textDim
                        font.pixelSize: Theme.fontSizeBody
                    }
                }

                Row {
                    spacing: 14
                    Label {
                        width: 170
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Terminal rendering resolution")
                        color: Theme.text
                        font.pixelSize: Theme.fontSizeBody
                    }
                    ChoiceBox {
                        objectName: "terminalPixelRatioChoice"
                        model: [qsTr("Follow screen"), qsTr("1x"), qsTr("1.5x"),
                                qsTr("2x"), qsTr("3x"), qsTr("4x")]
                        currentIndex: root.pixelRatioIndex()
                        onActivated: {
                            var settings = root.settingsObject();
                            if (settings)
                                settings.terminalPixelRatio =
                                    root.pixelRatioForIndex(currentIndex);
                        }
                    }
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    // Says what the control can and cannot do, because the
                    // page refuses a value above the screen's own resolution:
                    // claiming more pixels than the display has adds no detail
                    // and used to make the whole terminal draw magnified.
                    text: qsTr("Text size is set above and never changes with this. "
                               + "\"Follow screen\" renders at your display's own "
                               + "resolution, which is the sharpest it can be; a lower "
                               + "value trades sharpness for speed, and a value above "
                               + "your display's is ignored.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
        }
    }

    // The one list the terminal-resolution choice is built from. It used to be
    // spelled out three times — the ComboBox model above, the index lookup and
    // the reverse lookup — and the two lookups had drifted: a stored ratio
    // outside the list (a hand-edited config, or one written by a newer build)
    // fell through to the last index, so the box claimed "4x" for, say, 1.25.
    // Entry N here is entry N of that model.
    readonly property var pixelRatioValues: [0, 1, 1.5, 2, 3, 4]

    // -1 for an unrecognised ratio, deliberately: there is no entry to point
    // at, and naming the wrong one tells the user their terminal renders at a
    // resolution it does not.
    function pixelRatioIndex() {
        var settings = root.settingsObject();
        var ratio = settings ? Number(settings.terminalPixelRatio) : 0;
        if (!isFinite(ratio) || ratio <= 0)
            return 0; // "Follow screen"
        for (var i = 1; i < root.pixelRatioValues.length; ++i) {
            if (Math.abs(ratio - root.pixelRatioValues[i]) < 0.01)
                return i;
        }
        return -1;
    }

    function pixelRatioForIndex(index) {
        return index >= 0 && index < root.pixelRatioValues.length
             ? root.pixelRatioValues[index] : 0;
    }

    Component {
        id: viewerDefaultsPane

        Flickable {
            objectName: "viewerDefaultsPane"
            clip: true
            contentWidth: width
            contentHeight: viewerDefaultsColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar {}

            Column {
                id: viewerDefaultsColumn
                width: parent.width
                spacing: 12

                Label {
                    text: qsTr("File viewers")
                    color: Theme.text
                    font.bold: true
                    font.pixelSize: Theme.fontSizeTitle
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Choose the default viewer for a file extension. Changes apply to panes that are already open.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
                }

                Row {
                    spacing: 8
                    TextField {
                        id: viewerExtensionInput
                        objectName: "viewerDefaultExtensionInput"
                        width: 190
                        implicitHeight: 30
                        color: Theme.text
                        placeholderText: qsTr("Extension, for example md")
                        placeholderTextColor: Theme.textDim
                        selectByMouse: true
                        font.pixelSize: Theme.fontSizeBody
                        background: Rectangle {
                            color: Theme.surfaceSunken
                            radius: Theme.radiusSmall
                            border.width: viewerExtensionInput.activeFocus ? 2 : 1
                            border.color: viewerExtensionInput.activeFocus
                                          ? Theme.accent : Theme.borderSubtle
                        }
                    }
                    ChoiceBox {
                        id: viewerKindChoice
                        objectName: "viewerDefaultKindChoice"
                        implicitWidth: 220
                        textRole: "label"
                        model: {
                            var kinds =
                                    root.validViewerKindsForExtension(
                                        viewerExtensionInput.text);
                            var choices = [];
                            for (var i = 0; i < kinds.length; ++i)
                                choices.push({
                                    value: String(kinds[i]),
                                    label: root.viewerKindLabel(kinds[i])
                                });
                            return choices;
                        }
                        // Reset on every model change rather than as a
                        // `currentIndex:` binding. The list of valid kinds is
                        // rebuilt for each extension the user types, so a slot
                        // that meant "Rendered Markdown" for `md` means
                        // something else — or nothing — for the next
                        // extension, and a binding cannot express "start again
                        // at the top of whatever list this now is" without
                        // depending on the selection it would be overwriting.
                        // Without the reset, "Add or update" stored whatever
                        // viewer happened to sit at the stale index.
                        onModelChanged: viewerKindChoice.currentIndex =
                            viewerKindChoice.model.length > 0 ? 0 : -1
                        Component.onCompleted: viewerKindChoice.currentIndex =
                            viewerKindChoice.model.length > 0 ? 0 : -1
                    }
                    SheetButton {
                        objectName: "viewerDefaultAddButton"
                        text: qsTr("Add or update")
                        enabled: viewerKindChoice.model.length > 0
                        onClicked: {
                            var settings = root.settingsObject();
                            var choices = viewerKindChoice.model || [];
                            var index = viewerKindChoice.currentIndex;
                            if (!settings || index < 0 || index >= choices.length)
                                return;
                            if (settings.setViewerDefault(
                                        viewerExtensionInput.text,
                                        String(choices[index].value))) {
                                viewerExtensionInput.text = "";
                                root.syncViewerDefaults();
                            }
                        }
                    }
                }

                Label {
                    visible: viewerExtensionInput.text.length > 0
                             && viewerKindChoice.model.length === 0
                    text: qsTr("Use letters and numbers only; specialised viewers are offered only for compatible file types.")
                    color: Theme.warning
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }

                Label {
                    visible: root.viewerDefaultEntries.length === 0
                    text: qsTr("No customised file viewers.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
                }
                Rectangle {
                    objectName: "viewerDefaultsList"
                    width: Math.min(parent.width, 620)
                    height: Math.max(36, Math.min(4 * 38,
                                                    root.viewerDefaultEntries.length * 38))
                    color: Theme.surfaceSunken
                    border.width: 1
                    border.color: Theme.borderSubtle
                    ListView {
                        id: viewerDefaultsList
                        anchors.fill: parent
                        anchors.margins: 2
                        clip: true
                        ScrollBar.vertical: AppScrollBar {}
                        model: root.viewerDefaultEntries
                        delegate: Rectangle {
                            id: viewerDefaultRow
                            required property var modelData
                            required property int index
                            width: viewerDefaultsList.width - 4
                            height: 36
                            color: viewerDefaultRow.index % 2 === 0
                                   ? Theme.surfaceSunken : Theme.surfaceDeep
                            Label {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: removeButton.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: "." + viewerDefaultRow.modelData.extension
                                      + "  \u2014  "
                                      + root.viewerKindLabel(viewerDefaultRow.modelData.kind)
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.fontSizeBody
                                elide: Text.ElideRight
                            }
                            SheetButton {
                                id: removeButton
                                objectName: "viewerDefaultRemove:"
                                            + viewerDefaultRow.modelData.extension
                                anchors.right: parent.right
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Remove")
                                implicitHeight: 26
                                onClicked: root.removeViewerDefault(
                                               viewerDefaultRow.modelData.extension)
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: serverPane

        Flickable {
            objectName: "serverPane"
            clip: true
            contentWidth: width
            contentHeight: serverColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar {}

            Column {
                id: serverColumn
                width: parent.width
                spacing: 12

                Label {
                    text: qsTr("Server")
                    color: Theme.text
                    font.bold: true
                    font.pixelSize: Theme.fontSizeTitle
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Every saved server lives here. Editing a field updates the selected profile immediately \u2014 there is no separate Save. Adding one needs no reachable server, so this is also where a first-run setup starts.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
                }

                ListView {
                    id: serverProfileListView
                    objectName: "serverProfileList"
                    // An empty list is a 36-pixel gap that reads as a missing
                    // row; the hint below says what is going on instead.
                    visible: root.profileEntries.length > 0
                    width: Math.min(parent.width, 620)
                    // One row height for both the view and its delegate. The
                    // delegate is a SheetButton, whose implicit height is a
                    // stand-alone button's 30, while this height reserved 36
                    // per row: the rows tiled 30 tall inside a box six pixels
                    // per row taller than them, so with one profile the row —
                    // and the name in it — sat three pixels above the centre
                    // of the box, and with five or more the viewport ended
                    // mid-row and cut a name in half.
                    readonly property int rowHeight: 36
                    height: Math.max(rowHeight,
                                     Math.min(4 * rowHeight,
                                              root.profileEntries.length * rowHeight))
                    clip: true
                    ScrollBar.vertical: AppScrollBar {}
                    model: root.profileEntries
                    delegate: SheetButton {
                        required property var modelData
                        objectName: "serverProfile:" + root.profileText(modelData, "id")
                        // The view, NOT `parent`: a delegate's parent is the
                        // view's contentItem, whose width is not the viewport
                        // width, so the rows came out an arbitrary size. Same
                        // rule the two lists in the Appearance pane follow.
                        width: serverProfileListView.width
                        implicitHeight: serverProfileListView.rowHeight
                        textAlignment: Text.AlignLeft
                        text: root.profileText(modelData, "name")
                              || root.profileText(modelData, "host")
                              || qsTr("Unnamed profile")
                        checkable: true
                        // One profile is selected at a time, and clicking the
                        // selected one must not leave it looking unselected:
                        // see the group list above for why a plain checkable
                        // button desynchronises from its `checked` binding.
                        autoExclusive: true
                        checked: root.selectedProfileId === root.profileText(modelData, "id")
                        onClicked: {
                            root.selectedProfileId = root.profileText(modelData, "id");
                            root.loadSelectedProfile();
                        }
                    }
                }
                Label {
                    objectName: "serverEmptyHint"
                    visible: root.profileEntries.length === 0
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("No server is configured yet. Choose Add server below, then fill in the host and the user name you log in with.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
                }

                Row {
                    spacing: 8
                    SheetButton {
                        objectName: "serverAddButton"
                        text: qsTr("Add server")
                        enabled: typeof app !== "undefined" && app && app.serverProfiles
                        onClicked: root.addProfile()
                    }
                    SheetButton {
                        objectName: "serverDeleteButton"
                        text: qsTr("Delete server")
                        accent: Theme.danger
                        enabled: root.hasSelection()
                                  && typeof app !== "undefined" && app && app.serverProfiles
                        onClicked: root.confirmDeleteProfile()
                    }
                }

                // Add server hands the caret to the Name field, which lives in
                // here and cannot be reached from the root by id.
                Connections {
                    target: root
                    function onProfileNameFocusRequested() {
                        serverNameField.input.forceActiveFocus();
                    }
                }

                // Editing fields with nothing selected would be typing into a
                // record that does not exist: saveProfile() refuses such a
                // write, so the keystrokes would simply vanish. The form is not
                // shown at all in that state and the hint above stands in its
                // place, which also keeps a blank form from reading as a
                // profile whose every value happens to be empty.
                Grid {
                    visible: root.hasSelection()
                    columns: 2
                    columnSpacing: 14
                    rowSpacing: 10
                    width: Math.min(parent.width, 640)

                    Field {
                        id: serverNameField
                        objectName: "serverField:name"
                        width: 300
                        label: qsTr("Name")
                        text: root.profileName
                        onTextChanged: {
                            if (!root.loadingProfile) {
                                root.profileName = text;
                                root.profileDirty = true;
                            }
                        }
                        onEditingFinished: root.saveProfile()
                    }
                    Field {
                        objectName: "serverField:host"
                        width: 300
                        label: qsTr("Host")
                        text: root.profileHost
                        onTextChanged: {
                            if (!root.loadingProfile) {
                                root.profileHost = text;
                                root.profileDirty = true;
                            }
                        }
                        onEditingFinished: root.saveProfile()
                    }
                    Field {
                        objectName: "serverField:port"
                        width: 300
                        label: qsTr("Port")
                        text: root.profilePort
                        onTextChanged: {
                            if (!root.loadingProfile) {
                                root.profilePort = text;
                                root.profileDirty = true;
                            }
                        }
                        onEditingFinished: root.saveProfile()
                    }
                    Field {
                        objectName: "serverField:user"
                        width: 300
                        label: qsTr("User")
                        text: root.profileUser
                        onTextChanged: {
                            if (!root.loadingProfile) {
                                root.profileUser = text;
                                root.profileDirty = true;
                            }
                        }
                        onEditingFinished: root.saveProfile()
                    }
                    Field {
                        objectName: "serverField:identityFile"
                        width: 300
                        label: qsTr("Identity file")
                        text: root.profileIdentityFile
                        onTextChanged: {
                            if (!root.loadingProfile) {
                                root.profileIdentityFile = text;
                                root.profileDirty = true;
                            }
                        }
                        onEditingFinished: root.saveProfile()
                    }
                    Field {
                        objectName: "serverField:nodePath"
                        width: 300
                        label: qsTr("Remote Node path")
                        text: root.profileNodePath
                        onTextChanged: {
                            if (!root.loadingProfile) {
                                root.profileNodePath = text;
                                root.profileDirty = true;
                            }
                        }
                        onEditingFinished: root.saveProfile()
                    }
                }

                // Deliberately NOT a Grid cell. A Grid sizes each column to its
                // widest child, so a 614-wide field in column 0 pushed column 1
                // — Host, User, Node path — out past the pane's right edge and
                // off screen. It is a full-width field, so it lives on its own
                // below the grid.
                Field {
                    objectName: "serverField:repoRoot"
                    visible: root.hasSelection()
                    width: Math.min(parent.width, 614)
                    label: qsTr("Remote CodeHarbor directory")
                    text: root.profileRepoRoot
                    onTextChanged: {
                        if (!root.loadingProfile) {
                            root.profileRepoRoot = text;
                            root.profileDirty = true;
                        }
                    }
                    onEditingFinished: root.saveProfile()
                }

                // ServerProfiles.updateProfile() silently REFUSES an edit that
                // would leave a profile unable to connect (it will not let a
                // bad edit corrupt a working profile), so without this the
                // fields simply stopped saving and nothing on screen said why.
                // Same sentence the Connect sheet shows for the same rule.
                //
                // Gated on hasSelection(), not on the id being non-empty: a
                // just-added profile that the user has half emptied out is
                // exactly the case this sentence exists for, while an id left
                // over from a deleted profile has no fields on screen to
                // explain and must not raise a warning about a record that is
                // already gone.
                Label {
                    objectName: "serverValidationHint"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    visible: root.hasSelection() && !root.profileValid()
                    text: EndpointField.hasRejectedCharacters(root.profileHost)
                          ? qsTr("The host cannot contain spaces, line breaks or control characters; this edit will not be saved.")
                          : EndpointField.hasRejectedCharacters(root.profileUser)
                            ? qsTr("The user name cannot contain spaces, line breaks or control characters; this edit will not be saved.")
                            : qsTr("Host, user and a port in 1-65535 are required; edits are not saved until all three are filled in.")
                    color: Theme.warning
                    font.pixelSize: Theme.fontSizeSmall
                }

                Row {
                    spacing: 8
                    SheetButton {
                        objectName: "serverConnectButton"
                        text: qsTr("Connect")
                        enabled: root.hasSelection()
                                  && typeof app !== "undefined" && app
                        accent: Theme.success
                        onClicked: app.connectToProfile(root.selectedProfileId)
                    }
                    SheetButton {
                        objectName: "serverDisconnectButton"
                        text: qsTr("Disconnect")
                        enabled: typeof app !== "undefined" && app
                        onClicked: app.disconnectServer()
                    }
                    SheetButton {
                        objectName: "serverUpgradeButton"
                        text: qsTr("Update Remote Service")
                        enabled: root.hasSelection()
                                  && typeof app !== "undefined" && app
                        accent: Theme.warning
                        onClicked: app.upgradeRemoteService(root.selectedProfileId)
                    }
                }
            }
        }
    }

    Component {
        id: tmuxPane

        Flickable {
            objectName: "tmuxPane"
            clip: true
            contentWidth: width
            contentHeight: tmuxColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar {}
            Column {
                id: tmuxColumn
                width: parent.width
                spacing: 14
                Label {
                    text: qsTr("Tmux")
                    color: Theme.text
                    font.bold: true
                    font.pixelSize: Theme.fontSizeTitle
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Mouse reporting is enabled for each CodeHarbor tmux session, so wheel scrolling reaches tmux history. It is scoped to that session and cannot change another session.")
                    color: Theme.text
                    font.pixelSize: Theme.fontSizeBody
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Session names are minted and safely shaped on the server. They are not client preferences, so this group has no dead name-editing control.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
                }
            }
        }
    }
}
