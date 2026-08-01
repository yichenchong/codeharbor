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
  - **Verified at bootstrap:** `npm test` → 11/11 pass (a bootstrap-era number,
    kept only as a record of that moment; the suite is far larger now and no
    document pins a current count on purpose, because every pinned count in this
    repo has gone stale). RPC stdio + bridge socket smoke-tested.
- CI: a `remote` job (install/typecheck/test), a `client` job (web bundles +
  Qt/CMake configure/build/test), and a Windows job that only warms the shared
  vcpkg libssh cache on `main`.

> The bootstrap seams described above are all filled in by later waves; see
> "Delivery progress" for what actually landed and the corrections found when the
> live gates were first exercised.

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

### S — SSH transport — ✅ LANDED (Wave 1); live gate MET (Wave 5)
- **Start gate:** [x] Bootstrap · [x] libssh available (`libssh-dev`).
- **TODO:**
  - [x] `SshConnectionPool`: one authenticated connection, N channels (SPEC 5.3).
  - [x] Host-key verify + known-hosts store + changed-key/@revoked refusal (SPEC 12.1).
  - [x] Channel factory for PTY / RPC / agent-status channels.
  - [x] Credential handling: agent → configured/default key → typed key
    passphrase or server password, with the methods kept separate and no secret
    stored. `~/.ssh/config` (`IdentityFile`) and an explicit profile key path
    both feed libssh.
- **Contract exposed:** `openChannel(kind)`, host-key callback, state signals.
- **Stop gate:** ✅ MET — `tst_livessh` (label `live`) connects to a real sshd via
  ssh-agent auth, persists the first-use host key, runs an Exec channel, drives
  JSON-RPC to a real `codeharbord`, and encrypts a disposable fixture key to
  verify both explicit and OpenSSH-configured identity paths consume the UI
  passphrase callback. `tst_knownhosts` covers changed-key/@revoked refusal.
- **Parallel with:** M, R-server, P.

### R — Remote service + client — ✅ LANDED (R-server Wave 1, R-client Wave 2)
- **R-server (Node, codeharbord)**
  - **Start gate:** [x] Bootstrap · [x] C1 · [x] C2.
  - **TODO:** [x] file methods (`stat/readFile/writeFile/resolvePath`) with
    revision tokens (SPEC 8.4) + atomic save (SPEC 8.5); [x] `watch`/`unwatch`
    (fs.watch + poll fallback); [x] `listDirectory` (RPC, schema v2, for viewers)
    + `getMimeType` (internal helper); [x] `workspace.*` (with P). tmux: not yet.
  - **Stop gate:** [x] MET — `node --test` covers revision-mismatch rejection,
    atomic-save replace, watch-event emission.
- **R-client (C++ `ch_remote`)**
  - **Start gate:** [x] C1 · [x] S channel factory.
  - **TODO:** [x] JSONL framing over the RPC channel; [x] async request/response
    with typed results; [x] teardown/notification routing; [x] reconnect
    scheduling (`SessionBootstrap` state machine + 1/2/5/10/30/60 s backoff,
    capped at 10 attempts; SPEC 5.6).
  - **Stop gate:** [x] MET — `tst_rpcclient` (framing, id-matching, errors,
    teardown) + live `server.info` over a piped codeharbord +
    `tst_sessionbootstrap` / live `tst_livereconnect` (a real dropped SSH
    connection recovers on its own).

### T — Terminal — ✅ LANDED (Wave 2); live gate MET (Wave 5)
- **Start gate:** [x] S PTY channel · [x] C3.
- **TODO:**
  - [x] `TerminalController`: buffering (SPEC 5.5), state machine (SPEC 5.6),
    reconnect backoff, hidden-drain.
  - [x] tmux target naming with stable IDs (SPEC 5.2) + shell-safe command.
  - [x] xterm.js renderer in `src/web/terminal` + WebChannel bridge (C3).
  - [x] Recursive terminal-region split tree in QML.
- **Stop gate:** ✅ MET — `tst_liveterminal` attaches to a real tmux session over a
  real SSH PTY, and the remote side confirms each step: the marker is produced by
  remote execution, `tmux display-message` reports 80x24 → 100x30 after
  `TerminalController::resize`, and after dropping the channel a fresh PTY re-attach
  finds the same pane (identical shell pid, scrollback intact). Controller logic is
  covered by `tst_terminalcontroller`. Wave 5 added the missing production seam:
  `TerminalController::setTransport(QIODevice*)`, with resize reaching
  `SshChannelDevice::resizePty`. Kill/detach UI still lands with U.
