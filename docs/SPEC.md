# CodeHarbor — Product and Technical Specification

**Status:** Draft  
**Version:** 0.3  
**Working title:** CodeHarbor  
**Primary platform:** Cross-platform desktop via Qt  
**Architecture:** Remote-first client/server development workspace

---

## 1. Product Summary

CodeHarbor is a cross-platform desktop application for managing multiple persistent remote development workspaces from a single interface.

Each workspace, called a **Dev Session**, normally corresponds to one repository or project, but may instead represent a specific task within a project. A Dev Session contains:

- one or more web or file viewer panes;
- one or more persistent remote terminal panes;
- a fixed workspace layout;
- a remote repository root;
- optional agent-status integration;
- persistent session metadata and UI state.

The application uses a fixed three-region layout:

1. **Sessions sidebar**
2. **Viewer region**
3. **Terminal region**

All development files, commands, processes, repositories, terminals, tmux sessions, and authoritative session state live on a configured SSH server. The client machine provides only the user interface, rendering, input handling, and transport.

---

## 2. Core Design Principles

### 2.1 Remote-first

CodeHarbor must behave as a remote workspace client rather than a local IDE with SSH support added.

The following are always server-side:

- repositories;
- project files;
- working directories;
- shell processes;
- coding agents;
- build and test processes;
- tmux sessions;
- file watching;
- file editing and saving;
- Dev Session definitions;
- pane layouts;
- session metadata;
- unsaved-edit recovery data.

The client must not use the local filesystem for project work.

### 2.2 Persistent sessions

Terminal processes must remain alive when:

- the user switches Dev Sessions;
- a terminal pane is hidden;
- the application disconnects;
- the client application closes;
- the client machine restarts.

Persistence is provided by tmux on the remote SSH server.

### 2.3 Fixed outer layout

The application always contains the same three outer regions:

```text
┌────────────────┬──────────────────────────────┬──────────────────────────┐
│ Sessions       │ Viewers                      │ Terminals                │
│                │                              │                          │
│ Group A        │ ┌──────────────────────────┐ │ ┌──────────────────────┐ │
│   Project 1    │ │ Viewer pane              │ │ │ Terminal pane        │ │
│   Project 2    │ └──────────────────────────┘ │ └──────────────────────┘ │
│                │ ┌──────────────────────────┐ │ ┌──────────────────────┐ │
│ Group B        │ │ Viewer pane              │ │ │ Terminal pane        │ │
│   Task 1       │ └──────────────────────────┘ │ └──────────────────────┘ │
└────────────────┴──────────────────────────────┴──────────────────────────┘
```

Viewer panes never move into the terminal region, and terminal panes never move into the viewer region.

### 2.4 Server-side file semantics

Within CodeHarbor:

```text
file:///home/user/project/README.md
```

always refers to a file on the configured SSH server.

It must never refer to a file on the client machine.

### 2.5 Lightweight extensibility

The initial application does not require a general plugin platform.

The viewer system should instead use a lightweight handler registry so different URL schemes, MIME types, and file extensions can be rendered differently.

---

## 3. Core Concepts

### 3.1 Group

A **Group** is a collapsible container in the sessions sidebar.

Examples:

- Work
- Personal
- Tailbale
- Experiments
- Archived

Supported operations:

- create;
- rename;
- delete;
- reorder;
- collapse or expand;
- move sessions into or out of the group.

### 3.2 Dev Session

A **Dev Session** is a saved remote development workspace.

A Dev Session normally represents:

- one repository or project; or
- one specific task within a repository.

A Dev Session contains:

- name;
- group;
- remote repository root;
- default working directory;
- optional task description;
- viewer panes;
- terminal panes;
- viewer-region split layout;
- terminal-region split layout;
- selected pane state;
- lifecycle and connection state;
- agent-status metadata.

There is no separate Project or Task entity in the initial model. Both use the Dev Session abstraction.

### 3.3 Viewer Pane

A **Viewer Pane** displays one URL or remote resource.

Viewer panes may contain:

