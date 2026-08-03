pragma Singleton

import QtQuick

// The application's one colour and metric vocabulary (SPEC 4.1).
//
// Every surface in this module used to write its own hex literals, and the same
// eight or nine colours were repeated roughly two hundred times across eighteen
// files. That is not merely repetitive: it is why the four modal dialogs drifted
// into the Basic style's light default palette without anybody noticing, and why
// a scrollbar or a split handle could be left in the platform's own colours
// while the surface around it was dark.
//
// The palette table keeps the role names independent of a particular visual
// theme. The dark entry is Catppuccin Mocha, which is what the original literals
// already spelled out; the light entry follows Catppuccin Latte's quiet neutral
// surfaces and uses darker accent values so white glyphs remain readable. A
// future theme adds one table entry rather than another boolean at every call
// site.
//
// Registered as a QML singleton (see src/qml/CMakeLists.txt), so every file
// reaches it as `Theme.<name>` after `import CodeHarbor`.
QtObject {
    readonly property var palettes: ({
        dark: {
            surface: "#1e1e2e",
            surfaceDeep: "#181825",
            surfaceSunken: "#11111b",
            surfaceRaised: "#313244",
            surfaceHover: "#232338",
            surfaceSelected: "#313244",
            border: "#45475a",
            borderSubtle: "#313244",
            text: "#cdd6f4",
            textDim: "#6c7086",
            textOnAccent: "#11111b",
            textFaint: "#45475a",
            accent: "#89b4fa",
            success: "#a6e3a1",
            warning: "#f9e2af",
            danger: "#f38ba8",
            busy: "#cba6f7",
            // These are presentation-only shades used by the sidebar. They
            // stay in the theme table without becoming colour roles, because
            // callers should not duplicate the literals or expose a second
            // vocabulary through the singleton's public QColor properties.
            groupSelectedSurface: "#2a2a40",
            statusReconnecting: "#fab387",
            statusText: "#a6adc8",
            inactiveRail: "#585b70",
            dropHighlightSurface: "#302a4a",
            textPlaceholder: "#585b70",
            controlHoverSurface: "#3a3a52",
            errorSurface: "#3a1d28",
            warningSurface: "#3a2f1e",
            modalOverlaySurface: "#e61e1e2e",
            groupTextDarker: 1.0
        },
        light: {
            surface: "#eff1f5",
            surfaceDeep: "#e6e9ef",
            surfaceSunken: "#dce0e8",
            surfaceRaised: "#ccd0da",
            surfaceHover: "#dfe3eb",
            surfaceSelected: "#ccd0da",
            border: "#9ca0b0",
            borderSubtle: "#bcc0cc",
            text: "#4c4f69",
            textDim: "#7c7f93",
            textOnAccent: "#ffffff",
            textFaint: "#9ca0b0",
            // Accent meanings stay the same, but these values are deep enough
            // for Theme.textOnAccent to pass a readable contrast check on a
            // light surface.
            accent: "#1e66f5",
            success: "#2d7f24",
            warning: "#a85d00",
            danger: "#c01c3f",
            busy: "#7c2fce",
            groupSelectedSurface: "#d6d9e5",
            statusReconnecting: "#a65d00",
            statusText: "#5c6078",
            inactiveRail: "#8d91a4",
            dropHighlightSurface: "#dfe5ff",
            textPlaceholder: "#6f748a",
            controlHoverSurface: "#d8dce6",
            errorSurface: "#f7d8df",
            warningSurface: "#f7e4c8",
            modalOverlaySurface: "#e6eff1f5",
            groupTextDarker: 1.35
        }
    })

    // A missing context object is intentional in headless singleton probes and
    // standalone delegates. It follows the same rule as an unknown preference:
    // use the documented dark default rather than letting an absent value turn
    // into a blank/black colour.
    function configuredTheme() {
        if (typeof app === "undefined" || !app || !app.settings)
            return "dark";
        var requested = String(app.settings.theme);
        return palettes[requested] !== undefined ? requested : "dark";
    }
    readonly property string themeName: configuredTheme()
    readonly property var activePalette: palettes[themeName] || palettes.dark

    // ---- surfaces ----------------------------------------------------------

    // The window itself, and any panel that sits directly on it.
    readonly property color surface: activePalette.surface
    // A region's own background: one step darker, so the three regions read as
    // distinct areas rather than one undivided sheet.
    readonly property color surfaceDeep: activePalette.surfaceDeep
    // Sunken areas: text-field wells, the terminal's own background, the inside
    // of a scroll track.
    readonly property color surfaceSunken: activePalette.surfaceSunken
    // A raised control at rest: a button, a chip, a pane header.
    readonly property color surfaceRaised: activePalette.surfaceRaised
    // A raised control under the pointer.
    readonly property color surfaceHover: activePalette.surfaceHover
    // A selected row.
    readonly property color surfaceSelected: activePalette.surfaceSelected

    // ---- lines -------------------------------------------------------------

    // The ordinary divider between two areas.
    readonly property color border: activePalette.border
    // A divider that wants less weight than `border` (inside a panel).
    readonly property color borderSubtle: activePalette.borderSubtle

    // ---- text --------------------------------------------------------------

    readonly property color text: activePalette.text
    // Secondary text: hints, subtitles, placeholder text, disabled labels.
    readonly property color textDim: activePalette.textDim
    // Text that has to sit ON an accent fill.
    readonly property color textOnAccent: activePalette.textOnAccent
    // Decorative glyphs and the very faintest labels (an internal identifier
    // kept reachable for a bug report).
    readonly property color textFaint: activePalette.textFaint

    // ---- accents -----------------------------------------------------------

    // Focus, selection, links, and anything the user is meant to act on.
    readonly property color accent: activePalette.accent
    readonly property color success: activePalette.success
    // "Waiting on you": a host-key prompt, a password request.
    readonly property color warning: activePalette.warning
    readonly property color danger: activePalette.danger
    // An agent that is working.
    readonly property color busy: activePalette.busy

    // Presentation helpers for shades that are not part of the shared role
    // vocabulary. Functions keep the existing dark values pixel-identical while
    // still allowing their light counterparts to follow the active theme.
    function groupSelectedSurface() { return activePalette.groupSelectedSurface; }
    function statusReconnecting() { return activePalette.statusReconnecting; }
    function statusText() { return activePalette.statusText; }
    function inactiveRail() { return activePalette.inactiveRail; }
    function dropHighlightSurface() { return activePalette.dropHighlightSurface; }
    function textPlaceholder() { return activePalette.textPlaceholder; }
    function controlHoverSurface() { return activePalette.controlHoverSurface; }
    function errorSurface() { return activePalette.errorSurface; }
    function warningSurface() { return activePalette.warningSurface; }
    function modalOverlaySurface() { return activePalette.modalOverlaySurface; }
    function groupTextColor(color) {
        return Qt.darker(color, Number(activePalette.groupTextDarker));
    }

    // The same vocabulary as a plain object of "#rrggbb" strings, for the two
    // surfaces that are not QML: the xterm.js terminal page and the Monaco
    // editor page. Both are web documents that cannot read a QML singleton, so
    // their host pane pushes this map into the page (window.applyTheme) once
    // the page has loaded and again whenever it changes. One shape for both
    // pages, so neither invents its own payload.
    //
    // Written as hex strings rather than QML colours because that is what CSS
    // and both libraries accept; `toString()` on a QML colour yields
    // "#aarrggbb", which CSS reads as an eight-digit RGBA and would silently
    // shift every colour, so the alpha pair is dropped explicitly.
    function _hex(c) {
        const s = String(c);
        return s.length === 9 ? "#" + s.slice(3) : s;
    }
    readonly property var roles: ({
        surface: _hex(surface),
        surfaceDeep: _hex(surfaceDeep),
        surfaceSunken: _hex(surfaceSunken),
        surfaceRaised: _hex(surfaceRaised),
        surfaceHover: _hex(surfaceHover),
        surfaceSelected: _hex(surfaceSelected),
        border: _hex(border),
        borderSubtle: _hex(borderSubtle),
        text: _hex(text),
        textDim: _hex(textDim),
        textFaint: _hex(textFaint),
        textOnAccent: _hex(textOnAccent),
        accent: _hex(accent),
        success: _hex(success),
        warning: _hex(warning),
        danger: _hex(danger),
        busy: _hex(busy),
        statusText: _hex(activePalette.statusText),
        errorSurface: _hex(activePalette.errorSurface),
        warningSurface: _hex(activePalette.warningSurface)
    })

    // ---- metrics -----------------------------------------------------------

    // A region or pane header. One value, so the terminal and viewer headers
    // line up across the window instead of each picking its own height.
    readonly property int headerHeight: 30
    // A per-pane header, which is subordinate to the region header above it.
    readonly property int paneHeaderHeight: 26
    // The grab area of a split divider. 6 is a compromise: thin enough not to
    // read as a gutter, wide enough to hit without aiming (the visible line
    // inside it is 1 pixel).
    readonly property int splitHandleThickness: 6
    // Scrollbar width. Deliberately narrower than the platform default, which
    // is sized for a scroll track with arrow buttons this application does not
    // draw.
    readonly property int scrollBarThickness: 10
    readonly property int radiusSmall: 4
    readonly property int radiusMedium: 6

    readonly property int fontSizeSmall: 10
    readonly property int fontSizeBody: 12
    readonly property int fontSizeLabel: 13
    readonly property int fontSizeTitle: 14

    // The one monospace family name, used by every terminal, editor chrome and
    // path field. Qt resolves "Monospace" to the platform's fixed-pitch font.
    readonly property string monoFamily: "Monospace"
}
