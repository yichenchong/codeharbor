// Renderer-visibility predicate, split out from the page entry so the
// conjunction rule can be unit-tested without a DOM. See the OWNERSHIP note in
// index.ts: the page is the authoritative reporter of whether a byte written
// now would actually be seen (SPEC 5.4).

/**
 * True only when the pane BOTH intersects the viewport AND the page is not
 * backgrounded. The reported value is the conjunction of the two because either
 * condition alone reports a hidden pane as visible:
 *   * `intersecting` — the element collapsed or scrolled out of view.
 *   * `visibilityState` — Qt WebEngine marks the whole page hidden when its
 *     WebEngineView stops being rendered; the element still intersects then.
 */
export function isRendererVisible(
    intersecting: boolean,
    visibilityState: DocumentVisibilityState,
): boolean {
    return intersecting && visibilityState === "visible";
}
