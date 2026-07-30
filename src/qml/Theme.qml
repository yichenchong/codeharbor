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
// The palette is Catppuccin Mocha, which is what those literals already spelled
// out. Names describe the ROLE, not the shade, so a future light theme is a
// matter of reassigning these and nothing else.
//
// Registered as a QML singleton (see src/qml/CMakeLists.txt), so every file
// reaches it as `Theme.<name>` after `import CodeHarbor`.
QtObject {
    // ---- surfaces ----------------------------------------------------------

    // The window itself, and any panel that sits directly on it.
    readonly property color surface: "#1e1e2e"
    // A region's own background: one step darker, so the three regions read as
    // distinct areas rather than one undivided sheet.
    readonly property color surfaceDeep: "#181825"
    // Sunken areas: text-field wells, the terminal's own background, the inside
    // of a scroll track.
    readonly property color surfaceSunken: "#11111b"
    // A raised control at rest: a button, a chip, a pane header.
    readonly property color surfaceRaised: "#313244"
    // A raised control under the pointer.
    readonly property color surfaceHover: "#232338"
    // A selected row.
    readonly property color surfaceSelected: "#313244"

    // ---- lines -------------------------------------------------------------

    // The ordinary divider between two areas.
    readonly property color border: "#45475a"
    // A divider that wants less weight than `border` (inside a panel).
    readonly property color borderSubtle: "#313244"

    // ---- text --------------------------------------------------------------

    readonly property color text: "#cdd6f4"
    // Secondary text: hints, subtitles, placeholder text, disabled labels.
    readonly property color textDim: "#6c7086"
    // Text that has to sit ON an accent fill.
    readonly property color textOnAccent: "#11111b"
    // Decorative glyphs and the very faintest labels (an internal identifier
    // kept reachable for a bug report).
    readonly property color textFaint: "#45475a"

    // ---- accents -----------------------------------------------------------

    // Focus, selection, links, and anything the user is meant to act on.
    readonly property color accent: "#89b4fa"
    readonly property color success: "#a6e3a1"
    // "Waiting on you": a host-key prompt, a password request.
    readonly property color warning: "#f9e2af"
    readonly property color danger: "#f38ba8"
    // An agent that is working.
    readonly property color busy: "#cba6f7"

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
