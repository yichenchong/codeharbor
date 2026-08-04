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

// Pure, DOM-free logic split out so it can be unit-tested without loading the
// Monaco runtime this module imports above (see buffer.ts / language.ts /
// recovery.ts).
import { bufferText } from "./buffer";
import { selectLanguage } from "./language";
import { RecoveryReporter } from "./recovery";

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
    /** The host's `readOnly` Q_PROPERTY, cached alongside `fileState`. Same
     *  one-shot problem as above, with a worse failure: a page that connects
     *  after the host has already decided the file is unwritable would render
     *  an editable surface over it. OPTIONAL for the same reason. */
    readonly readOnly?: boolean;
}

/** Extra, host-supplied context that is NOT carried by the frozen bridge. */
export interface MountOptions {
    /** Remote path of the file this pane shows, used only to pick a Monaco
     *  language for syntax highlighting. Never opened, never read. */
    path?: string;
}

// The host pushes Theme.roles after the page loads. Keeping a complete,
// validated default here is important for the short interval before that first
// push and for standalone bundle tests that do not have a QML host at all.
export type ThemeRoleName =
    | "surface"
    | "surfaceDeep"
    | "surfaceSunken"
    | "surfaceRaised"
    | "surfaceHover"
    | "surfaceSelected"
    | "border"
    | "borderSubtle"
    | "text"
    | "textDim"
    | "textFaint"
    | "textOnAccent"
    | "accent"
    | "success"
    | "warning"
    | "danger"
    | "busy";

export type EditorThemeRoles = Partial<Record<ThemeRoleName, string>>;

const defaultThemeRoles: Record<ThemeRoleName, string> = {
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
};

const themeRoleNames = Object.keys(defaultThemeRoles) as ThemeRoleName[];
let activeThemeRoles: Record<ThemeRoleName, string> = { ...defaultThemeRoles };
let mountedEditor: monaco.editor.IStandaloneCodeEditor | null = null;

function isThemeColor(value: unknown): value is string {
    return typeof value === "string" && /^#[0-9a-fA-F]{6}$/.test(value);
}

function normaliseThemeRoles(input: unknown): Record<ThemeRoleName, string> {
    const candidate = input && typeof input === "object"
        ? input as Record<string, unknown>
        : {};
    const next = { ...defaultThemeRoles };
    for (const role of themeRoleNames) {
        if (isThemeColor(candidate[role]))
            next[role] = candidate[role];
    }
    return next;
}

