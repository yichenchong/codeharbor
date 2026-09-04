import QtQuick
import QtQuick.Controls
import CodeHarbor.Mobile
// MobileAppController.Servers and friends: the C++ type module.
import CodeHarbor.Mobile.Core

// The mobile shell's one window.
//
// The whole client is a single-pane stack: pick a server, pick a Dev Session,
// pick a pane, then look at that ONE pane. There is no split, no region, no
// sidebar and no window chrome — every one of those is a desktop affordance that
// assumes a pointer and a screen big enough to show two things at once.
//
// The stack DEPTH is derived from ch::MobileAppController::navStage rather than
// being pushed and popped by the pages themselves. That is the important
// structural decision here: the controller is the single source of truth for
// where the user is (it has to be — it also releases the live pane on the way
// out, and it is driven from C++ by things QML never sees, such as the active
// session disappearing from the authoritative workspace tree), so QML MIRRORS it.
// A stack that pushed its own pages would be a second, disagreeing copy of that
// state, and the first thing to break would be exactly the case the invariant
// exists for: a pane left alive because the view popped without telling anybody.
ApplicationWindow {
    id: window

    // Every context property is guarded, here and everywhere in this module. A
    // page loaded bare — by a test harness, by qmlls, by a future host — must
    // degrade to inert chrome, and a bare `mobile` is a ReferenceError rather
    // than `undefined`, so the `typeof` half of each guard is load bearing.
    readonly property var ctl: (typeof mobile !== "undefined") ? mobile : null

    visible: true
    // A size only a desktop host ever honours: on Android and iOS the window
    // fills the screen and these are ignored. They are the phone-shaped default
    // the shell is designed against, so the desktop-hosted build shows the same
    // layout a device does.
    width: 420
    height: 880
    title: qsTr("CodeHarbor")
    color: MobileTheme.surface

    // The system-bar insets, read HERE and published.
    //
    // SafeArea attaches to a Window, so this asks directly - no probe item. What
    // it does NOT work on is a Popup: measured, a Popup and a Popup's
    // contentItem both report zero, so a dialog cannot ask for its own insets
    // and has to be handed them. ConnectPage does that for the key sheet.
    //
    // This matters because the app targets SDK 35, where Android 15 enforces
    // edge-to-edge: the window extends BEHIND the navigation bar, so "inside the
    // window" is not "reachable".
    readonly property real safeAreaTop: SafeArea.margins.top
    readonly property real safeAreaBottom: SafeArea.margins.bottom

    // The platform back gesture, half one: the window close.
    //
    // On Android the back press arrives as a Qt::Key_Back KEY PRESS, and the
    // platform only turns it into a window close when nothing accepted that
    // press (QGuiApplicationPrivate::processKeyEvent). So this handler is the
    // path taken at the ROOT of the stack, where the Shortcut below is disabled
    // and therefore does not accept the key — and at the root, back IS a request
    // to leave the application, so the close is allowed through. The refusal
    // branch is what covers a host that closes the window some other way while
    // there is still somewhere to go back to.
    onClosing: function(close) {
        if (window.ctl && window.ctl.navStage !== MobileAppController.Servers) {
            close.accepted = false;
            window.ctl.back();
        }
    }

    // Half two, and the one that actually runs on a device below the root stage.
    // A shortcut is matched BEFORE the key is delivered to the focused item,
    // which is precisely why it is needed: TerminalPage accepts every key press
    // it is given, so without this the back gesture would be swallowed by the
    // terminal and the user could never leave a terminal pane.
    //
    // The sequences are spelled out rather than taken from StandardKey.Back /
    // StandardKey.Cancel, because those two drag in bindings that collide with
    // ordinary typing — and a window shortcut wins over the focused item:
    //   * StandardKey.Cancel is Escape, which the terminal pane must be able to
    //     send (a hardware or Bluetooth keyboard is common on a tablet, and Esc
    //     is how you leave insert mode);
    //   * StandardKey.Back includes Backspace on Windows, so on the
    //     desktop-hosted build every correction typed into the connect form
    //     navigated back instead of deleting a character.
    // "Back" is Qt::Key_Back — the device gesture — and Alt+Left is the
    // desktop-hosted equivalent, neither of which is a text key.
    Shortcut {
        sequences: ["Back", "Alt+Left"]
        enabled: window.ctl !== null
                 && window.ctl.navStage !== MobileAppController.Servers
        onActivated: window.ctl.back()
    }

    Column {
        anchors.fill: parent

        // Above the stack, not inside a page: the message that matters most —
        // a failed connect, a refused session — is usually the one that just
        // changed which page you are on, so a strip owned by a page would be
        // destroyed by the very transition it is explaining.
        MobileErrorBar {
            id: errorBar
            width: parent.width
            // Silent while the busy veil is up. The veil already shows
            // `statusText` in the middle of the screen, and a strip repeating
            // the same sentence at the top is the same message twice on a
            // surface with no room for it. Nothing is hidden by this: the veil
            // only covers "connecting" and "layout pending", and a FAILURE moves
            // connectionState to "error", which takes the veil down and brings
            // the strip straight back with the reason in it.
            message: (window.ctl && !busy.visible) ? window.ctl.statusText : ""
            // Only a genuine failure is coloured as one. Progress notes and
            // refusals that are not the connection's fault ("that pane is no
            // longer part of this session's layout") ride the quiet shade,
            // because a strip that is red for everything is red for nothing.
            failure: window.ctl !== null
                     && window.ctl.connectionState === "error"
        }

        StackView {
            id: stack
            width: parent.width
            height: parent.height - errorBar.height
            clip: true

            // Depth is navStage + 1: stage Servers is one page deep, Pane is
            // four. Kept in sync in ONE direction only (controller -> view), by
            // popping down to the wanted depth and pushing the pages that are
            // missing. Nothing here ever calls the controller back, so a sync
            // can never recurse.
            function stageComponent(stage) {
                switch (stage) {
                case MobileAppController.Servers:
                    return connectComponent;
                case MobileAppController.Sessions:
                    return sessionsComponent;
                case MobileAppController.Panes:
                    return panesComponent;
                default:
                    return paneHostComponent;
                }
            }

            function syncToStage() {
                const wanted = window.ctl
                             ? window.ctl.navStage + 1
                             : MobileAppController.Servers + 1;
                // Pop first: a deeper stack holds pages for stages the user has
                // left, and PaneHostPage in particular must be destroyed (and
                // its pane with it) before anything else happens.
                //
                // Each step checks that the depth actually MOVED. StackView
                // refuses a push or a pop issued while it is already modifying
                // its elements — it warns and returns null rather than throwing
                // — so a loop that only tested the depth against the target
                // would spin forever and freeze the UI thread the first time a
                // page changed the stage from inside its own construction.
                // Giving up on a refusal leaves the view one stage out of date;
                // hanging leaves the user with a dead phone.
                while (stack.depth > wanted) {
                    const beforePop = stack.depth;
                    stack.pop(StackView.Immediate);
                    if (stack.depth === beforePop)
                        return;
                }
                while (stack.depth < wanted) {
                    const beforePush = stack.depth;
                    stack.push(stack.stageComponent(stack.depth));
                    if (stack.depth === beforePush)
                        return;
                }
            }

            initialItem: connectComponent

            Component.onCompleted: syncToStage()
        }
    }

    Connections {
        target: window.ctl
        function onNavStageChanged() { stack.syncToStage(); }
    }

    // The busy veil is a sibling of the stack and covers the whole window, so a
    // page cannot be interacted with while the thing it is about is still on the
    // wire. Two occasions, both of them a round trip the user must not start
    // twice: a connect attempt, and a layout load.
    MobileBusy {
        id: busy
        anchors.fill: parent
        visible: window.ctl
                 && (window.ctl.connectionState === "connecting"
                     || window.ctl.layoutPending)
        message: {
            if (!window.ctl)
                return "";
            if (window.ctl.layoutPending)
                return qsTr("Loading the session layout…");
            return window.ctl.statusText;
        }
    }

    Component {
        id: connectComponent
        ConnectPage {}
    }
    Component {
        id: sessionsComponent
        SessionPickerPage {}
    }
    Component {
        id: panesComponent
        PanePickerPage {}
    }
    Component {
        id: paneHostComponent
        PaneHostPage {}
    }
}
