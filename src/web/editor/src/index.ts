// Monaco editor bridged to the remote file service over Qt WebChannel
// (SPEC 8.1). The editor never touches the client filesystem; reads/writes go
// through RemoteEditorBridge -> codeharbord. Saves carry the revision token
// loaded with the file (SPEC 8.4).
//
// Bootstrap placeholder: Monaco setup and the bridge contract land in
// workstream E. This file establishes the module contract only.

export interface RemoteEditorBridge {
    /** Persist buffer content with the revision originally loaded (SPEC 8.4). */
    save(content: string, expectedRevision: string): Promise<{ revision: string }>;
}

export interface EditorHost {
    setValue(content: string, revision: string): void;
    getValue(): string;
}

export function mountEditor(_element: HTMLElement, _bridge: RemoteEditorBridge): EditorHost {
    throw new Error("not implemented: workstream E");
}
