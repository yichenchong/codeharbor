# CodeHarbor — Delivery Plan

Concrete, parallelizable workstreams with explicit **start gates** (preconditions)
and **stop gates** (verifiable done criteria). Each workstream exposes a small
contract so downstream work can build against it before it is fully implemented.

- Spec: [`SPEC.md`](SPEC.md)
- Workstream IDs (`M`, `P`, `S`, …) are referenced from source comments to link
  code back to this plan.

## How to use this plan

1. **Freeze contracts first** (§ Contracts). These are small, blocking, and owned
   centrally — do them before fanning out.
2. **Dispatch a wave.** A workstream may start once every box in its *Start gate*
   is checked. Workstreams in the same wave have no edges between them and run in
   parallel.
3. **Stop at the gate.** A workstream is *done* only when its *Stop gate* is
   verified by the stated command/scenario — not when it compiles.
4. **Milestones are barriers.** A milestone integrates several stop gates into a
   user-visible capability (mapped to SPEC §16 phases).

## Current state (bootstrap — DONE, verified)

- Repo initialized (`main`), license, ignore/attrs/editorconfig.
- Build wiring: top-level `CMakeLists.txt` + `CMakePresets.json`; per-module
  `ch_*` static libs; `src/qml` QML module; `codeharbor` executable; npm
  workspace root.
- C++/QML skeleton compiles-by-construction: `main.cpp` (WebEngine init + QML
  load), three-region `SplitView` layout, per-region placeholder panes.
- Model enums with real logic: `SessionState.{h,cpp}` (terminal/agent/row/file
  state + `aggregateRowState`), typed `Ids.h`.
- **Remote workspace fully implemented + tested (Node, zero deps):**
  - `events.ts` — internal event schema, validation, socket-path resolution.
  - `adapters/` — `oh-my-pi` (SPEC 6.5 mapping), `pi`, `claude-code`, registry.
  - `bridge.ts` — Unix-socket → JSONL relay (dir-create + stale-socket guard).
  - `codeharbord.ts` — JSON-RPC 2.0 `--stdio` dispatch (`ping`, `server.info`).
  - **Verified:** `npm test` → 11/11 pass; RPC stdio + bridge socket smoke-tested.
- CI: `remote` job (install/typecheck/test) and `client` job (Qt+CMake build).

> Everything below is **not yet built**. No stubs are presented as complete; the
> `throw new Error("not implemented")` seams in `src/web/*` and the thin `ch_*`
> libs are explicitly bootstrap placeholders scoped to the workstreams here.

## Contracts (freeze before fan-out)

These are the cross-workstream interfaces. Land them first; changing them later
is a coordinated break.

- [x] **C1 — RPC method catalog.** Extend `codeharbord.ts` with typed
  request/result shapes for the SPEC 10.2 methods (start with the file set:
  `stat`, `readFile`, `writeFile`, `watch`, `resolvePath`). Publish as
  `remote/src/rpc-types.ts`; mirror in C++ `src/remote/`. Owner: R.
- [x] **C2 — Workspace DB schema.** Author `remote/sql/schema.sql` for the SPEC
  11.1 tables (`groups`, `dev_sessions`, `viewer_panes`, `terminal_panes`,
  `session_layouts`, `server_profiles`, `server_settings`, `app_settings`) with
  `server_id` columns (SPEC 3.5). Bump `WorkspaceDb::kSchemaVersion`. Owner: P.
- [x] **C3 — WebChannel bridge interfaces.** Freeze the JS↔C++ object shapes:
  `TerminalBridge`/`TerminalHost` (`src/web/terminal`) and
  `RemoteEditorBridge`/`EditorHost` (`src/web/editor`). Owners: T, E.

**Status: LANDED** — delegated to three parallel subagents,
adversarially reviewed, and verified (remote typecheck + 15/15 tests; cmake
configure+build+link). Wave 1 (S / M / R-server) is now unblocked.

## Dependency graph

```mermaid
graph TD
    F[Bootstrap: DONE] --> C[Contracts C1-C3]
    C --> M[M models]
    C --> S[S ssh transport]
    C --> RS[R-server codeharbord methods]
    M --> P[P persistence]
    RS --> P
    S --> T[T terminal]
    S --> RC[R-client RPC client]
    RS --> RC
    P --> U[U UI/QML + layout]
    M --> U
    RC --> Vf[V file viewers]
    T --> Vt[V/T terminal panes]
    RC --> E[E remote editor]
    Vf --> E
    S --> A[A agent awareness]
    U --> A
    RC --> A
```

## Workstreams