function cssVariableName(role: ThemeRoleName): string {
    return `--ch-${role.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`)}`;
}

function defineMonacoTheme(roles: Record<ThemeRoleName, string>): void {
    monaco.editor.defineTheme("codeharbor-dark", {
        base: "vs-dark",
        inherit: true,
        rules: [],
        colors: {
            "editor.background": roles.surfaceSunken,
            "editorGutter.background": roles.surfaceSunken,
            "editorLineNumber.foreground": roles.textFaint,
            "editorLineNumber.activeForeground": roles.text,
            "minimap.background": roles.surfaceSunken,
            "scrollbarSlider.background": roles.border,
            "scrollbarSlider.hoverBackground": roles.textDim,
            "scrollbarSlider.activeBackground": roles.accent,
            "minimapSlider.background": roles.borderSubtle,
            "minimapSlider.hoverBackground": roles.border,
            "minimapSlider.activeBackground": roles.textDim,
        },
    });
    if (mountedEditor)
        monaco.editor.setTheme("codeharbor-dark");
}

declare global {
    interface Window {
        applyTheme?: (roles: unknown) => void;
    }
}

/**
 * Apply the host's Theme.roles object to both the page CSS and Monaco. The
 * input is intentionally validated role-by-role: a malformed handoff must
 * preserve a readable editor instead of turning one bad value into a blank
 * surface or an invalid CSS declaration.
 */
export function applyTheme(roles: unknown): void {
    activeThemeRoles = normaliseThemeRoles(roles);
    const root = document.documentElement;
    for (const role of themeRoleNames)
        root.style.setProperty(cssVariableName(role), activeThemeRoles[role]);
    defineMonacoTheme(activeThemeRoles);
}

window.applyTheme = applyTheme;
applyTheme(defaultThemeRoles);

/**
 * Resolve a Monaco language id for the pane's remote path from the live
 * registration list. The precedence rule (exact filename beats extension) and
 * the fallback live in selectLanguage (language.ts), which is DOM-free and
 * unit-tested; this wrapper only supplies monaco.languages.getLanguages().
 */
export function languageForPath(path: string): string {
    return selectLanguage(path, monaco.languages.getLanguages());
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
    // sized by these options and coloured by the active theme's
    // scrollbarSlider roles. The page's stylesheet (../index.html) covers only
    // the scrollbars Chromium draws, so both halves are needed for a pane with
    // no foreign-looking scrollbar left in it.
    defineMonacoTheme(activeThemeRoles);

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
    mountedEditor = editor;

    // The revision the current buffer was loaded (or last saved) at; every save
    // is guarded by it (SPEC 8.4/8.6). Empty until the first contentLoaded.
    let loadedRevision = "";
    // The exact bytes represented by loadedRevision. Dirty state is based on
    // content divergence, not on whether an edit was later undone.
    let baselineContent = "";
    // Set while we push host-driven content into the model so the resulting
    // model-change event does NOT count as a user edit (no dirty, no report).
    let applyingHostEdit = false;
    // Whether the buffer diverges from baselineContent (unsaved user edits).
    let dirty = false;
    // Monotonic count of USER edits to the model. Its only job is to answer
    // whether the host may have seen an edit while the save was in flight.
    let editSerial = 0;
    // Value of editSerial when the bytes currently believed to be on the server
    // were taken: set by every save this page issues and by every host-driven
    // load.
    let baselineEditSerial = 0;
    // Bytes handed to the host by the current save. A successful reply may
    // arrive after more edits, so it must re-baseline against these bytes rather
    // than whatever the model contains at reply time.
    let pendingSaveContent: string | undefined;
    // Mirror of the host readOnly toggle (SPEC 8.2). A read-only buffer must
    // never issue a save, even via the Ctrl/Cmd+S command binding.
    let readOnly = false;
    // The host state also gates saves during a load. Until contentLoaded lands,
    // Monaco still contains the previous file's bytes; sending them with the
    // new path/revision would either be refused or race the load.
    let currentFileState = "";

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
    // to a server-side recovery path and marks the file dirty. The debounce,
    // the flush, and the save-cancels-the-snapshot rule (SPEC 8.7, WB4) live in
    // RecoveryReporter (recovery.ts) so they can be unit-tested with a fake
    // bridge and fake timer.
    // bufferText, never editor.getValue(): the bytes handed to the host are the
    // bytes that get written to the remote file, so the file's byte-order mark
    // and line endings must survive the trip (see buffer.ts).
    const reporter = new RecoveryReporter(bridge, () => bufferText(editor));

    /**
     * Persist the buffer guarded by `revision` (SPEC 8.4/8.6). reporter.save()
     * cancels the pending snapshot before sending, so a timer allowed to fire
     * AFTER the save succeeded cannot resurrect a stale "unsaved changes" copy
     * of an already-saved file. A save that FAILS re-arms it via schedule():
     * those edits really are still unsaved.
     */
    function requestSave(revision: string): void {
        if (readOnly || currentFileState === "loading") {
            return;
        }
        // Capture the exact bytes being handed to C++ so a successful reply can
        // re-baseline even if the model changes before the round trip returns.
        pendingSaveContent = bufferText(editor);
        baselineEditSerial = editSerial;
        reporter.save(revision);
    }

    // ---- signals: C++ -> JS ----
    // Every handler is attached through bind() so dispose() can take it back
    // off the proxy. qwebchannel.js holds each connected handler on the proxy
    // object for the lifetime of the channel, and the proxy outlives this
    // editor: the pane is torn down (or retargeted at another file) while the
    // channel stays open. A handler left attached then runs against a DISPOSED
    // Monaco editor — editor.setValue() on a disposed instance throws, and the
    // exception surfaces inside the WebChannel message dispatch rather than
    // anywhere a user could act on.
    const disconnects: Array<() => void> = [];
    function bind<F extends (...args: never[]) => void>(signal: Signal<F>, handler: F): void {
        signal.connect(handler);
        disconnects.push(() => signal.disconnect(handler));
    }

    bind(bridge.contentLoaded, (content: string, revision: string) => {
        loadedRevision = revision;
        baselineContent = content;
        pendingSaveContent = undefined;
        // The buffer now IS the file, so a save reported later must not be
        // second-guessed by edits this load already superseded.
        baselineEditSerial = editSerial;
        clearNotice();
        // A snapshot armed by edits this load supersedes must not be sent: it
        // would re-flag the freshly loaded buffer as dirty.
        reporter.cancel();
        const model = editor.getModel();
        if (model && bufferText(editor) === content) {
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

    bind(bridge.fileStateChanged, (state: string) => {
        currentFileState = state;
        // An open/reload can finish before the old page's debounce callback is
        // delivered to C++. Cancel it at the transition, not only when
        // contentLoaded arrives, so old-file bytes cannot dirty the new file.
        if (state === "loading") {
            reporter.cancel();
        }
        stateLabel.dataset.state = state;
        renderState();
    });

    bind(bridge.readOnlyChanged, (ro: boolean) => {
        readOnly = ro;
        editor.updateOptions({ readOnly: ro });
    });

    bind(bridge.saved, (revision: string) => {
        loadedRevision = revision;
        const savedContent = pendingSaveContent ?? bufferText(editor);
        pendingSaveContent = undefined;
        baselineContent = savedContent;
        // Compare bytes, not edit count: an edit that is undone before this
        // reply arrives leaves no unsaved divergence.
        const editedDuringSave = bufferText(editor) !== savedContent;
        dirty = editedDuringSave;
        clearNotice();
        renderState();
        if (editedDuringSave) {
            // The current bytes are not the bytes that just landed. Flush a
            // pending snapshot so the host keeps the edits that remain local.
            reporter.flush();
        } else {
            // If a report for edits that were later undone already reached the
            // host, tell it that the recovery bytes now match the saved file.
            reporter.cancel();
            if (editSerial !== baselineEditSerial)
                bridge.reportContent(savedContent);
        }
    });

    bind(bridge.saveConflict, (currentRevision: string) => {
        // The file moved on the server since we loaded it (SPEC 8.6). Offer the
        // user a choice: reload (discard local edits) or overwrite (force save
        // against the server's current revision).
        clearNotice();
        // The buffer is still unsaved, so keep the recovery snapshot current
        // while the notice waits for the user (requestSave cancelled it).
        reporter.schedule(dirty);
        const msg = doc.createElement("span");
        msg.textContent = "File changed on disk.";
        const reload = doc.createElement("button");
        reload.type = "button";
        reload.textContent = "Reload";
        reload.addEventListener("click", () => {
            clearNotice();
            // Discarding the local edits is the whole point of this button, so
            // the snapshot armed above must not outlive the click: a timer that
            // fires inside the reload round trip reports the buffer the user
            // just threw away, which re-flags the file dirty on the host and
            // rewrites the crash-recovery snapshot the reload is about to
            // retire (SPEC 11.3). requestSave() cancels for the same reason;
            // this path bypasses it.
            reporter.cancel();
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

    bind(bridge.saveError, (message: string) => {
        clearNotice();
        reporter.schedule(dirty); // as above: the buffer is still unsaved
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
        dirty = bufferText(editor) !== baselineContent;
        renderState();
        reporter.schedule(dirty);
    });

    // Cancel only; the pending snapshot is NOT flushed here. It is flushed in
    // EditorHost.dispose() (flushReport() runs before editor.dispose()). By the
    // time Monaco fires onDidDispose the editor is already being torn down, so
    // a flush here would call getValue() on a dead model. This handler exists so
    // that a host disposing the editor directly (not through the returned host)
    // at least does not leave a timer that would fire on that dead editor.
    editor.onDidDispose(() => {
        reporter.cancel();
        if (mountedEditor === editor)
            mountedEditor = null;
    });

    // The file state and read-only flag reached before this page finished
    // loading are already in the property cache, so the label and the editor's
    // writability are correct without waiting for a transition (the same trick
    // the terminal page uses for connectionState). readOnly matters more than
    // the label: without it a file the host already determined to be unwritable
    // opens as an editable surface, and the user only discovers the truth when
    // a save is refused.
    if (typeof bridge.fileState === "string") {
        currentFileState = bridge.fileState;
        stateLabel.dataset.state = bridge.fileState;
        renderState();
    }
    if (typeof bridge.readOnly === "boolean") {
        readOnly = bridge.readOnly;
        editor.updateOptions({ readOnly });
    }

    // READY HANDSHAKE — MUST be the last thing mountEditor does. Every signal
    // handler above is now attached, so the host may safely replay a load that
    // completed before this page connected (otherwise the first contentLoaded
    // is emitted into the void and the pane stays empty). Optional-called so an
    // older host without the slot is a no-op rather than a TypeError.
    bridge.ready?.();

    let disposed = false;
    return {
        dispose(): void {
            // Reachable twice: a host that tears the pane down explicitly still
            // has the "pagehide" handler registered in connectEditor() below.
            if (disposed) {
                return;
            }
            disposed = true;
            // Detach the WebChannel handlers BEFORE Monaco goes away, so a
            // signal already on its way from C++ cannot run against a disposed
            // editor.
            for (const disconnect of disconnects) {
                disconnect();
            }
            // A debounced snapshot still pending is the newest copy of the
            // unsaved buffer in existence. The pane is going away (closed, or
            // reloaded because the host retargeted it at another file), so send
            // it now instead of dropping it on the floor (SPEC 11.3).
            reporter.flush();
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