- **Parallel with:** V, E (different regions).

### V — Viewers — ✅ LANDED (Wave 3); live gate MET (Wave 5)
- **Start gate:** [x] Bootstrap · [x] R-client (for `file://`).
- **TODO:**
  - [x] `ViewerHandlerRegistry` by scheme + extension (SPEC 7.5 table).
  - [x] Dual `QQuickWebEngineProfile`: external (no bridge) vs internal (SPEC 7.3), isolation asserted.
  - [x] `codeharbor-internal://` scheme handler + bidirectional URL map (SPEC 7.4), served via file.readFile.
  - [x] Views: web, source/text, markdown, structured-data, image, PDF,
    directory, binary; recursive viewer-region split tree in QML.
- **Stop gate:** ✅ MET — `tst_liveviewers` loads real remote file bytes through
  `codeharbor-internal://` into a WebEngineView headless and reads them back out of
  the page via `runJavaScript`; live `file.listDirectory` populates a directory view
  per pane; the split tree is measured in 6 cases (even, 3-way, explicit ratios,
  pane added after first layout) with extents summing to the parent within 1px; and
  `grabWindow()` frame proofs are saved as PNGs. `tst_viewers` still covers the
  SPEC 7.5 table, URL round-trip, MIME and profile isolation.
  Wave 5 fixed a real defect this gate exposed: every branch child set
  `SplitView.fillWidth/fillHeight`, but SplitView stretches only the FIRST fill
  item, so a 2-pane split rendered as one full pane plus a zero-extent sibling.
  Splits now size from the node's persisted `ratios`.
- **Parallel with:** T.

### E — Remote editor — ✅ LANDED (Wave 4); live gate MET (Wave 5)
- **Start gate:** [x] R file methods (stat/read/write/watch) · [x] V pane host · [x] C3.
- **TODO:**
  - [x] Monaco bundle + WebChannel-native `EditorBridge` (C3, object "editor") in `src/web/editor`.
  - [x] File-state machine (SPEC 8.2), revision-guarded save + conflict (SPEC 8.6 — never silent overwrite), external-change reload for clean buffers / no-clobber when dirty.
  - [x] Unsaved recovery snapshots on the server (SPEC 11.3) via revision-guarded writeFile; per-pane EditorController (EditorFactory) with watch released on close.
- **Stop gate:** ✅ MET — `tst_liveeditor` drives the real packaged Monaco page in
  the real `EditorPaneView` over a real SSH connection: the load assertion is read
  from INSIDE the page (`monaco.editor.getModels()[0].getValue()`), the edit is made
  with Monaco's own type command, a real DOM Ctrl+S triggers the revision-guarded
  save, and the REMOTE DISK bytes are re-read out of band to confirm it landed.
  SPEC 8.6 is proven live: an external change makes the stale-revision save conflict,
  the doomed edit is absent from disk, and the page's own Reload path then adopts the
  server revision so the next save succeeds. `tst_editorcontroller` covers the state
  machine. Both previously-deferred refinements are DONE: the JS→C++ `ready()`
  handshake (content pushed before the page connects is buffered and replayed) and
  clearing the recovery snapshot on a successful save. The Monaco bundle is now
  really packaged (esbuild → qrc), built automatically at configure time.
- **Parallel with:** T.

### A — Agent awareness — ✅ LANDED (Wave 4) except SPEC 6.6 fallback detection; live gate MET (Wave 5)
- **Start gate:** [x] bridge+adapters DONE · [x] S agent channel (ChannelKind::AgentStatus) · [x] U sidebar.
- **TODO:**
  - [x] `AgentStatusMonitor` (C++) consuming AgentEvent JSONL over the agent channel.
  - [x] Map `AgentState` → sidebar row precedence + unseen-completion; markSeen clears the badge; notify() hook on waiting_input/idle_unseen (OS notification display-deferred).
  - [x] `oh-my-pi` installable hook emitting BridgeMessage through the bridge (single mapping point); `pi`/`claude-code` adapters already registered server-side.
  - [ ] Fallback coarse activity detection (SPEC 6.6) for the adapterless `generic`
    harness. **NOT DONE.** `FallbackActivityDetector`
    (`remote/src/adapters/fallback.ts`) exists and is correct, but nothing in
    production constructs it — the only caller is
    `remote/test/agent-hook.test.ts`. SPEC 6.6 needs exactly one input, terminal
    OUTPUT BYTES, and no part of the daemon has a source for them: `codeharbord.ts`
    reads only JSON-RPC lines from stdin, `bridge.ts` only hook messages from a Unix
    socket, and `tmux.ts` only ever runs `list-sessions`/`kill-session` (there is no
    `capture-pane` or `pipe-pane` anywhere in `remote/`). Terminal output exists on
    the CLIENT side only, so wiring this up is a new server-side output tap, not a
    missing connection.
