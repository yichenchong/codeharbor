/** The only bridge capability the page needs for remote subresources. */
export interface MarkdownBridge {
    /**
     * Mint one opaque internal URL for a relative remote image path.
     * QWebChannel consumes the optional trailing callback and supplies the
     * QML method's return value to it. A direct host may return the value.
     */
    resolveImage(
        relativePath: string,
        callback?: (internalUrl: string) => void,
    ): unknown;
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
    return new Promise((resolve) => {
        let settled = false;
        const settle = (value: unknown): void => {
            if (settled) {
                return;
            }
            settled = true;
            resolve(typeof value === "string" ? value : "");
        };

        try {
            // The generated proxy consumes `settle` before forwarding the
            // remaining declared arguments to QML. Supporting a direct string
            // return as well keeps the bridge usable in non-WebChannel hosts
            // without changing the WebChannel callback contract.
            const returned = bridge.resolveImage(relativePath, settle);
            if (typeof returned === "string") {
                settle(returned);
            }
        } catch {
            settle("");
        }
    });
}
