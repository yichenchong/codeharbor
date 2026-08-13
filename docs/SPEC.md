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
- Tasks
- Experiments
- Archived

Supported operations:

- create;
- delete;
- reorder;
- collapse or expand;
- move sessions into or out of the group.

The current client has no group-rename control; a group can be renamed only by a
future settings or sidebar surface.

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

Because a viewer pane is a browser, it carries the four controls a browser has: **Back**, **Forward**,
**Reload** and **Home**, beside the address field. Each pane keeps its OWN history of what it has shown;
splitting produces a pane with an empty one. Navigating somewhere new truncates whatever was ahead, exactly as
a browser does, and Back/Forward are disabled at the ends of the history rather than silently doing nothing.
Reload re-fetches the resource currently shown for EVERY handler kind, not only for web pages: a handler with
no reload primitive of its own (an image, a directory listing, a binary preview) is recreated, so stale content
cannot survive a reload. Home goes to the active Dev Session's repository root and is disabled when there is no
active session. There is no separate "open this address" button — the address field navigates on Enter.

History is per pane and in memory. It is deliberately NOT persisted to the workspace: where a pane has BEEN is
not workspace state, and a second client has no use for it.

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

The display name is the user's to change: a pane can be renamed from its header, and the name is stored with the
split layout, so it survives a restart and is seen by a second client. It is a LABEL and nothing more. A terminal's
identity is its server-minted `terminal_panes` row (§5.2), never its name and never its slot label, so renaming a
pane cannot re-point it at another shell. An empty or whitespace-only name means "no custom name" and the pane
falls back to its generated slot label; names are trimmed and length-capped on the way in.

### 3.5 Remote Server

The client stores as many server profiles as the user wants and connects to ONE at a time. The connect surface
lists the saved servers, marks the one in use, and switching to another tears the current session down before
dialling the new one.

The server may host many repositories, Dev Sessions, tmux sessions, terminal processes, the CodeHarbor workspace
database, the remote helper service, and agent-status adapters.

Every domain row carries a `server_id`, and the client keys its whole workspace by the id the SERVER mints for
itself (§11.1). That is what makes switching safe: rows, layouts, terminal identities and the remembered "last
open Dev Session" all belong to one server, and a reply that arrives after a switch is discarded rather than
painted over the new server's sidebar. Mixing two servers' rows would be a data-integrity fault, not a cosmetic
one.

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
> subtitle, ONE aggregate status dot, and a pin toggle. The sidebar data model
> (`SessionsModel::Roles` in `src/models/SessionsModel.h`) exposes `NameRole`,
> `SubtitleRole`, `RowStateRole`, `IsGroupRole`, `CollapsedRole`, `IdRole`,
> `GroupIdRole`, `PinnedRole` and `ArchivedRole`; there is no role for either terminal counter and
> none for unsaved files, so the QML delegate could not draw them even if it tried.
> Tracked in `docs/PLAN.md`.

Suggested state precedence:

1. Error
2. Waiting for input
3. Running
4. Finished with unseen output
5. Idle
6. Disconnected

`Disconnected` means a session that HAD a live terminal and lost it. A session whose panes have never been
opened, and a session with no panes at all, are `Idle`: absence of information is not a disconnection, and
reporting it as one made every row in a fresh workspace read as broken.

**Pinning.** Each row carries a pin toggle, and the sidebar toolbar carries a pin filter beside "add group".
With the filter on, the sidebar shows only pinned sessions and only the groups that contain one; with nothing
pinned it says so explicitly rather than showing the same blank panel a failed load would. Which sessions are
pinned is workspace state, stored on the server so a second client sees the same pins. Whether the FILTER is on
is client-local presentation state (§11.2) — one machine hiding rows must not hide them everywhere.

Sidebar operations:

- create session;
- rename session;
- duplicate session;
- delete session;
- archive session;
- move session between groups;
- reorder sessions;
- mark activity as seen.

Two operations in earlier drafts of this list are NOT implemented and no client
path offers them: "open repository shell" and "reconnect all terminals". A
repository shell is reached by creating a terminal pane in the Dev Session, and
terminals reconnect individually.

Duplicating a Dev Session should copy viewer definitions, terminal definitions, split layouts, repository root, and task metadata, while generating new tmux targets.

**Archiving and deleting are different, and the difference must be obvious in the
interface.** Archiving is reversible and destroys nothing: the session keeps its
panes, layouts and terminal identities, and simply leaves the sidebar's default
view. The archived bit lives on the SERVER, so a session archived on one machine
is archived everywhere; whether archived rows are currently SHOWN is client-local
presentation state (§11.2), because one machine choosing to hide rows must not
hide them for anybody else. An archived row that is shown is marked as archived,
distinguishably from the pin marker.

