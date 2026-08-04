import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models
import QtQuick.Layouts

// Sessions sidebar region (SPEC 4.2): collapsible groups, Dev Session rows with
// aggregate status, drag-and-drop reordering and keyboard navigation. Bound to
// app.sessionsModel (a two-level QAbstractItemModel) via a nested DelegateModel:
// the top level renders GroupHeader rows, each of which nests its group's
// SessionRow children (hidden while the group is collapsed). Role names
// consumed: name, subtitle, rowState, pinned, archived, collapsed, itemId, groupId.
//
// Drag-and-drop contract (SPEC 4.2): the sidebar NEVER reorders the model
// itself. A drop calls the matching app.* invokable with the INDEX/ordering the
// user asked for and nothing else; the server re-packs positions and the
// refreshed model is what moves the rows. A rejected RPC therefore leaves the
// visible order exactly as the server last reported it.
Rectangle {
    id: sidebar
    color: Theme.surface

    // Emitted when the user picks a Dev Session (click, or Enter on the
    // keyboard cursor). The host wires this to layout loading; the sidebar
    // itself owns no navigation.
    signal sessionActivated(string devSessionId)
    // Emitted by the always-visible server control. The window owns the
    // connection sheet, keeping server-profile editing outside the sidebar.
    signal serverSettingsRequested()


    // Keyboard/selection cursor. Groups and sessions have independent id
    // spaces, so the kind flag is part of the cursor.
    property string currentId: ""
    property bool currentIsGroup: false
    // Last selected Dev Session; drives the row highlight.
    property string selectedSessionId: ""
    // Client-local presentation filters. Pin/archive bits on each session come
    // from the server, but whether to hide those rows belongs to this sidebar
    // and is persisted by UiStateStore.
    property bool pinnedOnly:
        (typeof app !== "undefined" && app && app.uiState
         && typeof app.uiState.pinnedOnly === "function")
            ? app.uiState.pinnedOnly() : false
    property bool showArchived:
        (typeof app !== "undefined" && app && app.uiState
         && typeof app.uiState.showArchived === "function")
            ? app.uiState.showArchived() : false
    property bool pinFilterReady: false
    property bool archiveFilterReady: false
    // Live drag state, read by the delegates for their drag affordances.
    property string dragKind: "" // "", "session" or "group"
    property var dragItem: null  // the SessionRow / GroupHeader being dragged
    property var dropTarget: null // {groupId,index[,append]} | {index} | null

    activeFocusOnTab: true

    // Palette preferences are read here so the header binding depends on the
    // settings' NOTIFY signals. The groupPalette service itself is optional in
    // small QML harnesses; its absence follows the same plain-palette rule as
    // an unknown palette name.
    readonly property string groupPaletteName:
        (typeof app !== "undefined" && app && app.settings
         && app.settings.groupPalette !== undefined)
            ? String(app.settings.groupPalette) : "plain"
    readonly property int groupPaletteSize:
        (typeof app !== "undefined" && app && app.settings
         && app.settings.groupPaletteSize !== undefined)
            ? Number(app.settings.groupPaletteSize) : 5

    function groupColorFor(name, paletteName, paletteSize) {
        if (paletteName !== "tokyonight")
            return Theme.text;
        if (typeof groupPalette === "undefined" || !groupPalette)
            return Theme.text;
        return Theme.groupTextColor(
                    groupPalette.colorFor(String(name), paletteName, Number(paletteSize)));
    }

    // ---------------------------------------------------------------------
    // Link state
    //
    // The sidebar is where a user notices the server is gone: the rows keep
    // showing whatever the last refresh reported, and nothing else on screen
    // says otherwise. It therefore reads the host's connection surface
    // directly. Guarded, because a host that publishes none (a bare sidebar
    // harness) must keep working — it then simply has nothing to report and
    // the footer takes no space.
    // ---------------------------------------------------------------------
    readonly property string linkState:
        (typeof app !== "undefined" && app && app.connectionState !== undefined)
            ? String(app.connectionState) : ""
    // Rows on screen are known to predate the current reality.
    readonly property bool stale: linkState === "reconnecting" || linkState === "failed"
                                  || linkState === "disconnected"
    // The session the host considers loaded, which is not the same thing as the
    // keyboard cursor or the last row clicked.
    readonly property string hostActiveSessionId:
        (typeof app !== "undefined" && app && app.activeSessionId !== undefined)
            ? String(app.activeSessionId) : ""

    // Same three-way encoding as ConnectSheet's chip: colour, glyph and word.
    // Every word ch::AppController::setConnectionState() publishes needs a case
    // here — including "credential", which it enters while the connect attempt
    // is parked on a password or key passphrase. Falling through to the default
    // painted that as a grey "Not connected", i.e. the footer denied there was
    // anything to answer while the sheet was asking for it.
    function linkColor(state) {
        switch (state) {
        case "connected": return Theme.success;
        case "connecting":
        case "credential":
        // Installing the remote service on first connect. Same "in progress,
        // nothing is wrong" reading as connecting.
        case "provisioning":
        case "hostkey": return Theme.warning;
        // "Reconnecting" needs to read as worse than "connecting" but not as
        // final as "failed"; the active theme supplies its middle-severity
        // status tint.
        case "reconnecting": return Theme.statusReconnecting();
        case "failed": return Theme.danger;
        default: return Theme.textDim;
        }
    }
    function linkGlyph(state) {
        switch (state) {
        case "connected": return "\u2713";
        case "connecting": return "\u2219";
        case "provisioning": return "\u2193"; // downloading onto the server
        case "hostkey": return "?";
        case "credential": return "*"; // the password mask
        case "reconnecting": return "\u21bb";
        case "failed": return "\u2715";
        default: return "\u2013";
        }
    }
    function linkWords(state) {
        switch (state) {
        case "connected": return qsTr("Connected");
        case "connecting": return qsTr("Connecting\u2026");
        case "provisioning": return qsTr("Installing the remote service\u2026");
        case "hostkey": return qsTr("Host key needs approval");
        case "credential": return qsTr("Password or passphrase needed");
        case "reconnecting": return qsTr("Reconnecting\u2026 rows may be stale");
        case "failed": return qsTr("Connection failed");
        default: return qsTr("Not connected");
        }
    }

    function applySessionFilters() {
        if (typeof app !== "undefined" && app && app.sessionsModel) {
            app.sessionsModel.pinnedOnly = sidebar.pinnedOnly
            app.sessionsModel.showArchived = sidebar.showArchived
        }
        if (typeof app !== "undefined" && app && app.uiState) {
            if (typeof app.uiState.setPinnedOnly === "function")
                app.uiState.setPinnedOnly(sidebar.pinnedOnly)
            if (typeof app.uiState.setShowArchived === "function")
                app.uiState.setShowArchived(sidebar.showArchived)
        }
    }

    Component.onCompleted: {
        applySessionFilters()
        pinFilterReady = true
        archiveFilterReady = true
        if (typeof app !== "undefined" && app
                && typeof app.refresh === "function")
            app.refresh()
    }

    onPinnedOnlyChanged: {
        if (!pinFilterReady)
            return
        applySessionFilters()
        if (typeof app !== "undefined" && app
                && typeof app.refresh === "function")
            app.refresh()
    }

    onShowArchivedChanged: {
        if (!archiveFilterReady)
            return
        applySessionFilters()
        if (typeof app !== "undefined" && app
                && typeof app.refresh === "function")
            app.refresh()
    }



    // DelegateModel.items is the authoritative ordered set: it contains every
    // model row even when ListView has not instantiated that row's delegate.
    // The live registry remains useful only for geometry and per-row actions.
    property var rowItems: []
    property var headerItems: []

    function registerRow(item) {
        if (item && rowItems.indexOf(item) < 0)
            rowItems.push(item);
    }
    function unregisterRow(item) {
        var i = rowItems.indexOf(item);
        if (i >= 0)
            rowItems.splice(i, 1);
        if (dragItem === item)
            cancelDrag();
    }
    function registerHeader(item) {
        if (item && headerItems.indexOf(item) < 0)
            headerItems.push(item);
    }
    function unregisterHeader(item) {
        var i = headerItems.indexOf(item);
        if (i >= 0)
            headerItems.splice(i, 1);
        if (dragItem === item)
            cancelDrag();
    }

    function liveHeader(id) {
        for (var i = 0; i < headerItems.length; ++i) {
            if (headerItems[i] && headerItems[i].itemId === id)
                return headerItems[i];
        }
        return null;
    }

    function modelId(modelData) {
        if (!modelData)
            return "";
        var value = modelData.itemId !== undefined ? modelData.itemId : modelData.id;
        return value === undefined || value === null ? "" : String(value);
    }

    // Groups in model order. The DelegateModel item contains the model roles
    // even when its visual delegate is outside the ListView viewport.
    function orderedGroups() {
        var out = [];
        for (var g = 0; g < groupsDelegateModel.items.count; ++g) {
            var item = groupsDelegateModel.items.get(g);
            var modelData = item ? item.model : null;
            var id = sidebar.modelId(modelData);
            if (id === "")
                continue;
            var header = sidebar.liveHeader(id);
            var rows = [];
            for (var r = 0; r < rowItems.length; ++r) {
                if (rowItems[r] && rowItems[r].groupId === id)
                    rows.push(rowItems[r]);
            }
            rows.sort(function (a, b) { return a.index - b.index; });
            out.push({ header: header, rows: rows, itemId: id,
                       index: g, model: modelData });
        }
        return out;
    }

    function groupEntry(groupId) {
        var gs = orderedGroups();
        for (var i = 0; i < gs.length; ++i) {
            if (gs[i].itemId === groupId)
                return gs[i];
        }
        return null;
    }

    function visibleRows(entry) {
        var out = [];
        for (var i = 0; i < entry.rows.length; ++i) {
            if (entry.rows[i].visible)
                out.push(entry.rows[i]);
        }
        return out;
    }

    // ---------------------------------------------------------------------
    // Geometry, all in sidebar coordinates.
    // ---------------------------------------------------------------------
    function itemTop(item) {
        return item.mapToItem(sidebar, 0, 0).y;
    }

    // Vertical extent of a whole group block: the header alone when collapsed,
    // header plus its visible rows otherwise.
    function groupSpan(entry) {
        var top = itemTop(entry.header);
        var bottom = top + entry.header.height;
        if (!entry.header.collapsed) {
            var rows = visibleRows(entry);
            for (var i = 0; i < rows.length; ++i) {
                var b = itemTop(rows[i]) + rows[i].height;
                if (b > bottom)
                    bottom = b;
            }
        }
        return { top: top, bottom: bottom };
    }

    function visibleGroupEntries() {
        var all = orderedGroups();
        var out = [];
        for (var i = 0; i < all.length; ++i) {
            if (all[i].header)
                out.push(all[i]);
        }
        return out;
    }

    // ---------------------------------------------------------------------
    // Drop resolution
    // ---------------------------------------------------------------------

    // Insertion point for a dragged session: the group under the cursor plus
    // the index within it. Dropping onto a collapsed group appends to it,
    // because its rows offer no insertion points to aim at.
    function resolveSessionDrop(y) {
        var gs = sidebar.visibleGroupEntries();
        if (gs.length === 0)
            return null;

        var chosen = gs[gs.length - 1];
        for (var i = 0; i < gs.length; ++i) {
            if (y < groupSpan(gs[i]).bottom) {
                chosen = gs[i];
                break;
            }
        }

        if (chosen.header.collapsed)
            return { groupId: chosen.itemId, index: chosen.rows.length, append: true };

        var rows = visibleRows(chosen);
        var index = rows.length;
        for (var j = 0; j < rows.length; ++j) {
            if (y < itemTop(rows[j]) + rows[j].height / 2) {
                index = j;
                break;
            }
        }
        return { groupId: chosen.itemId, index: index };
    }

    // Insertion point for a dragged group: the model index in the top-level
    // ordering, not the position among whichever headers ListView realised.
    function resolveGroupDrop(y) {
        var gs = sidebar.visibleGroupEntries();
        if (gs.length === 0)
            return null;
        for (var i = 0; i < gs.length; ++i) {
            var span = groupSpan(gs[i]);
            if (y < (span.top + span.bottom) / 2)
                return { index: gs[i].index };
            if (y < span.bottom)
                return { index: gs[i].index + 1 };
        }
        return { index: gs[gs.length - 1].index + 1 };
    }

    // ---------------------------------------------------------------------
    // Drag lifecycle, driven by the delegates' DragHandlers.
    // ---------------------------------------------------------------------
    function beginDrag(kind, item) {
        dragKind = kind;
        dragItem = item;
        dropTarget = null;
        dropLineVisible = false;
        dropHighlightVisible = false;
    }

    function updateDrag(scenePosition) {
        if (dragKind === "" || !dragItem)
            return;
        var local = sidebar.mapFromItem(null, scenePosition.x, scenePosition.y);
        dragProxyX = local.x + 12;
        dragProxyY = local.y - dragProxy.height / 2;
        dropTarget = dragKind === "group" ? resolveGroupDrop(local.y)
                                          : resolveSessionDrop(local.y);
        applyIndicator();
    }

    function cancelDrag() {
        dragKind = "";
        dragItem = null;
        dropTarget = null;
        dropLineVisible = false;
        dropHighlightVisible = false;
    }

    // Commit the pending drop, then clear the drag regardless of outcome. Only
    // the app.* invokable is called: the model is refreshed by the server round
    // trip, never by us.
    function endDrag() {
        var kind = dragKind;
        var item = dragItem;
        var target = dropTarget;
        cancelDrag();
        if (kind === "" || !item || !target)
            return;
        if (kind === "session")
            commitSessionDrop(item, target);
        else if (kind === "group")
            commitGroupDrop(item, target);
    }

    function commitSessionDrop(row, target) {
        if (target.groupId !== row.groupId) {
            // Cross-group: the server re-packs the target group's positions, so
            // the insertion index is the whole request.
            app.moveSession(row.itemId, target.groupId, target.index);
            return;
        }

        var entry = groupEntry(target.groupId);
        if (!entry)
            return;
        var ids = [];
        for (var i = 0; i < entry.rows.length; ++i)
            ids.push(entry.rows[i].itemId);

        var from = ids.indexOf(row.itemId);
        if (from < 0)
            return;
        // The index the user aimed at counts the dragged row; removing it first
        // shifts every later slot up by one.
        var to = target.index > from ? target.index - 1 : target.index;
        if (to === from)
            return;
        ids.splice(from, 1);
        ids.splice(to, 0, row.itemId);
        app.reorderSessions(target.groupId, ids);
    }

    function commitGroupDrop(header, target) {
        var gs = orderedGroups();
        var ids = [];
        for (var i = 0; i < gs.length; ++i)
            ids.push(gs[i].itemId);

        var from = ids.indexOf(header.itemId);
        if (from < 0)
            return;
        var to = target.index > from ? target.index - 1 : target.index;
        if (to === from)
            return;
        ids.splice(from, 1);
        ids.splice(to, 0, header.itemId);
        app.reorderGroups(ids);
    }

    // ---------------------------------------------------------------------
    // Row context-menu mutations
    //
    // Routed through here for the same reason every drop is: the sidebar owns
    // every app.* call. A SessionRow documents itself as usable with no host
    // (that is what all its `host !== null` guards are for), and a bare `app`
    // lookup inside a row would make that false — the row would raise a
    // ReferenceError the moment its menu was used. The group header already
    // took this route for its own collapse call; the row's menu did not, which
    // is the drift these close.
    // ---------------------------------------------------------------------
    function renameSession(sessionId, name) {
        app.renameSession(sessionId, name);
    }
    function togglePinned(sessionId, pinned) {
        if (app && typeof app.setSessionPinned === "function")
            app.setSessionPinned(sessionId, pinned)
    }
    function toggleArchived(sessionId, archived) {
        if (!app)
            return
        if (archived) {
            if (typeof app.archiveSession === "function")
                app.archiveSession(sessionId)
        } else if (typeof app.unarchiveSession === "function") {
            app.unarchiveSession(sessionId)
        }
    }
    function duplicateSession(sessionId) {
        app.duplicateSession(sessionId);
    }
    function moveSessionToTop(sessionId, groupId) {
        app.moveSession(sessionId, groupId, 0);
    }
    function sessionCountForGroup(groupId) {
        // AppController counts from m_lastNodes, the unfiltered authoritative
        // tree. The fallback keeps this sidebar usable with the small QML
        // harness stubs, whose model only exposes the visible tree.
        if (app && typeof app.sessionCountForGroup === "function")
            return Number(app.sessionCountForGroup(groupId));
        var entry = groupEntry(groupId);
        if (!entry || !app || !app.sessionsModel)
            return 0;
        var parentIndex = app.sessionsModel.index(entry.index, 0);
        return app.sessionsModel.rowCount(parentIndex);
    }
    function deleteGroup(groupId) {
        if (app && typeof app.deleteGroup === "function")
            app.deleteGroup(groupId);
    }
    function deleteSession(sessionId) {
        app.deleteSession(sessionId);
    }

    // ---------------------------------------------------------------------
    // Drop indicator
    // ---------------------------------------------------------------------
    property bool dropLineVisible: false
    property real dropLineY: 0
    property bool dropHighlightVisible: false
    property real dropHighlightY: 0
    property real dropHighlightHeight: 0
    property real dragProxyX: 0
    property real dragProxyY: 0

    function applyIndicator() {
        var target = dropTarget;
        if (!target) {
            dropLineVisible = false;
            dropHighlightVisible = false;
            return;
        }

        if (dragKind === "group") {
            var gs = sidebar.visibleGroupEntries();
            if (gs.length === 0) {
                dropLineVisible = false;
                dropHighlightVisible = false;
                return;
            }
            var next = null;
            for (var n = 0; n < gs.length; ++n) {
                if (gs[n].index >= target.index) {
                    next = gs[n];
                    break;
                }
            }
            dropLineY = next ? groupSpan(next).top
                             : groupSpan(gs[gs.length - 1]).bottom;
            dropLineVisible = true;
            dropHighlightVisible = false;
            return;
        }

        var entry = groupEntry(target.groupId);
        if (!entry || !entry.header) {
            dropLineVisible = false;
            dropHighlightVisible = false;
            return;
        }

        // Appending into a collapsed group has no insertion line to draw, so
        // the whole header lights up instead.
        if (entry.header.collapsed) {
            dropHighlightY = itemTop(entry.header);
            dropHighlightHeight = entry.header.height;
            dropHighlightVisible = true;
            dropLineVisible = false;
            return;
        }

        var rows = visibleRows(entry);
        if (target.index < rows.length)
            dropLineY = itemTop(rows[target.index]);
        else if (rows.length > 0)
            dropLineY = itemTop(rows[rows.length - 1]) + rows[rows.length - 1].height;
        else
            dropLineY = itemTop(entry.header) + entry.header.height;
        dropLineVisible = true;
        dropHighlightVisible = false;
    }

    // Flat, visually ordered cursor list: every model group header, plus the
    // sessions of expanded groups whose delegate is currently realised.
    function navItems() {
        var gs = orderedGroups();
        var out = [];
        for (var i = 0; i < gs.length; ++i) {
            out.push({ id: gs[i].itemId, isGroup: true, item: gs[i].header,
                       modelIndex: gs[i].index, model: gs[i].model });
            if (!gs[i].header || gs[i].header.collapsed)
                continue;
            for (var j = 0; j < gs[i].rows.length; ++j)
                out.push({ id: gs[i].rows[j].itemId, isGroup: false,
                           item: gs[i].rows[j], modelIndex: -1,
                           model: null });
        }
        return out;
    }

    function setCurrent(id, isGroup) {
        currentId = id;
        currentIsGroup = isGroup;
        if (!isGroup)
            selectedSessionId = id;
    }

    function selectSession(row) {
        setCurrent(row.itemId, false);
        sidebar.forceActiveFocus();
        sidebar.sessionActivated(row.itemId);
    }

    function selectGroup(header) {
        setCurrent(header.itemId, true);
        sidebar.forceActiveFocus();
    }

    function moveSelection(delta) {
        var items = navItems();
        if (items.length === 0)
            return;
        var current = -1;
        for (var i = 0; i < items.length; ++i) {
            if (items[i].id === currentId && items[i].isGroup === currentIsGroup) {
                current = i;
                break;
            }
        }
        var next = current < 0 ? (delta > 0 ? 0 : items.length - 1)
                               : Math.max(0, Math.min(items.length - 1, current + delta));
        setCurrent(items[next].id, items[next].isGroup);
        ensureVisible(items[next].item, items[next].modelIndex);
    }

    function ensureVisible(item, modelIndex) {
        if (!item) {
            if (modelIndex >= 0)
                sessionsList.positionViewAtIndex(modelIndex, ListView.Contain);
            return;
        }
        var top = item.mapToItem(sessionsList.contentItem, 0, 0).y;
        var bottom = top + item.height;
        if (top < sessionsList.contentY)
            sessionsList.contentY = top;
        else if (bottom > sessionsList.contentY + sessionsList.height)
            sessionsList.contentY = bottom - sessionsList.height;
    }

    // Enter on a session is the activation the host listens for; on a group it
    // falls back to the same toggle Space performs, so the cursor is never a
    // dead end.
    function activateCurrent() {
        if (currentId === "")
            return;
        if (currentIsGroup)
            toggleCurrentGroup();
        else
            sidebar.sessionActivated(currentId);
    }

    function toggleCurrentGroup() {
        if (!currentIsGroup || currentId === "")
            return;
        var entry = groupEntry(currentId);
        if (!entry)
            return;
        var collapsed = entry.header ? entry.header.collapsed : entry.model.collapsed;
        app.setGroupCollapsed(currentId, !collapsed);
    }

    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Down) {
            moveSelection(1);
            event.accepted = true;
        } else if (event.key === Qt.Key_Up) {
            moveSelection(-1);
            event.accepted = true;
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            activateCurrent();
            event.accepted = true;
        } else if (event.key === Qt.Key_Space) {
            toggleCurrentGroup();
            event.accepted = true;
        } else if (event.key === Qt.Key_Escape && dragKind !== "") {
            cancelDrag();
            event.accepted = true;
        }
    }

    // Header bar with the compact '+' add-group action.
    Rectangle {
        id: headerBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 40
        color: Theme.surfaceDeep

        Label {
            text: qsTr("Sessions")
            color: Theme.text
            font.bold: true
            font.pixelSize: Theme.fontSizeTitle
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
        }
        Button {
            id: pinFilterButton
            objectName: "pinFilterButton"
            anchors.right: archiveFilterButton.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 24
            implicitHeight: 24
            width: 24
            height: 24
            padding: 0
            focusPolicy: Qt.StrongFocus
            text: sidebar.pinnedOnly ? "\u2605" : "\u2606"

            readonly property string actionText:
                sidebar.pinnedOnly ? qsTr("Show all sessions")
                                    : qsTr("Show pinned sessions only")
            Accessible.role: Accessible.Button
            Accessible.name: pinFilterButton.actionText

            AppToolTip {
                objectName: "pinFilterButtonTip"
                text: pinFilterButton.actionText
                visible: pinFilterButton.hovered
                delay: 600
            }

            contentItem: Label {
                text: pinFilterButton.text
                color: pinFilterButton.down ? Theme.accent : Theme.text
                font.pixelSize: Theme.fontSizeTitle
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: Theme.radiusSmall
                color: pinFilterButton.down ? Theme.surfaceRaised
                     : pinFilterButton.hovered ? Theme.surfaceHover : "transparent"
                border.width: pinFilterButton.visualFocus ? 2 : 1
                border.color: pinFilterButton.visualFocus ? Theme.accent : Theme.borderSubtle
            }
            onClicked: sidebar.pinnedOnly = !sidebar.pinnedOnly
        }
        Button {
            id: archiveFilterButton
            objectName: "archiveFilterButton"
            anchors.right: newGroupButton.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 24
            implicitHeight: 24
            width: 24
            height: 24
            padding: 0
            focusPolicy: Qt.StrongFocus
            // A box glyph is intentionally different from the pin star.
            text: "\u25a3"

            readonly property string actionText:
                sidebar.showArchived ? qsTr("Hide archived sessions")
                                     : qsTr("Show archived sessions")
            Accessible.role: Accessible.Button
            Accessible.name: archiveFilterButton.actionText

            AppToolTip {
                objectName: "archiveFilterButtonTip"
                text: archiveFilterButton.actionText
                visible: archiveFilterButton.hovered
                delay: 600
            }

            contentItem: Label {
                text: archiveFilterButton.text
                color: archiveFilterButton.down ? Theme.accent : Theme.text
                font.pixelSize: Theme.fontSizeTitle
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: Theme.radiusSmall
                color: archiveFilterButton.down ? Theme.surfaceRaised
                     : archiveFilterButton.hovered ? Theme.surfaceHover : "transparent"
                border.width: archiveFilterButton.visualFocus ? 2 : 1
                border.color: archiveFilterButton.visualFocus ? Theme.accent : Theme.borderSubtle
            }
            onClicked: sidebar.showArchived = !sidebar.showArchived
        }

        Button {
            id: newGroupButton
            objectName: "newGroupButton"
            text: "+"
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            // A compact square rather than the old "+ New group" words: this bar
            // is 320 pixels wide at most and the words took a fifth of it. 24
            // logical pixels is the floor for a reliable pointer target on a
            // high-density display, so the box stays 24 even though the glyph in
            // it is smaller.
            implicitWidth: 24
            implicitHeight: 24
            padding: 0
            // Reachable without a pointer: Tab lands here, and the accessible
            // name below is what a screen reader announces.
            focusPolicy: Qt.StrongFocus

            // A bare "+" says nothing about what it adds, so this sentence is
            // the button's real name. The tooltip is a pointer-only hint, so the
            // SAME sentence is also the accessible name — a tooltip is never the
            // only label a control has.
            readonly property string actionText: qsTr("Add a group")

            Accessible.role: Accessible.Button
            Accessible.name: newGroupButton.actionText

            // The module's one tooltip (AppToolTip.qml). The attached
            // `ToolTip.text` form is drawn by the Basic style in that style's
            // own light palette, so a hint about this dark panel arrived as a
            // white box. Named so tst_sidebar can read the hint back.
            AppToolTip {
                objectName: "newGroupButtonTip"
                text: newGroupButton.actionText
                visible: newGroupButton.hovered
                // Long enough that crossing the bar on the way somewhere else
                // does not flash it, matching the row tooltip in SessionRow.qml.
                delay: 600
            }

            contentItem: Label {
                text: newGroupButton.text
                color: newGroupButton.down ? Theme.textOnAccent : Theme.text
                font.pixelSize: Theme.fontSizeTitle
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: Theme.radiusSmall
                color: newGroupButton.down ? Theme.accent
                     : newGroupButton.hovered ? Theme.border : Theme.surfaceRaised
                border.width: newGroupButton.visualFocus ? 2 : 1
                border.color: newGroupButton.visualFocus ? Theme.accent : Theme.border
            }
            onClicked: newGroupDialog.open()
        }
    }

    ListView {
        id: sessionsList
        objectName: "sessionsList"
        anchors.top: headerBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: statusFooter.top
        clip: true
        model: groupsDelegateModel
        // Keep nearby group/session blocks realised while scrolling so drag
        // geometry and keyboard navigation do not lose their delegates at the
        // viewport edge. The model remains authoritative; this only controls
        // the visual cache.
        cacheBuffer: 256
        ScrollBar.vertical: AppScrollBar {}
    }

    // Nothing to show. WHICH nothing matters: a fresh install has no server to
    // ask, a connected workspace may have no sessions, and a default archive
    // filter may simply be hiding all of the sessions that do exist.
    Column {
        id: sidebarEmptyState
        objectName: "sidebarEmptyState"
        anchors.centerIn: sessionsList
        width: sidebar.width - 40
        spacing: 8
        visible: groupsDelegateModel.count === 0

        readonly property bool serverReachable: sidebar.linkState === ""
                                                || sidebar.linkState === "connected"
        readonly property bool allSessionsArchived:
            !sidebar.pinnedOnly && !sidebar.showArchived
            && app && app.sessionsModel
            && app.sessionsModel.hasSessions
            && !app.sessionsModel.hasUnarchivedSessions
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: sidebarEmptyState.serverReachable ? "\u2637" : "\u26a0"
            color: Theme.textFaint
            font.pixelSize: 26
        }
        Label {
            objectName: "sidebarEmptyTitle"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: sidebarEmptyState.serverReachable
                  ? (sidebar.pinnedOnly ? qsTr("No pinned sessions")
                     : sidebarEmptyState.allSessionsArchived
                       ? qsTr("All your sessions are archived")
                       : qsTr("No sessions yet"))
                  : qsTr("No server")
            color: Theme.text
        }
        Label {
            objectName: "sidebarEmptyHint"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: sidebarEmptyState.serverReachable
                  ? (sidebarEmptyState.allSessionsArchived
                     ? qsTr("Show archived sessions to see them here.")
                     : sidebar.pinnedOnly
                       ? qsTr("Pin a session to see it here.")
                       : qsTr("Press the \u201c+\u201d at the top of this panel to add a group, then add a Dev Session to it."))
                  : qsTr("Connect to the machine that holds your checkout, and its groups and Dev Sessions appear here.")
            color: Theme.textDim
        }
    }

    // Link status. Zero-height (and absent) when the host publishes no
    // connection surface, so a sidebar driven by a bare model is laid out
    // exactly as before.
    Rectangle {
        id: statusFooter
        objectName: "statusFooter"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: sidebar.linkState !== ""
        height: visible ? 26 : 0
        color: Theme.surfaceDeep

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.borderSubtle
        }

        Button {
            id: serverSettingsButton
            objectName: "serverSettingsButton"
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Server…")
            leftPadding: 6
            rightPadding: 6

            contentItem: Label {
                text: serverSettingsButton.text
                color: Theme.text
                font: serverSettingsButton.font
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: serverSettingsButton.down ? Theme.border
                                                  : (serverSettingsButton.hovered ? Theme.surfaceRaised
                                                                                  : "transparent")
                radius: Theme.radiusSmall
            }
            onClicked: sidebar.serverSettingsRequested()
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.right: serverSettingsButton.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            clip: true
            spacing: 6

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                // Squared off for every state that is waiting on the user, the
                // same silhouette rule ConnectSheet's chip uses.
                radius: sidebar.linkState === "hostkey" || sidebar.linkState === "credential"
                        || sidebar.linkState === "failed" ? 2 : 6
                color: sidebar.linkColor(sidebar.linkState)

                Label {
                    anchors.centerIn: parent
                    text: sidebar.linkGlyph(sidebar.linkState)
                    color: Theme.textOnAccent
                    font.bold: true
                }
            }
            Label {
                objectName: "linkStatusLabel"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(0, parent.width - 18)
                text: sidebar.linkWords(sidebar.linkState)
                // The ordinary status label is a presentation shade, so Theme
                // changes it with the rest of the footer.
                color: sidebar.stale ? Theme.warning : Theme.statusText()
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
    }

    DelegateModel {
        id: groupsDelegateModel
        model: app.sessionsModel

        delegate: Column {
            id: groupBlock
            required property int index
            required property string name
            required property bool collapsed
            required property string itemId

            width: sessionsList.width

            GroupHeader {
                width: parent.width
                host: sidebar
                index: groupBlock.index
                name: groupBlock.name
                collapsed: groupBlock.collapsed
                itemId: groupBlock.itemId
                sessionCount: sidebar.sessionCountForGroup(groupBlock.itemId)
            }

            // Collapsed groups hide their sessions (SPEC 4.2).
            Column {
                id: sessionsColumn
                width: parent.width
                visible: !groupBlock.collapsed
                height: visible ? implicitHeight : 0

                Repeater {
                    model: DelegateModel {
                        model: app.sessionsModel
                        rootIndex: groupsDelegateModel.modelIndex(groupBlock.index)
                        delegate: SessionRow {
                            width: sessionsColumn.width
                            host: sidebar
                        }
                    }
                }
            }
        }
    }

    // Insertion indicator: a line between rows, or a filled header when the
    // drop appends into a collapsed group. Clipped to the list viewport so a
    // target scrolled out of view cannot paint over the header bar.
    Item {
        id: dropOverlay
        anchors.fill: sessionsList
        clip: true
        z: 5

        Rectangle {
            id: dropHighlight
            objectName: "dropHighlight"
            visible: sidebar.dropHighlightVisible
            x: 0
            y: sidebar.dropHighlightY - dropOverlay.y
            width: dropOverlay.width
            height: sidebar.dropHighlightHeight
            // This violet-tinted wash follows the active theme rather than
            // leaving a dark drop surface behind in light mode.
            color: Theme.dropHighlightSurface()
            border.color: Theme.accent
            border.width: 1
        }

        Rectangle {
            id: dropLine
            objectName: "dropIndicator"
            visible: sidebar.dropLineVisible
            x: 0
            y: sidebar.dropLineY - dropOverlay.y - height / 2
            width: dropOverlay.width
            height: 2
            color: Theme.accent
        }
    }

    // Small label following the cursor so the drag reads as a drag even though
    // the source row stays put.
    Rectangle {
        id: dragProxy
        objectName: "dragProxy"
        visible: sidebar.dragKind !== "" && sidebar.dragItem !== null
        x: sidebar.dragProxyX
        y: sidebar.dragProxyY
        width: Math.min(sidebar.width - 24, dragProxyLabel.implicitWidth + 16)
        height: dragProxyLabel.implicitHeight + 8
        radius: Theme.radiusSmall
        color: Theme.surfaceRaised
        border.color: Theme.accent
        border.width: 1
        opacity: 0.9
        z: 7

        Label {
            id: dragProxyLabel
            anchors.centerIn: parent
            width: parent.width - 16
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            elide: Text.ElideRight
            text: sidebar.dragItem ? sidebar.dragItem.name : ""
            textFormat: Text.PlainText
        }
    }

    AppDialog {
        id: newGroupDialog
        objectName: "newGroupDialog"
        title: qsTr("New group")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        // `Dialog` defaults to a 320-pixel frame, but this 300-pixel field
        // needs the 12-pixel padding on both sides too.
        width: newGroupField.Layout.preferredWidth + leftPadding + rightPadding

        // Reset the field each open so a cancelled edit doesn't resurface as
        // stale text next time (imperative input breaks the initial binding),
        // then SELECT it: assigning `text` leaves the cursor at the end, so a
        // user who opens this and types "api" was getting "New groupapi".
        // Selecting the placeholder default makes the first keystroke replace
        // it, which is what every other pre-filled field here now does too.
        onOpened: {
            newGroupField.text = qsTr("New group");
            newGroupField.forceActiveFocus();
            newGroupField.selectAll();
        }

        // A Dialog sizes itself from its content item's IMPLICIT width, and a
        // bare TextField with an explicit `width` still reports a much smaller
        // implicit width — so the field used to spill out past the dialog's
        // edge. A ColumnLayout does report the preferred widths of its children,
        // which is the convention newSessionDialog below already uses.
        ColumnLayout {
            // Layout.preferredWidth sizes the child only; Dialog measures this
            // layout's implicit width. Keep both values explicit so the frame
            // includes its 12-pixel padding on either side.
            implicitWidth: 300
            spacing: 8

            TextField {
                id: newGroupField
                objectName: "newGroupField"
                Layout.preferredWidth: 300
                placeholderText: qsTr("Group name")
                text: qsTr("New group")
            }
        }

        onAccepted: {
            var groupName = newGroupField.text.length > 0
                            ? newGroupField.text : qsTr("New group");
            app.createGroup(groupName);
        }
    }

    // Which group the pending session belongs to. Captured when the action is
    // invoked rather than read back from the selection, so a click elsewhere
    // while the dialog is open cannot retarget the creation.
    property string pendingSessionGroupId: ""

    function requestNewSession(groupId) {
        sidebar.pendingSessionGroupId = groupId;
        newSessionDialog.open();
    }

    AppDialog {
        id: newSessionDialog
        objectName: "newSessionDialog"
        // Size the frame around the fixed-width content rather than letting the
        // Basic-style 320-pixel default clip its horizontal padding.
        width: newSessionField.Layout.preferredWidth + leftPadding + rightPadding
        title: qsTr("New Dev Session")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay

        // Same rule as the group dialog: pre-filled name, selected so typing
        // replaces it instead of appending to it.
        onOpened: {
            newSessionField.text = qsTr("New session");
            newSessionRepoField.text = "";
            newSessionField.forceActiveFocus();
            newSessionField.selectAll();
        }

        ColumnLayout {
            // See the group dialog above: Dialog uses the content layout's
            // implicit width, not the children's Layout.preferredWidth.
            implicitWidth: 300
            spacing: 8

            TextField {
                id: newSessionField
                objectName: "newSessionField"
                Layout.preferredWidth: 300
                placeholderText: qsTr("Session name")
            }

            // The repository root is not decoration: it becomes the working
            // directory of every terminal in the session and the root the
            // viewers browse, so a session without one opens in the remote
            // home directory instead of the project.
            TextField {
                id: newSessionRepoField
                objectName: "newSessionRepoField"
                Layout.preferredWidth: 300
                placeholderText: qsTr("Repository path on the server, e.g. /srv/repos/app")
            }

            Label {
                Layout.preferredWidth: 300
                text: qsTr("Terminals in this session start in the repository path.")
                color: Theme.textDim
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
        }

        onAccepted: {
            const name = newSessionField.text.length > 0
                       ? newSessionField.text : qsTr("New session");
            app.createSession(sidebar.pendingSessionGroupId, name,
                              newSessionRepoField.text);
            sidebar.pendingSessionGroupId = "";
        }
        onRejected: sidebar.pendingSessionGroupId = ""
    }
}