- arbitrary HTTP or HTTPS websites;
- remote source files;
- remote text files;
- Markdown documents;
- JSON, YAML, or TOML;
- images;
- PDFs;
- directories;
- custom internal viewer pages.

Viewer panes are individually splittable within the fixed viewer region.

### 3.4 Terminal Pane

A **Terminal Pane** is an xterm.js-based terminal connected over SSH to a remote PTY attached to a tmux session.

Each terminal pane has:

- stable ID;
- display name;
- Dev Session ID;
- remote working directory;
- tmux target;
- optional startup command;
- harness type;
- connection state;
- attention state.

### 3.5 Remote Server

Version 1 assumes one configured SSH server.

The server may host many repositories, Dev Sessions, tmux sessions, terminal processes, the CodeHarbor workspace database, the remote helper service, and agent-status adapters.

The internal data model should still include a `server_id` so multi-server support can be added later.

---

## 4. User Interface

### 4.1 Main Window

The main window consists of a horizontal split containing:

1. Sessions sidebar
2. Viewer region
3. Terminal region

The widths of all three regions are adjustable and persisted.

Each outer region may be collapsible, but the underlying three-region structure remains fixed.

### 4.2 Sessions Sidebar

Each Dev Session row should display:

- session name;
- optional repository or branch subtitle;
- aggregate terminal connection state;
- number of active terminals;
- number of terminals requiring attention;
- unsaved-file indicator;
- error indicator.

Suggested state precedence:

1. Error
2. Waiting for input
3. Running
4. Finished with unseen output
5. Idle
6. Disconnected

Sidebar operations:

- create session;
- rename session;
- duplicate session;
- delete session;
- archive session;
- move session between groups;
- reorder sessions;
- open repository shell;
- reconnect all terminals;
- mark activity as seen.

Duplicating a Dev Session should copy viewer definitions, terminal definitions, split layouts, repository root, and task metadata, while generating new tmux targets.

### 4.3 Viewer Region

The viewer region contains one or more viewer panes arranged using a recursive split tree.

Supported operations:

- split horizontally;
- split vertically;
- close pane;
- duplicate pane;
- move pane within the viewer region;
- change viewer handler;
- navigate to another URL;
- reload;
- open externally;
- zoom;
- open developer tools where applicable.

### 4.4 Terminal Region

The terminal region contains one or more terminal panes arranged using a recursive split tree.

Closing a terminal pane must not kill its tmux session by default.

Possible operations:

- **Close pane:** remove the pane from the current layout and detach locally.
- **Detach:** disconnect while preserving the pane definition.
- **Kill terminal:** explicitly kill the remote tmux session.
- **Reconnect:** reattach to the same tmux target.

### 4.5 Split Trees

The viewer and terminal regions each use an independent recursive split-tree model.

```text
VerticalSplit
├── Pane A
└── HorizontalSplit
    ├── Pane B
    └── Pane C
```

A new pane should default to splitting the currently focused pane vertically. Split ratios must be persisted per Dev Session.

---

## 5. Terminal Architecture

### 5.1 Recommended Stack

```text
xterm.js
    ↕ Qt WebChannel
C++ TerminalController
    ↕ libssh
Remote PTY
    ↕
tmux
```

Responsibilities:

- **xterm.js:** terminal rendering and terminal emulation;
- **Qt WebChannel:** JavaScript-to-C++ bridge;
- **TerminalController:** input, output, state, buffering, reconnect logic;
- **libssh:** SSH connection, authentication, channel, and PTY transport;
- **tmux:** remote process persistence.

### 5.2 Terminal Creation

When opening a terminal pane:

1. Resolve the configured SSH server.
2. Connect or reuse an existing SSH connection.
3. Verify the host key.
4. Authenticate.
5. Open an SSH channel.
6. Request a remote PTY.
7. Set the terminal type.
8. Attach to or create the terminal pane's tmux target.
9. Connect remote output to xterm.js.
10. Connect xterm.js input to the SSH channel.
11. Forward terminal-size changes to the remote PTY.

Suggested command:

