// Renderer preferences shared by the QML settings bridge and the xterm page.
// AppSettings stores the terminal size in points, while CSS and xterm measure
// the browser font in CSS pixels. Keeping that conversion here prevents a QML
// binding from accidentally treating a point value as a pixel value.

export const kPointsPerCssPixel = 72 / 96;
export const kMinTerminalFontPoints = 6;
export const kMaxTerminalFontPoints = 48;
export const kMinTerminalPixelRatio = 1;
export const kMaxTerminalPixelRatio = 4;

export interface TerminalPreferenceValues {
    fontSize: number;
    pixelRatio: number;
}

export interface TerminalPreferenceTarget {
    readonly cols: number;
    readonly rows: number;
    options: { fontSize?: number };
}

export interface TerminalFit {
    fit(): void;
}

export interface TerminalResizeBridge {
    resize(cols: number, rows: number): void;
}

export function normalizeTerminalPreferences(
    fontPoints: number,
    pixelRatio: number,
): TerminalPreferenceValues {
    const finiteFont = Number.isFinite(fontPoints) ? Math.round(fontPoints) : 13;
    const fontSize = Math.min(
        kMaxTerminalFontPoints,
        Math.max(kMinTerminalFontPoints, finiteFont),
    );
    if (!Number.isFinite(pixelRatio) || pixelRatio <= 0) {
        return { fontSize, pixelRatio: 0 };
    }
    return {
        fontSize,
        pixelRatio: Math.min(
            kMaxTerminalPixelRatio,
            Math.max(kMinTerminalPixelRatio, pixelRatio),
        ),
    };
}

/** Convert the user-facing point value into the CSS pixels xterm expects. */
export function terminalFontPointsToCssPixels(points: number): number {
    return points / kPointsPerCssPixel;
}

/**
 * Apply settings without rebuilding the page. xterm's option setter remeasures
 * its character cell, then FitAddon recomputes cols/rows; the final explicit
 * resize is intentional because a ratio/font change can leave the integer grid
 * unchanged while still requiring the remote PTY to receive the current size.
 */
export function applyTerminalPreferences(
    terminal: TerminalPreferenceTarget,
    fit: TerminalFit,
    canFit: boolean,
    bridge: TerminalResizeBridge,
    values: TerminalPreferenceValues,
    setPixelRatio: (ratio: number) => void,
): TerminalPreferenceValues {
    const normalized = normalizeTerminalPreferences(values.fontSize, values.pixelRatio);
    setPixelRatio(normalized.pixelRatio);

    const cssFontSize = terminalFontPointsToCssPixels(normalized.fontSize);
    if (terminal.options.fontSize !== cssFontSize) {
        terminal.options.fontSize = cssFontSize;
    }

    if (canFit) {
        fit.fit();
    }
    bridge.resize(terminal.cols, terminal.rows);
    return normalized;
}
