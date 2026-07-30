// Monaco editor bridged to the remote file service over Qt WebChannel
// (SPEC 8.1). The editor never touches the client filesystem; reads/writes go
// through the host EditorController -> codeharbord. Saves carry the revision
// token loaded with the file (SPEC 8.4).
//
// CONTRACT (C3, FROZEN — consumed by both src/web/editor [this file, JS half]
// and src/editor/EditorController.{h,cpp} [C++ half]). It is WebChannel-native:
// QWebChannel cannot return a value synchronously from C++ to JS, so there is NO
// synchronous getValue()/save(): Promise. Instead the single C++ EditorController
// is exposed to this page under the QWebChannel object name "editor"; its SIGNALS
// push state to the editor (host callbacks) and its SLOTS receive editor actions
// (the bridge). Save results arrive as signals, never as a return value.

// The typed standalone API surface (monaco-editor/esm/vs/editor/editor.api) is
// the only entry that ships .d.ts. The two side-effect imports below add the
// runtime that entry deliberately omits:
//   * edcore.main            — every standalone editor contribution (find,
//                              suggest, context menu, quick access, ...).
//   * basic-languages        — Monarch tokenizers for ~80 languages. These run
//                              on the MAIN THREAD.
// The heavy monaco-editor root entry (editor.main) is deliberately NOT used: it
// also pulls the css/html/json/typescript LANGUAGE SERVICES, which are Web
// Worker based. This page is served from a local (qrc) origin where Chromium
// refuses to construct workers, so those services would never work; excluding
// them halves the bundle instead of shipping dead weight.
import * as monaco from "monaco-editor/esm/vs/editor/editor.api";
import "monaco-editor/esm/vs/editor/edcore.main";
import "monaco-editor/esm/vs/basic-languages/monaco.contribution";

// Monaco's editor worker (diff, link detection, word-based suggestions,
// unicode highlighting). It is bundled to a sibling script and spawned
// SAME-ORIGIN: a blob: worker is refused by this page's CSP, and there is no
// remote origin to load from. Without this Monaco silently degrades to running
// the worker's code on the UI thread, which stalls the editor on large files.
// A construction failure still lands in that fallback, so this is an
// optimisation, never a hard dependency.
// monaco-editor declares `Window.MonacoEnvironment` globally, so no cast.
window.MonacoEnvironment = {
    getWorker: () => new Worker(new URL("editor.worker.js", document.baseURI)),
};

/** Minimal QWebChannel signal shape (obj.signalName.connect(handler)). */
export interface Signal<F extends (...args: never[]) => void> {
    connect(handler: F): void;
    disconnect(handler: F): void;
}

export interface EditorBridge {
    // ---- signals: C++ -> JS ----
    /** New buffer content + the revision token it was loaded at (SPEC 8.4).
     *  Also used for host-driven reloads after an external change (SPEC 8.7). */
    readonly contentLoaded: Signal<(content: string, revision: string) => void>;
    /** ch::FileState string from SessionState.h (SPEC 8.2). */
    readonly fileStateChanged: Signal<(state: string) => void>;
    /** Toggle editor read-only mode (SPEC 8.2). */
    readonly readOnlyChanged: Signal<(readOnly: boolean) => void>;
    /** A save succeeded; carries the new revision token to adopt (SPEC 8.4). */
    readonly saved: Signal<(revision: string) => void>;
    /** A save was refused because the file changed since load (SPEC 8.6);
     *  carries the file's current revision so the UI can offer reload/overwrite. */
    readonly saveConflict: Signal<(currentRevision: string) => void>;
    /** A save failed for a non-conflict reason. */
    readonly saveError: Signal<(message: string) => void>;

    // ---- slots: JS -> C++ (fire-and-forget; results come back via signals) ----
    /** Persist the buffer guarded by the revision originally loaded
     *  (SPEC 8.4/8.6). Result arrives via saved/saveConflict/saveError. */
    save(content: string, expectedRevision: string): void;
    /** Push the current buffer so the host can snapshot it for crash recovery
     *  (SPEC 11.3). Call debounced on edits; never blocks. */
    reportContent(content: string): void;
    /** Ask the host to re-fetch the file from the server (SPEC 8.7). */
    requestReload(): void;