- **Stop gate:** ✅ MET — `tst_liveagent` runs the REAL hook on the remote side
  (one node process per firing) into the REAL bridge, over an SSH AgentStatus
  channel, and observes the ordered transitions
  `starting → running → waiting_input → running → idle_unseen`, with the summary
  surviving the whole chain, `markSeen` clearing the badge idempotently, and the
  sidebar row going FinishedUnseen → Idle. Remote bridge pids are asserted reaped
  (an SSH exec channel sends no SIGHUP). OS desktop notifications remain
  display-deferred: `notify()` fires and is asserted, but nothing renders headless.
- **Parallel with:** V, E (after U).

### U — UI shell & persistence — ✅ LANDED (Wave 3); live gate MET (Wave 5)
- **Start gate:** [x] M models · [x] P (for layout persistence).
- **TODO:**
  - [x] Sidebar: groups (collapse), session rows with aggregate status, ops
    create/rename/duplicate/move/delete (SPEC 4.2), and drag-and-drop reorder /
    cross-group move / group reorder with keyboard selection (Wave 6, covered by
    `tst_sidebar` driving real QTest drags).
  - [x] Persist region widths + selected pane per client (QSettings, SPEC 4.1); split ratios persist server-side via P layouts.
  - [x] Command palette + keyboard shortcuts (SPEC 15) — `CommandPalette.qml`,
    hosted in Main.qml, with per-command global `Shortcut`s (Wave 6).
- **Stop gate:** ✅ MET — `tst_liveshell` performs live CRUD through AppController's
  invokables against a real `codeharbord`, then re-reads every mutation through a
  SECOND independent codeharbord process (so a local-only mutation fails), and
  proves width persistence three ways: the real `Main.qml` restores stored widths
  in-process, a real-binary launch does NOT rewrite them (even when the window is
  too narrow to honour them), and a real handle drag DOES persist to disk.
  Wave 5 fixed a defect this gate exposed: persistence fired on every width change,
  so a restored width that did not fit was clamped and the clamped value overwrote
  the user's preference permanently. Writes now happen only on drag end.
  Pane FOCUS is not tracked, so palette split commands act on a region's first
  pane rather than the focused one (see the Wave 7 gap list).
- **Parallel with:** V/T rendering (consumes their pane views).

## Milestones (integration barriers → SPEC §16)

- **M0 — Bootstrap (DONE).** Repo, build wiring, remote core tested.
- **M1 — Terminal vertical slice (SPEC Phase 0).** ✅ REACHED (live, Wave 5) — real
  SSH attach to a real tmux session, remote-confirmed resize, and reconnect onto the
  same pane (`tst_livessh`, `tst_liveterminal`).
- **M2 — Core workspace (SPEC Phase 1).** ✅ REACHED (live, Wave 5) — live CRUD
  against a real `codeharbord`, verified through a second independent server process,
  plus width restore and drag-persistence (`tst_liveshell`).
- **M3 — Remote viewers (SPEC Phase 2).** ✅ REACHED (live, Wave 5) — real remote
  bytes rendered through `codeharbor-internal://`, live directory listings, measured
  split geometry, frame proofs (`tst_liveviewers`).
- **M4 — Remote editing (SPEC Phase 3).** ✅ REACHED (live, Wave 5) — real Monaco
  editing a real remote file over SSH, save landing on remote disk, live conflict
  refusal + reload (`tst_liveeditor`).
- **M5 — Agent awareness (SPEC Phase 4).** ✅ REACHED (live, Wave 5) — the real hook
  on the remote side driving ordered sidebar transitions and badge clearing
  (`tst_liveagent`). OS desktop notifications remain display-deferred.

## Delivery progress

**Wave 0 — contracts: ✅ DONE.** C1/C2/C3 landed + adversarially reviewed.

**Wave 1 — ✅ DONE** (S, M, R-server) + parallel adversarial review + bug-hunt. Live S gate deferred.

**Wave 2 — ✅ DONE** (P server+client, R-client, T) + adversarial-review wave. Live T gate deferred.