```bash
tmux new-session -A \
  -s 'ch_<dev-session-uuid>_<terminal-uuid>' \
  -c '/remote/working/directory'
```

Stable IDs should be used for tmux names rather than user-facing display names.

### 5.3 Connection Pooling

Use one authenticated SSH connection to the configured server, with multiple independent SSH channels.

```text
SSH Connection
├── Terminal Channel A
├── Terminal Channel B
├── Terminal Channel C
├── CodeHarbor RPC Channel
└── Agent Status Channel
```

### 5.4 Background Connections

The following must be treated as separate concerns:

1. remote process persistence;
2. SSH channel persistence;
3. xterm.js renderer lifetime.

Recommended behavior:

- tmux session remains alive at all times;
- terminal SSH channel normally remains connected;
- hidden xterm.js renderers may be suspended;
- terminal output continues to be drained and buffered while hidden.

A terminal controller should therefore exist independently from its visible xterm.js view.

### 5.5 Output Buffering

Suggested batching:

- flush every 5–15 ms; or
- flush after 4–16 KiB;
- whichever occurs first.

For hidden terminals:

- retain the most recent 1–4 MiB locally;
- continue draining the SSH channel;
- rely on tmux history for older output;
- do not persist full terminal transcripts by default.

### 5.6 Reconnection

Suggested terminal states:

```text
Unloaded
Connecting
Authenticating
OpeningChannel
AttachingTmux
Ready
Disconnected
Reconnecting
Error
```

Suggested retry delays:

```text
1 s, 2 s, 5 s, 10 s, 30 s, then every 60 s
```

Manual reconnect should bypass the wait.

---

## 6. Agent Awareness

### 6.1 Purpose

CodeHarbor should show whether a coding agent is starting, running, waiting for input, idle, finished with unseen output, stopped, errored, or disconnected.

Terminal-screen scraping should not be the primary mechanism.

### 6.2 Supported Harnesses

Initial harness metadata:

```text
generic
oh-my-pi
pi
claude-code
```

Oh My Pi is the highest-priority integration.

### 6.3 Remote Agent Bridge

A small remote helper should receive harness events and forward them to the Qt client.

```text
Oh My Pi extension ───────┐
Pi extension ─────────────┼── Unix socket ── codeharbor-bridge
Claude Code hooks ────────┘                       │
                                                 │ JSONL over SSH
                                                 ▼
                                      AgentStatusMonitor
```

Suggested socket:

```text
$XDG_RUNTIME_DIR/codeharbor.sock
```

Fallback:

```text
~/.cache/codeharbor/events.sock
```

### 6.4 Internal Event Schema

```json
{
  "version": 1,
  "timestamp": "2026-07-20T01:20:30.123Z",
  "harness": "oh-my-pi",
  "devSessionId": "session-uuid",
  "terminalId": "terminal-uuid",
  "state": "waiting_input",
  "event": "ask_started",
  "summary": "Agent is waiting for structured input",
  "metadata": {
    "toolName": "ask"
  }
}
```

Supported states:

```text
starting
running
waiting_input
idle_unseen
idle
error
stopped
unknown
```

Harness adapters must never block or break the coding agent if CodeHarbor is unavailable.

### 6.5 Oh My Pi Adapter

Suggested initial mapping:

```text
session_start            → starting
agent_start              → running
tool_call: ask           → waiting_input
tool_result: ask         → running
agent_end / settled      → idle_unseen
session_shutdown         → stopped
agent or hook error      → error
```

### 6.6 Fallback Activity Detection

Without a harness adapter, CodeHarbor may expose only coarse states:

```text
connected
activity detected
idle
disconnected
```

It must not pretend to reliably distinguish agent work, long-running commands, waiting for input, or completion.

---

## 7. Viewer Architecture

### 7.1 Supported Inputs

Viewer panes should initially support:

```text
http://
https://
file://
codeharbor-internal://
```

Within CodeHarbor, `file://` always refers to the remote SSH server.

Relative paths resolve against the Dev Session repository root.

### 7.2 Arbitrary Websites