Deleting is permanent. Deleting a Dev Session destroys its panes and layouts with
it; deleting a GROUP destroys every session inside it, because the server removes
the group's sessions and their panes and layouts in one transaction. Both also
kill the remote tmux sessions of the terminal panes they destroy (§4.4), and the
confirmation says so: the destroyed row is the only record of a pane's tmux
target, so a session left running after it could never be reached again. Both are
therefore confirmed before anything happens, and the confirmation NAMES its
subject — a group's confirmation states how many sessions will be destroyed with
it, counted from the authoritative workspace rather than from the filtered
sidebar, so hidden rows are included in the number. A deletion the server refuses
leaves the row where it is, kills nothing, and reports the failure; the row must
never disappear optimistically and reappear on the next refresh. A kill that
fails after a deletion the server accepted is reported by name and changes
nothing about the deletion.

Both filters are presentation only. Neither may narrow what the client believes
the workspace contains: the same tree answers "does the open Dev Session still
exist" and "how much would this deletion destroy", and a filtered answer to either
is a correctness fault, not a cosmetic one.

### 4.3 Viewer Region

The viewer region contains one or more viewer panes arranged using a recursive split tree.

Supported operations currently implemented in the client:

- split horizontally;
- split vertically;
- close pane;
- navigate to another URL;
- reload;
- choose another applicable viewer handler ("Open as");
- hand a directory-listing entry to a local desktop application through an
  `<appName>://` scheme, from the same "Open as" menu. The binary/metadata view
  itself offers a download action only.

Pane duplication and moving within the viewer region are not implemented. Zoom
controls and an in-app developer-tools action are not implemented either.

**Open as.** A file or directory in a directory listing can be opened with a specific handler instead of the
default one, from a context menu or a small affordance on the row (both reachable from the keyboard). The menu
offers ONLY the handlers that claim that target (§7.5), marks which one is the default, and offers the same
choices "in a new pane", which splits the region through the ordinary split path rather than a second one. A
plain click is unchanged and still opens the default handler in place.

**Open with `<appName>://`.** The same menu can hand the target to a local desktop application by scheme, for
plugins this client knows nothing about. The scheme is validated against the URL grammar (a letter followed by
letters, digits, `+`, `-` or `.`) and refused with a visible message otherwise; the path is escaped before it is
handed over. This is the one path that gives a remote-derived path to the LOCAL desktop, so it never guesses and
never runs anything itself.

### 4.4 Terminal Region

The terminal region contains one or more terminal panes arranged using a recursive split tree.

Closing a terminal pane from that pane's own close button kills its tmux session. The two used to be
separate controls and they are now one: the button asks for confirmation, then ends the remote tmux
session and removes the pane.

**Nothing else kills, unless the row that owns the pane is destroyed.** Only a deliberate press of a
terminal pane's own close button ends a remote session for a pane whose row survives. Every other way
a pane can disappear leaves the tmux session running on the server, to be re-attached later: closing
the application window or quitting the client, a dropped SSH connection and every rung of the
reconnect ladder, switching to another Dev Session, a layout being replaced, reloaded or repaired from
the server, closing a region from the sidebar, and a terminal region destroying a pane for any
internal reason. Those paths all end at the pane's destruction handler, which DETACHES. This is the
point of running the shells under tmux at all, and wiring the kill to pane destruction rather than to
the button press would silently destroy every long-running remote session the moment the user closed
the window.

**Deleting the owning row is the one exception.** Deleting a Dev Session — or deleting a group, which
destroys the Dev Sessions inside it — also kills the tmux sessions of the terminal panes those rows
own (§4.2). Every path listed above leaves a row behind: the pane's `terminal_panes` row still records
its server-minted target, so the user can come back and re-attach, and that is exactly what makes not
killing there the right thing. A DELETED row can never name its session again — nothing in the product
can, because the server is the only place a target is ever minted — so the session would run forever,
holding its shell and any agent inside it, killable only by hand over SSH. Not killing there does not
preserve the user's work, it strands it.

The ordering is delete FIRST, then kill, and **the SERVER says what to kill.** The delete collects the
tmux target of every terminal pane it is about to destroy inside the same transaction that destroys
them, and answers with that list; the client kills exactly those. It must not kill from its own last
workspace read: that is a snapshot, and a pane another client created or retargeted since then is
destroyed by the same delete, so killing from the snapshot would miss it and strand precisely the
orphan described above. A delete the server refuses reports no targets and kills nothing. A pane that
was never attached has no target and the server leaves it out. A kill that fails is reported by name
so the user can clean up by hand, but it never rolls back or blocks the deletion, which has already
happened on the server. A client talking to a server too old to report the list kills nothing rather
than guessing: target names are minted by the server and nowhere else (§5.2).

Possible operations:

- **Close pane:** confirm, kill the pane's remote tmux session, then remove the pane from the layout.
- **Detach:** disconnect while preserving the pane definition, leaving the remote session running.
- **Reconnect:** reattach to the same tmux target.

**Pane controls belong to the PANE.** Splitting and closing are offered on each
pane's own header, and each acts on the pane whose header was used. They are
deliberately not region-level controls acting on "the focused pane": with
several panes open, the pane a region-level button would act on is a guess, and a
wrong guess splits or closes something the user was not looking at. Because
closing a terminal pane is now irreversible, that guess would destroy running
remote work, so there is no palette command to kill a terminal at all: the
palette's "Close Focused Terminal Pane" removes the pane and leaves its session
running, and of the terminal region's own controls only the pane's own button —
which names itself — kills. Closing a
terminal pane is confirmed before it happens; closing a viewer pane is not,
because it destroys nothing on the server.

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

