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
 * How long one resolveImage() round trip may stay unanswered before it is
 * treated as "no URL". The WebChannel proxy answers by invoking the callback,
 * so an answer that never arrives means the transport died mid-render — and
 * nothing else in the page would ever notice.
 */
export const IMAGE_REQUEST_TIMEOUT_MS = 30_000;

/**
 * Invoke the QWebChannel proxy. qwebchannel.js consumes the trailing callback
 * argument and invokes it with the QML method's return value; the QML method
 * itself receives only relativePath.
 *
 * The returned promise ALWAYS settles: with the minted URL, or with "" when the
 * bridge refuses, throws, or never answers within `timeoutMs`. The renderer
 * awaits one of these per image, so a bridge that simply goes quiet — a torn
 * down WebChannel transport, a host that neither calls back nor returns a
 * string — would otherwise leave that render pending for the life of the page.
 */
export function requestImageUrl(
    bridge: MarkdownBridge,
    relativePath: string,
    timeoutMs: number = IMAGE_REQUEST_TIMEOUT_MS,
): Promise<string> {
    // Promise.withResolvers() would read better, but this bundle is built for
    // chrome110 (see build.mjs) and typechecked against lib ES2022, neither of
    // which has it. The executor form is the portable spelling here.
    return new Promise((resolve) => {
        let settled = false;
        const settle = (value: unknown): void => {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timer);
            resolve(typeof value === "string" ? value : "");
        };
        // Declared after `settle` so its type is inferred from setTimeout;
        // `settle` only reads it when called, which is never before this line.
        const timer = setTimeout(() => settle(""), timeoutMs);

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