Viewer panes may display arbitrary websites, including authenticated services.

External websites must use a separate browser security context from privileged internal viewers.

External pages must not receive:

- Qt WebChannel access;
- SSH access;
- remote file access;
- internal application APIs;
- workspace secrets.

### 7.3 Browser Profiles

Use separate Qt WebEngine profiles for external pages and internal pages.

External pages may use persistent cookies and local storage but receive no privileged bridge.

Internal pages may use Qt WebChannel and controlled remote-file access through a custom internal URL scheme.

### 7.4 Remote `file://` Handling

Remote `file://` URLs must not be passed directly to Chromium because Chromium would interpret them as client-machine paths.

The address bar should display:

```text
file:///home/yc/project/README.md
```

Internally, the application may resolve it to:

```text
codeharbor-internal://file/<opaque-id>
```

The internal URL is an implementation detail.

### 7.5 Viewer Handler Registry

The initial extensibility mechanism is a lightweight handler registry.

Possible resolution types:

```text
DirectWebNavigation
InternalHtmlRenderer
TextEditor
ImageViewer
PdfViewer
DirectoryViewer
Download
OpenExternally
Error
```

Initial handlers:

| Handler | Inputs |
|---|---|
| Web | HTTP and HTTPS |
| Source editor | source code and text |
| Markdown | `.md`, `.markdown` |
| Structured data | JSON, YAML, TOML |
| Image | PNG, JPEG, WebP, GIF, SVG |
| PDF | PDF |
| Directory | remote directories |
| Binary | metadata and download/open actions |

---

## 8. Remote File Editing

### 8.1 Editor

Text-based remote files should be editable in an embedded editor, preferably Monaco Editor.

```text
Qt WebEngine
    ↕ Qt WebChannel
RemoteEditorBridge
    ↕
CodeHarbor Remote File Service
    ↕
Server filesystem
```

Monaco should provide syntax highlighting, find and replace, multiple cursors, folding, bracket matching, minimap, and future diagnostics integration.

### 8.2 File States

Editable viewer panes should expose:

```text
Loading
Clean
Modified
Saving
Saved
Externally modified
Conflict
Read-only
Error
Disconnected
```

Unsaved-file state should appear both in the pane header and in the Dev Session row.

### 8.3 Remote File API

The server-side file service should eventually support:

```text
stat(path)
readFile(path, offset?, length?)
writeFile(path, content, expectedRevision)
createFile(path)
createDirectory(path)
listDirectory(path)
rename(source, destination)
copy(source, destination)
delete(path)
watch(path)
unwatch(path)
resolvePath(path)
getMimeType(path)
```

Initial editing support requires:

```text
stat
readFile
writeFile
watch
resolvePath
```

### 8.4 Revision Tokens

Every read should return a revision token. Every save includes the revision originally loaded. The server rejects the save if the remote file changed.

### 8.5 Atomic Saves

Saving should be atomic:

1. verify the expected revision;
2. create a temporary file in the same directory;
3. write the new contents;
4. flush the file;
5. preserve mode and relevant metadata;
6. rename the temporary file over the original;
7. return the new revision.

### 8.6 Conflict Handling

When the remote file changed after being opened:

```text
File changed externally.

[Compare] [Reload] [Overwrite] [Save As]
```

CodeHarbor must never silently overwrite a changed remote file.

### 8.7 Remote File Watching

File watching should run on the server.

Behavior:

- clean buffers may reload automatically;
- dirty buffers show a conflict warning;
- Markdown previews refresh;
- images and PDFs refresh;
- directory listings update.

Polling may be used as a fallback.

### 8.8 File Type Behavior

| Type | Default behavior |
|---|---|
| Source code | Monaco editor |
| Plain text | Monaco editor |
| Markdown | editor with preview toggle |
| JSON, YAML, TOML | Monaco editor |
| HTML | source editor or rendered page |
| Images | image viewer |
| PDF | PDF viewer |
| Directory | remote directory browser |
| Binary | metadata and download/open options |
| Very large text file | streaming read-only viewer |

---

