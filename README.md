# CodeHarbor

A cross-platform desktop application for managing multiple persistent **remote**
development workspaces (Dev Sessions) from a single interface.

CodeHarbor is remote-first: repositories, files, shells, coding agents, tmux
sessions, and authoritative workspace state all live on a configured SSH server.
The client provides only UI, rendering, input, and transport.

> Full product/technical specification: [`docs/SPEC.md`](docs/SPEC.md)
> Delivery plan and parallelizable TODOs: [`docs/PLAN.md`](docs/PLAN.md)

## Architecture at a glance

```text
┌────────────────┬──────────────────────────────┬──────────────────────────┐
│ Sessions       │ Viewers                      │ Terminals                │
└────────────────┴──────────────────────────────┴──────────────────────────┘
       client (Qt Quick / QML + C++20)  ── SSH ──▶  remote SSH dev server
                                                     ├── codeharbord (RPC)
                                                     ├── codeharbor-bridge (agent events)
                                                     ├── tmux (process persistence)
                                                     └── repositories
```

- **Client** (`src/`): Qt 6, Qt Quick/QML, C++20, Qt WebEngine, Qt WebChannel,
  libssh. Terminals use xterm.js; the editor uses Monaco (`src/web/`).
- **Remote** (`remote/`): `codeharbord` RPC service, `codeharbor-bridge` agent
  event relay, and per-harness adapters. TypeScript/Node, zero runtime deps.

## Repository layout

```text
src/            C++/QML client
  app/          application entry (main.cpp) + AppController, session bootstrap
  models/       core data model (Group, DevSession, panes)
  persistence/  workspace load/save, over codeharbord RPC (no local database)
  ssh/          libssh connection pool, channels, host-key handling
  terminal/     TerminalController, buffering, reconnect
  viewers/      viewer handler registry, WebEngine profiles
  remote/       client-side RPC client for codeharbord
  editor/       EditorController: remote file state machine, guarded saves
  agent/        AgentStatusMonitor: coding-agent status over the bridge
  qml/          shared QML components
  web/          bundled web assets (terminal, editor, markdown)
remote/         server-side service, agent bridge, viewer-control channel,
                harness adapters, the codeharbor-mcp server and codeharbor-view CLI
docs/           SPEC.md, PLAN.md, DEVELOPMENT.md, plus dated bug-hunt records
```

## Install

