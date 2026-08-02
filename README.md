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
  web/          bundled web assets (terminal, editor)
remote/         server-side service, agent bridge, harness adapters
docs/           SPEC.md, PLAN.md, DEVELOPMENT.md
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

Both need `tar` plus either `curl` or `wget` on the server. Without outbound
network access, stage `codeharbor-remote.tar.gz` on the server and set
`CH_REMOTE_ARTIFACT_URL` to its path — it is copied instead of downloaded. Set
the same variable to an internal mirror URL to install from there.

A **git checkout** is never overwritten by either path. **Update server** on one
says so and changes nothing: update it with `git pull` and a build instead.

### 2. The client

| Platform | Asset | Notes |
|---|---|---|
| Linux | `CodeHarbor-x86_64.AppImage` | `chmod +x` then run. Needs FUSE and an X11/XWayland display. |
| macOS | `codeharbor.dmg` | Unsigned: right-click → **Open**, or `xattr -d com.apple.quarantine`. |
| Windows | `CodeHarbor-<version>-windows-x64-setup.exe` | Per-user installer, no admin rights. SmartScreen will warn (unsigned). |
| Windows (portable) | `codeharbor-windows.zip` | Extract, run `codeharbor.exe`. Same files, no shortcuts or uninstaller. |

### 3. Connect

Launch it, add a server in the connect sheet, and fill in:

- **Host / Port / User** — your SSH details.
- **Private key file** — optional local path to the key you want this profile to
  use. CodeHarbor also reads `~/.ssh/config`, including `IdentityFile` entries.
- **Node path** — absolute path to `node` on the server (`ssh <host> command -v node`).
  It is not looked up on `PATH`, because a non-interactive SSH session often
  does not have the one you expect.
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
| `Ctrl+Shift+W` | Close Window (the window is frameless, so it has no close button) |
| `Ctrl+S` | Save the file in the focused editor |

Splitting and closing panes, killing a terminal's remote tmux session, disconnecting,
and marking agent output seen are palette commands with no key sequence. There is no
Dev Session switcher shortcut and no "focus next pane" shortcut.

## Build

Full environment setup (all platforms, exact packages, troubleshooting) is in
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md). Quick reference:

### Client (Qt / CMake)

Requires Qt 6.9+, a C++20 compiler, CMake 3.25+, Ninja, and libssh.

```bash
npm install                   # once: builds need the web-asset workspaces
cmake --preset dev            # configure (also builds the web bundles)
cmake --build --preset dev    # -> build/dev/src/app/codeharbor
```

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
or deleting a group (creating one is wired); crash-recovery snapshots that are taken
but never offered back; a reconnect that cannot prompt for a newly-unknown host key;
and a connect that briefly blocks the GUI thread.

**Building requires Node** — the Monaco and xterm.js bundles are build artifacts
embedded as Qt resources, and CMake builds them at configure time (it refuses to
configure a client whose editor or terminal pane would silently load nothing;
`-DCODEHARBOR_SKIP_WEB_BUNDLE=ON` opts out).

## License

MIT — see [`LICENSE`](LICENSE).
