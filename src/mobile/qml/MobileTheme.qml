pragma Singleton

import QtQuick

// The mobile shell's one colour and metric vocabulary, and the deliberate mirror
// of src/qml/Theme.qml.
//
// The COLOUR half is copied role for role and value for value. That is the point:
// the two shells are two presentations of one product, they show the same
// server-authoritative state, and a user who reads a session name in one and then
// the other must not be looking at two different palettes. The role NAMES matter
// as much as the values — a mobile page that invented `surfaceAlt` or `error`
// would be a second vocabulary, and the next colour change would update one shell
// and not the other. So: same names, same table, same dark/light selection out of
// the same `app.settings.theme` preference.
//
// The METRIC half deliberately does NOT match, and that is the whole reason this
// is a separate file rather than an import of the desktop singleton (which is in
// a QML module that is not even built for a mobile target). A 26-pixel pane
// header and a 12-pixel body size are correct for a pointer and wrong for a
// thumb: every interactive row here is at least touchTarget tall, and the text
// sizes step up accordingly. There are no split-handle or scrollbar metrics
// because the mobile shell has neither.
QtObject {
    // Byte-identical to Theme.qml's table, including the presentation-only
    // shades below the role block, which are exposed through the same functions
    // rather than as colour roles so no call site duplicates a literal.
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
            textDim: "#949ab3",
            textOnAccent: "#11111b",
            textFaint: "#45475a",
            accent: "#89b4fa",
            success: "#a6e3a1",
            warning: "#f9e2af",
            danger: "#f38ba8",
            busy: "#cba6f7",
            statusText: "#a6adc8",
            textPlaceholder: "#585b70",
            errorSurface: "#3a1d28",
            warningSurface: "#3a2f1e"
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
            textDim: "#555871",
            textOnAccent: "#ffffff",
            textFaint: "#9ca0b0",
            accent: "#1e66f5",
            success: "#2d7f24",
            warning: "#a85d00",
            danger: "#c01c3f",
            busy: "#7c2fce",
            statusText: "#5c6078",
            textPlaceholder: "#6f748a",
            errorSurface: "#f7d8df",
            warningSurface: "#f7e4c8"
        }
    })

    // A missing context object is intentional in headless singleton probes and
    // in a page loaded bare. Same rule as an unknown preference: use the
    // documented dark default rather than letting an absent value turn into a
    // blank colour.
    function configuredTheme() {
        if (typeof app === "undefined" || !app || !app.settings)
            return "dark";
        var requested = String(app.settings.theme);
        return palettes[requested] !== undefined ? requested : "dark";
    }
    readonly property string themeName: configuredTheme()
    readonly property var activePalette: palettes[themeName] || palettes.dark

    // ---- surfaces ----------------------------------------------------------

    // The page itself.
    readonly property color surface: activePalette.surface
    // A list or sheet sitting on the page.
    readonly property color surfaceDeep: activePalette.surfaceDeep
    // Sunken areas: text-field wells and the terminal's own background.
    readonly property color surfaceSunken: activePalette.surfaceSunken
    // A raised control at rest: a button, a chip, a row.
    readonly property color surfaceRaised: activePalette.surfaceRaised
    // A row under the finger. There is no hover on a touch screen, so this is
    // the PRESSED shade here; the name is kept because the role is the same one
    // and renaming it would fork the vocabulary.
    readonly property color surfaceHover: activePalette.surfaceHover
    readonly property color surfaceSelected: activePalette.surfaceSelected

    // ---- lines -------------------------------------------------------------

    readonly property color border: activePalette.border
    readonly property color borderSubtle: activePalette.borderSubtle

    // ---- text --------------------------------------------------------------

    readonly property color text: activePalette.text
    // Secondary text: subtitles, status lines, explanations. Secondary is about
    // importance, not about being hard to read.
    readonly property color textDim: activePalette.textDim
    readonly property color textOnAccent: activePalette.textOnAccent
    // Decoration ONLY: placeholder glyphs and disabled labels. Anything a user
    // is expected to READ uses textDim or text.
    readonly property color textFaint: activePalette.textFaint

    // ---- accents -----------------------------------------------------------

    readonly property color accent: activePalette.accent
    readonly property color success: activePalette.success
    // "Waiting on you": a host-key prompt, a password request.
    readonly property color warning: activePalette.warning
    readonly property color danger: activePalette.danger
    // An agent that is working.
    readonly property color busy: activePalette.busy

    // Presentation helpers for shades that are not part of the shared role
    // vocabulary, spelled as functions exactly as the desktop spells them.
    function statusText() { return activePalette.statusText; }
    function textPlaceholder() { return activePalette.textPlaceholder; }
    function errorSurface() { return activePalette.errorSurface; }
    function warningSurface() { return activePalette.warningSurface; }

    // The same vocabulary as a plain object of "#rrggbb" strings, for the one
    // consumer that is not a QML colour: ch::VtScreen's default foreground and
    // background, which are QRgb values a C++ object has to be told. Written as
    // hex because that is what the palette table already holds; `toString()` on
    // a QML colour yields "#aarrggbb", whose leading alpha pair would silently
    // shift every value.
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

    // The minimum size of anything a finger is meant to hit. 48 is the figure
    // both platform guidelines land on (Material's 48dp touch target, Apple's
    // 44pt plus its own spacing recommendation), and it is a FLOOR, not a
    // suggestion: every delegate and control in this module sets its height or
    // implicitHeight to at least this.
    readonly property int touchTarget: 48

    readonly property int spacingSmall: 4
    readonly property int spacing: 8
    readonly property int spacingLarge: 16

    readonly property int radiusSmall: 4
    readonly property int radiusMedium: 6

    // Pixel sizes, like the desktop's, and larger than the desktop's: a phone is
    // held closer but has a much finer pixel pitch, and 12-pixel body text is
    // unreadable on either platform at native density.
    readonly property int fontSizeSmall: 12
    readonly property int fontSizeBody: 14
    readonly property int fontSizeLabel: 15
    readonly property int fontSizeTitle: 18

    // The one monospace family name, used by the terminal and by every path
    // shown to the user. Qt resolves "Monospace" to the platform's fixed-pitch
    // font.
    readonly property string monoFamily: "Monospace"
}