**Wave 3 — ✅ DONE** (U shell, V viewers) → milestones **M2** and **M3** reached
(logic verified; live display/server render gates deferred). Delivered with a
concurrent Wave-2 bug-hunt (R-client null-error HIGH, P schema_version latent) and
a Wave-3 adversarial review (U callback use-after-free + silent-no-op sidebar HIGHs,
V profile-type M3 blocker). Contract C1 gained `file.listDirectory` (schema v2).

**Wave 4 — ✅ DONE** (E editor, A agent) → milestones **M4** and **M5** reached
(logic verified; live editor / agent-sidebar render deferred). Delivered with a
concurrent Wave-3 bug-hunt (U setServerId + stale-refresh ordering; V nosniff +
active-MIME gate) and a Wave-4 adversarial review (E watch-subscription leak +
read-only save; A markSeen badge-clear + version fidelity + transport UAF).
Contract: the oh-my-pi hook emits BridgeMessage (the bridge stays the single mapping point).

> **Corrections (each found only when something real was exercised).** Three
> times now a green suite has coexisted with a product that did not work. Each
> entry is here because the suite said "fine" and the truth was otherwise.
>
> 1. **No transport spine** (Wave 5). `CodeharbordClient::setTransport` and
>    `AgentStatusMonitor::setTransport` existed and were unit-tested, but there was
>    no `QIODevice` over an `ssh_channel` and `main.cpp` never called either. The
>    shipped app could not reach a remote server at all — the client subsystems
>    were only ever driven by test doubles. Fixed by `SshChannelDevice` +
>    `SessionBootstrap`.
> 2. **The app could not start** (Wave 5). `ViewerRegion.qml`/`TerminalRegion.qml`
>    instantiated their own type recursively, which QML rejects, so `Main.qml`
>    failed to load. No test instantiated the real QML tree, so a green 9/9 suite
>    coexisted with an app that could not launch. Fixed by url-sourced `Loader`
>    recursion plus a permanent `tst_qmlload` gate.
> 3. **Unknown SSH host keys were trusted silently** (Wave 7, SECURITY).
>    `SessionBootstrap::attemptWire` unconditionally installed an accept-all
>    host-key callback, overwriting the prompting callback `AppController` had
>    just installed. Every unknown key was therefore accepted and written to
>    `known_hosts` with no consent, and the entire host-key prompt UI —
>    `hostKeyPrompt`, `resolveHostKey`, the ConnectSheet fingerprint view — was
>    dead code. This deviates from SPEC 12.1, and it is the sharpest example of
>    the pattern: the Wave-5 and Wave-6 cold-start walkthroughs "succeeded"
>    PARTLY BECAUSE of this bug, since nothing ever had to answer a prompt. Fixed
>    by installing the accept-all default only when no policy is set
>    (headless/unattended use), and now defended end to end by `tst_coldstart`.
>
> Treat "code complete" as "unit-tested logic" unless a workstream's live gate is
> explicitly marked MET — and treat a gate that has never failed with suspicion
> until you have watched it fail.

**Wave 5 — ✅ DONE (live gates).** Every deferred live gate is now MET against a real
sshd + real `codeharbord`: S, T, U, V, E, A (`tst_livessh`, `tst_liveterminal`,
`tst_liveshell`, `tst_liveviewers`, `tst_liveeditor`, `tst_liveagent`, all labelled
`live` and QSKIPped when `CH_LIVE_SSH` is unset, so the default suite stays portable).
Milestones M1–M5 are reached LIVE, not just unit-tested.

Wave 5 built the missing production spine (`SshChannelDevice` — a `QIODevice` over an
`ssh_channel`; `SessionBootstrap` wiring RPC + agent channels in `main.cpp`;
`TerminalController::setTransport`) and packaged the real Monaco bundle (esbuild → qrc,
auto-built at configure time, with CMake refusing to configure a silently editor-less
client). Defects the live gates exposed — none of which the headless suite could see:

- **QML could not load at all** — recursive self-instantiation in both region types.
- **Zero-extent split panes** — `SplitView` stretches only the first fill item, so a
  2-pane split showed one pane plus an invisible sibling.
- **Region widths destroyed on launch** — persistence fired on clamped layout changes,
  overwriting the user's stored preference permanently.
- **Non-deterministic sidebar ordering** — `moveSessionToGroup` left rows tied on
  `position`, so `ORDER BY position, id` fell back to UUID order.
- **Orphaned remote process per launch** — an SSH exec channel sends no SIGHUP, so the
  agent bridge outlived the app.
