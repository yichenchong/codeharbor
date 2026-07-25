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

Requires Qt 6.6+, a C++20 compiler, CMake 3.24+, Ninja, and libssh.

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

Milestones **M1–M5 are reached LIVE**, not merely unit-tested: every workstream's
live gate now runs against a real `sshd` and a real `codeharbord`, covering SSH
transport, terminal (tmux attach / remote-confirmed resize / reconnect), workspace
CRUD + persistence, remote viewers, remote editing in real Monaco, and agent
awareness driven by the real hook.

Those gates are QTest targets labelled `live`; they **QSKIP** unless `CH_LIVE_SSH`
is set, so `ctest --preset dev` stays portable. To run them you need a reachable SSH
server, `tmux`, and Node on the remote side — see
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).

Exercising them for the first time paid for itself: it exposed a missing transport
spine (nothing carried RPC over SSH), QML that could not load at all, split panes
rendering at zero width, stored region widths being destroyed on launch, and
non-deterministic sidebar ordering. See the Wave 5 entry and correction note in
[`docs/PLAN.md`](docs/PLAN.md) for the full list and what remains.

**Building requires Node** — the Monaco editor bundle is a build artifact embedded
as a Qt resource, and CMake builds it at configure time (it refuses to configure a
silently editor-less client; `-DCODEHARBOR_SKIP_WEB_BUNDLE=ON` opts out).

## License

MIT — see [`LICENSE`](LICENSE).
