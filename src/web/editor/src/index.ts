// Monaco editor bridged to the remote file service over Qt WebChannel
// (SPEC 8.1). The editor never touches the client filesystem; reads/writes go
// through RemoteEditorBridge -> codeharbord. Saves carry the revision token
// loaded with the file (SPEC 8.4).
//
// Bootstrap placeholder: Monaco setup and the bridge contract land in
// workstream E. This file establishes the module contract only.

export interface RemoteEditorBridge {
    /** Persist buffer content guarded by the revision originally loaded;
     *  resolves with the new revision token (SPEC 8.4). */
    save(content: string, expectedRevision: string): Promise<{ revision: string }>;
    /** Ask the host to re-fetch the file after an external change (SPEC 8.7). */
    requestReload(): void;
}

export interface EditorHost {
    /** Called by C++ to load buffer content with its revision token (SPEC 8.4). */
    setValue(content: string, revision: string): void;
    /** Called by C++ to read the current buffer content (SPEC 8.1). */
    getValue(): string;
    /** Called by C++ with the file lifecycle state; the value is a
     *  ch::FileState string from SessionState.h (SPEC 8.2). */
    setFileState(state: string): void;
    /** Called by C++ to toggle the editor read-only mode (SPEC 8.2). */
    setReadOnly(readOnly: boolean): void;
}

export function mountEditor(_element: HTMLElement, _bridge: RemoteEditorBridge): EditorHost {
    throw new Error("not implemented: workstream E");
}