### M — Core data model — ✅ LANDED (Wave 1)
- **Start gate:** [x] Bootstrap done · [x] C2 fields known.
- **TODO:**
  - [x] `Group`, `DevSession`, `ViewerPane`, `TerminalPane` value types + a
    `SplitNode` recursive split-tree type (SPEC 4.5), all under `src/models`.
  - [x] QAbstractItemModel adapter for the sidebar (`SessionsModel`).
  - [x] `aggregateRowState` wiring from per-terminal states.
- **Contract exposed:** header types other libs bind to (`WorkspaceTypes.h`,
  `SplitTree.h`, `SessionsModel.h`). QML registration deferred to U.
- **Stop gate:** [x] MET — `tst_models` (tree build, split round-trip,
  precedence) + `QAbstractItemModelTester`.

### P — Persistence — ✅ LANDED (Wave 2)
- **Start gate:** [x] C2 · [x] M types exist.
- **TODO:**
  - [x] `schema.sql` + migration runner (server-side, in codeharbord).
  - [x] CRUD for groups/sessions/panes/layouts; duplicate-session copy semantics
    (SPEC 4.2) generating fresh tmux targets + layout paneId remap.
  - [x] Client mapping in `ch_persistence` over RPC (no direct client DB access).
- **Contract exposed:** RPC methods `workspace.*` (list/create/update/reorder/…).
- **Stop gate:** [x] MET — `workspace.test.ts` create→reopen→reload identical;
  `tst_workspacedb` client parse/serialize + live round-trip.

### S — SSH transport — ✅ code complete (Wave 1); ⏳ live gate deferred
- **Start gate:** [x] Bootstrap · [x] libssh available (`libssh-dev`).
- **TODO:**
  - [x] `SshConnectionPool`: one authenticated connection, N channels (SPEC 5.3).
  - [x] Host-key verify + known-hosts store + changed-key/@revoked refusal (SPEC 12.1).
  - [x] Channel factory for PTY / RPC / agent-status channels.
  - [x] Credential handling via SSH agent → key → password callback (no secrets stored).
- **Contract exposed:** `openChannel(kind)`, host-key callback, state signals.
- **Stop gate:** ⏳ DEFERRED — the live SSH round-trip needs a test server (not
  available in CI here). Host-key logic covered by `tst_knownhosts`; the
  connect→verify→auth→openChannel path is implemented but unexercised live.
- **Parallel with:** M, R-server, P.

### R — Remote service + client — ✅ LANDED (R-server Wave 1, R-client Wave 2)
- **R-server (Node, codeharbord)**
  - **Start gate:** [x] Bootstrap · [x] C1 · [x] C2.
  - **TODO:** [x] file methods (`stat/readFile/writeFile/resolvePath`) with
    revision tokens (SPEC 8.4) + atomic save (SPEC 8.5); [x] `watch`/`unwatch`
    (fs.watch + poll fallback); [x] `listDirectory`/`getMimeType` (internal
    helpers, not RPC-exposed); [x] `workspace.*` (with P). tmux discovery: not yet.
  - **Stop gate:** [x] MET — `node --test` covers revision-mismatch rejection,
    atomic-save replace, watch-event emission.
- **R-client (C++ `ch_remote`)**
  - **Start gate:** [x] C1 · [x] S channel factory.
  - **TODO:** [x] JSONL framing over the RPC channel; [x] async request/response
    with typed results; [x] teardown/notification routing (reconnect scheduling TBD).
  - **Stop gate:** [x] MET — `tst_rpcclient` (framing, id-matching, errors,
    teardown) + live `server.info` over a piped codeharbord.

### T — Terminal — ✅ code complete (Wave 2); ⏳ live gate deferred
- **Start gate:** [x] S PTY channel · [x] C3.
- **TODO:**
  - [x] `TerminalController`: buffering (SPEC 5.5), state machine (SPEC 5.6),
    reconnect backoff, hidden-drain.
  - [x] tmux target naming with stable IDs (SPEC 5.2) + shell-safe command.
  - [x] xterm.js renderer in `src/web/terminal` + WebChannel bridge (C3).
  - [x] Recursive terminal-region split tree in QML.
- **Stop gate:** ⏳ DEFERRED — live attach/resize/reconnect needs a server +
  display. Controller logic covered by `tst_terminalcontroller`; kill/detach/
  reconnect wiring lands with the app shell (U).
- **Parallel with:** V, E (different regions).

