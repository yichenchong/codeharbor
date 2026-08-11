// Bound: the ListView delegate below reads this file's `root` id, and binding
// the component's context is what makes that resolvable statically (qmllint)
// instead of only at run time.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import CodeHarbor

// Server connection sheet (SPEC 4.1, 12.1): the one piece of UI that lets a
// user reach a server at all — pick one of the saved connection profiles,
// connect, and answer the first-use host-key and credential prompts.
//
// It is a CONNECTOR, not an editor. Profiles are created, edited and deleted in
// the application settings window's Server pane, which is the single place that
// owns them; this sheet only displays what is stored and asks to be pointed at
// that window (settingsRequested) when there is nothing to connect to yet.
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
//   settingsRequested()          open the settings window that owns profiles
//   credentialSubmitted(secret, kind) answer pending credential; "" cancels
//   dismissed()                  user closed the sheet (Esc / Close)
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
    signal settingsRequested()
    signal hostKeyDecision(bool accept)
    signal credentialSubmitted(string secret, string kind)
    signal dismissed()

    // ---- internal state ---------------------------------------------------
    // The profile the list has picked out; "" when there is nothing to connect
    // to. Connect and the detail panel both read it.
    property string selectedId: ""
    // The last explicit choice survives asynchronous model replacement.
    property string pendingSelectionId: ""
    // What held the keyboard before this sheet covered the workspace, so
    // closing it puts the user back where they were rather than dropping focus
    // at the window root. Maintained by containKeyboard() below.
    property var focusReturnItem: null
    // The last item inside the boundary that held the keyboard. It is what says
    // which DIRECTION an escape was travelling in, and it is where focus goes
    // if the chain cannot be re-entered at all.
    property var lastInsideFocus: null

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

    // A full-surface sheet that takes the keyboard and blocks every region
    // behind it is a dialog in everything but type; without a role and a name
    // a screen reader announces an unlabelled rectangle and never says that a
    // connection sheet has opened. Same rule LogView and SettingsWindow follow.
    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("Connect to a server")

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

    function selectIndex(index) {
        var list = root.profileList();
        if (index < 0 || index >= list.length)
            return;
        root.selectedId = root.textOf(list[index], "id");
        profileSelector.currentIndex = index;
    }

    // The profile Connect would use, or null when nothing is picked.
    function selectedEntry() {
        var index = root.indexOfId(root.selectedId);
        return index >= 0 ? root.profileList()[index] : null;
    }

    // The ListView owns currentIndex for key navigation, but `selectedId` is
    // the selection: a model swap can clamp or reset currentIndex behind our
    // back, so the highlight follows selectedId and currentIndex is re-applied
    // once the new model has actually been installed.
    function applyCurrentIndex() {
        profileSelector.currentIndex = root.indexOfId(root.selectedId);
    }

    // Re-anchor the selection after the host swapped the list in: stay on the
    // same profile when it survived, otherwise fall back to the host's active
    // one and finally to the first row, so Connect is never aimed at nothing
    // while there is something to connect to.
    //
    // Gaining a selection out of nothing (first load with saved servers, or the
    // first profile arriving from the settings window) also moves focus to the
    // list, so the very next keystroke can pick a server and Enter connects.
    function syncFromModel(preferredId) {
        var list = root.profileList();
        var preservedId = preferredId || root.selectedId;
        var hadSelection = preservedId !== "";
        var index = root.indexOfId(preservedId);
        if (index < 0)
            index = root.indexOfId(root.activeId);
        if (index < 0 && list.length > 0)
            index = 0;
        if (index >= 0)
            root.selectIndex(index);
        else
            root.selectedId = "";
        root.pendingSelectionId = "";
        Qt.callLater(root.applyCurrentIndex);
        if (!hadSelection && root.selectedId !== "")
            profileSelector.forceActiveFocus();
    }

    function connectNow() {
        if (root.selectedId !== "")
            root.connectRequested(root.selectedId);
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

    // A QVariantList property can notify before the ListView has installed the
    // replacement model. Defer the reconciliation so the lookup sees the new
    // rows; otherwise a preserved selection can be replaced by activeId.
    onProfilesChanged: {
        if (root.selectedId !== "")
            root.pendingSelectionId = root.selectedId;
        Qt.callLater(() => root.syncFromModel(root.pendingSelectionId));
    }
    onActiveIdChanged: {
        // Follow the host's active profile: whatever it just connected to is
        // what this sheet should be describing.
        if (root.activeId === root.selectedId)
            return;
        var index = root.indexOfId(root.activeId);
        if (index >= 0)
            root.selectIndex(index);
    }
    // A focus grab that has been queued with Qt.callLater runs a turn later, by
    // which time something else can legitimately own the keyboard — a modal
    // AppDialog opened in between is the case that actually happens, since
    // dismissing a prompt and opening the SSH details view are two clicks a user
    // makes in a row. Taking focus back then would drag the user out of the
    // dialog they just opened, so a queued grab checks that the sheet is still
    // the right place for the keyboard before it takes it.
    function focusWhenStillOurs(item) {
        if (!root.visible || !item || !item.visible || !item.enabled)
            return;
        const holder = root.Window.activeFocusItem;
        if (holder && root.itemWithin(Overlay.overlay, holder))
            return;
        item.forceActiveFocus();
    }

    onPendingHostKeyChanged: {
        if (root.pendingHostKey)
            Qt.callLater(() => root.focusWhenStillOurs(hostKeyReject));
        else if (root.visible)
            Qt.callLater(() => root.focusWhenStillOurs(profileSelector));
    }
    onPendingCredentialChanged: {
        // Cleared on BOTH edges: on open so a previous attempt's keystrokes can
        // never be resubmitted, and on close so the secret is not left sitting
        // in a live QML item (and its undo stack) after it has been spent.
        secretField.clear();
        if (root.pendingCredential)
            Qt.callLater(() => root.focusWhenStillOurs(secretField));
        else if (root.visible)
            Qt.callLater(() => root.focusWhenStillOurs(profileSelector));
    }
    onVisibleChanged: {
        if (!root.visible) {
            // Re-checked for liveness: the item can have been destroyed, hidden
            // or disabled while the sheet was up.
            var previous = root.focusReturnItem;
            root.focusReturnItem = null;
            if (previous && previous.visible && previous.enabled)
                previous.forceActiveFocus(Qt.OtherFocusReason);
            return;
        }
        // Normally containKeyboard() has already recorded where the keyboard
        // was, because it records on every focus change while this sheet is
        // down. The exception is the very first open of a session in which
        // focus has never moved since startup: nothing changed, so nothing was
        // recorded, and closing would drop the user at the window root. Take
        // the reading here too, before the focus binding below moves it.
        if (!root.focusReturnItem) {
            const holder = root.Window.activeFocusItem;
            if (holder && !root.itemWithin(root, holder))
                root.focusReturnItem = holder;
        }
        Qt.callLater(() => {
            if (root.pendingCredential)
                secretField.forceActiveFocus();
            else if (root.pendingHostKey)
                hostKeyReject.forceActiveFocus();
            else if (root.profileList().length > 0)
                profileSelector.forceActiveFocus();
            else
                // Nothing to pick: the only useful thing on screen is the way
                // out to the window that can create a server.
                openSettingsButton.forceActiveFocus();
        });
    }
    Component.onCompleted: root.syncFromModel()

    // ---- keyboard containment ---------------------------------------------
    // This sheet has to contain the KEYBOARD as well as the pointer. The
    // MouseArea shield above stops clicks reaching the regions behind it, but
    // Tab is not a click: Qt Quick's focus chain runs over the whole item tree,
    // so tabbing off the last control here walked straight into workspace
    // controls the user cannot see — acting on a session they are not looking
    // at — and nothing told a screen reader the dialog had been left.
    //
    // Corrected AFTER the move rather than intercepted before it. Intercepting
    // is not available: Qt performs Tab navigation inside the FOCUSED item's
    // own key handling, so a Keys handler on this root never sees the press —
    // measured, not assumed. Watching where focus actually landed works for
    // every route into an item, including a programmatic one.

    // The subtree the keyboard may not leave. While a prompt is up that is the
    // PROMPT, not the whole sheet: the prompt is what the connection is parked
    // on, and containing only the sheet would leave Tab free to wander the body
    // behind it.
    function focusBoundary() {
        if (root.pendingCredential)
            return credentialPanel;
        if (root.pendingHostKey)
            return hostKeyPanel;
        return root;
    }

    function itemWithin(scope, item) {
        for (var walk = item; walk; walk = walk.parent) {
            if (walk === scope)
                return true;
        }
        return false;
    }

    function containKeyboard() {
        var holder = root.Window.activeFocusItem;
        if (!holder)
            return;
        if (!root.visible) {
            // Down: remember where the keyboard is, so opening and closing the
            // sheet returns the user to the control they were on.
            if (!root.itemWithin(root, holder))
                root.focusReturnItem = holder;
            return;
        }
        // A modal AppDialog (the SSH details view) draws into the window's
        // overlay, which is not a child of this sheet. It owns the keyboard
        // while it is up and must not be fought for it.
        if (root.itemWithin(Overlay.overlay, holder))
            return;
        var boundary = root.focusBoundary();
        if (root.itemWithin(boundary, holder)) {
            root.lastInsideFocus = holder;
            return;
        }
        var previous = root.lastInsideFocus;
        if (!previous)
            return;
        // Keep going the way the user was going: continuing forward past the
        // item Tab escaped to re-enters the boundary at its FIRST control,
        // which is the wrap, and continuing backward re-enters at its last.
        var forward = previous.nextItemInFocusChain(true) === holder;
        var step = holder;
        for (var guard = 0; guard < 500; ++guard) {
            step = step.nextItemInFocusChain(forward);
            if (!step || step === holder)
                break;
            if (root.itemWithin(boundary, step)) {
                step.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason);
                return;
            }
        }
        // Nothing else in the boundary is reachable; stay where we were.
        previous.forceActiveFocus();
    }

    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() { root.containKeyboard(); }
    }

    Keys.onEscapePressed: (event) => {
        if (root.pendingCredential)
            root.cancelSecret();
        else if (root.pendingHostKey)
            root.hostKeyDecision(false);
        else
            root.dismissed();
        event.accepted = true;
    }

    // ---- one read-only detail row -----------------------------------------
    component DetailRow: Row {
        id: detailRow
        property string label: ""
        property string value: ""

        spacing: 8
        // A profile need not carry an identity file or a repository root, and a
        // row reading "Private key file:" with nothing after it is noise.
        visible: detailRow.value.length > 0

        Label {
            width: 116
            text: detailRow.label
            color: Theme.statusText()
            font.pixelSize: 11
        }
        Label {
            width: Math.max(0, detailRow.width - 116 - detailRow.spacing)
            // Same rule as errorLabel below: a stored profile is data (it can
            // arrive hand-edited from disk), never markup.
            textFormat: Text.PlainText
            text: detailRow.value
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            elide: Text.ElideRight
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
                // Focusable on purpose: the one sentence that says what the
                // current state MEANS used to be reachable by hovering only, so
                // a keyboard user could never read it. Tab stops here and Tab
                // again leaves, so nothing is trapped.
                activeFocusOnTab: true
                border.width: statusChip.activeFocus ? 2 : 1
                border.color: statusChip.activeFocus
                              ? Theme.accent : root.stateColor(root.connectionState)

                // The chip says the state three ways on screen but had no
                // identity at all to assistive technology. The word stays the
                // name; the explanation is the description, so it is announced
                // without a pointer anywhere near it.
                Accessible.role: Accessible.StaticText
                Accessible.name: stateLabel.text
                Accessible.description: root.stateExplanation(root.connectionState)

                HoverHandler { id: chipHover }
                // The module's one tooltip (AppToolTip.qml). The attached
                // `ToolTip.text` form is drawn by the Basic style in that
                // style's own light palette, so an explanation of this dark
                // sheet's status chip arrived as a white box. Shown on keyboard
                // focus too, because a hover-only explanation does not exist
                // for anyone without a pointer.
                AppToolTip {
                    objectName: "statusChipTip"
                    visible: chipHover.hovered || statusChip.activeFocus
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
                                    // Nothing in this project tells QML that
                                    // the user prefers reduced motion: Qt
                                    // 6.10's accessibility hints carry contrast
                                    // preference and nothing else, and there is
                                    // no CodeHarbor setting for it either. The
                                    // one honest gate is visibility — an
                                    // infinite animator used to keep turning
                                    // while the sheet was hidden, which is
                                    // motion nobody can see and nobody asked
                                    // for. Progress is still carried without
                                    // any movement by the dot's glyph and the
                                    // state word beside it.
                                    running: connectingIndicator.running && root.visible
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
                        id: stateLabel
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
        // An error that is only drawn is an error a screen reader user editing
        // the field that caused it never hears about. AlertMessage is the role
        // for a notification that is not a dialog; Main.qml's toast uses it for
        // the same reason.
        Accessible.role: Accessible.AlertMessage
        Accessible.name: errorLabel.text

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
                anchors.bottom: parent.bottom
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
                    if (root.textOf(list[profileSelector.currentIndex], "id") !== root.selectedId)
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
                                         === root.selectedId

                    background: Rectangle {
                        color: root.textOf(profileDelegate.modelData, "id") === root.selectedId
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

                    contentItem: Item {
                        Column {
                            id: profileContent
                            objectName: "profileContent" + profileDelegate.index
                            anchors.centerIn: parent
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
                    text: qsTr("No servers yet.\nAdd one in the settings window.")
                }
            }
        }

        // What Connect will actually use. Read-only on purpose: profiles are
        // created and edited in the settings window's Server pane, so this
        // sheet has no second copy of that form to disagree with the store.
        Item {
            id: detailPane
            anchors.top: parent.top
            anchors.left: listPane.right
            anchors.right: parent.right
            anchors.bottom: parent.bottom

            Column {
                id: detail
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 14
                spacing: 8

                // First run: an empty list says nothing about what CodeHarbor
                // wants from you, and there is no form here to fill in any
                // more. The only way forward is the settings window, so name it
                // and point at the button below that opens it.
                Label {
                    objectName: "coldStartIntro"
                    width: detail.width
                    visible: root.profileList().length === 0
                    wrapMode: Text.WordWrap
                    color: Theme.statusText()
                    font.pixelSize: Theme.fontSizeBody
                    text: qsTr("CodeHarbor edits a checkout that lives on another machine, over SSH. "
                               + "No such machine is described yet. Press \u201cServer settings\u2026\u201d "
                               + "below to add one — its address, your login, and the absolute path "
                               + "to node on it — then come back here and press Connect.")
                }

                Label {
                    objectName: "detailTitle"
                    width: detail.width
                    visible: root.selectedId !== ""
                    textFormat: Text.PlainText
                    text: root.textOf(root.selectedEntry(), "name")
                    color: Theme.text
                    font.pixelSize: Theme.fontSizeLabel
                    font.bold: true
                    elide: Text.ElideRight
                }

                DetailRow {
                    objectName: "detailEndpoint"
                    width: detail.width
                    label: qsTr("Address")
                    value: root.selectedId === ""
                           ? "" : root.textOf(root.selectedEntry(), "user") + "@"
                                  + root.textOf(root.selectedEntry(), "host") + ":"
                                  + root.textOf(root.selectedEntry(), "port")
                }
                DetailRow {
                    objectName: "detailIdentityFile"
                    width: detail.width
                    label: qsTr("Private key file")
                    value: root.textOf(root.selectedEntry(), "identityFile")
                }
                DetailRow {
                    objectName: "detailNodePath"
                    width: detail.width
                    label: qsTr("Node path")
                    value: root.textOf(root.selectedEntry(), "nodePath")
                }
                DetailRow {
                    objectName: "detailRepoRoot"
                    width: detail.width
                    label: qsTr("Repository root")
                    value: root.textOf(root.selectedEntry(), "repoRoot")
                }

                Label {
                    objectName: "editHint"
                    width: detail.width
                    visible: root.selectedId !== ""
                    wrapMode: Text.WordWrap
                    color: Theme.textDim
                    font.pixelSize: 11
                    text: qsTr("Servers are added, changed and removed in the settings window.")
                }
            }

            Row {
                id: paneActions
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.margins: 14
                spacing: 8

                SheetButton {
                    id: openSettingsButton
                    objectName: "openSettingsButton"
                    text: qsTr("Server settings\u2026")
                    onClicked: root.settingsRequested()

                    HoverHandler { id: settingsHover }
                    AppToolTip {
                        objectName: "openSettingsButtonTip"
                        visible: settingsHover.hovered
                        text: qsTr("Open the settings window, where servers are added, "
                                   + "edited, removed and updated.")
                    }
                }
                SheetButton {
                    objectName: "connectButton"
                    accent: Theme.accent
                    text: qsTr("Connect")
                    enabled: root.selectedId !== ""
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
            objectName: "hostKeyPanel"
            anchors.centerIn: parent
            width: Math.min(520, hostKeyPrompt.width - 48)
            height: hostKeyColumn.implicitHeight + 32
            radius: Theme.radiusMedium
            color: Theme.surfaceDeep
            border.width: 1
            border.color: Theme.warning

            // A panel that covers the sheet and disables every control under it
            // is a modal dialog. Without a role and a name a screen reader is
            // never told that a blocking security decision took over the
            // surface the user was working on.
            Accessible.role: Accessible.Dialog
            Accessible.name: hostKeyTitle.text

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
                    id: hostKeyTitle
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
            objectName: "credentialPanel"
            anchors.centerIn: parent
            width: Math.min(520, credentialPrompt.width - 48)
            height: credentialColumn.implicitHeight + 32
            radius: Theme.radiusMedium
            color: Theme.surfaceDeep
            border.width: 1
            border.color: Theme.accent

            // Modal for the same reason the host-key panel is, and announced
            // the same way: the connection is parked on this one answer.
            Accessible.role: Accessible.Dialog
            Accessible.name: credentialTitle.text

            Column {
                id: credentialColumn
                anchors.centerIn: parent
                width: credentialPanel.width - 32
                spacing: 8

                Label {
                    id: credentialTitle
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
                Label {
                    id: secretFieldLabel
                    objectName: "credentialFieldLabel"
                    // The field below is masked and its placeholder is the
                    // server's own prompt string, which disappears the moment
                    // the first character is typed. A low-vision user then has
                    // nothing on screen saying what the box wants, so the label
                    // is drawn rather than only spoken.
                    text: root.credentialKind() === "password"
                          ? qsTr("Password") : qsTr("Key passphrase")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeSmall
                }
                TextField {
                    id: secretField
                    objectName: "credentialField"
                    width: parent.width
                    // The visible Label above is a separate item, so without
                    // this the masked box is announced as an unlabelled
                    // password field.
                    Accessible.name: secretFieldLabel.text
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