## 9. Working Directory Semantics

Each Dev Session has:

```text
repositoryRoot
defaultWorkingDirectory
```

Usually both are identical.

New terminal panes default to the Dev Session working directory.

Relative viewer paths resolve against the repository root.

Paths outside the repository root are allowed, but the UI should indicate that the file is outside the project.

---

## 10. Remote Service

### 10.1 Purpose

A small remote service, `codeharbord`, should handle remote workspace operations.

```text
Qt client
    │
    │ SSH
    │
    ├── terminal PTY channels
    ├── codeharbord RPC channel
    └── agent-status channel
             │
             ▼
       SSH development server
             │
             ├── codeharbord
             ├── tmux
             ├── repositories
             └── agent adapters
```

The service may be launched over SSH:

```bash
codeharbord rpc --stdio
```

It does not need to run permanently.

### 10.2 Responsibilities

`codeharbord` should eventually handle:

- workspace database;
- groups and sessions;
- viewer and terminal definitions;
- layout persistence;
- file reads and writes;
- file watching;
- directory listings;
- MIME detection;
- tmux discovery;
- agent-status events;
- Git metadata;
- session recovery.

### 10.3 RPC

Use newline-delimited JSON or framed JSON-RPC.

---

## 11. Persistence

### 11.1 Server Database

The authoritative SQLite database lives on the server.

Suggested location:

```text
~/.local/share/codeharbor/codeharbor.sqlite
```

Suggested tables:

```text
groups
dev_sessions
viewer_panes
terminal_panes
session_layouts
server_profiles
server_settings
app_settings
```

### 11.2 Local State

The client may store only:

- SSH server connection profile;
- window geometry;
- client display preferences;
- SSH-agent or credential-store references;
- browser cookies and cache;
- temporary reconnect state.

The client must not store project repositories or project files.

### 11.3 Unsaved Recovery

Unsaved buffers should be recoverable after client failure.

Recovery snapshots should be stored on the server:

```text
~/.local/share/codeharbor/recovery/<client-id>/<pane-id>.json
```

Recovery files should use mode `0600`.

---

## 12. Security

### 12.1 SSH

Requirements:

- host-key verification;
- persistent known-hosts store;
- fingerprint prompt for unknown hosts;
- refusal of changed host keys unless explicitly approved;
- SSH agent preferred;
- password or key passphrases stored only in the operating-system credential store;
- no credentials stored in the workspace database.

### 12.2 Viewer Isolation

External websites must not receive Qt WebChannel objects, SSH functionality, remote filesystem access, internal application APIs, agent-status APIs, or workspace secrets.

Internal viewers must use a separate WebEngine profile.

### 12.3 Logging

Do not log by default:

- terminal input;
- terminal output;
- file contents;
- agent prompts;
- SSH credentials;
- browser cookies.

### 12.4 Client Filesystem

CodeHarbor must never interpret `file://` as a client-local file.

Client-side file pickers should not be used for project operations.

---

## 13. Web Uploads and Downloads

Arbitrary websites are rendered locally by Qt WebEngine, even though development state is remote.

### Downloads

Preferred behavior:

- intercept the download;
- prompt for a remote server destination;
- stream the downloaded content to the server;
- avoid writing it to client disk.

### Uploads

Preferred behavior:

- replace or intercept the client-side file picker where possible;
- allow selecting a file from the SSH server;
- stream selected bytes through client memory if required;
- do not persist the file to client disk.

A fully server-side browser would require remote Chromium and display streaming, which is outside the initial scope.

The practical invariant is:

> All development files, commands, terminals, and persistent workspace state are server-side. The client renders and transports data.

---

## 14. Proposed Code Architecture

```text
src/
├── app/
├── models/
├── persistence/
├── ssh/
├── terminal/
├── viewers/
├── remote/
├── qml/
└── web/
    ├── terminal/
    └── editor/
```

Remote component:

```text
remote/
├── codeharbord
├── codeharbor-bridge
└── adapters/
    ├── oh-my-pi.ts
    ├── pi.ts
    └── claude-code-hook
```