A layout load is a server round trip, so the user can act on a region before its tree arrives, and can switch
Dev Session while one is in flight. Every layout read and every layout write therefore carries the Dev Session
it belongs to plus a monotonic load generation. A write stamped for a session that is no longer current is
discarded silently — replaying it would corrupt the tree the user is now looking at, and it is not an error the
user did anything about. A write stamped for the CURRENT session that arrives before its tree does is queued and
applied when the tree lands. A region whose layout genuinely cannot be loaded still reports that, as before.

**The pane the user was last working in is remembered per Dev Session**, and
restored when that session is opened again, so reopening a session puts the
keyboard back where it was — including inside a terminal, where the user can carry
on typing without clicking first. Because a terminal is a web page and its focus
is the page's own state, restoring focus means driving it into the page, not
merely marking a QML item focused. The remembered pane is client-local (§11.2):
it describes what somebody was doing at this desk. A restore is stamped like any
other layout operation and is dropped if the user has since switched away, it
waits for the region's tree to arrive rather than firing into an empty region, and
it never overrides a pane the user has clicked in the meantime. Today, pane
selection persistence is driven by pane activation (including pointer/header
actions); keyboard-only traversal between viewer and terminal panes is not
implemented (see §15). A remembered pane that no longer exists falls back to
the first viewer pane silently — a closed pane is not an error.

### 4.6 Settings

Preferences live in one surface with a group list on the left and the selected group's controls on the right,
reachable from the command palette. Groups: **Appearance**, **File viewers**, **Server**, **Tmux**.

Appearance owns:

- **Theme** — Dark or Light. The theme is a named palette, not a boolean, so further themes need no new plumbing:
  every colour in the application resolves through one role vocabulary (`Theme.qml`), and switching repaints
  everything, including the three web surfaces — terminal, editor and Markdown renderer — which are handed the
  same role map (§5.1, §7.5, §8.1).
- **Group colour palette** and its size. A group's name is hashed to a stable index into the active palette and
  that colour tints the group's presentation, so groups stay distinguishable without the user choosing colours.
  The hash is stable across runs and machines. `plain` is the default and applies no tint at all — the historical
  look. `tokyonight` starts from five seed colours and the size is a user preference in the range 5–64. Five is
  the seed itself and is used unchanged; for a larger n the generator repeatedly finds the largest gap between
  neighbouring colours in OKLCH space and inserts their midpoint, so it only ever ADDS to the seed and is only
  invoked when n exceeds the seed count.
- **Toolbar button order.** Each toolbar button declares a stable id; the stored order is reconciled against the
  buttons a given build actually has (unknown ids ignored, missing ones appended), so the setting survives a
  build that adds or removes one. Reconciliation is a view, never a write: only the user reordering persists.
- **Terminal text size** and **rendering resolution** (§5.1).

File viewers owns which handler opens which file extension (§7.5). An empty mapping
means "use the built-in defaults", which is the shipped behaviour; the page lists
what the user has customised and lets them add and remove a mapping, offering only
the handlers that can actually display the type in question. The store is a plain
text file a person can edit, so junk in it — an unknown handler word, a malformed
extension, a wrong value type — is rejected or normalised on the way in rather
than trusted.

Server exposes the stored connection profiles and the actions that already exist for them (connect, disconnect,
update the remote service). Tmux currently explains the fixed per-session mouse-reporting behavior; it has no
editable options. No control in this window may be decorative: every one changes something observable.

All of it is client-local (§11.2). A preference is a property of this desktop, not of the workspace.

### 4.7 Log View

