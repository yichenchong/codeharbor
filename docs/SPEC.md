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

A **Viewer Pane** is a web browser addressed by a URL. It has an address bar, navigation, and a security context,
and it displays one URL or remote resource at a time.

Everything else it can do — editing a source file, rendering Markdown, showing an image, a PDF, or a directory — is
a *handler* the browser delegates to once a URL has resolved (§7.5). A viewer pane is therefore a browser that can
also edit, not an editor that can also browse. The distinction is normative: it fixes what an unrecognised resource
does (it stays with the browser), and it means no capability may assume it owns the pane.

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
- number of active terminals — **not yet implemented**;
- number of terminals requiring attention — **not yet implemented**;
- unsaved-file indicator — **not yet implemented** (see section 8.2);
- error indicator (delivered as the `Error` value of the aggregate state above,
  not as a separate badge).

> **Implementation status.** The shipped row renders the session name, the optional
> subtitle, and ONE aggregate status dot. The sidebar data model
> (`SessionsModel::Roles` in `src/models/SessionsModel.h`) exposes only `NameRole`,
> `SubtitleRole`, `RowStateRole`, `IsGroupRole`, `CollapsedRole`, `IdRole` and
> `GroupIdRole`; there is no role for either terminal counter and none for unsaved
> files, so the QML delegate could not draw them even if it tried. Tracked in
> `docs/PLAN.md`.

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
    "tool": "ask"
  }
}
```

`metadata` is free-form, but the tool name is keyed **`tool`**. That is not a
preference: all three shipped adapters in `remote/src/adapters/` emit it under
that exact key — `oh-my-pi.ts` and `pi.ts` pass their harness's own `tool` field
through, and `claude-code.ts` renames the harness's `tool_name` to it. An adapter
that picks a different key compiles, validates and relays fine, and the client
simply never sees a tool name.

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

PRECEDENCE. The error flag is not part of the event vocabulary — it is a separate marker
the harness may set on any firing — so the mapping needs an order, and it is:

1. A **shutdown** event (`session_shutdown`; `SessionEnd` for Claude Code) always maps to
   `stopped`, whether or not the error flag is set. The session is over and observed to be
   over, nothing further can ever arrive for that terminal, and `stopped` is therefore the
   terminal's last word. A flag that could mask it would leave the sidebar row permanently
   in `error` for a session that no longer exists — reachable by the ordinary mistake of a
   producer that sets the flag once and never unsets it.
2. Otherwise the **error flag** outranks the native event name: an `agent_end` that blew up
   is an `error`, not a completion.
3. Otherwise the table above applies.

The same order holds for every adapter, not just Oh My Pi. Pi shares Oh My Pi's mapping
outright (`remote/src/adapters/pi-family.ts`, used by both) and Claude Code implements the
same rule over its own hook names.

### 6.6 Fallback Activity Detection

Without a harness adapter, CodeHarbor may expose only coarse states. Those states are
drawn from the SAME `state` vocabulary as section 6.4 — a fallback must never invent a
value the desktop client cannot interpret — and only this subset is used:

```text
starting     no terminal output observed yet
running      output observed within the idle threshold
idle         no output for longer than the idle threshold
```

The derivation lives on the CLIENT, in `ch::AgentStatusMonitor`
(`src/agent/AgentStatusMonitor.{h,cpp}`), not in the daemon. The daemon has no per-pane
source of terminal output: an adapter never sees a byte of it, and giving the daemon one
would mean either a per-pane server-side tmux tap or a client-to-daemon stream duplicating
every terminal byte back over SSH. The client already has the bytes —
`ch::TerminalController` ingests them for the renderer — so `ch::TerminalFactory`, the one
object that knows both a pane's `terminal_panes` row id and the PTY channel its bytes
arrive on, reports the FACT of output (never its content) to the monitor, which derives
the three states above. The full vocabulary is `AGENT_STATES` in `remote/src/events.ts`.

Only a pane whose `terminal_panes.harness` column is literally `generic` takes its state
this way. A pane with no harness configured is a plain shell, and treating a shell's
output as agent activity would light up every terminal in the sidebar; a pane with an
adapter harness gets its state from the wire, which is strictly better information.

Loss of the SSH channel is a TRANSPORT condition, reported by the client's own connection
state, so there is deliberately no `disconnected` agent state here.

It must not pretend to reliably distinguish agent work, long-running commands, waiting for input, or completion.

### 6.7 Silence Timeout

A harness that is killed — or whose host reboots — emits no shutdown event, so the last
state it reported would otherwise be kept for the lifetime of the client. After
`ch::AgentStatusMonitor::kStaleTimeoutMs` (15 minutes) of silence on BOTH channels — no
agent event and no terminal output — a pane is demoted to `unknown`.

Three rules make that safe:

- Only `starting` and `running` age. They are the states that assert a live agent is doing
  something right now. `waiting_input` and `idle_unseen` are the user's to-do list and stay
  true until somebody acts on them; `idle`, `stopped`, `error` and `unknown` assert no
  liveness to withdraw.
- The target is `unknown`, never `idle`. The client does not know whether a silent agent
  died or is thinking, and `unknown` is the vocabulary's word for exactly that.
- Terminal output refutes the silence for EVERY harness, not just `generic`. An agent that
  is working almost always prints, so the window only elapses for a pane that has said
  nothing at all on either channel.

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

Resolution is browser-first. Web navigation is the default disposition, and a more specialised handler only takes a
resource when it positively claims it:

1. `http` and `https` always resolve to direct web navigation. No other handler may intercept them.
2. `codeharbor-internal` resolves by explicit extension when it carries one, and renders as HTML otherwise.
3. `file://` resolves by directory, then by extension, then by well-known filename. The text editor is reached only
   on a positive match; it is never the fallback for an unrecognised resource.
