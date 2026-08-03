pragma Singleton

import QtQuick

// One registry for every toolbar affordance. A button declares its stable id on
// the button itself, then registers while it exists; the settings pane can name
// the same buttons without walking the visual tree (which would miss panes that
// have not been created yet). The four navigation ids are kept as defaults so
// a settings window opened before the first viewer pane still offers a complete
// order, while buttons from a build that has disappeared are filtered when the
// order is reconciled.
QtObject {
    id: registry

    // The ids currently declared by live controls. This is intentionally a
    // QVariant-style list rather than a model: callers bind to idsChanged and
    // derive the small ordered list they need without sharing delegate objects.
    property var ids: []
    // More than one viewer pane can own a button with the same id. Reference
    // counts keep destroying one pane from hiding an id that another pane still
    // displays.
    property var counts: ({})

    // Keep all pane-header controls available to Appearance settings even
    // before a region has materialised its first pane. Live actions still
    // reference-count these ids when panes come and go.
    readonly property var defaultIds: [
        "nav.back", "nav.forward", "nav.reload", "nav.home",
        "pane.split.horizontal", "pane.split.vertical", "pane.close",
        "terminal.kill"
    ]

    signal orderChanged()

    function _cleanId(raw) {
        if (raw === undefined || raw === null)
            return "";
        return String(raw).trim();
    }

    function _unique(values) {
        var result = [];
        if (!values)
            return result;
        for (var i = 0; i < values.length; ++i) {
            var id = registry._cleanId(values[i]);
            if (id.length > 0 && result.indexOf(id) < 0)
                result.push(id);
        }
        return result;
    }

    function registerButton(id) {
        var clean = registry._cleanId(id);
        if (clean.length === 0)
            return;
        var nextCounts = Object.assign({}, registry.counts);
        nextCounts[clean] = (nextCounts[clean] || 0) + 1;
        registry.counts = nextCounts;
        if (registry.ids.indexOf(clean) >= 0)
            return;
        registry.ids = registry.ids.concat([clean]);
        registry.orderChanged();
    }

    function unregisterButton(id) {
        var clean = registry._cleanId(id);
        if (clean.length === 0 || !registry.counts[clean])
            return;
        var nextCounts = Object.assign({}, registry.counts);
        nextCounts[clean] -= 1;
        if (nextCounts[clean] > 0) {
            registry.counts = nextCounts;
            return;
        }
        delete nextCounts[clean];
        registry.counts = nextCounts;
        var next = [];
        for (var i = 0; i < registry.ids.length; ++i) {
            if (registry.ids[i] !== clean)
                next.push(registry.ids[i]);
        }
        registry.ids = next;
        registry.orderChanged();
    }


    // IDs that can be shown by the settings pane. Defaults are included even
    // before their visual controls exist, while a live control can add another
    // id without any central list having to be edited.
    function knownIds() {
        return registry._unique(registry.defaultIds.concat(registry.ids));
    }

    // Keep stored entries that name a button in this build, then append every
    // known button omitted by the stored list. Unknown ids are deliberately not
    // copied: they came from a different build and cannot be rendered here.
    function reconcile(stored, known) {
        var available = registry._unique(known);
        var result = [];
        var saved = registry._unique(stored);
        for (var i = 0; i < saved.length; ++i) {
            if (available.indexOf(saved[i]) >= 0)
                result.push(saved[i]);
        }
        for (var j = 0; j < available.length; ++j) {
            if (result.indexOf(available[j]) < 0)
                result.push(available[j]);
        }
        return result;
    }

    function storedOrder() {
        if (typeof app === "undefined" || !app || !app.settings)
            return [];
        return app.settings.toolbarOrder || [];
    }

    // Return only the supplied live controls, sorted by the reconciled order.
    // This is used by standalone address-bar actions as well as header actions;
    // it does not mutate the preference merely because a pane was rebuilt.
    function ordered(buttonIds) {
        var live = registry._unique(buttonIds);
        var all = registry.reconcile(registry.storedOrder(), live);
        var result = [];
        for (var i = 0; i < all.length; ++i) {
            if (live.indexOf(all[i]) >= 0)
                result.push(all[i]);
        }
        return result;
    }

    function index(id) {
        var clean = registry._cleanId(id);
        var orderedIds = registry.ordered(registry.knownIds());
        return orderedIds.indexOf(clean);
    }

    // QtObject has no default property, so this cannot be an anonymous child:
    // it has to be held by a named property to be part of the singleton at all.
    property Connections settingsLink: Connections {
        target: (typeof app !== "undefined" && app && app.settings)
                ? app.settings : null
        function onToolbarOrderChanged() { registry.orderChanged(); }
    }
}