A log surface, reachable from the command palette, shows what the client has recorded: its own diagnostics plus
what it collects from the remote side (SSH diagnostics and the daemon's channel output), each labelled by origin,
because a remote-first application is not debuggable from the client's half alone.

It reads a bounded in-memory ring buffer — capped in entries AND in bytes, so a chatty session cannot grow
without limit — that is filled whether or not the view is open, so a message is not lost by having been emitted
too early. Messages continue to reach the terminal as before: the buffer chains to whatever handler was already
installed rather than replacing it. The view offers severity and text filters, follow-tail that stops when the
user scrolls up, copy, and clear. Credentials never enter the buffer.

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

**Rendering.** The terminal draws through xterm.js's WebGL renderer where the platform provides it and falls
back to the DOM renderer where it does not, and the canvas is sized at the DEVICE pixel ratio of the window it
is in. Getting that ratio wrong is what makes a terminal look soft: the grid is then drawn at fewer physical
pixels than the screen has and scaled up. Text size and rendering resolution are both user preferences (§4.6):
the size changes how big the cells are, the resolution changes how many physical pixels each cell is drawn with,
and a resolution of "follow the screen" is the default. Changing either re-fits the grid and tells the remote
PTY its new dimensions, so the shell re-wraps rather than drawing into a size it does not know about.

The resolution preference may only LOWER the ratio below the display's own. There are no physical pixels above
it for the extra detail to occupy, and a canvas larger than its box on screen can be mapped straight to physical
pixels by the compositor, which draws the whole terminal magnified by the ratio — the two preferences must stay
independent, and this is what keeps them so.

A WebGL context is capped per renderer process, so opening panes can take one away from a pane that already has
it, and a WebGL renderer that loses its context never paints again while its buffer, its bridge and its remote
shell all stay healthy. A pane that loses its context therefore reloads its page once with WebGL disabled: the
mount handshake replays the controller's retained output, and one window-change request makes the remote shell
repaint the screen.

**Clipboard.** The pane's tmux session runs with mouse reporting on, so an ordinary drag inside the grid belongs
to the program in the terminal rather than to the browser. A local selection is therefore made by holding the
modifier xterm.js reserves for exactly this — Shift and drag, or Option and drag on macOS — and that selection
stays until it is replaced. Nothing is ever put on the clipboard without being asked for: copying is
`Ctrl+Shift+C` (`⌘C` on macOS), the terminal's own menu, or — with something selected, and outside macOS —
plain `Ctrl+C`, which then clears the selection so that the next `Ctrl+C` is the interrupt again (§5.7). All
three put the current selection on the system
clipboard. Pasting the system clipboard is `Ctrl+Shift+V` (`⌘V` on macOS) or the menu, and sends it to
the shell, guarded by bracketed paste where the application asked for it. (A middle click is a mouse button like
any other and is reported to the remote side, where tmux answers it by pasting its OWN most recent copy — which
is not the system clipboard and is empty until something has been copied inside tmux.) A paste is chunked through the same
flow control as ordinary output and, like it, is never split inside an escape sequence (§5.5). Scrolling and
scrollback behaviour are unchanged by any of this: the wheel is reported to the remote side, so it scrolls
tmux's history. The full division of mouse actions between the remote program and the application is §5.7.

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
  -c '/remote/working/directory' \
  -e 'OMP_DEV_SESSION_ID=<dev-session-uuid>' \
  -e 'OMP_TERMINAL_ID=<terminal-uuid>' \
  \; set-option -t '=ch_<dev-session-uuid>_<terminal-uuid>:' mouse on \
  \; set-environment -t '=ch_<dev-session-uuid>_<terminal-uuid>:' \
      OMP_DEV_SESSION_ID '<dev-session-uuid>' \
  \; set-environment -t '=ch_<dev-session-uuid>_<terminal-uuid>:' \
      OMP_TERMINAL_ID '<terminal-uuid>'
```

Stable IDs should be used for tmux names rather than user-facing display names.

PANE IDENTITY IN THE ENVIRONMENT. The two `OMP_*` variables are how an agent hook
says which pane it is in: `remote/src/hooks/oh-my-pi-hook.ts` reads exactly these
two names, and an event that arrives without them names no pane and is dropped.
They are passed to the command builder as values, never parsed back out of the
tmux target — the target is a formatted name, not a data source. Both are
exported or neither is: a hook needs both halves.

This does NOT retrofit a pane that is already running. `new-session -e` applies
at session creation, and the command uses `-A`, so an attach to a session that
already exists ignores every `-e`; the `set-environment` commands correct that
session's environment, but a variable set now reaches only processes started
afterwards, never the shell tmux is already running or an agent already running
in it. Both limits verified on tmux 3.6. So the variables reach newly created
sessions, new windows and panes, and agents started after the attach — an
already-live agent keeps whatever environment it was forked with.

The server owns the target string. Terminal targets supplied through the
workspace RPC are accepted only when they are 1–200 characters from
`[A-Za-z0-9_-]`. In particular, `:` and `.` are rejected rather than rewritten,
because tmux interprets them as session/window/pane separators. The tmux
`killSession` operation applies the same separator and control-character
rejection before it builds its exact-match target.

### 5.3 Connection Model

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

**Escape-sequence boundaries.** A flush is never allowed to cut an ANSI escape
sequence in half: an incomplete trailing sequence is held back and emitted with
the bytes that complete it, and the same rule governs a paste (§5.1) and the
point at which a hidden terminal's rolling buffer is trimmed. A bound on the
held-back tail keeps malformed remote output from growing the pending buffer
without limit.

**Backpressure.** Output is emitted to the renderer against an acknowledged
consumed-byte count: past a fixed window of emitted-but-unacknowledged bytes,
further flushes go into the same rolling buffer the hidden case uses instead of
being handed to the renderer. This is what stops a runaway remote process from
queueing an unbounded amount of data in the JavaScript bridge and the browser
engine. The renderer acknowledges what it has consumed, and buffered output is
released as the window reopens.

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

### 5.7 Mouse Behaviour

The pane's tmux session is created with mouse reporting on (§5.2), and that is deliberate: it is what lets the
wheel scroll tmux's history and lets a program that asked for the mouse — vim, htop, and the like — receive it.
The consequence is that almost every mouse action inside the grid belongs to the program in the terminal, not to
CodeHarbor, and the application must not quietly take actions away from it.

| Action | Who handles it | What happens |
|---|---|---|
| Wheel | The remote program | Scrolls the tmux session's history; a program that asked for the mouse gets the wheel itself. |
| Left click | The remote program | Reported to whatever is running, which is how you move the cursor or hit a button in a full-screen program. |
| Left drag | The remote program | Inside tmux this starts tmux's copy-mode highlight, which tmux clears again on release. That is tmux's own default binding, and the application cannot change it for one session because tmux key tables are global. |
| Shift+drag (Option+drag on macOS) | The application | Makes a local selection in xterm.js, the modifier xterm.js documents for this. The selection stays until it is replaced, and nothing is copied automatically. |
| Double click, triple click | The remote program | Reported like any other press, so a word or line selection inside tmux is tmux's, with tmux's release behaviour. Hold the same Shift (Option) modifier to select a word or line locally instead. |
| Right click | The application | Never reported to the remote side. It opens CodeHarbor's own menu — see below. |
| Middle click | The remote program | Reported like any other button. Inside tmux that pastes tmux's own most recent copy, which is not the system clipboard. |
| `Ctrl+Shift+C` (`⌘C` on macOS) | The application | Copies the local selection to the system clipboard. |
| `Ctrl+C` with a selection (not macOS) | The application | Copies, and clears the selection. See below. |
| `Ctrl+C` with nothing selected | The remote program | The interrupt, unchanged. On macOS `Ctrl+C` is always the interrupt. |
| `Ctrl+Shift+V` (`⌘V` on macOS) | The application | Pastes the clipboard into the shell (§5.1). |

**Why the right button is the exception.** tmux's default right-button binding draws a menu inside the terminal
grid, and that menu closes again on the next mouse report — so simply moving the pointer dismissed it before it
could be used. Making the right button usable therefore means not reporting it at all: a right press is
swallowed in the browser's capture phase, before xterm.js can turn it into a mouse report, and CodeHarbor opens
its own menu instead. This is the only button the application withholds from the remote side.

The menu offers **Copy**, **Paste** and **Select All**. Copy is greyed out when there is no selection, because
with nothing selected there is nothing to copy. The menu also names the modifier for local selection, so the
Shift (Option) rule is discoverable from the terminal rather than only from this document. It closes when an
item is chosen, on Escape, and on a mouse press outside it — and, unlike tmux's, never on pointer movement.
Nothing here copies anything the user did not ask for: there is no copy-on-select and no copy-on-release.

**`Ctrl+C`.** With text selected, `Ctrl+C` copies rather than interrupting, as Windows Terminal does: the user
has just selected something, so there is nothing they could have meant to interrupt. Copying then CLEARS the
selection, and that is what makes the rule safe — the very next `Ctrl+C` is the interrupt again, so a selection
left behind by accident can cost one keypress and never more. macOS is excluded: copy has a key of its own
there (`⌘C`), so `Ctrl+C` has nothing to disambiguate and stays the interrupt in every state.

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

The timestamp is strict: it must be a valid ISO 8601 date-time with exactly
three fractional-second digits and either `Z` or a numeric timezone offset.
Invalid calendar dates, missing milliseconds, and other timestamp shapes are
dropped by the bridge.

`metadata` is free-form, but the tool name is keyed **`tool`**. That is not a
preference: all three shipped adapters in `remote/src/adapters/` emit it under
that exact key — the shared `pi-family.ts` mapping handles the Pi and Oh My Pi
adapters' `tool` field, and `claude-code.ts` renames the harness's `tool_name`
to it. An adapter that picks a different key compiles, validates and relays
fine, and the client simply never sees a tool name.

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

A pane the client mints is created WITH `harness = "generic"`
(`ch::SessionLayouts::mintTerminalPaneRow`), so this detection is on by default for
terminals opened from the UI. It has to be set at creation by somebody: for a long time
nothing wrote the column at all, which left every pane NULL, meant the clock never ran for
anyone, and made the sidebar row permanently read "Idle" — the visible bug that is the
reason this paragraph exists. The column remains the switch, not the default: a user can
change a pane's harness from the pane itself (naming its adapter, or "plain shell" to opt
out of the clock entirely), and a pane stored before this was fixed keeps its NULL,
and stays off the clock, until somebody sets one.

AUTODETECTION. A live agent event names the harness it came from, which is better
information than the column: an event can only come from an adapter that exists.
When one names a harness for a pane whose stored harness is exactly `generic`, the
client writes the observed name to the column, through the same
`workspace.updateTerminalPane` mutation the pane's own harness control uses, and
only on a real change — an agent repeats its harness on every event and this must
not become a write per event.

An observation can arrive before the client has any workspace tree to judge it
against: the agent bridge relays as soon as its socket is up, which on a cold
start is before `workspace.list` has answered. Because an observation is reported
only when the observed harness CHANGES, discarding that first one would lose the
detection for the whole session — the steady stream of same-harness events that
follows never mentions it again. Such an observation is held and settled against
the next authoritative tree, which either lists the pane (judge it by the rule
above) or proves it gone by listing its Dev Session without it (discard it). The
state the agent reported survives that wait: a pane row that exists only because
an event created it has never been registered from the tree, so registering it —
as `generic` first, and as the observed adapter once the write lands — must not
discard what the wire already said about it.

`generic` is the ONLY value an observation may overwrite. A NULL or empty harness
is the user's "plain shell" — they have said this pane is not an agent, so
nothing may relabel it on their behalf. That governs where the pane's state may
be DERIVED from, not what may be reported about it: output is never read as
activity for such a pane, and this write never happens, but a real agent event
naming the pane is a fact rather than a guess and is displayed like any other.
An explicit adapter is the user's answer to this same question and outranks an
observation. A user who deliberately chose "Generic agent" is indistinguishable
from the mint default and will be upgraded — accepted deliberately, rather than
recording which of two identical values was meant in the schema.

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
| Binary | metadata and a download action |

**Markdown renders; it does not open in the editor.** A `.md` file resolves to a
rendered document, and its SOURCE is reached by "Open as -> Editor" (§4.3), which
is also how it is changed. The renderer is a web document like the terminal and
editor surfaces, and it follows the active theme live (§4.6).

Markdown from the server is UNTRUSTED input: it may contain raw HTML, script
tags, event-handler attributes and `javascript:` or `data:` URLs. The rendered
HTML is therefore sanitised with a maintained sanitiser before it reaches the
document, the page's content policy forbids inline and injected script outright,
and the page may reach nothing but the internal scheme it was served from — no
network, no filesystem, no embedded frames. Sanitising strips the offending
construct and keeps rendering the rest: a document with one stray tag must stay
readable. The page's bridge to the application is deliberately minimal, and in
particular exposes no "read any path" call.

**Default handler per file type.** The table above is the DEFAULT mapping, not a
fixed one: the user may set which handler opens which file extension (§4.6). The
resolution order is an explicit "Open as" choice first, then the user's mapping,
then the built-in table. Only handlers that can actually display a file type may
be chosen for it — any extension may be opened as text or as a binary/metadata
view, while the display-specific handlers are offered only where they genuinely
claim the type, so a mapping cannot produce a broken pane. Extensions the built-in
table has never heard of are still mappable, because that is the case the setting
exists for. Changing the mapping applies to panes already open, not only to the
next file opened.

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
> `aggregateRowState()` in `src/models/SessionState.cpp`, takes six booleans — error,
> waiting-for-input, running, finished-with-unseen-output, connected, disconnected — all derived from
> terminal and coding-agent conditions. No file state is passed in, so an unsaved
> buffer never reaches the sidebar row. Tracked in `docs/PLAN.md`.

### 8.3 Remote File API

The currently implemented file RPC surface is:

```text
stat(path)
readFile(path, offset?, length?)
writeFile(path, content, expectedRevision, encoding?, mode?)
watch(path)
unwatch(subscriptionId)
resolvePath(path, base?)
listDirectory(path)
```

`offset` and `length` are non-negative byte ranges. `encoding` is either
`utf-8` or `base64`; base64 input is checked against the complete alphabet,
padding grammar, and group length before it is decoded. Malformed base64 is
reported as an **Invalid params** RPC error, not as an internal server error.
`mode` is an optional POSIX mode used, among other things, for private recovery
snapshots.

The server refuses a full read or requested range over 16 MiB, and refuses a
serialized `readFile` response (regardless of content encoding) over 15 MiB. A
serialized `listDirectory` response also has a 15 MiB limit. These refusals use
the resource-limit error described in §10.3 rather than returning partial data.

The editor's initial editing support requires:

```text
stat
readFile
writeFile
watch
resolvePath
```

`listDirectory` supports the remote directory viewer, and `unwatch` releases a
watch subscription when a pane closes.

These operations remain planned rather than registered RPC methods:

```text
createFile(path)
createDirectory(path)
rename(source, destination)
copy(source, destination)
delete(path)
```

There is no MIME-type call, and none is planned. The client picks a viewer by
file extension in `ViewerHandlerRegistry`, and the one place a real content type
is needed — serving bytes to the embedded browser — reads it from Qt's own MIME
database in `src/viewers/InternalUrlSchemeHandler.cpp`. A server-side table
existed for a while and was never called by anything; it has been removed.

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

Watch changes are sent as the id-less `file.watchEvent` notification. If the
client stops reading, the daemon coalesces pending events by subscription and
path in a bounded queue (at most 512 entries and 1 MiB). Events that still
cannot fit are dropped and reported through `file.watchEventsLost`, naming the
subscriptions whose paths must be re-read. The daemon also refuses a 513rd
live watch with the resource-limit error.

### 8.8 File Type Behavior

| Type | Default behavior |
|---|---|
| Source code | Monaco editor |
| Plain text | Monaco editor |
| Markdown | rendered document; "Open as → Editor" opens the source |
| JSON, YAML, TOML | Monaco editor |
| HTML | source editor or rendered page |
| Images | image viewer |
| PDF | PDF viewer |
| Directory | remote directory browser |
| Binary | metadata and a download action |
| Very large text file | no streaming viewer yet; oversized inline reads are refused with “file is too large to display inline” |
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

The stdio daemon closes its watch handles and exits cleanly when stdin reaches
end-of-input or when it receives `SIGHUP`, `SIGINT`, or `SIGTERM`; it does not
need to remain running between SSH sessions.

### 10.2 Responsibilities

`codeharbord` handles:

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
- session recovery.

Git metadata integration remains planned and is not currently exposed by the
remote service.

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

- **Frames are bounded.** An input or output JSON line over 16 MiB is not
  accepted; the daemon drops the transport instead of buffering an unbounded
  request or response.
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

- **Invalid parameters are client errors.** A malformed field, including
  malformed base64 content or a wrong field type, is answered with Invalid
  params (`-32602`), not Internal error (`-32603`).
- **Unknown method** yields Method not found (`-32601`), and it outranks bad
  params. An exception escaping a handler yields Internal error (`-32603`).
- **A notification never receives a response.** A request with no `id` member is
  dispatched for its side effects only and answered with nothing at all, whether it
  succeeds, hits an unknown method, or throws. Server-initiated messages use the
  same id-less form — see the `file.watchEvent` notification in section 8.7.

- **Backpressure.** The daemon starts independent handlers as input lines arrive,
  so replies may be written in completion order and are correlated by id. If
  stdout is stalled, responses wait in a separate bounded queue of at most 512
  responses and 16 MiB of encoded lines; crossing either bound closes the
  transport rather than growing memory without limit.

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
    returned when the parameters are valid and nothing was applied, but the
    server refuses to answer at that size: a `file.readFile` full read or
    requested range over 16 MiB, a serialized read response over 15 MiB, a
    `file.listDirectory` whose serialized listing would exceed 15 MiB, or a
    `file.watch` past 512 live subscriptions (sections 8.3 and 8.7).

- **Liveness heartbeat.** JSON-RPC has no keep-alive of its own and a half-open
  SSH channel can stay silently "open" for hours, so the client probes the
  daemon with a `ping` request every 15 seconds once a transport is wired. Any
  inbound bytes at all count as liveness. After four consecutive intervals in
  which nothing was read — roughly a minute — the transport is declared lost:
  every pending call fails with the same transport error a real disconnect
  produces, and the reconnection ladder of section 5.6 takes over. An interval
  whose probe could not even be written counts as a silent one, because a failed
  write is not evidence of a live peer.

---

## 11. Persistence

### 11.1 Server Database

The authoritative SQLite database lives on the server.

Suggested location:

```text
~/.local/share/codeharbor/codeharbor.sqlite
```

The schema's metadata tables are:

```text
schema_version
server_identity
```

The domain tables are:

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

`server_identity` is the stable server-owned id returned by `server.info`; it
is not a client profile id. `schema_version` records the migration level.
Lookup indexes are kept separately in `remote/sql/indexes.sql` and are applied
on every database open, including databases already at the current schema
version.

The `dev_sessions` table carries `pinned` (§4.2) and `archived` (§4.2). `pinned` arrived with schema version 5; a
version 4 database gains the column with a default of "not pinned", so nothing already stored is lost and every
existing session simply starts unpinned. The workspace schema version is stated in the server's schema file, in
the daemon, and in the client's `WorkspaceDb::kSchemaVersion`, and those must move together; only the daemon
migrates, and the client's copy records what it expects to find. Note this is NOT the version that gates
compatibility — that is the separate RPC schema version (§10.3).

### 11.2 Local State

The client may store only:

- SSH server connection profiles;
- window geometry and region widths;
- client display preferences (theme, group-colour palette, toolbar order, terminal text size and rendering
  resolution) — see §4.6;
- which Dev Session was last open, per server, and which pane the user was last working in (§4.5);
- whether the sidebar's pin filter is on, and whether archived sessions are shown;
- which handler opens which file type (§4.6);
- SSH-agent or credential-store references;
- browser cookies and cache;
- temporary reconnect state.

The client must not store project repositories or project files.
The current profile store records connection metadata (including host, port, user,
remote Node path, repository root, and an optional local identity-file path) but
never stores a password, passphrase, private-key bytes, repository, or project
file.

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

> **Implementation status.** No credential store is integrated. A password or
> key passphrase is prompted for, handed straight to libssh for that one
> authentication attempt, and then discarded; nothing is written anywhere. The
> profile store (`src/app/ServerProfiles.h`) applies a field whitelist that
> drops a stray `password`/`passphrase` key before writing, so a secret cannot
> reach disk even by accident. The requirement above is therefore met in its
> strong form — no secret is persisted at all — at the cost of retyping it each
> connection.

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

These are target behaviors rather than current download/upload plumbing: the
client currently renders the external page in Qt WebEngine but does not yet
intercept its download destination or replace its file picker with an SSH-backed
picker.

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
    ├── markdown/
    └── editor/
```

Remote component:

```text
remote/
├── src/
│   ├── codeharbord.ts        RPC service (JSON-RPC 2.0 over newline-delimited JSON)
│   ├── rpc-types.ts          request/result shapes shared by the RPC modules
│   ├── files.ts              file.* methods: stat/readFile/writeFile/resolvePath/watch/unwatch/listDirectory
│   ├── workspace.ts          workspace.* methods over the SQLite database
│   ├── tmux.ts               tmux.* session discovery/kill methods
│   ├── validate.ts           request-parameter validation helpers
│   ├── events.ts             agent event schema + socket-path resolution
│   ├── bridge.ts             agent event relay
│   ├── adapters/             pi-family.ts (shared Pi mapping), oh-my-pi.ts,
│   │                         pi.ts, claude-code.ts, types.ts, index.ts (registry)
│   └── hooks/                oh-my-pi-hook.ts (the native harness hook)
└── sql/                      schema.sql, indexes.sql
```

Technologies actually used:

- Qt 6 (floor 6.10);
- Qt Quick and QML;
- C++20;
- Qt WebEngine;
- Qt WebChannel;
- libssh (runtime floor 0.11.2; exactly 0.12.0 is warned about and gets the
  ML-KEM subtraction workaround; the Windows vcpkg overlay is separately pinned
  to 0.12.2);
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
Ctrl+,          Settings
Ctrl+S          Save active remote file (inside the focused editor)
Ctrl+Shift+C    Copy the selection (inside the focused terminal; ⌘C on macOS)
Ctrl+C          Copy the selection, if there is one, and clear it; otherwise the
                interrupt (inside the focused terminal; never on macOS, where
                Ctrl+C is always the interrupt)
Ctrl+Shift+V    Paste the clipboard (inside the focused terminal; ⌘V on macOS)
```

`Ctrl+Shift+P` opens the palette (`activationSequence` in
`src/qml/CommandPalette.qml`; Qt maps `Ctrl` in a key sequence to Command on macOS,
so it is `⌘⇧P` there). `Ctrl+Shift+O`, `Ctrl+Shift+R`, `Ctrl+Shift+W` and `Ctrl+,` are
`shortcut` entries on the command list in `src/qml/Main.qml`, which the palette
turns into real window-wide `Shortcut` objects, so they fire whether or not the
palette is open. `Ctrl+S` is registered on the Monaco instance itself in
`src/web/editor/src/index.ts`, so it applies to the focused editor rather than
window-wide. `Ctrl+Shift+C` and `Ctrl+Shift+V` are likewise the terminal page's
own, so they apply to the focused terminal and leave the rest of the window
alone; they are the keyboard half of the terminal's mouse and clipboard rules
(§5.7).

Two of these deviate from the plain sequence this section originally suggested,
for the same reason: a window-wide `Shortcut` is matched before the key ever
reaches the focused item, and every terminal pane hosts a real shell.
`Ctrl+R` is reverse-history-search there, and `Ctrl+W` is delete-word, so
**Refresh Workspace** is `Ctrl+Shift+R` and **Close Window** is `Ctrl+Shift+W`.
`Close Window` exists as a binding at all because the window is frameless and
therefore has no window-manager close button of its own.

Being frameless also means the window must ask the SYSTEM to move and resize it rather than repositioning
itself, or it loses every gesture the window manager attaches to those operations — on Windows, dragging to the
top to maximise, to a side to half-tile, and the snap-layouts flyout on the maximise button. Qt's
`startSystemMove()`/`startSystemResize()` provide the portable half. On Windows a frameless window is also
created without the native style bits the shell requires before it will snap a window at all, so the client
restores those (keeping the caption itself absent) and answers the hit test for its own maximise button, which
is what the snap-layouts flyout is attached to. Nothing about this changes behaviour on Linux or macOS.

Every remaining command — splitting a viewer or terminal pane, closing a focused
pane, disconnecting from the server, marking agent output seen — is reachable
through the palette only and carries no key sequence. Killing a terminal's remote
tmux session is not among them and has no key sequence either: it happens only
as part of a terminal pane's own confirmed close, or as part of a confirmed
deletion of the Dev Session or group that owns the pane (§4.4).

Originally suggested defaults, and how they were reconciled:

| Suggested | Status |
|---|---|
| `Ctrl+K` Command palette | Superseded by `Ctrl+Shift+P` (`activationSequence` in `src/qml/CommandPalette.qml`). `Ctrl+K` is not bound at all. |
| `Ctrl+P` Switch Dev Session | Not implemented — there is no session switcher. |
| `Ctrl+Shift+T` New terminal | Not implemented as a key sequence; a new terminal pane comes from the palette's "Split Terminal Pane Horizontally/Vertically". |
| `Ctrl+Shift+V` New viewer | Not implemented as a key sequence, and it will not be: inside a terminal pane `Ctrl+Shift+V` is paste (§5.7). A new viewer pane comes from the palette's "Split Viewer Pane Horizontally/Vertically". |
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
