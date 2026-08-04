pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import CodeHarbor

// Full-surface settings sheet. Main owns the `shown` flag; this component owns
// focus, Escape and the preference controls so each value is written directly
// to the client-local AppSettings object.
Rectangle {
    id: root
    objectName: "settingsWindow"
    property bool shown: false
    property string selectedGroup: "appearance"
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
    signal dismissed()

    visible: shown
    color: Theme.surface
    border.width: 1
    border.color: Theme.borderSubtle
    focus: shown

    component SheetButton: Button {
        id: button
        property color accent: Theme.border
        implicitHeight: 30
        leftPadding: 12
        rightPadding: 12
        focusPolicy: Qt.StrongFocus
        contentItem: Label {
            text: button.text
            textFormat: Text.PlainText
            color: button.enabled ? Theme.text : Theme.textFaint
            font.pixelSize: Theme.fontSizeBody
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: !button.enabled ? Theme.surfaceHover
                 : button.down ? Theme.border
                 : button.hovered ? Theme.surfaceHover : Theme.surfaceRaised
            border.width: button.visualFocus ? 2 : 1
            border.color: button.visualFocus ? Theme.accent
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
            text: numberBox.textFromValue(numberBox.value, numberBox.locale)
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
        case "terminal.kill": return qsTr("Kill terminal session");
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

    function saveProfile() {
        if (root.loadingProfile || root.selectedProfileId.length === 0)
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

    function closeSheet() {
        root.shown = false;
        root.dismissed();
    }

    onShownChanged: {
        if (root.shown) {
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
                        model: root.toolbarItems
                        delegate: Rectangle {
                            required property string modelData
                            required property int index
                            width: toolbarList.width - 4
                            height: 32
                            color: index % 2 === 0 ? Theme.surfaceSunken : Theme.surfaceDeep
                            Label {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: moveButtons.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.toolbarLabel(modelData)
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
                                    objectName: "toolbarUp:" + modelData
                                    text: qsTr("Up")
                                    enabled: index > 0
                                    implicitWidth: 42
                                    implicitHeight: 26
                                    onClicked: root.moveToolbar(index, -1)
                                }
                                SheetButton {
                                    objectName: "toolbarDown:" + modelData
                                    text: qsTr("Down")
                                    enabled: index + 1 < root.toolbarItems.length
                                    implicitWidth: 52
                                    implicitHeight: 26
                                    onClicked: root.moveToolbar(index, 1)
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
            }
        }
    }

    function pixelRatioIndex() {
        var settings = root.settingsObject();
        var ratio = settings ? Number(settings.terminalPixelRatio) : 0;
        if (ratio <= 0)
            return 0;
        if (Math.abs(ratio - 1.0) < 0.01)
            return 1;
        if (Math.abs(ratio - 1.5) < 0.01)
            return 2;
        if (Math.abs(ratio - 2.0) < 0.01)
            return 3;
        if (Math.abs(ratio - 3.0) < 0.01)
            return 4;
        return 5;
    }

    function pixelRatioForIndex(index) {
        return [0, 1, 1.5, 2, 3, 4][index] || 0;
    }

    Component {
        id: viewerDefaultsPane

        Flickable {
            objectName: "viewerDefaultsPane"
            clip: true
            contentWidth: width
            contentHeight: viewerDefaultsColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

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
                        currentIndex: model.length > 0 ? 0 : -1
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
                        model: root.viewerDefaultEntries
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            width: viewerDefaultsList.width - 4
                            height: 36
                            color: index % 2 === 0
                                   ? Theme.surfaceSunken : Theme.surfaceDeep
                            Label {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: removeButton.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: "." + modelData.extension + "  \u2014  "
                                      + root.viewerKindLabel(modelData.kind)
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.fontSizeBody
                                elide: Text.ElideRight
                            }
                            SheetButton {
                                id: removeButton
                                objectName: "viewerDefaultRemove:"
                                            + modelData.extension
                                anchors.right: parent.right
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Remove")
                                implicitHeight: 26
                                onClicked: root.removeViewerDefault(
                                               modelData.extension)
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
                    text: qsTr("These fields are the saved ServerProfiles connection values. Editing a field updates the profile immediately; use the Connect sheet to add or remove profiles.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
                }

                ListView {
                    objectName: "serverProfileList"
                    width: Math.min(parent.width, 620)
                    height: Math.max(36, Math.min(4 * 36, root.profileEntries.length * 36))
                    clip: true
                    model: root.profileEntries
                    delegate: SheetButton {
                        required property var modelData
                        objectName: "serverProfile:" + root.profileText(modelData, "id")
                        width: parent ? parent.width : 500
                        text: root.profileText(modelData, "name")
                              || root.profileText(modelData, "host")
                              || qsTr("Unnamed profile")
                        checkable: true
                        checked: root.selectedProfileId === root.profileText(modelData, "id")
                        onClicked: {
                            root.selectedProfileId = root.profileText(modelData, "id");
                            root.loadSelectedProfile();
                        }
                    }
                }
                Label {
                    visible: root.profileEntries.length === 0
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("No saved profile is available. Use Connect to Server from the command palette to add one.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
                }

                Grid {
                    columns: 2
                    columnSpacing: 14
                    rowSpacing: 10
                    width: Math.min(parent.width, 640)

                    Field {
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
                    Field {
                        width: 614
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
                }

                Row {
                    spacing: 8
                    SheetButton {
                        objectName: "serverConnectButton"
                        text: qsTr("Connect")
                        enabled: root.selectedProfileId.length > 0
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
                        enabled: root.selectedProfileId.length > 0
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
