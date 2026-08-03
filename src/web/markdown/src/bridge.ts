/** The only bridge capability the page needs for remote subresources. */
export interface MarkdownBridge {
    /** Mint one opaque internal URL for a relative remote image path. */
    resolveImage(relativePath: string): string;
}

/**
 * Invoke the QWebChannel proxy. qwebchannel.js consumes the trailing callback
 * argument and invokes it with the QML method's return value; the QML method
 * itself receives only relativePath.
 */
export function requestImageUrl(
    bridge: MarkdownBridge,
    relativePath: string,
): Promise<string> {
    // The callback is supplied by qwebchannel.js, so this promise has one
    // settlement path and remains compatible with the Chrome version embedded
    // by Qt 6.10 (Promise.withResolvers is newer than that runtime).
    return new Promise((resolve) => {
        try {
            const invoke = bridge.resolveImage as unknown as (
                path: string,
                callback: (internalUrl: string) => void,
            ) => void;
            invoke(relativePath, (internalUrl) => {
                resolve(typeof internalUrl === "string" ? internalUrl : "");
            });
        } catch {
            resolve("");
        }
    });
}