- **Fatal Chromium flag** — `--single-process` aborts once a second `QWebEngineProfile`
  exists, which the viewer stack creates by design.

**Wave 6 — ✅ DONE (usability).** The live gates passed but a person still could not
use the app: nothing could reach a server, selecting a session did nothing, and there
was no terminal in the UI at all. Wave 6 added stored connection profiles + a connect
/host-key sheet, a real packaged xterm.js bundle with a per-pane `TerminalFactory`
(live remote shells in panes), `SessionLayouts` (a session's split trees load from and
persist to the server), sidebar drag-and-drop, the SPEC 15 command palette with global
shortcuts, reconnect with the SPEC 5.6 backoff ladder, OS notifications, and `tmux.*`
discovery RPC. The workspace is now keyed by a SERVER-OWNED id (`server.info.serverId`,
persisted in `server_identity`) — a client-local id would have orphaned the user's real
groups and sessions whenever a profile was re-added or a second machine connected.

**Wave 7 — ⚠️ PARTIALLY COMPLETED (adversarial round).** Be precise about this one: the
eight-reviewer wave was **cut short by a harness limit and every reviewer was killed
mid-flight**. Their code changes were already on disk, so the round was salvaged by
re-dispatching finishers scoped to the failing tests each had left behind, and by the
orchestrator finishing the rest. What that means:

- **Swept and completed:** the cold-start acceptance gate (`tst_coldstart`, 9/9 —
  first run → add server → key prompt/accept/persist → session → live tmux pane →
  edit+save a remote file → relaunch restores), pane identity across splits, the UI/UX
  pass, and a verification pass over the interrupted fixes.
- **Salvaged, NOT completed:** the security and integration reviews. Their FIXES
  landed and are tested, but each reviewer died before writing its findings list, so
  any defect it had noticed and not yet written down was lost with it. The host-key
  hole below survived only because that reviewer happened to report it over IRC first.
  **A fresh security review is still owed** — treat Wave 7 as evidence that the
  reviewed areas contain bugs, not as evidence that they are now clean.

Defects Wave 7 found and fixed: unknown SSH host keys silently trusted (see correction
3 — security); splitting a region **destroyed the pane the user was working in**, and a
republished tree rebuilt every delegate because the Repeater was keyed on the children
array; `file.watch` was never re-established after a reconnect, so external-change
reload silently died for every open editor; deliberate teardown and the deliberate
host-key refusal were painted as red error toasts; three "fixed" items were green only
because no test exercised the path at all.

**Wave 8 — ✅ DONE (the review that was owed) + ⚠️ one slice cut short.** The Wave-7
security and integration reviewers died before filing findings, so that review was
re-run with agents streaming findings over IRC as they worked — which is the only
reason the analysis survived two further terminations. It found and fixed a HIGH
host-key TYPE downgrade (correction 3's neighbour: a MITM presenting a different
ALGORITHM turned the hard refusal into a friendly first-use prompt, and the defect was
CODIFIED as expected behaviour by an existing test), an unpinned privileged editor
WebEngineView, a never-retired active Dev Session, unreachable password/passphrase
auth, a client that could only launch a dev checkout (the released tarball was
unlaunchable), a missing schema-compatibility gate, unwritable pane focus, dead
read-only derivation, and the workspace.* contract drift hole. A final slice
(group operations UI, crash-recovery prompt) was killed at ~4 minutes with its
features written but essentially untested; the untested destructive half
(`deleteGroup` cascades through every session in the group) was REVERTED rather than
shipped, and the contained half (pane URL persistence) was completed and tested here.

One more finding closed at the end, and it is the sharpest example of the pattern
this document keeps recording: **`createSession` had no production caller at all**, so
a user could add a server, accept its host key and create a group — and then hit a
dead end, unable to create a Dev Session and therefore unable to reach a terminal, an
editor, or anything else the product is for. The sidebar's own empty state told them
to "add a Dev Session to it", which the UI could not do. `tst_coldstart` passed 9/9
throughout, because it creates its session over RawRpc: a gate doing what the USER
cannot. Now wired (group header "+ Session" → dialog → `app.createSession`), with the
drag-reorder regression it briefly introduced caught by `tst_sidebar` and fixed.

