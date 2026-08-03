// Theme roles are pushed from QML as one plain object. This adapter keeps the
// terminal page useful on its own (the dark defaults) while making every host
// update a single idempotent operation for both CSS and xterm.

export type ThemeRoles = Record<string, string>;

export const defaultThemeRoles: ThemeRoles = {
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
    textFaint: "#45475a",
    textOnAccent: "#11111b",
    accent: "#89b4fa",
    success: "#a6e3a1",
    warning: "#f9e2af",
    danger: "#f38ba8",
    busy: "#cba6f7",
    textPlaceholder: "#585b70",
    controlHoverSurface: "#3a3a52",
    errorSurface: "#45222c",
    warningSurface: "#3a2f1e",
    modalOverlaySurface: "#e61e1e",
    statusReconnecting: "#fab387",
    statusText: "#a6adc8",
    inactiveRail: "#585b70",
    dropHighlightSurface: "#302a4a",
};

function isColor(value: unknown): value is string {
    return typeof value === "string" && /^#[0-9a-fA-F]{6}(?:[0-9a-fA-F]{2})?$/.test(value);
}

function cssVariableName(role: string): string {
    return `--ch-${role.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`)}`;
}

export function normalizeThemeRoles(input: unknown): ThemeRoles {
    const roles: ThemeRoles = { ...defaultThemeRoles };
    if (!input || typeof input !== "object") {
        return roles;
    }
    for (const role of Object.keys(defaultThemeRoles)) {
        const value = (input as Record<string, unknown>)[role];
        if (isColor(value)) {
            roles[role] = value;
        }
    }
    return roles;
}

export function applyThemeToDocument(
    document: Pick<Document, "documentElement">,
    input: unknown,
): ThemeRoles {
    const roles = normalizeThemeRoles(input);
    for (const [role, value] of Object.entries(roles)) {
        document.documentElement.style.setProperty(cssVariableName(role), value);
    }
    return roles;
}

export function xtermTheme(roles: ThemeRoles): Record<string, string> {
    return {
        background: roles.surfaceSunken,
        foreground: roles.text,
        cursor: roles.accent,
        cursorAccent: roles.surfaceSunken,
        selectionBackground: roles.surfaceSelected,
        selectionForeground: roles.textOnAccent,
    };
}
