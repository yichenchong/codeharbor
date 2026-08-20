import QtQuick
import QtQuick.Controls
import CodeHarbor.Mobile
// MobileTerminalView, and the session type createTerminalSession() returns.
import CodeHarbor.Mobile.Core

// THE single terminal pane of the mobile client (SPEC 3.4 / 5.1 on a touch
// device). One page, one terminal, no splits: the mobile shell shows exactly one
// pane at a time and the user reaches it by picking a Dev Session and then a
// pane, so there is no region, no ratio and no window chrome here.
//
// It is the mobile counterpart of src/qml/TerminalPaneView.qml and shares NO code
// with it, because that pane is a Qt WebEngine view hosting xterm.js and neither
// WebEngine nor WebChannel exists on Android or iOS. The bytes are interpreted by
// ch_vt in process and drawn by ch::MobileTerminalView; the flow control, the tmux
// identity and the attach sequence are the same ones the desktop uses, through
// ch::MobileTerminalSession.
//
// WIRING (set by PaneHostPage through Loader.setSource, never read from
// `mobile.selectedPane`): the host clears its selection the instant it navigates
// away, so a binding to it would empty out underneath this page while it is being
// torn down and the pane would try to reattach to nothing.
//
// Context properties (`mobile`, `app`, `terminalFactory`) are guarded with typeof
// exactly like the desktop pane, so a page loaded bare is inert chrome that
// explains itself rather than a ReferenceError.
Page {
    id: page

    // ---- pane identity, set by the host ----
    property string devSessionId: ""
    // The layout slot LABEL ("terminal-1", …). It is not this terminal's
    // identity: it is recycled per Dev Session, and no tmux target is ever built
    // from it.
    property string paneId: ""
    // The server's `terminal_panes` row id the layout leaf carries — THIS is the
    // terminal's identity (SPEC 5.2). Empty only for a legacy leaf stored before
    // layouts carried one.
    property string terminalPaneId: ""
    // Working directory a newly created tmux session is rooted at
    // (app.activeSessionRepoRoot).
    property string repoRoot: ""
    // The pane's own label, as the picker knows it. NOT called `title`: Page
    // declares `title` FINAL, and shadowing it makes the whole component fail to
    // load — which on a single-pane client means the terminal simply never
    // appears, with the reason buried in a QML warning.
    property string paneTitle: ""

    // Ask the host to relabel its header. Emitted for the remote window title
    // (OSC 0/2) so a pane running `vim` says so, which on a single-pane phone UI
    // is the only place that information can appear.
    signal titleRequested(string title)

    readonly property var mobileController: (typeof mobile !== "undefined") ? mobile : null

    // ch::MobileTerminalSession, minted by the shell and PARENTED TO THIS PAGE:
    // leaving the page destroys the session, its controller, its buffers and its
    // PTY channel with it. That is the single-live-pane invariant's teeth.
    property var session: null
    // Reason there is no terminal at all, as opposed to a reason the terminal is
    // not attached (which the session reports itself).
    property string pageStatus: ""
    readonly property string statusText: page.pageStatus.length > 0
                                         ? page.pageStatus
                                         : (page.session ? page.session.statusText : "")

    // Whether a renderer can currently show this pane (SPEC 5.4). BOTH halves
    // matter: a backgrounded app must stop being handed output it cannot draw, and
    // a page buried in the stack likewise. StackView.view is null when this page
    // was loaded by a plain Loader rather than pushed onto a stack, and then the
    // item's own visibility is the honest answer — an attached property read off a
    // non-stack parent would report Inactive for ever and the pane would never
    // receive a byte.
    readonly property bool paneVisible: Qt.application.state === Qt.ApplicationActive
                                        && (StackView.view !== null
                                            ? StackView.status === StackView.Active
                                            : page.visible)
    onPaneVisibleChanged: if (page.session) page.session.setVisible(page.paneVisible)

    background: Rectangle { color: MobileTheme.surfaceSunken }
    padding: 0

    // Longest remote window title this page will forward. The remote picks this
    // string, so it is bounded here for the same reason
    // SplitNode::kMaxCustomTitleLength bounds a stored one: a header is not a
    // place a remote host gets to put a kilobyte.
    readonly property int maxTitleLength: 128

    function reattach() {
        if (!page.session)
            return
        // close() releases the channel and leaves the remote tmux session RUNNING
        // (that is the whole reattach mechanism), so this resumes the same shell
        // with its scrollback and its processes rather than starting a new one.
        page.session.close()
        page.openNow()
    }

    function openNow() {
        if (!page.session)
            return
        page.session.setVisible(page.paneVisible)
        page.session.open(page.devSessionId, page.paneId, page.terminalPaneId,
                          page.repoRoot, view.columns, view.rows)
    }

    Component.onCompleted: {
        if (page.mobileController
                && typeof page.mobileController.createTerminalSession === "function") {
            page.session = page.mobileController.createTerminalSession(page)
        }
        if (!page.session) {
            page.pageStatus = qsTr("No terminal service in this window.")
            return
        }
        view.screen = page.session.screen
        page.openNow()
        // The keyboard is not raised automatically: on a phone it covers half the
        // screen, and a user arriving at a pane usually wants to READ it first.
        // A tap raises it (see the gesture area below).
    }

    Component.onDestruction: {
        // Deterministic, rather than left to the parent-child destruction that
        // would do it anyway: the channel is released before the screen and the
        // view start tearing down, so no final flush is delivered into a
        // half-destroyed page.
        if (page.session)
            page.session.close()
    }

    Connections {
        target: page.session ? page.session.screen : null
        function onWindowTitleChanged() {
            const remote = page.session.screen.windowTitle
            const trimmed = remote.length > page.maxTitleLength
                            ? remote.substring(0, page.maxTitleLength)
                            : remote
            page.titleRequested(trimmed.length > 0 ? trimmed : page.paneTitle)
        }
    }

    // ---- the terminal itself ----
    MobileTerminalView {
        id: view

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: statusStrip.visible ? statusStrip.bottom : parent.top
        anchors.bottom: keyBar.top

        font.family: MobileTheme.monoFamily
        font.pixelSize: MobileTheme.fontSizeBody
        // The two sentinel colours a cell carries when the stream set none.
        defaultBackgroundColor: MobileTheme.surfaceSunken
        defaultForegroundColor: MobileTheme.text
        // The cursor is drawn filled only while this pane actually owns the
        // keyboard, and the item that owns it is the invisible TextInput below,
        // never this one — Qt raises a soft keyboard for the focused item that
        // accepts input methods, and a QQuickPaintedItem does not. The view
        // cannot read that from its own hasActiveFocus(), so the page tells it.
        cursorFocused: keyboardInput.activeFocus
    }

    // The view reports its cell grid; the SESSION resizes the screen and the
    // remote PTY. Debounced because a keyboard sliding up, an orientation change
    // and a stack transition each walk the grid through several intermediate
    // values, and every one of them would otherwise be an SSH window-change and a
    // full remote redraw.
    Timer {
        id: resizeTimer
        interval: 120
        onTriggered: if (page.session) page.session.resize(view.columns, view.rows)
    }

    Connections {
        target: view
        // gridChanged covers all three triggers: a columns change, a rows change,
        // and the keyboard inset (which changes the view's height, and so its
        // rows).
        function onGridChanged() { resizeTimer.restart() }
    }

    // ---- soft keyboard and IME ----
    //
    // An invisible TextInput is the only way to get a soft keyboard and an input
    // method on Android and iOS: Qt raises the keyboard for the focused item that
    // accepts input methods, and nothing else does.
    TextInput {
        id: keyboardInput

        anchors.fill: view
        // Present and focusable but never drawn: the terminal grid is drawn by the
        // view, and a second visible caret would be a second cursor.
        opacity: 0
        cursorVisible: false
        selectByMouse: false
        autoScroll: false
        activeFocusOnTab: false

        // No auto-capitalisation (a shell is case-sensitive), no predictive text
        // (it rewrites commands and batches keystrokes the terminal must see
        // one at a time), and sensitive-data so the platform keeps the pane out of
        // its learning dictionary and its clipboard history — everything typed
        // here is a command, and some of it is a password.
        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhSensitiveData

        // Re-entrancy guard for the clear inside onTextChanged.
        property bool clearing: false

        Keys.onPressed: (event) => {
            // EVERY key is accepted, unconditionally and even with no session:
            // that is what keeps a keystroke out of this TextInput's own text, and
            // the emptiness of that text is what makes the commit path below
            // unambiguous.
            event.accepted = true
            const modifierOnly = event.key === Qt.Key_Shift || event.key === Qt.Key_Control
                              || event.key === Qt.Key_Alt || event.key === Qt.Key_Meta
                              || event.key === Qt.Key_AltGr || event.key === Qt.Key_CapsLock
                              || event.key === Qt.Key_NumLock || event.key === Qt.Key_ScrollLock
            if (modifierOnly || !page.session)
                return
            // The key bar's latched Ctrl/Alt apply to a key from the hardware or
            // soft keyboard too — the two input paths must not disagree about what
            // is armed, and takeLatchedModifiers() is the ONE place that clears
            // them. Note the early return above: a bare Shift or Ctrl press must
            // NOT consume a latch, or arming Ctrl and then holding Shift for a
            // capital would spend the latch on the Shift.
            page.session.sendKey(event.key,
                                 event.modifiers | keyBar.takeLatchedModifiers(),
                                 event.text)
        }

        // IME COMMIT PATH. Read this before touching it.
        //
        // Key presses are consumed in Keys.onPressed with event.accepted = true,
        // so they NEVER insert into this TextInput. Therefore text appearing here
        // can only be an input-method COMMIT (QInputMethodEvent's commitString),
        // which is delivered as an input method event and not as a key event —
        // that is precisely why a commit cannot also arrive through the handler
        // above, and why the committed run is sent exactly ONCE.
        //
        // The field is cleared BEFORE the text is sent, and the clear is flagged:
        // the clear re-enters this handler with an empty string (dropped by the
        // length guard), and an IME that commits again while we are clearing
        // cannot make the same run go out twice.
        onTextChanged: {
            if (keyboardInput.clearing || keyboardInput.text.length === 0)
                return
            const committed = keyboardInput.text
            keyboardInput.clearing = true
            keyboardInput.text = ""
            keyboardInput.clearing = false
            if (!page.session)
                return

            // A LATCHED MODIFIER HAS TO BE CONSUMED HERE TOO, not only in
            // Keys.onPressed. On Android an ordinary letter from the soft keyboard
            // arrives as an input-method COMMIT and not as a key event, and the key
            // bar deliberately carries no letters — so a latch consumed only by the
            // key handler made "Ctrl" then "c" type a literal c, left Ctrl armed to
            // ambush the next key, and put Ctrl-C out of reach on the one client
            // where stopping a runaway build matters most.
            const latched = keyBar.takeLatchedModifiers()
            if (latched === Qt.NoModifier) {
                page.session.sendText(committed)
                return
            }
            // The modifier applies to the FIRST character of the commit and to
            // nothing else, which is the same "exactly one following key" rule the
            // bar advertises. The rest of a multi-character commit (a predictive
            // word — the input hints ask the platform not to send one, but cannot
            // forbid it) goes out as plain text.
            const first = committed.codePointAt(0)
            const headLength = first > 0xffff ? 2 : 1
            const head = committed.substring(0, headLength)
            // ch::vt::encodeKey() derives a control code from the KEY, never from
            // the text, and a commit carries no key. For an ASCII printable the
            // Qt::Key value IS its uppercase code point (Qt.Key_A === 0x41), so
            // only a-z has to be folded; anything else has no Qt::Key of its own
            // and falls through to encodeKey's text path unchanged.
            const qtKey = (first >= 0x61 && first <= 0x7a) ? first - 0x20 : first
            page.session.sendKey(qtKey, latched, head)
            if (committed.length > headLength)
                page.session.sendText(committed.substring(headLength))
        }
    }

    // Composition in progress. It is deliberately NOT sent — a preedit is not
    // input yet — but it has to be VISIBLE, or a user composing CJK or an accent
    // sees nothing at all happen until they commit.
    Rectangle {
        visible: keyboardInput.preeditText.length > 0
        anchors.left: view.left
        anchors.bottom: view.bottom
        anchors.margins: MobileTheme.spacing
        implicitWidth: preedit.implicitWidth + MobileTheme.spacing * 2
        implicitHeight: preedit.implicitHeight + MobileTheme.spacing
        radius: MobileTheme.radiusSmall
        color: MobileTheme.surfaceRaised
        border.width: 1
        border.color: MobileTheme.accent

        Text {
            id: preedit
            anchors.centerIn: parent
            text: keyboardInput.preeditText
            textFormat: Text.PlainText
            color: MobileTheme.text
            font.family: MobileTheme.monoFamily
            font.pixelSize: MobileTheme.fontSizeBody
        }
    }

    // ---- gestures ----
    //
    // Above the TextInput on purpose: a tap must focus the terminal and raise the
    // keyboard, not place a caret in an invisible text field. Focus is independent
    // of stacking, so the input below still receives every key.
    MouseArea {
        id: gestures
        anchors.fill: view

        property real pressY: 0
        property int pressOffset: 0
        property bool dragging: false

        onPressed: (mouse) => {
            gestures.pressY = mouse.y
            gestures.pressOffset = view.scrollOffset
            gestures.dragging = false
        }
        onPositionChanged: (mouse) => {
            const dy = mouse.y - gestures.pressY
            if (!gestures.dragging && Math.abs(dy) < Qt.styleHints.startDragDistance)
                return
            gestures.dragging = true
            // Dragging DOWN pulls older lines into view, which is the direction
            // the content moves under the finger.
            view.scrollOffset = gestures.pressOffset + Math.round(dy / view.lineHeight)
        }
        onClicked: {
            if (gestures.dragging)
                return
            keyboardInput.forceActiveFocus()
            Qt.inputMethod.show()
        }
        onPressAndHold: (mouse) => {
            contextMenu.pressRow = view.absoluteRowAt(mouse.y)
            contextMenu.popup()
        }
    }

    Menu {
        id: contextMenu

        // Absolute row the long press landed on, i.e. what Copy copies.
        property int pressRow: 0

        // Copy is per LINE, not per selection, and that is a consequence of the
        // gesture budget rather than a shortcut: a drag scrolls history (which a
        // terminal on a phone needs far more than it needs a selection handle), so
        // there is no gesture left to define a range with. The row under the
        // finger is unambiguous and is what the user pointed at.
        MenuItem {
            text: qsTr("Copy line")
            height: Math.max(implicitHeight, MobileTheme.touchTarget)
            enabled: page.session !== null
            onTriggered: {
                const row = contextMenu.pressRow
                // endCol is EXCLUSIVE, and it is the SCREEN's width rather than
                // the view's: the two differ for a frame whenever a resize is in
                // flight, and the line belongs to the screen.
                page.session.copyToClipboard(
                    page.session.screen.textRange(row, 0, row,
                                                  page.session.screen.columns))
            }
        }
        MenuItem {
            text: qsTr("Paste")
            height: Math.max(implicitHeight, MobileTheme.touchTarget)
            enabled: page.session !== null
            // Bracketed when the remote asked for it, so a shell can tell pasted
            // text from typing.
            onTriggered: page.session.pasteFromClipboard()
        }
        MenuItem {
            text: qsTr("Reattach")
            height: Math.max(implicitHeight, MobileTheme.touchTarget)
            enabled: page.session !== null
            onTriggered: page.reattach()
        }
    }

    // ---- status ----
    //
    // On a single-pane phone UI this is the ONLY place a failure can be reported,
    // so it carries a whole sentence and the action that fixes it.
    Rectangle {
        id: statusStrip

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        implicitHeight: Math.max(MobileTheme.touchTarget,
                                 statusLabel.implicitHeight + MobileTheme.spacing * 2)
        visible: page.statusText.length > 0
        color: MobileTheme.surfaceRaised

        Text {
            id: statusLabel
            anchors.left: parent.left
            anchors.right: retryButton.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: MobileTheme.spacingLarge
            anchors.rightMargin: MobileTheme.spacing
            text: page.statusText
            // PlainText, non-negotiable (SPEC 7.5): this string can carry a
            // message the SERVER wrote (ch::TerminalFactory::error forwards remote
            // diagnostics), and no rich-text format may ever be applied to a
            // server-controlled string in mobile QML.
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            elide: Text.ElideRight
            maximumLineCount: 3
            color: MobileTheme.text
            font.pixelSize: MobileTheme.fontSizeSmall
        }

        Button {
            id: retryButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: MobileTheme.spacing
            implicitHeight: MobileTheme.touchTarget
            visible: page.session !== null
            text: qsTr("Reattach")
            onClicked: page.reattach()
        }
    }

    KeyBar {
        id: keyBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        session: page.session
    }
}