> **Remaining known gaps — honest list.**
> - **No rename or delete for groups.** `renameGroup` and `deleteGroup` have no
>   reachable affordance, so a group created by mistake is permanent. Creating groups
>   and Dev Sessions IS wired ("+ New group", "+ Session"); delete was implemented once
>   and reverted for lack of tests, and it cascades to every session in the group, so
>   it needs a confirmation that states the real cost.
> - **Crash-recovery is written but never offered.** `EditorController::recoveryAvailable`
>   has no consumer, so SPEC 11.3 snapshots are taken, found on reopen, and ignored:
>   `checkRecovery()` reads the server-side snapshot, sees it differs from the file on
>   disk, emits the signal — and because no QML connects to it the recovered text is
>   dropped on the floor. The user is never asked, and the unsaved work the snapshot
>   exists to protect is silently lost. The fix belongs in `EditorPaneView.qml`.
> - **The sidebar row shows a status dot, not the counters SPEC 4.2 asks for.** SPEC 4.2
>   wants the number of active terminals, the number of terminals requiring attention,
>   an unsaved-file indicator and an error indicator on each Dev Session row. Only the
>   aggregate state (which does cover the error case) is delivered:
>   `SessionsModel::Roles` in `src/models/SessionsModel.h` has no role for either
>   counter, so the delegate has nothing to bind to. Needs two new roles plus the
>   per-session terminal tallies to feed them. SPEC 4.2 now marks these unimplemented.
> - **Unsaved-file state never reaches the Dev Session row (SPEC 8.2).**
>   `aggregateRowState()` in `src/models/SessionState.cpp` folds only terminal and
>   coding-agent conditions — error, waiting-for-input, running, finished-unseen,
>   connected. No file state is an input, so a session with a dirty editor buffer looks
>   identical to a clean one in the sidebar. Fixing it means routing `FileState` from
>   the editor controllers up to the per-session aggregation, which is a real design
>   step, not a wiring change. SPEC 8.2 now records this.
> - **Conflict handling offers two of the four documented choices (SPEC 8.6).** The
>   notice in `src/web/editor/src/index.ts` has **Reload** and **Overwrite**;
>   **Compare** and **Save As** are absent because the C++ bridge
>   (`src/editor/EditorController.h`) has no diff-view and no write-to-another-path
>   method. The safety requirement of that section is met — no silent overwrite — so
>   this is a missing affordance, not a correctness hole. SPEC 8.6 now records it.
> - **`viewer_panes`/`terminal_panes` CRUD is unused.** Pane URLs ride in the layout
>   tree instead (atomic with the structure, no extra RPC); those six methods and the
>   two tables stay dead until per-pane metadata needs a home.
> - **A reconnect that meets a NEW unknown host key cannot prompt** — `hostKeyPrompt`
>   is only raised from an interactive connect, so the ladder dead-ends on an opaque
>   failure. Needs a design call about prompting outside a user-initiated attempt.
> - **Connect blocks the GUI thread** — a bounded, timed-out libssh handshake, but the
>   real fix is moving the session to a worker thread.
> - **Pane focus is click-based**; focus moved purely by keyboard does not report, so a
>   future "focus next pane" command must tell the region directly.
> - **`tmux.*` discovery has no client consumer** — the RPC group exists and is tested
>   server-side, and nothing calls it.
> - **The DEFAULT trust-store path is never exercised by a test.** In production the
>   known-hosts file lives at `QStandardPaths::AppConfigLocation + "/known_hosts"`,
>   which on Linux is `<config>/CodeHarbor/CodeHarbor/known_hosts` — org AND app, a
>   nested directory beside `CodeHarbor.conf`, not next to it. Every live test
>   overrides it with `CH_LIVE_KNOWN_HOSTS`, so the path a real user actually gets is
>   the one path no gate covers. This cost real debugging time: a launch smoke seeded
>   `known_hosts` one level up, the app correctly saw an empty trust store, treated
>   the key as unknown, and parked awaiting a prompt nobody could answer headlessly —
>   which reads exactly like a broken auto-reconnect. The product was right; the
>   coverage is not.
> - **Two live gates hardcode the dev layout.**
>   `TstLiveSsh::rpcServerInfoOverSshChannel()` and
>   `TstLiveAgent::bridgeChannelWiresMonitor()` build their remote command from
>   `<CH_LIVE_REPO>/remote/src/*.ts` instead of going through
>   `SessionBootstrap::entryCandidates()`, so they only pass against a git
>   checkout and fail against an unpacked release tarball. Not a product defect —
>   `tst_coldstart` drives the real path and passes against the tarball — but it
>   means the live suite is not a check on the layout a normal user installs.
>   Found by installing the published v0.1.0 artifacts and running the suite
>   against them, which is the only reason it is known at all.