    // ---- ADDITIVE (backwards compatible), NOT part of the frozen shapes above.
    /** Tell the host this page is live and every signal handler above is
     *  attached. The host buffers a load that completed before the WebChannel
     *  page connected and replays it here, so the first buffer is never lost.
     *  OPTIONAL so this bundle still runs against an older C++ host that does
     *  not expose the slot (the proxy simply has no `ready` property). */
    ready?(): void;
    /** The host's `fileState` Q_PROPERTY, cached by qwebchannel.js when the
     *  channel opens (SPEC 8.2). fileStateChanged only fires on a TRANSITION,
     *  and the transitions for a file whose load finished before this page
     *  connected have already happened, so without reading the cached value the
     *  status label stays blank until the file next changes state. OPTIONAL
     *  because a host that does not publish the property leaves it undefined. */
    readonly fileState?: string;
}

/** Extra, host-supplied context that is NOT carried by the frozen bridge. */
export interface MountOptions {
    /** Remote path of the file this pane shows, used only to pick a Monaco
     *  language for syntax highlighting. Never opened, never read. */
    path?: string;
}

/**
 * Resolve a Monaco language id from a remote path by matching the registered
 * language contributions. An exact FILENAME match wins over an extension match
 * everywhere, hence two passes: a filename registration ("Dockerfile",
 * "Gemfile", ".gitconfig") is the more specific statement, and a single pass
 * would let whichever language happens to be registered first claim the file on
 * its extension alone. Falls back to "plaintext" — the editor must render even
 * for an unknown file type.
 */
export function languageForPath(path: string): string {
    const slash = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
    const name = path.slice(slash + 1);
    // A leading dot is part of the NAME, not an extension (".gitconfig"), so
    // only a dot after the first character starts one.
    const dot = name.lastIndexOf(".");
    const ext = dot > 0 ? name.slice(dot).toLowerCase() : "";
    const languages = monaco.languages.getLanguages();
    for (const lang of languages) {
        if (lang.filenames?.some((f) => f === name)) {
            return lang.id;
        }
    }
    if (ext) {
        for (const lang of languages) {
            if (lang.extensions?.some((e) => e.toLowerCase() === ext)) {
                return lang.id;
            }
        }
    }
    return "plaintext";
}

/** Handle the page bootstrap keeps so it can tear the editor down when the pane
 *  goes away. Mirrors TerminalHost in src/web/terminal/src/index.ts. */
export interface EditorHost {
    /** Flush a pending crash-recovery snapshot (SPEC 11.3) and release Monaco. */
    dispose(): void;
}

/**
 * Mount a Monaco editor into `element` wired to `bridge` (the QWebChannel proxy
 * for the C++ EditorController, i.e. channel.objects.editor). Implemented by
 * workstream E-web against the frozen contract above; the C++ half implements
 * the matching signals/slots under the object name "editor".
 */
