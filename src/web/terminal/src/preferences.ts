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

/** The media query that answers "has this user asked for less animation?". */
export const kReducedMotionQuery = "(prefers-reduced-motion: reduce)";

/** What followMotionPreference() needs from a MediaQueryList. Narrowed to the
 *  two members it touches so a test can hand it a plain object. */
export interface MotionPreferenceQuery {
    readonly matches: boolean;
    addEventListener(
        type: "change",
        listener: (event: { matches: boolean }) => void,
    ): void;
    removeEventListener(
        type: "change",
        listener: (event: { matches: boolean }) => void,
    ): void;
}

/** The one xterm option followMotionPreference() drives. */
export interface CursorBlinkTarget {
    options: { cursorBlink?: boolean };
}

/**
 * Hold `terminal.options.cursorBlink` to the user's motion preference and keep
 * following it. A blinking cursor is a small animation that never stops for as
 * long as the pane is open, which is exactly what a user who asked the system
 * for reduced motion asked to be rid of; it is also this page's entire motion
 * budget, since nothing else here moves on its own. The preference can be
 * changed while the pane is open, so a pane that read it once and stopped
 * listening would keep blinking for the rest of its life.
 *
 * Returns the unsubscribe the caller runs on teardown. A null query is a host
 * with no matchMedia at all: there is no preference to read, so the cursor
 * keeps xterm's own default.
 */
export function followMotionPreference(
    query: MotionPreferenceQuery | null,
    terminal: CursorBlinkTarget,
): () => void {
    const apply = (prefersReducedMotion: boolean): void => {
        if (terminal.options.cursorBlink !== !prefersReducedMotion) {
            terminal.options.cursorBlink = !prefersReducedMotion;
        }
    };
    if (!query) {
        return () => {};
    }
    apply(query.matches);
    const onChange = (event: { matches: boolean }): void => apply(event.matches);
    query.addEventListener("change", onChange);
    return () => query.removeEventListener("change", onChange);
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
