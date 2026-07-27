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
  app/          application entry + QML
  models/       core data model (Group, DevSession, panes)
  persistence/  Qt SQL access to the remote SQLite workspace DB
  ssh/          libssh connection pool, channels, host-key handling
  terminal/     TerminalController, buffering, reconnect
  viewers/      viewer handler registry, WebEngine profiles
  remote/       client-side RPC client for codeharbord
  qml/          shared QML components
  web/          bundled web assets (terminal, editor)
remote/         server-side service, agent bridge, harness adapters
docs/           SPEC.md, PLAN.md
```

## Build

Full environment setup (all platforms, exact packages, troubleshooting) is in
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md). Quick reference:

### Client (Qt / CMake)

Requires Qt 6.9+, a C++20 compiler, CMake 3.24+, Ninja, and libssh.

```bash
cmake --preset dev            # configure
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
list beside it is deliberately honest about what is still missing: no UI for creating,
renaming or deleting a group; crash-recovery snapshots that are taken but never
offered back; a reconnect that cannot prompt for a newly-unknown host key; and a
connect that briefly blocks the GUI thread.

**Building requires Node** — the Monaco and xterm.js bundles are build artifacts
embedded as Qt resources, and CMake builds them at configure time (it refuses to
configure a silently editor-less client; `-DCODEHARBOR_SKIP_WEB_BUNDLE=ON` opts out).

## License

MIT — see [`LICENSE`](LICENSE).