Suggested technologies:

- Qt 6;
- Qt Quick and QML;
- C++20;
- Qt WebEngine;
- Qt WebChannel;
- Qt SQL;
- libssh;
- xterm.js;
- Monaco Editor;
- SQLite;
- CMake.

---

## 15. Keyboard Shortcuts

Suggested defaults:

```text
Ctrl+K          Command palette
Ctrl+P          Switch Dev Session
Ctrl+Shift+T    New terminal
Ctrl+Shift+V    New viewer
Ctrl+S          Save active remote file
Ctrl+W          Close active pane
Ctrl+Tab        Next pane
Alt+1..9        Select Dev Session
```

Split shortcuts should be configurable because terminal applications and tmux may use overlapping combinations.

---

## 16. MVP Plan

### Phase 0 — Terminal Vertical Slice

- Qt window;
- one xterm.js terminal;
- Qt WebChannel bridge;
- SSH PTY;
- remote tmux attach;
- input and output;
- terminal resize;
- reconnect.

### Phase 1 — Core Workspace

- fixed three-region layout;
- session groups;
- Dev Session creation and switching;
- multiple viewer panes;
- multiple terminal panes;
- recursive splits within each region;
- server-side SQLite state;
- one SSH server;
- background terminal connections;
- session status aggregation.

### Phase 2 — Remote Viewers

- arbitrary HTTP and HTTPS websites;
- persistent external browser profile;
- remote `file://`;
- remote text and source viewer;
- Markdown viewer;
- image viewer;
- PDF viewer;
- directory browser;
- secure separation between external and internal pages.

### Phase 3 — Remote Editing

- Monaco editor;
- remote reads and writes;
- revision tokens;
- atomic save;
- conflict detection;
- remote file watching;
- unsaved recovery;
- Markdown edit and preview mode.

### Phase 4 — Agent Awareness

- `codeharbor-bridge`;
- Oh My Pi adapter;
- running, idle, waiting, error states;
- unseen completion badges;
- desktop notifications;
- Pi adapter;
- Claude Code hook adapter.

Because Oh My Pi is the primary daily driver, its adapter may be included in the first personally usable release.

---

## 17. Deferred Features

- general-purpose plugin marketplace;
- native binary plugins;
- multi-server workspaces;
- remote Chromium;
- collaborative multi-user editing;
- terminal transcript indexing;
- full three-way merge;
- language-server integration;
- debugger integration;
- Git GUI;
- remote process manager;
- multi-window layouts;
- workspace synchronization between different servers.

---

## 18. Fixed Product Decisions

- The application is named **CodeHarbor**.
- The UI is built in Qt Quick and QML.
- The core backend is C++.
- The outer layout has exactly three fixed regions.
- Viewer and terminal regions use independent recursive split trees.
- Terminals use xterm.js.
- Terminals connect only to the remote SSH server.
- tmux provides process persistence.
- One SSH server is sufficient for version 1.
- Multiple terminal channels share one SSH connection.
- A Dev Session normally maps to one repository or project.
- A Dev Session may instead represent a specific task.
- Hidden terminal controllers remain connected where practical.
- Hidden terminal renderers may be suspended.
- Arbitrary websites are permitted.
- External websites receive no privileged bridge.
- `file://` always refers to a remote server file.
- Relative file paths resolve against the Dev Session repository root.
- Remote file editing and saving are first-class features.
- Conflicting saves never silently overwrite server changes.
- Workspace state is authoritative on the server.
- Unsaved recovery data is stored on the server.
- The client machine is never treated as a development host.
- Initial extensibility uses a viewer-handler registry, not a full plugin system.
- Oh My Pi is the highest-priority agent integration.

---

## 19. Product Definition

CodeHarbor is a purpose-built remote development command center for switching between persistent repository- and task-oriented workspaces.

It is not primarily a local IDE, terminal emulator, browser, remote desktop client, or tmux frontend.

It combines those elements into one remote-first workspace where projects, files, processes, and session state remain safely docked on the server while the user connects from any supported client platform.