CodeHarbor has two halves: a **client** on your desktop and a small **service**
on the dev server. Both ship in every
[release](https://github.com/yichenchong/codeharbor/releases).

### 1. The server

Needs SSH access, `tmux`, and Node 23.6+. Unpack the tarball anywhere you like:

```bash
mkdir -p ~/codeharbor
curl -fsSL https://github.com/yichenchong/codeharbor/releases/latest/download/codeharbor-remote.tar.gz \
  | tar -xz -C ~/codeharbor
```

That is the whole install: the service has **zero runtime dependencies**, so
there is no `npm install` and no `node_modules`. You get `dist/`, `sql/` and
`package.json`, and nothing needs to be running — the client starts
`codeharbord` over SSH on demand and it exits when you disconnect.

You can also skip this step entirely: point a profile's **Repository root** at a
directory that does not exist yet and the client installs the matching release
there on the first connect.

### Updating the server

The client and the service are released together, and the client only drives the
release that matches it. Two things keep them in step:

- **Automatically.** When a client installed the service itself, upgrading the
  client makes the next connect replace the copy on the server with the matching
  release. Nothing to do.
- **On demand.** Open **Server…**, select the profile, and press **Update
  server**. This installs the release matching this client into that profile's
  repository root and reconnects. Use it when the server was set up by hand (the
  `curl … | tar -xz` above), which the automatic path deliberately never
  overwrites, or after a client update if you want to force the issue.

URL-based automatic or on-demand updates need `tar` and either `curl` or `wget` on the server. For an air-gapped install, stage `codeharbor-remote.tar.gz` on the server and set `CH_REMOTE_ARTIFACT_URL` to its local path; that mode copies the tarball and needs `tar` but no downloader. Set the same variable to an internal mirror URL when downloading from a mirror.

A **git checkout** is never overwritten by either path. **Update server** on one
says so and changes nothing: update it with `git pull` and a build instead.

**A failed update costs you nothing but the update.** The archive is unpacked
into a staging directory beside the installation and only swapped in once it has
been proven to hold a service this client can launch, with whatever it displaces
kept until the swap has finished — so a truncated download, a full disk or a
dropped connection puts the previous installation back, release marker included.
A prerequisite the server does not meet (a Node older than the service needs, no
`tar`, no downloader) is reported the same way: the update does not happen, the
copy that was working stays exactly where it was, and the session connects to it.
You are only refused outright when there is nothing installed there to fall back
to.

### 2. The client

| Platform | Asset | Notes |
|---|---|---|
| Linux | `CodeHarbor-<version>-x86_64.AppImage` | `chmod +x` then run. Needs FUSE and an X11/XWayland display. |
| macOS | `CodeHarbor-<version>-macos-<arch>.dmg` | Unsigned: right-click → **Open**, or `xattr -d com.apple.quarantine`. |
| Windows | `CodeHarbor-<version>-windows-x64-setup.exe` | Per-user installer, no admin rights. SmartScreen will warn (unsigned). |
| Windows (portable) | `codeharbor-windows.zip` | Extract, run `codeharbor.exe`. Same files, no shortcuts or uninstaller. |

### 3. Connect

Launch it, add a server in the connect sheet, and fill in:

- **Host / Port / User** — your SSH details.
- **Private key file** — optional local path to the key you want this profile to
  use. CodeHarbor also reads `~/.ssh/config`, including `IdentityFile` entries.
- **Node path** — absolute path to `node` on the server (`ssh <host> command -v node`).
  It is not looked up on `PATH`, because a non-interactive SSH session often
  does not have the one you expect. **Required**: the mobile connect form
  refuses to submit without it rather than letting the failure surface as a
  server-side "no Node" report a handshake later. A profile saved blank by an
  older build falls back to a bare `node` so it probes something runnable, but
  that fallback is a guard, not a substitute for the path.
- **Repository root** — where you unpacked the tarball (`~/codeharbor` above).
  A git checkout of this repo works too.

Authentication tries a libssh-readable SSH agent, then configured/default keys.
On Windows, the built-in OpenSSH agent is a named pipe that libssh cannot use:
set **Private key file** to the local private key for that profile instead of
relying only on `ssh-add`. An encrypted key prompts for its **private-key
passphrase** and supplies it only to key authentication; it is never sent as
the server account password. Choose **Use password** to authenticate with the
server password instead. Neither secret is stored. The first connection shows
the host's fingerprint and refuses to continue until you accept it — an unknown
key is never trusted silently.

**Diagnose an SSH failure:** open **Server…** in the sessions sidebar and read the connection error
banner. It reports whether `SSH_AUTH_SOCK` reached the CodeHarbor process,
whether a profile key path was found, and libssh's final error. **Details…** on
that banner opens the full connection log for the attempt — the libssh version in
use, which SSH config was parsed, every handshake stage, and libssh's own trace —
selectable so it can go into a bug report. A desktop
launcher often does not inherit the agent socket from a terminal; set **Private
key file** to the local key (absolute paths and `~/…` are accepted) to bypass
that environment boundary. Do not paste a passphrase into logs or bug reports.

Then create a group, add a Dev Session pointing at a project directory on the
server, and you have terminals, an editor, and viewers against that project.

> **Install path last verified end to end at `v0.1.0`**, by installing the
> published artifacts: the downloaded AppImage, pointed at an unpacked
> `codeharbor-remote.tar.gz`, connected over SSH and launched both remote
> services from `dist/`. No source checkout involved. Later releases have not
> been re-verified that way, so this is a record of one dated run rather than a
> statement about the current release.

## Keyboard shortcuts

Almost everything is reached through the command palette rather than a key sequence.
The complete set of bindings — with the reasoning, and the reconciliation against the
shortcuts SPEC 15 originally suggested, in [`docs/SPEC.md`](docs/SPEC.md) — is:

| Keys | Action |
|---|---|
| `Ctrl+Shift+P` (`⌘⇧P` on macOS) | Open the command palette |
| `Ctrl+Shift+O` | Connect to Server… |
| `Ctrl+Shift+R` | Refresh Workspace |
| `Ctrl+Shift+W` | Close Window (the window is frameless, so the window manager draws no close button; the in-app title bar has one) |
| `Ctrl+S` | Save the file in the focused editor |
| `Ctrl+Shift+C` (`⌘C` on macOS) | Copy the selection in the focused terminal |
| `Ctrl+C` | In a terminal with text selected: copy it and clear the selection. With nothing selected it is the usual interrupt, and on macOS it is always the interrupt |
| `Ctrl+V` (`⌘V` on macOS) | Paste into the focused terminal. `Ctrl+Shift+V` does the same. Because `Ctrl+V` pastes, it no longer reaches the shell as `^V` (readline's literal-next); on macOS plain `Ctrl+V` still does |
| `Ctrl+,` | Settings |

Splitting and closing panes, killing a terminal's remote tmux session, disconnecting,
marking agent output seen, renaming a terminal pane, pinning a Dev Session, and
showing the log are palette commands or in-place controls with no key sequence.
There is no Dev Session switcher shortcut and no "focus next pane" shortcut.

A few things worth knowing about the shipped client:

- **Settings** (`Ctrl+,`) has Appearance, File viewers, Server and Tmux groups.
  Appearance carries the Dark/Light theme, the palette that tints Dev Session group
  names, the order of the viewer's toolbar buttons, and the terminal's text size and
  rendering resolution. File viewers is where you choose which viewer opens which kind
  of file — for example open `.md` in the editor instead of the renderer.
- **Markdown files render**, with headings, tables, code blocks and task lists,
  themed to match the app. "Open as → Editor" gives you the source to edit.
- **Split, close and kill are on each pane's own header**, so they act on the pane
  you are looking at rather than on whichever pane the app thinks is focused. The
  same commands remain in the palette for keyboard use. Killing a terminal asks
  first, because it destroys running remote work.
- **Reopening a Dev Session puts the keyboard back where you left it.** If you were
  typing in a terminal, you can keep typing without clicking first.
- **Viewer panes navigate like a browser**: Back, Forward, Reload and Home beside the
  address field, with a history per pane. The address field opens on Enter.
- **The explorer can open a file with a specific viewer** ("Open as"), in place or in
  a new pane, and can hand it to a local application by `<appName>://` scheme.
- **Terminal panes can be renamed** from their header, and the name travels with the
  layout, so it is there after a restart and on another machine.
- **Each Dev Session shows what it is doing** — the coloured dot beside its name in
  the sidebar. A pane running a supported coding agent reports its own status
  (running, waiting for you, finished); an ordinary terminal is worked out from
  whether it is printing anything. Panes are set to that second mode when they are
  created, and the gear button in a terminal's header changes it — name the agent
  the pane runs, or pick "Plain shell" to stop CodeHarbor guessing from output. A
  pane whose agent reports its own status still shows it either way.
- **That keeps working for the sessions you are not looking at**, which is the
  point of the dot. A supported agent reports over its own channel, so switching
  away changes nothing. An ordinary terminal is judged from tmux's own record of
  when each session last printed, asked for on a short poll, so a build or a test
  run in a session you left behind still goes quiet-then-idle on screen. A pane
  CodeHarbor cannot date at all reads "unknown" rather than guessing "idle" — the
  dot never claims a session finished when nobody knows.
- **A coding agent has to be connected once before it can report anything.** The
  status a supported agent reports comes from a small module you point the agent
  at, not from CodeHarbor watching it. For Oh My Pi, link it into your agent's
  extension directory **once per machine** — not per pane, and not per session:

  ```bash
  mkdir -p ~/.omp/agent/extensions
  ln -s /path/to/codeharbor/remote/src/hooks/oh-my-pi-extension.ts \
        ~/.omp/agent/extensions/codeharbor.ts
  ```

  Every `omp` you start from then on loads it, including agents you start inside
  CodeHarbor panes, and a symlink keeps it current when CodeHarbor updates.
  `~/.omp/agent/hooks/pre/` works the same way; `<project>/.omp/extensions/` limits
  it to one project. To try it once without installing anything, pass
  `omp --hook=/path/to/.../oh-my-pi-extension.ts` on a single run.

  Until this is done the pane has no agent status at all and falls back to "is it
  printing?", which cannot tell finished from busy — an agent that redraws a spinner
  looks like work forever. The module does nothing when it is not running inside a
  CodeHarbor pane, so it is safe to leave enabled everywhere.
- **Dev Sessions can be pinned**, and the sidebar can be filtered to show only pinned
  ones. Pins live on the server; the filter is local to the machine.
- **Dev Sessions can be archived** to get them out of the sidebar without losing
  them, and shown again with a toolbar toggle. Archiving is stored on the server;
  whether archived rows are visible is local to the machine.
- **Sessions and groups can be deleted.** Both ask first and name what they are
  about to destroy; deleting a group also states how many sessions go with it.
  Deleting is permanent — archiving is the reversible option.
- **Drag to select text; the highlight stays put.** A plain left drag selects in
  CodeHarbor, exactly as it does in any other desktop application, and the
  selection survives until you type or select something else. Nothing is copied
  until you ask: `Ctrl+Shift+C` (`⌘C` on macOS), or plain `Ctrl+C` with something
  selected — which clears the selection, so the next `Ctrl+C` interrupts as usual.
  (On macOS `Ctrl+C` is always the interrupt.) Paste with `Ctrl+V` (`⌘V`),
  `Ctrl+Shift+V`, or the right-click menu.
- **Hold Shift and drag to give the mouse to the program** (**Option and drag** on
  macOS). Each tmux session has mouse reporting on, so that is how vim, htop or
  lazygit see clicks and drags. The wheel is never taken away: it always scrolls
  tmux's history, which is why mouse reporting is on at all. (A middle click goes
  to tmux, which pastes its own last copy rather than the system clipboard.)
- **Right-click opens CodeHarbor's own terminal menu** — Copy, Paste and Select All,
  with Copy greyed out when nothing is selected. The right button is the one button
  that is not passed to the remote side, because tmux's own right-button menu is
  drawn inside the terminal grid and closes on the next mouse report, so moving the
  pointer towards it made it vanish.
- **Several servers can be saved** and switched between; closing the app leaves the
  remote tmux sessions running, so a reconnect finds the shells where they were.

## Letting your coding agent drive the viewer panes

An AI agent running in a CodeHarbor terminal pane can put things on your screen —
a file, a directory listing, a rendered document, a web page — instead of pasting
them into the terminal. It can also split, focus, reload and close viewer panes.

It works because the agent talks to a Unix socket the server-side service owns,
and the desktop client picks the command up over the SSH connection it already
has. Nothing is exposed to the network, and every command goes through the same
code path a click does, so it lands on the layout that is on screen and persists
the same way.

Set-up is per assistant. The repository ships the pieces for three:

| Assistant | What to do |
| --- | --- |
| **Claude Code** | Nothing. `.mcp.json` and `.claude/skills/codeharbor-viewer` are in the repository; approve the project's MCP server when Claude asks. |
| **Codex** | Once: `codex mcp add codeharbor -- node /path/to/codeharbor/remote/src/mcp/server.ts`. The skill is at `.agents/skills/codeharbor-viewer`. |
| **Oh My Pi / pi** | Nothing. `.omp/mcp.json` and `.omp/skills/codeharbor-viewer` are in the repository. |

The tools are `viewer_list`, `viewer_open`, `viewer_split`, `viewer_focus`,
`viewer_close` and `viewer_reload`. From a shell — for a harness with no MCP
support, or to check that the path works at all:

```bash
node remote/src/tools/viewctl.ts list
node remote/src/tools/viewctl.ts open --url README.md --new-pane
node remote/src/tools/viewctl.ts close --pane viewer-2
```

Three things to know:

- **It only drives the Dev Session you are looking at.** A command from a pane of
  another session is refused by name rather than applied to the wrong layout.
- **Closing a pane does not ask about unsaved editor changes**, the same as the
  command palette's Close Pane. The shipped skills tell the agent to ask you first.
- **It needs a pane CodeHarbor created.** Three variables are exported into each
  pane's tmux session: `OMP_DEV_SESSION_ID` and `OMP_TERMINAL_ID` say which pane
  this is, and `CODEHARBOR_CONTROL_SOCKET` says which window owns it. A shell that
  was already running before CodeHarbor attached has none of them, and an agent
  without the socket is told so rather than being routed to another window.
  `echo "$CODEHARBOR_CONTROL_SOCKET"` says which case you are in.
- **Several windows work independently.** Each CodeHarbor window has its own socket,
  so an agent drives the window whose pane it runs in. The one exception is a
  terminal pane you have open in two windows at once — that is one shell shown
  twice, and the window that attached most recently owns agents started after it.

## Build

Full environment setup (all platforms, exact packages, troubleshooting) is in
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md). Quick reference:

### Client (Qt / CMake)

Requires Qt 6.10+, a C++20 compiler, CMake 3.25+, Ninja, and libssh.

```bash
npm install                   # once: builds need the web-asset workspaces
cmake --preset dev            # configure (also builds the web bundles)
cmake --build --preset dev    # -> build/dev/src/app/codeharbor (Linux/Unix path)
```

macOS builds an app bundle (`build/dev/src/app/codeharbor.app`) and Windows puts
`codeharbor.exe` in the build root beside its DLLs; see
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).

### Remote service (Node)

Requires Node 23.6+ (tests run TypeScript via native type stripping).

```bash
cd remote
npm install        # dev-only deps; runtime has zero deps
npm test           # node --test
npm run build      # tsc -> dist/
```

## Status

CodeHarbor is **usable end to end**: launch it with no configuration, add a server
in the connect sheet, accept its host key, create a group and a Dev Session in it,
and you get live remote shells in terminal panes, remote files in Monaco with
revision-guarded saves, and your layout — including which files were open — back
where you left it on the next launch.

That walkthrough is covered by `tst_coldstart`, which runs against a real `sshd` and
a real `codeharbord` on every live run. One honest caveat: `tst_coldstart` creates
its Dev Session over raw RPC rather than through the sidebar, so the UI path for
that one step is covered separately by `tst_sidebar` against a fake controller. A
gate that exercises what the user cannot is exactly the mistake this project has
made before, so it is called out rather than papered over.

