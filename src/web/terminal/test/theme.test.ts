import { test } from "node:test";
import assert from "node:assert/strict";

import {
    applyThemeToDocument,
    defaultThemeRoles,
    normalizeThemeRoles,
    type ThemeStyleTarget,
    xtermTheme,
} from "../src/theme.ts";

// A stand-in for document.documentElement.style: it records every custom
// property the adapter sets, which is the only observable
// applyThemeToDocument has.
function fakeDocument(): {
    document: ThemeStyleTarget;
    properties: Map<string, string>;
} {
    const properties = new Map<string, string>();
    return {
        properties,
        document: {
            documentElement: {
                style: {
                    setProperty(name: string, value: string): void {
                        properties.set(name, value);
                    },
                },
            },
        },
    };
}

test("a host update overrides only the roles it supplies", () => {
    const roles = normalizeThemeRoles({ text: "#ffffff", accent: "#010203" });

    assert.equal(roles.text, "#ffffff");
    assert.equal(roles.accent, "#010203");
    // Everything the host did not mention keeps the standalone dark default.
    assert.equal(roles.surface, defaultThemeRoles.surface);
    assert.equal(Object.keys(roles).length, Object.keys(defaultThemeRoles).length);
});

// QML's Theme.roles exports opaque #rrggbb. Qt spells an ALPHA colour
// #aarrggbb, while CSS reads eight digits as #rrggbbaa — so accepting eight
// digits would silently render a different colour from the one QML asked for.
test("only opaque six-digit hex is accepted; anything else keeps the default", () => {
    for (const rejected of [
        "#ff112233", // Qt #aarrggbb, which CSS would read as #rrggbbaa
        "#abc", // the short CSS form
        "red", // a named colour
        "rgb(1,2,3)",
        "#12345g", // not hex
        "",
        42,
        null,
        undefined,
        { toString: () => "#ffffff" },
    ]) {
        const roles = normalizeThemeRoles({ text: rejected });
        assert.equal(roles.text, defaultThemeRoles.text, `accepted ${JSON.stringify(rejected)}`);
    }

    // Case is not significant; both spellings are valid CSS.
    assert.equal(normalizeThemeRoles({ text: "#AaBbCc" }).text, "#AaBbCc");
});

test("a missing, non-object or unknown-keyed payload yields the defaults", () => {
    for (const input of [undefined, null, "#ffffff", 7, []]) {
        assert.deepEqual(normalizeThemeRoles(input), defaultThemeRoles);
    }
    // An unknown role is not smuggled into the result: the role set is fixed by
    // defaultThemeRoles, so the CSS variables the page defines cannot grow from
    // whatever the host happens to send.
    const roles = normalizeThemeRoles({ notARole: "#ffffff" });
    assert.equal("notARole" in roles, false);
});

test("every role becomes one --ch- CSS variable, camelCase spelled as kebab-case", () => {
    const { document, properties } = fakeDocument();
    const roles = applyThemeToDocument(document, { surfaceDeep: "#123456" });

    assert.equal(properties.size, Object.keys(defaultThemeRoles).length);
    assert.equal(properties.get("--ch-surface-deep"), "#123456");
    assert.equal(properties.get("--ch-text"), defaultThemeRoles.text);
    assert.equal(properties.get("--ch-text-on-accent"), defaultThemeRoles.textOnAccent);
    // The applied roles are returned so the caller can push the same values at
    // xterm without normalising twice.
    assert.equal(roles.surfaceDeep, "#123456");
});

// Applying the same payload twice must not accumulate anything: the host pushes
// its whole theme on every change.
test("applying a theme is idempotent", () => {
    const { document, properties } = fakeDocument();
    applyThemeToDocument(document, { accent: "#0a0b0c" });
    const first = new Map(properties);
    applyThemeToDocument(document, { accent: "#0a0b0c" });

    assert.deepEqual([...properties.entries()].sort(), [...first.entries()].sort());
});

test("the xterm palette is drawn from the role names the page renders with", () => {
    const roles = normalizeThemeRoles({
        surfaceSunken: "#000001",
        text: "#000002",
        accent: "#000003",
        surfaceSelected: "#000004",
    });

    assert.deepEqual(xtermTheme(roles), {
        background: "#000001",
        foreground: "#000002",
        cursor: "#000003",
        // The cursor's own glyph is drawn in the background colour, so it stays
        // legible on top of the cursor block.
        cursorAccent: "#000001",
        selectionBackground: "#000004",
    });
});