export function mountEditor(
    element: HTMLElement,
    bridge: EditorBridge,
    options: MountOptions = {},
): EditorHost {
    // ---- DOM scaffold: a thin status bar above the editor surface. ----
    // Appearance and layout live in the page's stylesheet next to the rest of
    // the page's rules (see ../index.html); only STATE the script owns —
    // whether the notice is showing — is set as an inline style here.
    const doc = element.ownerDocument;

    const statusEl = doc.createElement("div");
    statusEl.className = "ch-editor-status";

    const stateLabel = doc.createElement("span");
    stateLabel.className = "ch-editor-state";
    statusEl.appendChild(stateLabel);

    // Conflict/error affordance, hidden until a save fails.
    const notice = doc.createElement("span");
    notice.className = "ch-editor-notice";
    notice.style.display = "none";
    statusEl.appendChild(notice);

    const editorEl = doc.createElement("div");
    editorEl.className = "ch-editor-surface";

    element.appendChild(statusEl);
    element.appendChild(editorEl);

    // Monaco does NOT use browser scrollbars for the editor: it draws its own,
    // sized by these options and coloured by the theme's scrollbarSlider colours
    // (defined in "codeharbor-dark" below). The page's stylesheet
    // (../index.html) covers only the scrollbars Chromium draws, so both halves
    // are needed for a pane with no foreign-looking scrollbar left in it.
    monaco.editor.defineTheme("codeharbor-dark", {
        // Inherit vs-dark's ~hundreds of syntax rules and widget colours; only
        // the surfaces and the scrollbar are restated, so the editor stops being
        // a lighter grey rectangle inside a Catppuccin window.
        base: "vs-dark",
        inherit: true,
        rules: [],
        // COLOUR MIRROR: these are copies of roles in src/qml/Theme.qml, which
        // cannot be imported into a web page. Keep them in step by hand, together
        // with the stylesheet in ../index.html.
        colors: {
            "editor.background": "#11111b",                  // surfaceSunken
            "editorGutter.background": "#11111b",             // surfaceSunken
            "editorLineNumber.foreground": "#45475a",         // textFaint
            "editorLineNumber.activeForeground": "#cdd6f4",   // text
            "minimap.background": "#11111b",                  // surfaceSunken
            "scrollbarSlider.background": "#45475a",          // border
            "scrollbarSlider.hoverBackground": "#6c7086",     // textDim
            "scrollbarSlider.activeBackground": "#89b4fa",    // accent
            "minimapSlider.background": "#313244",            // borderSubtle
            "minimapSlider.hoverBackground": "#45475a",       // border
            "minimapSlider.activeBackground": "#6c7086",      // textDim
        },
    });

    const editor = monaco.editor.create(editorEl, {
        value: "",
        // The host drives content; the language comes from the pane's remote
        // path (highlighting only). No client-side file access is implied.
        language: options.path ? languageForPath(options.path) : "plaintext",
        theme: "codeharbor-dark",
        readOnly: false,
        automaticLayout: true,
        // SPEC 8.1 lists the minimap among the things the embedded editor is
        // expected to provide, alongside find/replace, multiple cursors,
        // folding and bracket matching (all Monaco defaults).
        minimap: { enabled: true },
        scrollBeyondLastLine: false,
        scrollbar: {
            // Theme.scrollBarThickness, and no arrow buttons — the same shape as
            // the application's own scrollbars.
            verticalScrollbarSize: 10,
            horizontalScrollbarSize: 10,
            verticalHasArrows: false,
            horizontalHasArrows: false,
            // The drop shadow Monaco draws where content scrolls under the
            // chrome belongs to its own visual language, not this one.
            useShadows: false,
        },
    });

    // The revision the current buffer was loaded (or last saved) at; every save
    // is guarded by it (SPEC 8.4/8.6). Empty until the first contentLoaded.
    let loadedRevision = "";
    // Set while we push host-driven content into the model so the resulting
    // model-change event does NOT count as a user edit (no dirty, no report).
    let applyingHostEdit = false;
    // Whether the buffer diverges from loadedRevision (unsaved user edits).
    let dirty = false;
    // Monotonic count of USER edits to the model. Its only job is to answer
    // "did the buffer change since the bytes of the in-flight save were taken?",
    // which decides whether a reported success leaves the buffer clean.
    let editSerial = 0;
    // Value of editSerial when the bytes currently believed to be on the server
    // were taken: set by every save this page issues and by every host-driven
    // load, both of which re-baseline the buffer against the file.
    let baselineEditSerial = 0;
    // Mirror of the host readOnly toggle (SPEC 8.2). A read-only buffer must
    // never issue a save, even via the Ctrl/Cmd+S command binding.
    let readOnly = false;

    function clearNotice(): void {
        notice.style.display = "none";
        notice.replaceChildren();
    }

    function renderState(): void {
        // fileStateChanged provides the authoritative label; append a dirty mark.
        const base = stateLabel.dataset.state || "";
        stateLabel.textContent = dirty && base ? `${base} \u2022` : base;
    }

    // ---- crash-recovery snapshots and saves (both send the buffer to C++) ----
    // Debounced snapshot of the unsaved buffer (SPEC 11.3): the host writes it
    // to a server-side recovery path and marks the file dirty.
    let reportTimer: number | undefined;

    function cancelReport(): void {
        clearTimeout(reportTimer);
        reportTimer = undefined;
    }

    function scheduleReport(): void {
        cancelReport();
        if (!dirty) {
            // Nothing to recover. Snapshotting a clean buffer would flag the
            // file dirty on the host for no reason, and a dirty flag suppresses
            // the automatic reload of a clean buffer (SPEC 8.7).
            return;
        }
        reportTimer = setTimeout(() => {
            reportTimer = undefined;
            bridge.reportContent(editor.getValue());
        }, 500);
    }

    function flushReport(): void {
        if (reportTimer === undefined) {
            return; // nothing pending
        }
        cancelReport();
        bridge.reportContent(editor.getValue());
    }

    /**
     * Persist the buffer guarded by `revision` (SPEC 8.4/8.6). Cancelling the
     * pending snapshot is part of saving, not an optimisation: reportContent()
     * marks the file dirty on the host and rewrites the recovery snapshot, so a
     * timer allowed to fire AFTER the save succeeded would resurrect a stale
     * "unsaved changes" copy of an already-saved file (the host discards the
     * snapshot on a successful save) and would leave the buffer flagged dirty,
     * which suppresses the automatic reload of a clean buffer on an external
     * change (SPEC 8.7). A save that FAILS re-arms it: those edits really are
     * still unsaved.
     */
    function requestSave(revision: string): void {
        cancelReport();
        // The bytes handed over are the buffer as it is NOW; edits after this
        // point are not in them (see the saved handler).
        baselineEditSerial = editSerial;
        bridge.save(editor.getValue(), revision);
    }

    // ---- signals: C++ -> JS ----
    bridge.contentLoaded.connect((content: string, revision: string) => {
        loadedRevision = revision;
        // The buffer now IS the file, so a save reported later must not be
        // second-guessed by edits this load already superseded.
        baselineEditSerial = editSerial;
        clearNotice();
        // A snapshot armed by edits this load supersedes must not be sent: it
        // would re-flag the freshly loaded buffer as dirty.
        cancelReport();
        const model = editor.getModel();
        if (model && model.getValue() === content) {
            // Identical buffer (e.g. reload of unchanged file): just re-baseline.
            dirty = false;
            renderState();
            return;
        }
        applyingHostEdit = true;
        try {
            // setValue resets the buffer and its undo stack to the loaded content.
            editor.setValue(content);
        } finally {
            applyingHostEdit = false;
        }
        dirty = false;
        renderState();
    });

    bridge.fileStateChanged.connect((state: string) => {
        stateLabel.dataset.state = state;
        renderState();
    });

    bridge.readOnlyChanged.connect((ro: boolean) => {
        readOnly = ro;
        editor.updateOptions({ readOnly: ro });
    });

    bridge.saved.connect((revision: string) => {
        loadedRevision = revision;
        // Anything typed while the write was in flight is NOT in the bytes that
        // just landed, so the buffer still diverges from the file and MUST stay
        // marked as having unsaved changes.
        //
        // ch::EditorController::save() applies exactly this rule to its own
        // dirty flag and FileState (src/editor/EditorController.cpp), but it can
        // only notice edits it was TOLD about: its edit counter advances on the
        // reportContent() slot, which this page debounces by 500 ms. So if a
        // snapshot is still pending here, the host saw no edit, cleared its
        // dirty flag and published the "clean" file state. Flushing the pending
        // snapshot now corrects the host in the same turn, and it is also the
        // only way those edits reach the crash-recovery snapshot at all
        // (SPEC 11.3): a successful save discards the previous one. When the
        // debounced report already went out during the write, nothing is pending
        // and the host is dirty already, so flushReport() is a no-op.
        const editedDuringSave = editSerial !== baselineEditSerial;
        dirty = editedDuringSave;
        clearNotice();
        renderState();
        if (editedDuringSave) {
            flushReport();
        }
    });

    bridge.saveConflict.connect((currentRevision: string) => {
        // The file moved on the server since we loaded it (SPEC 8.6). Offer the
        // user a choice: reload (discard local edits) or overwrite (force save
        // against the server's current revision).
        clearNotice();
        // The buffer is still unsaved, so keep the recovery snapshot current
        // while the notice waits for the user (requestSave cancelled it).
        scheduleReport();
        const msg = doc.createElement("span");
        msg.textContent = "File changed on disk.";
        const reload = doc.createElement("button");
        reload.type = "button";
        reload.textContent = "Reload";
        reload.addEventListener("click", () => {
            clearNotice();
            bridge.requestReload();
        });
        const overwrite = doc.createElement("button");
        overwrite.type = "button";
        overwrite.textContent = "Overwrite";
        overwrite.addEventListener("click", () => {
            // Re-issue the save guarded by the server's now-current revision so
            // it is accepted; adopt it locally so a subsequent saved lines up.
            loadedRevision = currentRevision;
            clearNotice();
            requestSave(currentRevision);
        });
        notice.replaceChildren(msg, reload, overwrite);
        notice.style.display = "flex";
    });

    bridge.saveError.connect((message: string) => {
        clearNotice();
        scheduleReport(); // as above: the buffer is still unsaved
        const msg = doc.createElement("span");
        msg.textContent = `Save failed: ${message}`;
        const retry = doc.createElement("button");
        retry.type = "button";
        retry.textContent = "Retry";
        retry.addEventListener("click", () => {
            clearNotice();
            requestSave(loadedRevision);
        });
        notice.replaceChildren(msg, retry);
        notice.style.display = "flex";
    });

    // ---- slots: JS -> C++ ----
    // Ctrl/Cmd+S persists the buffer guarded by the loaded revision (SPEC 8.4).
    editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS, () => {
        if (readOnly) {
            return;
        }
        requestSave(loadedRevision);
    });

    // Arm the snapshot on USER edits only; host-driven setValue is excluded via
    // applyingHostEdit.
    editor.onDidChangeModelContent(() => {
        if (applyingHostEdit) {
            return;
        }
        ++editSerial;
        if (!dirty) {
            dirty = true;
            renderState();
        }
        scheduleReport();
    });

    // A host that disposes the editor directly (not through the returned host)
    // must not leave a timer that would call getValue() on a dead editor.
    editor.onDidDispose(cancelReport);

    // The file state reached before this page finished loading is already in the
    // property cache, so the label is correct without waiting for a transition
    // (the same trick the terminal page uses for connectionState).
    if (typeof bridge.fileState === "string") {
        stateLabel.dataset.state = bridge.fileState;
        renderState();
    }

    // READY HANDSHAKE — MUST be the last thing mountEditor does. Every signal
    // handler above is now attached, so the host may safely replay a load that
    // completed before this page connected (otherwise the first contentLoaded
    // is emitted into the void and the pane stays empty). Optional-called so an
    // older host without the slot is a no-op rather than a TypeError.
    bridge.ready?.();

    return {
        dispose(): void {
            // A debounced snapshot still pending is the newest copy of the
            // unsaved buffer in existence. The pane is going away (closed, or
            // reloaded because the host retargeted it at another file), so send
            // it now instead of dropping it on the floor (SPEC 11.3).
            flushReport();
            // Disposing a standalone editor also disposes the model it created
            // from `value`, so nothing is left behind.
            editor.dispose();
        },
    };
}