Milestones **M1–M5 are reached LIVE**: SSH transport, terminal (tmux attach /
remote-confirmed resize / reconnect), workspace CRUD + persistence, remote viewers,
remote editing, and agent awareness driven by the real hook each have a gate that
exercises the real thing.

Those gates are QTest targets labelled `live`; they **QSKIP** unless `CH_LIVE_SSH`
is set, so `ctest --preset dev` stays portable. To run them you need a reachable SSH
server, `tmux`, and Node on the remote side — see
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).

Exercising real paths has repeatedly paid for itself. It exposed a missing transport
spine (nothing carried RPC over SSH), QML that could not load at all, split panes
rendering at zero width, stored region widths destroyed on launch, splitting a pane
killing the terminal you were working in, password authentication that no UI could
ever reach, a client that could not launch its own released server tarball, and —
worst — two ways an attacker's SSH host key could be accepted: one where unknown keys
were trusted silently, and one where presenting a different key ALGORITHM downgraded
a hard refusal into a friendly "trust this new host?" prompt.

The corrections note in [`docs/PLAN.md`](docs/PLAN.md) records each one, and the gap
list beside it is deliberately honest about what is still missing: no UI for renaming
a group (group deletion is wired and confirmed, including its session-count warning);
reconnect cannot prompt for a newly-unknown host key; and a connect briefly blocks
the GUI thread. Crash-recovery snapshots now offer an explicit Restore or Discard
choice when the file is reopened.

**Building requires Node** — the Monaco, xterm.js and Markdown bundles are build
artifacts embedded as Qt resources, and CMake builds them at configure time (it
refuses to configure a client whose editor, terminal or Markdown pane would
silently load nothing; `-DCODEHARBOR_SKIP_WEB_BUNDLE=ON` opts out).

## License

MIT — see [`LICENSE`](LICENSE).
