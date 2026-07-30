// Delegates below reach the `root` id of this component; Bound makes that a
// compile-time-resolved reference instead of a dynamic context lookup.
pragma ComponentBehavior: Bound

import QtQml
import QtQuick
import QtQuick.Controls.Basic

// Command palette + global keyboard shortcuts (SPEC 15).
//
// Self-contained overlay: it never reaches into `app`, `viewers` or any other
// context object. The host supplies everything through `commands`, an array of
//
//     { id: "editor.save", title: "Save File", shortcut: "Ctrl+S", invoke: fn }
//
// where `shortcut` is an optional QKeySequence string and `invoke` is a JS
// function (a Q_INVOKABLE on an injected QObject works identically).
//
// Two things happen with that list:
//   1. It is searchable here (Ctrl+Shift+P, ⌘⇧P on macOS).
//   2. Every entry carrying a `shortcut` gets a real `Shortcut` object, so the
//      binding fires window-wide while the palette is CLOSED. That is what makes
//      this a shortcuts feature rather than a searchable list.
//
// Hosting is one line inside the window (NOT inside a SplitView — it must not
// become a split item):
//
//     CommandPalette { id: commandPalette; commands: root.paletteCommands }
//
// The root Item is deliberately zero-sized: the visible part is a Popup parented
// to the window Overlay, so the component adds nothing to the host's layout.
Item {
    id: root

    // ---------------------------------------------------------------- API ---

    // [{id, title, shortcut, invoke}]; `shortcut` and `invoke` are optional.
    property var commands: []

    // True while the palette is showing.
    readonly property bool opened: popup.visible

    // The filtered, ranked commands (best first) — the same objects the host
    // passed in, so a host can mirror the current match set if it wants to.
    readonly property var matches: root._matches

    // Index into `matches` of the row Enter would invoke; -1 when empty.
    property int highlightedIndex: -1

    // Qt maps Ctrl in a key SEQUENCE to the Command key on macOS, so one
    // portable string already means ⌘⇧P there and Ctrl+Shift+P everywhere else.
    // Qt.platform only decides how the hint is SPELLED to the user.
    readonly property string activationSequence: "Ctrl+Shift+P"
    readonly property string activationHint: Qt.platform.os === "osx" ? "\u2318\u21e7P"
                                                                      : "Ctrl+Shift+P"

    // Emitted after a command's invoke() has run. Empty id for commands without.
    signal commandInvoked(string id)

    // Clears the filter, resets the highlight and takes keyboard focus.
    function open() {
        filterField.text = "";
        root._rebuild();
        popup.open();
        filterField.forceActiveFocus();
        filterField.selectAll();
    }

    function close() {
        popup.close();
    }

    function toggle() {
        if (root.opened)
            root.close();
        else
            root.open();
    }

    // ------------------------------------------------------------ ranking ---
    //
    // Matching is case-insensitive over `title` only. A command is scored by the
    // STRONGEST way the query hits its title, and lower ranks sort first:
    //
    //   0  exact title match                       ("save" -> "Save")
    //   1  title starts with the query             ("sav"  -> "Save File")
    //   2  a WORD of the title starts with it      ("fil"  -> "Save File")
    //   3  substring anywhere, mid-word            ("ave"  -> "Save File")
    //   4  subsequence: query letters in order     ("svf"  -> "Save File")
    //
    // So a prefix match always beats a mid-word match. Ties break on the match
    // position (earlier first), then the shorter title, then the order the host
    // declared the command in — the sort is therefore total and stable.
    // An empty query matches everything and preserves the host's order.

    function _score(title, query) {
        var t = title.toLowerCase();
        var q = query.toLowerCase();

        if (t === q)
            return { rank: 0, at: 0 };

        var idx = t.indexOf(q);
        if (idx === 0)
            return { rank: 1, at: 0 };
        if (idx > 0)
            return { rank: /[\s\-_:./\\()\[\]]/.test(t.charAt(idx - 1)) ? 2 : 3, at: idx };

        // Subsequence fallback: every query character appears, in order.
        var first = -1;
        var j = 0;
        for (var i = 0; i < t.length && j < q.length; ++i) {
            if (t.charAt(i) === q.charAt(j)) {
                if (j === 0)
                    first = i;
                ++j;
            }
        }
        return j === q.length ? { rank: 4, at: first } : null;
    }

    function _title(command) {
        if (!command)
            return "";
        return command.title === undefined || command.title === null ? "" : String(command.title);
    }

    // Backing store for the read-only `matches`.
    property var _matches: []

    function _rebuild() {
        // The filter field lives inside the Popup, so a `commands` binding
        // applied by the host can land before it exists; completion re-runs us.
        if (!filterField)
            return;
        var list = Array.isArray(root.commands) ? root.commands : [];
        var query = filterField.text.trim();
        var out = [];

        if (query.length === 0) {
            for (var i = 0; i < list.length; ++i) {
                if (list[i])
                    out.push(list[i]);
            }
        } else {
            var scored = [];
            for (var k = 0; k < list.length; ++k) {
                var command = list[k];
                if (!command)
                    continue;
                var score = root._score(root._title(command), query);
                if (!score)
                    continue;
                scored.push({ command: command, rank: score.rank, at: score.at, order: k });
            }
            scored.sort(function (a, b) {
                return (a.rank - b.rank)
                        || (a.at - b.at)
                        || (root._title(a.command).length - root._title(b.command).length)
                        || (a.order - b.order);
            });
            for (var m = 0; m < scored.length; ++m)
                out.push(scored[m].command);
        }

        root._matches = out;
        root.highlightedIndex = out.length > 0 ? 0 : -1;
    }

    // ----------------------------------------------------------- invoking ---

    function _invoke(command) {
        if (!command)
            return;
        // Close first: the palette had keyboard focus, and Popup hands it back
        // to whoever held it before, so the command runs with the terminal or
        // editor already focused again.
        root.close();
        if (typeof command.invoke === "function")
            command.invoke();
        root.commandInvoked(command.id === undefined || command.id === null ? ""
                                                                            : String(command.id));
    }

    // Enter. A no-match filter (or an empty command list) does nothing at all,
    // deliberately leaving the palette open so the query can be corrected.
    function _acceptHighlighted() {
        if (root.highlightedIndex < 0 || root.highlightedIndex >= root.matches.length)
            return;
        root._invoke(root.matches[root.highlightedIndex]);
    }

    function _move(delta) {
        var count = root.matches.length;
        if (count === 0) {
            root.highlightedIndex = -1;
            return;
        }
        // Wraps, so Up from the first row lands on the last.
        var next = (root.highlightedIndex + delta + count) % count;
        root.highlightedIndex = next;
        resultsList.positionViewAtIndex(next, ListView.Contain);
    }

    onCommandsChanged: root._rebuild()
    Component.onCompleted: root._rebuild()

    // ---------------------------------------------------------- shortcuts ---

    Shortcut {
        sequences: [root.activationSequence]
        onActivated: root.toggle()
    }

    // One real Shortcut per command that declares one. They are intentionally
    // disabled while the palette is open: the palette owns the keyboard then.
    Instantiator {
        model: root.commands
        delegate: Shortcut {
            required property var modelData

            sequence: modelData && modelData.shortcut ? String(modelData.shortcut) : ""
            enabled: !root.opened && sequence.length > 0
            onActivated: root._invoke(modelData)
        }
    }

    // ------------------------------------------------------------ overlay ---

    Popup {
        id: popup

        parent: Overlay.overlay
        anchors.centerIn: parent

        // Not "availableWidth/Height": those are FINAL on Popup.
        readonly property real overlayWidth: parent ? parent.width : 720
        readonly property real overlayHeight: parent ? parent.height : 480

        width: Math.max(320, Math.min(640, overlayWidth - 64))
        height: Math.max(140, Math.min(420, overlayHeight - 96))

        modal: true
        focus: true
        padding: 1
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: Theme.surface
            border.color: Theme.border
            border.width: 1
            radius: Theme.radiusMedium
        }

        onOpened: filterField.forceActiveFocus()

        TextField {
            id: filterField

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 40

            objectName: "filterField"
            placeholderText: qsTr("Type a command\u2026")
            // #585b70 is a step between Theme.textDim and Theme.textFaint and
            // has no Theme role.
            placeholderTextColor: "#585b70"
            color: Theme.text
            selectByMouse: true
            leftPadding: 12
            rightPadding: 12
            background: Rectangle {
                color: Theme.surfaceDeep
                // The palette opens with the field focused, so the ring is the
                // resting state rather than an exception; without it the field
                // and the popup body are the same slab of dark grey.
                border.color: filterField.activeFocus ? Theme.accent : Theme.border
                border.width: filterField.activeFocus ? 2 : 1
            }

            onTextChanged: root._rebuild()

            Keys.onDownPressed: function (event) {
                root._move(1);
                event.accepted = true;
            }
            Keys.onUpPressed: function (event) {
                root._move(-1);
                event.accepted = true;
            }
            Keys.onReturnPressed: function (event) {
                root._acceptHighlighted();
                event.accepted = true;
            }
            Keys.onEnterPressed: function (event) {
                root._acceptHighlighted();
                event.accepted = true;
            }
            Keys.onEscapePressed: function (event) {
                root.close();
                event.accepted = true;
            }
        }

        ListView {
            id: resultsList

            objectName: "resultsList"
            anchors.top: filterField.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            clip: true
            model: root.matches
            currentIndex: root.highlightedIndex
            visible: root.matches.length > 0
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: AppScrollBar {}

            delegate: Rectangle {
                id: resultRow

                required property int index
                required property var modelData

                width: ListView.view.width
                height: 36
                color: resultRow.index === root.highlightedIndex
                       ? Theme.surfaceSelected
                       : (rowHover.hovered ? Theme.surfaceHover : "transparent")

                // A plain Rectangle carries no accessibility at all, and this
                // list IS the keyboard surface of the application: without a
                // name a screen reader announces the command list as a stack of
                // anonymous boxes.
                Accessible.role: Accessible.ListItem
                Accessible.name: root._title(resultRow.modelData)
                Accessible.description: resultRow.modelData && resultRow.modelData.shortcut
                                        ? String(resultRow.modelData.shortcut) : ""
                Accessible.selected: resultRow.index === root.highlightedIndex

                // The row Enter will run. A wash of Theme.surfaceSelected on the
                // popup's own surface is a faint one, and it is the only thing
                // distinguishing "this is what the next keystroke does" from
                // every other row.
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 3
                    color: Theme.accent
                    visible: resultRow.index === root.highlightedIndex
                }

                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: shortcutLabel.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: root._title(resultRow.modelData)
                    color: Theme.text
                    font.pixelSize: Theme.fontSizeLabel
                    elide: Text.ElideRight
                }

                Label {
                    id: shortcutLabel

                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: resultRow.modelData && resultRow.modelData.shortcut
                          ? String(resultRow.modelData.shortcut) : ""
                    color: Theme.textDim
                    font.pixelSize: 11
                    visible: text.length > 0
                }

                HoverHandler {
                    id: rowHover
                }

                TapHandler {
                    onTapped: {
                        root.highlightedIndex = resultRow.index;
                        root._invoke(resultRow.modelData);
                    }
                }
            }
        }

        Column {
            id: emptyState
            anchors.top: filterField.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 16
            spacing: 6
            visible: root.matches.length === 0

            Label {
                objectName: "emptyLabel"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                color: Theme.textDim
                font.pixelSize: Theme.fontSizeBody
                text: (Array.isArray(root.commands) ? root.commands.length : 0) === 0
                      ? qsTr("No commands available") : qsTr("No matching commands")
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                color: Theme.textFaint
                font.pixelSize: 11
                visible: (Array.isArray(root.commands) ? root.commands.length : 0) > 0
                text: qsTr("Esc closes \u2014 %1 reopens").arg(root.activationHint)
            }
        }
    }
}