// ---- QWebChannel page-entry bootstrap ----
// The QML host (QWebEngineView) injects `qt.webChannelTransport` and serves
// qwebchannel.js, and registers the C++ EditorController under the object name
// "editor". connectEditor() opens the channel and mounts Monaco against it.

/** Minimal ambient shape of the qwebchannel.js runtime injected by the host. */
interface QWebChannelObjects {
    [name: string]: unknown;
}
interface QWebChannelInstance {
    objects: QWebChannelObjects;
}
type QWebChannelCtor = new (
    transport: unknown,
    callback: (channel: QWebChannelInstance) => void,
) => QWebChannelInstance;

// Both are injected by the host, so a page opened OUTSIDE Qt WebEngine (or one
// whose qwebchannel.js failed to load) has neither. Typed as possibly-undefined
// so the guards in connectEditor() are the honest shape rather than a cast:
// only `typeof` may touch a global that was never declared at all.
declare const QWebChannel: QWebChannelCtor | undefined;
declare const qt: { webChannelTransport: unknown } | undefined;

/**
 * Replace the pane with a single explanatory line. Used only for the failures
 * that happen BEFORE an editor exists, which would otherwise leave a blank grey
 * rectangle and no clue (the QML pane can only report a page that failed to
 * LOAD, not one that loaded and found no bridge). textContent, never innerHTML:
 * this is a privileged page and never builds markup from a string.
 */
