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

import * as monaco from "monaco-editor";

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
}

/**
 * Mount a Monaco editor into `element` wired to `bridge` (the QWebChannel proxy
 * for the C++ EditorController, i.e. channel.objects.editor). Implemented by
 * workstream E-web against the frozen contract above; the C++ half implements
 * the matching signals/slots under the object name "editor".
 */
export function mountEditor(element: HTMLElement, bridge: EditorBridge): void {
    // ---- DOM scaffold: a thin status bar above the editor surface. ----
    element.style.display = "flex";
    element.style.flexDirection = "column";

    const statusEl = document.createElement("div");
    statusEl.className = "ch-editor-status";
    statusEl.style.flex = "0 0 auto";
    statusEl.style.font = "12px/1.6 system-ui, sans-serif";
    statusEl.style.padding = "2px 8px";
    statusEl.style.display = "flex";
    statusEl.style.alignItems = "center";
    statusEl.style.gap = "8px";

    const stateLabel = document.createElement("span");
    stateLabel.className = "ch-editor-state";
    statusEl.appendChild(stateLabel);

    // Conflict/error affordance, hidden until a save fails.
    const notice = document.createElement("span");
    notice.className = "ch-editor-notice";
    notice.style.display = "none";
    notice.style.marginLeft = "auto";
    notice.style.gap = "6px";
    statusEl.appendChild(notice);

    const editorEl = document.createElement("div");
    editorEl.className = "ch-editor-surface";
    editorEl.style.flex = "1 1 auto";
    editorEl.style.minHeight = "0";

    element.appendChild(statusEl);
    element.appendChild(editorEl);

    const editor = monaco.editor.create(editorEl, {
        value: "",
        // The host drives content; default to plaintext and let callers pick a
        // language later if desired. No client-side file access is implied.
        language: "plaintext",
        readOnly: false,
        automaticLayout: true,
        minimap: { enabled: false },
    });

    // The revision the current buffer was loaded (or last saved) at; every save
    // is guarded by it (SPEC 8.4/8.6). Empty until the first contentLoaded.
    let loadedRevision = "";
    // Set while we push host-driven content into the model so the resulting
    // model-change event does NOT count as a user edit (no dirty, no report).
    let applyingHostEdit = false;
    // Whether the buffer diverges from loadedRevision (unsaved user edits).
    let dirty = false;
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

    // ---- signals: C++ -> JS ----
    bridge.contentLoaded.connect((content: string, revision: string) => {
        loadedRevision = revision;
        clearNotice();
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
        dirty = false;
        clearNotice();
        renderState();
    });

    bridge.saveConflict.connect((currentRevision: string) => {
        // The file moved on the server since we loaded it (SPEC 8.6). Offer the
        // user a choice: reload (discard local edits) or overwrite (force save
        // against the server's current revision).
        clearNotice();
        const msg = document.createElement("span");
        msg.textContent = "File changed on disk.";
        const reload = document.createElement("button");
        reload.type = "button";
        reload.textContent = "Reload";
        reload.addEventListener("click", () => {
            clearNotice();
            bridge.requestReload();
        });
        const overwrite = document.createElement("button");
        overwrite.type = "button";
        overwrite.textContent = "Overwrite";
        overwrite.addEventListener("click", () => {
            // Re-issue the save guarded by the server's now-current revision so
            // it is accepted; adopt it locally so a subsequent saved lines up.
            loadedRevision = currentRevision;
            clearNotice();
            bridge.save(editor.getValue(), currentRevision);
        });
        notice.replaceChildren(msg, reload, overwrite);
        notice.style.display = "flex";
    });

    bridge.saveError.connect((message: string) => {
        clearNotice();
        const msg = document.createElement("span");
        msg.textContent = `Save failed: ${message}`;
        const retry = document.createElement("button");
        retry.type = "button";
        retry.textContent = "Retry";
        retry.addEventListener("click", () => {
            clearNotice();
            bridge.save(editor.getValue(), loadedRevision);
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
        bridge.save(editor.getValue(), loadedRevision);
    });

    // Debounced crash-recovery snapshot on user edits (SPEC 11.3). Host-driven
    // setValue is excluded via applyingHostEdit.
    let reportTimer: number | undefined;
    editor.onDidChangeModelContent(() => {
        if (applyingHostEdit) {
            return;
        }
        if (!dirty) {
            dirty = true;
            renderState();
        }
        clearTimeout(reportTimer);
        reportTimer = setTimeout(() => {
            reportTimer = undefined;
            bridge.reportContent(editor.getValue());
        }, 500);
    });

    // Flush any pending snapshot and release Monaco when the surface goes away.
    editor.onDidDispose(() => {
        clearTimeout(reportTimer);
    });
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

declare const QWebChannel: QWebChannelCtor;
declare const qt: { webChannelTransport: unknown };

/**
 * Page entry point: open the WebChannel injected by the QML host and mount the
 * editor against the "editor" object (the C++ EditorController proxy).
 */
export function connectEditor(element: HTMLElement): void {
    new QWebChannel(qt.webChannelTransport, (channel: QWebChannelInstance) => {
        mountEditor(element, channel.objects.editor as EditorBridge);
    });
}