### V — Viewers
- **Start gate:** [x] Bootstrap · [x] R-client (for `file://`).
- **TODO:**
  - [ ] `ViewerHandlerRegistry` by scheme + MIME + extension (SPEC 7.5 table).
  - [ ] Separate WebEngine profiles: external (no bridge) vs internal (SPEC 7.3).
  - [ ] `codeharbor-internal://` scheme handler resolving remote `file://`.
  - [ ] Handlers: web, source/text, markdown, structured-data, image, PDF,
    directory, binary; recursive viewer-region split tree.
- **Stop gate:** open an HTTPS site (external profile, no bridge access asserted),
  a remote text file, an image, and a directory in split panes.
- **Parallel with:** T.

### E — Remote editor
- **Start gate:** [x] R file methods (stat/read/write/watch) · [ ] V pane host · [x] C3.
- **TODO:**
  - [ ] Monaco bundle in `src/web/editor`; `RemoteEditorBridge` (C3).
  - [ ] File-state machine (SPEC 8.2), revision-guarded save, conflict UI
    (SPEC 8.6 — never silent overwrite), external-change reload for clean buffers.
  - [ ] Unsaved recovery snapshots on server (SPEC 11.3, mode 0600).
- **Stop gate:** edit + save a remote file; concurrent external change triggers
  conflict prompt (no data loss); kill client mid-edit → recover buffer.
- **Parallel with:** T.

### A — Agent awareness
- **Start gate:** [x] bridge+adapters DONE · [x] S agent channel (ChannelKind::AgentStatus) · [ ] U sidebar.
- **TODO:**
  - [ ] `AgentStatusMonitor` (C++) consuming JSONL over the agent channel.
  - [ ] Map `AgentState` → sidebar badges/precedence; unseen-completion badges;
    desktop notifications.
  - [ ] Ship the `oh-my-pi` adapter as an installable hook (highest priority,
    SPEC 6.2); then `pi`, `claude-code`.
  - [ ] Fallback coarse activity detection (SPEC 6.6) when no adapter.
- **Stop gate:** run Oh My Pi in a terminal; sidebar reflects
  running→waiting_input→idle_unseen; badge clears on "mark seen".
- **Parallel with:** V, E (after U).

### U — UI shell & persistence
- **Start gate:** [x] M models · [x] P (for layout persistence).
- **TODO:**
  - [ ] Sidebar: groups (collapse/reorder), session rows with aggregate status,
    all sidebar ops (SPEC 4.2).
  - [ ] Persist region widths, split ratios, selected pane per Dev Session.
  - [ ] Command palette + keyboard shortcuts (SPEC 15).
- **Stop gate:** create/rename/duplicate/move sessions; layout + widths persist
  across app restart (state read from server DB).
- **Parallel with:** V/T rendering (consumes their pane views).

## Milestones (integration barriers → SPEC §16)

- **M0 — Bootstrap (DONE).** Repo, build wiring, remote core tested.
- **M1 — Terminal vertical slice (SPEC Phase 0).** ⏳ Code complete (S + T), but
  the stop gate (live attach/resize/reconnect) needs a test SSH server + display
  and the app shell (U) to wire the PTY channel into the renderer. Transport +
  controller are unit-tested.
- **M2 — Core workspace (SPEC Phase 1).** Partially landed: M + P + R-client done;
  needs **U** (app shell) + live **T** to be user-visible.
- **M3 — Remote viewers (SPEC Phase 2).** Pending — workstream V.
- **M4 — Remote editing (SPEC Phase 3).** Pending — workstream E.
- **M5 — Agent awareness (SPEC Phase 4).** Pending — workstream A; Oh My Pi first.

## Delivery progress

**Wave 0 — contracts: ✅ DONE.** C1 (`rpc-types.ts`), C2 (`schema.sql`), C3
(WebChannel interfaces) — landed + adversarially reviewed.

**Wave 1 — ✅ DONE** (S, M, R-server), each with a parallel adversarial-review +
bug-hunt pass. Live S gate deferred (needs test server).

**Wave 2 — ✅ DONE** (P server+client, R-client, T), with an adversarial-review
wave. Live T gate deferred (needs server + display).

**Wave 3 — next (fan out; dependencies noted):**
1. **U** — UI shell & sidebar + layout persistence. Needs M ✅ + P ✅. Unblocks
   the live M1/M2 gates (wires S/T into the window).
2. **V** — viewers (handler registry, WebEngine profiles). Needs R-client ✅ for `file://`.
3. **E** — remote editor (Monaco). Needs R-server file methods ✅ + V pane host + C3 ✅.
4. **A** — agent awareness client (AgentStatusMonitor). Needs S ✅ agent channel + U sidebar.

> Closing the deferred **S** and **T** live stop gates (and milestone **M1**)
> requires a reachable SSH server + a display — do those once **U** provides the
> window to drive them.