function showFatal(element: HTMLElement, message: string): void {
    const status = element.ownerDocument.createElement("div");
    status.className = "ch-editor-status";
    const label = element.ownerDocument.createElement("span");
    label.className = "ch-editor-state";
    label.textContent = message;
    status.appendChild(label);
    element.replaceChildren(status);
}

/**
 * Page entry point: open the WebChannel injected by the QML host and mount the
 * editor against the "editor" object (the C++ EditorController proxy).
 */
export function connectEditor(element: HTMLElement, options: MountOptions = {}): void {
    if (typeof QWebChannel === "undefined" || typeof qt === "undefined"
        || !qt.webChannelTransport) {
        showFatal(element, "This page requires the CodeHarbor host: no WebChannel transport.");
        return;
    }
    new QWebChannel(qt.webChannelTransport, (channel: QWebChannelInstance) => {
        const bridge = channel.objects.editor as EditorBridge | undefined;
        if (!bridge) {
            showFatal(element, "The editor bridge is missing from this window.");
            return;
        }
        const host = mountEditor(element, bridge, options);
        // The pane is destroyed, or navigated to another file (the QML host
        // reloads this page to change the language hint). Either way the pending
        // crash-recovery snapshot has to reach the host before the page dies,
        // and Monaco has to be released (SPEC 11.3).
        window.addEventListener("pagehide", () => host.dispose(), { once: true });
    });
}

// The packaged page (dist/index.html) carries no inline script — its CSP
// forbids one — so the bundle boots itself. The pane's remote path arrives as
// the `path` query parameter on the bundle URL (see src/qml/EditorPaneView.qml);
// it is used ONLY to choose a syntax-highlighting language.
function bootstrap(): void {
    const root = document.getElementById("ch-editor-root");
    if (!root) {
        return; // embedded by a host that mounts explicitly
    }
    const path = new URLSearchParams(window.location.search).get("path");
    connectEditor(root, path ? { path } : {});
}

if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bootstrap, { once: true });
} else {
    bootstrap();
}