4. Anything unclaimed becomes a download or metadata view, never an editor buffer.

A handler is therefore a delegate the browser hands a resource to, not a competing pane type.

Possible resolution types — the complete set the four steps above can yield, one
per line:

```text
DirectWebNavigation
InternalHtmlRenderer
TextEditor
ImageViewer
PdfViewer
DirectoryViewer
Download
Error
```

Opening a resource in the desktop's own application is **not** in that list. It is
an action the download/metadata view offers once resolution has already landed on
`Download` (see the Binary row below, and §4.3's viewer-region operations), not a
disposition a URL can resolve to.

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

The text editor is one of the viewer pane's handlers (§7.5), not a separate kind of pane. A pane reaches it by
navigating to a `file://` URL that resolves to text; the pane keeps its address bar, its history, and its identity
throughout. Nothing in this section creates an "editor pane" as a distinct concept.

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

> **Implementation status.** Only the pane header shows it. The row aggregation,
> `aggregateRowState()` in `src/models/SessionState.cpp`, takes five booleans — error,
> waiting-for-input, running, finished-with-unseen-output, connected — all derived from
> terminal and coding-agent conditions. No file state is passed in, so an unsaved
> buffer never reaches the sidebar row. Tracked in `docs/PLAN.md`.

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

> **Implementation status.** The conflict notice in `src/web/editor/src/index.ts` offers
> **Reload** and **Overwrite** only. **Compare** would need a diff view, and **Save As**
> a "write this buffer to a different path" call; neither has a method on the C++ editor
> bridge (`src/editor/EditorController.h`), so neither button can be wired yet. The
> mandatory sentence above still holds: an overwrite happens only when the user asks for
> one. Tracked in `docs/PLAN.md`.

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
| Anything not matched above | metadata and download view. The editor is reached on a positive match only, never as a fallback (§7.5). |

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

The wire protocol is JSON-RPC 2.0 carried as newline-delimited JSON on the RPC
channel's stdin/stdout. The rules below are the ones the implementation
(`remote/src/codeharbord.ts`, with the shared constants in
`remote/src/rpc-types.ts`) actually enforces:

- **One request per line, one response per line.** Each line is one complete
  JSON-RPC request object.
- **No batch requests.** A JSON *array* of request objects is deliberately
  unsupported: it fails the request-shape check and is answered with a single
  Invalid Request (`-32600`), never with an array of responses. The only client
  writes one request per line and correlates replies by `id`.
- **Blank lines are ignored.** A line containing no non-whitespace character is
  skipped silently and produces no response, so stray separators are harmless.
- **Unparseable or structurally invalid input is answered with `id: null`.**
  Malformed JSON yields Parse error (`-32700`). A decoded value that is not a
  request object — wrong or missing `jsonrpc`, a non-string `method`, or an `id`
  that is neither a string, a number, nor `null` — yields Invalid Request
  (`-32600`). Both use `id: null` because no usable request id could be recovered.
  Note that an explicit `"id": null` IS a legal id and is echoed back as such.
- **`params`, when present, must be an object or an array.** A primitive or `null`
  is rejected up front with Invalid params (`-32602`) rather than failing later,
  confusingly, inside a method handler.
- **Unknown method** yields Method not found (`-32601`), and it outranks bad
  params. An exception escaping a handler yields Internal error (`-32603`).
- **A notification never receives a response.** A request with no `id` member is
  dispatched for its side effects only and answered with nothing at all, whether it
  succeeds, hits an unknown method, or throws. Server-initiated messages use the
  same id-less form — see the `file.watchEvent` notification in section 8.7.
- **Application-level failures use JSON-RPC's implementation-defined
  `-32000..-32099` range.** Each code has a constant in `remote/src/rpc-types.ts`
  mirrored in C++ in `src/remote/RpcTypes.h`, and `remote/test/rpc-mirror.test.ts`
  fails if the two sides drift apart. Three are defined:
  - **`-32001`, revision mismatch** (`RPC_REVISION_MISMATCH` / `kRevisionMismatch`),
    returned for a `file.writeFile` whose `expectedRevision` no longer matches the
    file (sections 8.4 and 8.6).
  - **`-32002`, database busy** (`RPC_DATABASE_BUSY` / `kDatabaseBusy`), returned
    for a workspace write that could not take the server database's write lock
    before the busy timeout ran out (section 11.1).
  - **`-32003`, resource limit** (`RPC_RESOURCE_LIMIT` / `kResourceLimit`),
    returned when the parameters are valid and nothing was applied, but the server
    refuses to answer at that size: a `file.listDirectory` whose serialized listing
    would exceed 15 MiB, or a `file.watch` past 512 live subscriptions
    (sections 8.3 and 8.7).

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

Unsaved buffers must be recoverable after client failure.

Recovery snapshots are stored on the server, one file per viewer pane holding an
unsaved buffer, in a dedicated recovery directory under the server's user data
location. The server reports that absolute directory in its `server.info` result
as `recoveryDir` (`$XDG_DATA_HOME` or `~/.local/share`, plus
`/codeharbor/recovery`); the client appends the pane's stable layout id:

```text
<recoveryDir>/<paneId>
```

`EditorController::recoveryPath()` in `src/editor/EditorController.cpp` builds
this, and it is a deliberate choice over a directory beside the edited file:

- One file PER PANE, keyed by the pane's stable id rather than by the edited
  path. Two panes editing the same file therefore keep independent snapshots
  instead of sharing (and clobbering) one file, and a snapshot never appears
  inside the user's repository as an untracked file.
- The base directory is chosen by the SERVER, not derived from the client's own
  data location. Recovery writes are remote (section 8.3 `writeFile`), so a
  client-derived path could name a directory that does not exist, or belongs to
  a different user, on the server; a server-reported directory is correct on any
  host. A server too old to report `recoveryDir` leaves the client with recovery
  disabled rather than guessing a path.
- Because a pane's single snapshot file is reused as the pane opens different
  files, each snapshot is SELF-DESCRIBING: it records the remote path it belongs
  to alongside the buffer, and is offered on open only when that recorded path
  matches the file now open. A snapshot from a file the pane has since left is
  never handed back as the current file's unsaved work.
- The snapshot is written and read through the ordinary remote file methods of
  section 8.3 (`stat`, `readFile`, `writeFile`) and guarded by the same revision
  tokens as a normal save (section 8.4), so concurrent writers cannot destroy
  each other's snapshots.

Confidentiality comes from the file mode: each snapshot is written with mode
`0600` through `writeFile`'s optional `mode` parameter, so it is readable and
writable by its owner only, regardless of the process umask. The recovery
directory itself is created by `writeFile`'s recursive parent creation at mode
`0700` (`remote/src/files.ts`).

---

## 12. Security

### 12.1 SSH

Requirements:

- host-key verification;
- persistent known-hosts store;
- fingerprint prompt for unknown hosts;
- unconditional refusal of a changed host key. When the known-hosts store already
  trusts a host but under a DIFFERENT key — including the same key presented under a
  different algorithm — the connection is refused outright and the user is never
  offered a way to approve it. `SshConnectionPool::verifyHostKey()` in
  `src/ssh/SshConnectionPool.cpp` consults the user-decision callback only for a host
  that is not in the store at all. A user who genuinely needs to accept a new key for
  an already-known host must edit the known-hosts file deliberately, outside
  CodeHarbor; that friction is the point — an in-app "approve" button on this path is
  exactly what a man-in-the-middle needs;
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

## 14. Code Architecture

This section was written as a proposal. It is kept up to date with the tree as
built, so it describes what exists rather than what was once suggested.

```text
src/
├── app/
├── models/
├── persistence/
├── ssh/
├── terminal/
├── viewers/
├── remote/
├── editor/
├── agent/
├── qml/
└── web/
    ├── terminal/
    └── editor/
```

Remote component:

```text
remote/
├── src/
│   ├── codeharbord.ts        RPC service (JSON-RPC 2.0 over newline-delimited JSON)
│   ├── rpc-types.ts          request/result shapes shared by the RPC modules
│   ├── files.ts              file.* methods: stat/read/write/watch/listDirectory
│   ├── workspace.ts          workspace.* methods over the SQLite database
│   ├── tmux.ts               tmux.* discovery methods
│   ├── validate.ts           request-parameter validation helpers
│   ├── events.ts             agent event schema + socket-path resolution
│   ├── bridge.ts             agent event relay
│   ├── adapters/             oh-my-pi.ts, pi.ts (both over pi-family.ts),
│   │                         claude-code.ts, types.ts, index.ts (the registry)
│   └── hooks/                oh-my-pi-hook.ts (the native harness hook)
└── sql/                      schema.sql, indexes.sql
```

Technologies actually used:

- Qt 6 (floor 6.9);
- Qt Quick and QML;
- C++20;
- Qt WebEngine;
- Qt WebChannel;
- libssh (floor 0.11.2, never 0.12.0);
- xterm.js;
- Monaco Editor;
- SQLite, on the server only — the workspace database is opened by
  `codeharbord`, never by the client, so the client links no Qt SQL module
  (§ 11.2, *Local State*);
- CMake (floor 3.25) and Ninja;
- Node.js (floor 23.6) and TypeScript for `remote/` and `src/web/`.

---

## 15. Keyboard Shortcuts

Implemented bindings:

```text
Ctrl+Shift+P    Command palette
Ctrl+Shift+O    Connect to Server…
Ctrl+Shift+R    Refresh Workspace
Ctrl+Shift+W    Close Window
Ctrl+S          Save active remote file (inside the focused editor)
```

`Ctrl+Shift+P` opens the palette (`activationSequence` in
`src/qml/CommandPalette.qml`; Qt maps `Ctrl` in a key sequence to Command on macOS,
so it is `⌘⇧P` there). `Ctrl+Shift+O`, `Ctrl+Shift+R` and `Ctrl+Shift+W` are
`shortcut` entries on the command list in `src/qml/Main.qml`, which the palette
turns into real window-wide `Shortcut` objects, so they fire whether or not the
palette is open. `Ctrl+S` is registered on the Monaco instance itself in
`src/web/editor/src/index.ts`, so it applies to the focused editor rather than
window-wide.

Two of these deviate from the plain sequence this section originally suggested,
for the same reason: a window-wide `Shortcut` is matched before the key ever
reaches the focused item, and every terminal pane hosts a real shell.
`Ctrl+R` is reverse-history-search there, and `Ctrl+W` is delete-word, so
**Refresh Workspace** is `Ctrl+Shift+R` and **Close Window** is `Ctrl+Shift+W`.
`Close Window` exists as a binding at all because the window is frameless and
therefore has no window-manager close button of its own.

Every remaining command — splitting a viewer or terminal pane, closing a focused
pane, killing a terminal's remote tmux session, disconnecting from the server,
marking agent output seen — is reachable through the palette only and carries no key
sequence.

Originally suggested defaults, and how they were reconciled:

| Suggested | Status |
|---|---|
| `Ctrl+K` Command palette | Superseded by `Ctrl+Shift+P` (`activationSequence` in `src/qml/CommandPalette.qml`). `Ctrl+K` is not bound at all. |
| `Ctrl+P` Switch Dev Session | Not implemented — there is no session switcher. |
| `Ctrl+Shift+T` New terminal | Not implemented as a key sequence; a new terminal pane comes from the palette's "Split Terminal Pane Horizontally/Vertically". |
| `Ctrl+Shift+V` New viewer | Not implemented as a key sequence; a new viewer pane comes from the palette's "Split Viewer Pane Horizontally/Vertically". |
| `Ctrl+S` Save active remote file | Implemented, in the viewer pane's text-editor handler (§8.1). |
| `Ctrl+W` Close active pane | Not implemented as a key sequence; "Close Focused Viewer/Terminal Pane" are palette commands. Note `Ctrl+Shift+W` is **not** this — it closes the whole window. |
| `Ctrl+Tab` Next pane | Not implemented. Pane focus is click-based today, so a "focus next pane" command would first have to be able to move focus (see `docs/PLAN.md`). |
| `Alt+1..9` Select Dev Session | Not implemented. |

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
That statement is about the product as a whole. It is not a statement about the viewer pane, which **is**
a web browser (§3.3) and is specified as one.

It combines those elements into one remote-first workspace where projects, files, processes, and session state remain safely docked on the server while the user connects from any supported client platform.
