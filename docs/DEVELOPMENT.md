# Development Environment Setup

How to provision a machine to build and run CodeHarbor. Covers the C++/Qt client
and the Node/TypeScript remote workspace.

> The client is a Qt 6 desktop app; the remote workspace (`remote/`) is Node with
> zero runtime dependencies. You can work on the remote parts with only Node.

## Verified toolchain

This stack has been confirmed to configure, build, and link the full tree
(`cmake --preset dev && cmake --build --preset dev` → `codeharbor` executable):

| Component | Verified version | Minimum |
|---|---|---|
| OS | Ubuntu 26.04 LTS | — |
| C++ compiler | GCC 15.2 | C++20 (GCC 12+/Clang 15+) |
| CMake | 4.2.3 | 3.25 (CMakePresets schema v6) |
| Ninja | 1.13.2 | any |
| Qt 6 | 6.10.2 | 6.9 |
| libssh | 0.11.3 (verified runtime) | audited runtime floor 0.11.2; exactly 0.12.0 is warned about and worked around |
| Node.js | 24.16 | 23.6 (native TS type-stripping) |

The Windows release uses the separate in-tree vcpkg overlay, pinned to libssh
0.12.2. That overlay pin does not change the runtime floor for ordinary Linux
and macOS builds, which use libssh 0.11.2 or newer.

The CMake floor is Qt **6.9**; newer (6.10 here) works unchanged. 6.9 is not a
guess: it is the first release with `QQuickWebEngineProfile(storageName, parent)`,
and CI builds and runs the portable suite on 6.9 to keep the claim honest.

**libssh 0.12.0 cannot complete a handshake against a modern sshd.** Its hybrid
ML-KEM key exchange (`mlkem768x25519-sha256`, first in libssh's own default list)
hands `ssh_buffer_pack()` an un-cast `int` where it reads a `size_t`, so the
client KEX init fails to pack — `Failed to construct client init buffer`, before
any host key is seen. Upstream fixed the cast in 0.12.1. Two consequences:

- Windows release artifacts take libssh from the in-tree overlay port
  ([`packaging/vcpkg-ports/libssh`](../packaging/vcpkg-ports/libssh)) pinned to
  0.12.2, because vcpkg's registry port is still 0.12.0. Bumping that pin also
  changes the release workflow's vcpkg cache key, which embeds the version.
- If the *runtime* libssh is 0.12.0 anyway (a distro or Homebrew build),
  `SshConnectionPool` drops just that one algorithm from libssh's defaults for
  the attempt and records it in the connection log, so post-quantum KEX stays
  available through `mlkem768nistp256-sha256`.

The libssh floor is **0.11.2**, audited symbol by symbol against what
`src/ssh/SshConnectionPool.cpp` and `src/ssh/SshChannelDevice.cpp` actually call
(the audit is recorded as a comment at the top of `SshConnectionPool.cpp`).
CMake records that audited runtime floor as `CODEHARBOR_LIBSSH_FLOOR=0.11.2`
and emits warnings rather than making it a hard configure-time version
constraint. It warns separately when the runtime is exactly 0.12.0.
Three separate bounds stack up to that number:

- **0.8.0** is the hard compile/link floor: every libssh symbol these two files
  use exists in 0.7.0 except `ssh_get_server_publickey()`, which is used by
  `SshConnectionPool::verifyHostKey()` and first appears in 0.8.0.
- **0.11.0** is required by the 0.12.0 workaround above: it passes
  `-mlkem768x25519-sha256` to `ssh_options_set(SSH_OPTIONS_KEY_EXCHANGE, ...)`,
  and the leading `-` ("subtract from the default list") modifier only arrived in
  0.11.0. The workaround is gated on the runtime being exactly 0.12.0, so an
  older libssh never reaches that call — but the code would not work there.
- **0.11.2** is the first release without CVE-2025-5351, a double free in
  libssh's public-key blob export. That path backs
  `ssh_pki_export_pubkey_base64()`, which `verifyHostKey()` calls on **every**
  connection. The 0.10 series is also end-of-life upstream and is missing the
  CVE-2023-6004 fix (command injection through a `ProxyCommand` in the user's
  `~/.ssh/config`, which `ssh_options_parse_config()` reads) before 0.10.6.

The previously documented "0.10+" floor was not derived from anything: no symbol
in `src/ssh` needs 0.10, and 0.10.x cannot run the shipped 0.12.0 workaround.

## Ubuntu / Debian (apt)

### 1. Client build dependencies

```bash
sudo apt-get update && sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config libssh-dev \
  qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools \
  qt6-webengine-dev qt6-webengine-dev-tools qt6-webchannel-dev \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-window \
  qml6-module-qtquick-layouts qml6-module-qtwebengine
```

What each group provides (maps 1:1 to the `find_package(Qt6 ... COMPONENTS ...)`
in the top-level `CMakeLists.txt`):

| Requirement | Package(s) |
|---|---|
| Compiler + make | `build-essential` |
| CMake + Ninja generator | `cmake`, `ninja-build` |
| `pkg_check_modules(libssh)` (`ch_libssh`) | `pkg-config`, `libssh-dev` |
| Qt6 Core / Gui / Network | `qt6-base-dev` |
| Qt6 Qml / Quick / **QuickControls2** | `qt6-declarative-dev` |
| Qt6 **WebEngineQuick** | `qt6-webengine-dev` |
| Qt6 **WebChannel** | `qt6-webchannel-dev` |
| `qt_add_qml_module` / `moc` / `qmltc` tooling | `qt6-base-dev-tools`, `qt6-declarative-dev-tools`, `qt6-webengine-dev-tools` |
| Runtime QML imports for the three-region UI | `qml6-module-qtquick*`, `qml6-module-qtwebengine` |

> `qt6-tools-dev` (Qt Linguist etc.) is **not** required, and neither is any Qt
> SQL driver: the client keeps no local database (SPEC 11.2), so nothing links
> `Qt6::Sql`.

### 2. Node.js (remote workspace)

Node **23.6+** is required — the remote tests run TypeScript directly via Node's
native type stripping (no ts-node/tsx). If your distro's Node is older, install a
current one (e.g. via [nodesource](https://github.com/nodesource/distributions)
or `nvm`):

```bash
# nvm example
curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.1/install.sh | bash
nvm install 24
```

## Other platforms (reference)

The package names differ but the component set is identical (Qt 6 base +
declarative + webengine + webchannel, a C++20 compiler, CMake, Ninja, libssh,
Node 23.6+).

- **Fedora:** `sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config libssh-devel qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtwebengine-devel qt6-qtwebchannel-devel`
- **Arch:** `sudo pacman -S base-devel cmake ninja libssh qt6-base qt6-declarative qt6-webengine qt6-webchannel`
- **macOS (Homebrew):** `brew install cmake ninja libssh node` then install Qt 6
  via `brew install qt` or the official Qt online installer; point CMake at it
  with `-DCMAKE_PREFIX_PATH=$(brew --prefix qt)`.
- **Cross-platform:** the official [Qt online installer](https://www.qt.io/download-qt-installer)
  or [`aqtinstall`](https://github.com/miurahr/aqtinstall) (what CI uses) work on
  all three OSes.

CI provisions the same set on **all three release platforms** — see
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml). Its `client` job is a
matrix over `ubuntu-latest`, `windows-latest` and `macos-latest`: Qt via
`jurplel/install-qt-action`, CMake and Ninja via `lukka/get-cmake`, and libssh via
`apt` on Linux, Homebrew on macOS and vcpkg (in-tree overlay port) on Windows —
the same per-OS provisioning `release.yml` uses, so a green run means the release
build compiles on every platform it ships to. What CI does *not* do is package:
`linuxdeploy`/`windeployqt`/`macdeployqt` and the Inno Setup installer stay
release-only.

## Build & run

### Client (Qt / CMake)

```bash
npm install                   # once: web-asset workspaces
npm run build                 # Monaco + xterm.js + Markdown bundles -> src/web/*/dist/
cmake --preset dev            # configure (Debug, Ninja, build/dev/)
cmake --build --preset dev    # build -> build/dev/src/app/codeharbor
./build/dev/src/app/codeharbor
```

There are three web bundles — the Monaco editor, the xterm.js terminal, and the
Markdown renderer — all gitignored build artifacts that CMake embeds as
`qrc:/codeharbor/web/editor/index.html`,
`qrc:/codeharbor/web/terminal/index.html` and
`qrc:/codeharbor/web/markdown/index.html` (see `src/qml/CMakeLists.txt`). CMake
builds them for you at **configure** time when any is missing or older than its
sources — so `npm install` above is the only real prerequisite, and editing
`src/web/editor/**`, `src/web/terminal/**` or `src/web/markdown/**` re-triggers
the rebuild on the next configure.

If a bundle is missing and cannot be built (no `npm` on `PATH`), configure
**fails** with the command to run: a client whose editor, terminal or Markdown
pane silently loads nothing is never the default. Pass
`-DCODEHARBOR_SKIP_WEB_BUNDLE=ON` to deliberately build a client with those panes
inert.

Source maps are a configure-time cache variable, `CODEHARBOR_WEB_SOURCEMAP`, with
three values: `AUTO` (the default), `ON` and `OFF`. `AUTO` means **ON for a
`Debug` configure and OFF otherwise**, and is re-derived on every configure, so
re-configuring an existing Debug directory as Release stops embedding maps rather
than carrying the first configure's answer forward. It is declared in the
top-level `CMakeLists.txt`, so it shows up in `cmake -LH`, `ccmake` and the IDE
preset UIs with those three values offered. When it resolves to on, CMake runs
each workspace's `build:sourcemap` script instead of `build` and embeds the
resulting `*.js.map` / `*.css.map` alongside the bundle, so a JavaScript fault in
the editor or terminal page resolves to the original TypeScript in WebEngine's
developer tools instead of pointing into one minified line. The maps are not
small — Monaco's three come to ~15 MB — which is exactly why a Release binary
never contains them. A multi-config generator has no `CMAKE_BUILD_TYPE` at
configure time (and the bundle is built at configure time), so `AUTO` resolves to
off there; pass `-DCODEHARBOR_WEB_SOURCEMAP=ON`/`=OFF` to decide explicitly, which
persists in the cache until it is set back to `AUTO`.

Both build directories share the one `src/web/*/dist`, and the option changes what
has to be in it, so switching between a Debug and a Release configure rebuilds all
three bundles. In a clone that has both `build/dev/` and `build/release/`, re-run
`cmake --preset dev` — not just `cmake --build --preset dev` — after configuring
`release`, or the dev build will look for map files the release configure just
removed.

By hand the same build is `npm run build:sourcemap --workspace src/web/editor`
(equivalently, from that workspace directory, `node build.mjs --sourcemap` or
`CODEHARBOR_WEB_SOURCEMAP=1 node build.mjs`) and plain `npm run build` for the
mapless variant. Either script writes into a fresh `src/web/<name>/dist.tmp/` and
swaps it into place only after every step has succeeded: a build that fails
midway leaves the previous `dist/` exactly as it was rather than leaving CMake
with no bundle to embed, and a build that succeeds leaves no renamed or deleted
output behind. Both builds are reproducible — rebuilding an unchanged tree, with
or without maps, produces byte-identical files.

Presets are defined in [`CMakePresets.json`](../CMakePresets.json): `dev`
(Debug + tests, `build/dev/`), `release` (Release, tests off, `build/release/`)
and `release-tests` (Release **with** tests, also `build/release/` — it is what
CI and the release workflow configure, build, and `ctest` against, so the tree
that is packaged is the tree that was tested). `release` and `release-tests`
deliberately share one binary directory; switching between them reconfigures it.

> Running the GUI needs a display. On a headless box use an X/Wayland session or
> `xvfb-run ./build/dev/src/app/codeharbor`. WebEngine may need
> `--no-sandbox` in constrained containers.

### Build speed

**Historical build-speed snapshot.** A clean Debug build with tests took about
**13½ minutes** on the reference box (2 cores, 7 GB RAM, `ninja -j2`, GCC 15.2,
Qt 6.9). The edge totals below were measured from clean scratch trees before the
current test expansion, so they are not a current timing claim.

`ctest --preset dev -N` is the authoritative way to count configured tests.
Use the command rather than copying a pinned count when the suite changes; it
includes script and fixture tests as well as executable targets.

**Install a compiler cache — it is the one large lever.** CMake looks for
`ccache` and then `sccache` at configure time and wires whichever it finds into
`CMAKE_C_COMPILER_LAUNCHER` / `CMAKE_CXX_COMPILER_LAUNCHER` automatically. There
is nothing to pass and nothing to remember; the configure output says which way
it went:

```text
-- compiler cache: /usr/bin/ccache
-- compiler cache: not found (looked for ccache, sccache); builds are uncached. …
```
`sudo apt install ccache` (or `brew install ccache`) and re-run `cmake --preset dev`
is the whole procedure. An explicit `-DCMAKE_CXX_COMPILER_LAUNCHER=…` still wins,
so this never fights a deliberate choice.



**Where the time actually went in that snapshot.** Summing every compile/link edge
of a clean `dev` build (1985 seconds of work, which `-j2` turned into ~13½ minutes
of wall clock):

| Bucket | Edge-seconds | Share |
| --- | --- | --- |
| 35 test executables (snapshot), one translation unit each | 767 | 39% |
| `qmlcachegen` units for the QML module | 321 | 16% |
| AUTOMOC/AUTOGEN steps | ~200 | 10% |
| module library objects (all of `src/*/*.cpp`) | 282 | 14% |
| `mocs_compilation.cpp` for every target | 80 | 4% |
| **all linking, including the 36 executables (snapshot)** | **~40** | **2%** |

The two things people reach for first are therefore the wrong two things here.
The linker is not the problem — 40 seconds of 1985. And the C++ *the project
wrote* is a minority of the build: the test executables and the generated QML
cache units together are over half of it.

**Precompiled headers were measured and rejected — do not re-add them without
re-measuring.** Roughly 60% of a Qt-heavy translation unit is re-parsing Qt
headers (`-fsyntax-only` on `src/viewers/ViewerModel.h` alone is 3.08 s of the
4.97 s it takes to compile `ViewerModel.cpp`), so per-module
`target_precompile_headers` looks like an obvious win, and on the files it
touches it delivers: module library objects went 282 s → 177 s (−37%) and
`mocs_compilation.cpp` 80 s → 43 s (−46%). It still did not pay:

* Generating six module PCHs cost **115 seconds and 1.2 GB of `.gch`** on every
  clean tree — about what it saved. Three clean builds measured 13:31.6
  (baseline), 13:26.6 (PCH on) and 13:37.2 (same tree, PCH off): all within the
  ±5% noise of a loaded 2-core box, i.e. **no clean-build improvement at all**.
* A PCH only amortises over a target with roughly four or more objects. The
  small modules measured *worse* with one (`ch_persistence` +15 s, `ch_remote`
  +12 s).
* It cannot touch the two biggest buckets. Test executables are one translation
  unit each, so a per-target PCH costs more to build than the single file saves,
  and sharing one between sibling tests (`target_precompile_headers REUSE_FROM`)
  fails for a subtler reason: the tests carry different `-D` flags
  (`CH_REPO_ROOT`, `CH_CONNECTSHEET_QML`, …), which silently invalidates a shared
  GCC PCH — 5 of the 6 targets in `src/qml/tests` got no speedup whatsoever.
* The only real win was the incremental loop (touch a widely included project
  header, rebuild): 6:13 → 5:52. Not worth 1.2 GB and a mechanism that can hide
  a missing `#include`.

**`CODEHARBOR_TEST_MINIMAL_DEBUGINFO`** (default **OFF**) is the one knob that
did pay. It compiles the `tst_*` executables with `-g1` — line tables and
function boundaries, no local variables or type descriptions — which measured
15.2 s → 11.7 s (≈23%) for `tst_qmlload.cpp`, the largest test translation unit.
It is off by default on purpose: the reason to want a debugger in this tree is
almost always a failing test, and `-g1` removes exactly what that session needs.
Turn it on while grinding a build loop, off when you are about to step through
something:

```bash
cmake --preset dev -DCODEHARBOR_TEST_MINIMAL_DEBUGINFO=ON
```

Only `tst_*` executables are affected, and only in `Debug`/`RelWithDebInfo`; the
application binary and every packaged artifact are byte-for-byte unaffected.

Unity builds are deliberately not offered: they interact badly with Qt's
meta-object compilation and hide missing includes.

### Remote workspace (Node)

```bash
cd remote
npm install        # dev-only deps (typescript, @types/node); runtime has none
npm test           # node --test (count deliberately not pinned here: nothing
                   # checks it, and it went stale three times before this line)
npm run typecheck  # tsc --noEmit
npm run build      # tsc -> dist/ (codeharbord, codeharbor-bridge)
```

Smoke-test the services without a build:

```bash
# JSON-RPC over stdio
echo '{"jsonrpc":"2.0","id":1,"method":"server.info"}' | node src/codeharbord.ts rpc --stdio

# Agent-status bridge (listens on $XDG_RUNTIME_DIR/codeharbor.sock)
node src/bridge.ts
```

**Schema changes.** Two different version numbers exist and they mean different
things.

The WORKSPACE DATABASE is at version 5 (the session `pinned` flag). It is stated in
three places that must move together: `remote/sql/schema.sql`,
`WORKSPACE_SCHEMA_VERSION` in `remote/src/workspace.ts`, and
`WorkspaceDb::kSchemaVersion` in `src/persistence/WorkspaceDb.h`. Only the daemon
migrates; the client's copy is a record of what the schema is expected to be, and
tests on both sides pin it. A new column also needs a migration step for databases
already in the field: version 5 adds `pinned` with a default of 0, so an existing
database keeps every row and simply starts unpinned. Indexes are the exception and
belong in `remote/sql/indexes.sql`, which is applied on every open rather than only
on migration.

The RPC schema is separate and IS the compatibility gate: `RPC_SCHEMA_VERSION` in
`remote/src/codeharbord.ts` (currently 6) against
`AppController::kMinimumServerSchemaVersion`. A client refuses a server whose RPC
schema is older than it needs, which is the "Server too old" path.

## Test suites

```bash
ctest --preset dev                 # default unit + integration; desktop proof skips without a session bus
ctest --preset dev -L live         # live gates (need a real SSH server, see below)
ctest --preset dev -L desktop      # desktop-only proofs (need a session bus)
```

Targets added with the settings, pin, log and viewer-navigation work, all in the
default (portable) suite:

| Target | Covers |
|---|---|
| `tst_appsettings` | The client-local preferences store: defaults, validation of a hand-edited file, clamping, and that a reset leaves window-layout state alone |
| `tst_logbuffer` | The bounded diagnostics buffer: entry and byte caps, chaining to a previously installed message handler, filtering, and logging while a buffer is destroyed |
| `tst_logview` | The log surface, including a message emitted while it was closed still being there when it opens |
| `tst_openas` | The explorer's "Open as" menu: only applicable handlers offered, and "open in new pane" leaving the first pane alone |
| `tst_ui_polish` | The shared scrollbar's fit/overflow states and the viewer pane's cursor neutrality |

Existing targets that gained coverage in the same period:

- `tst_paneidentity` — viewer navigation (history truncation, disabled ends,
  reload of non-web content, Home with and without a session), per-pane header
  actions naming their own pane, the kill confirmation, and restoring the pane the
  user was last working in (including the stale-stamp drop and the missing-pane
  fallback).
- `tst_sessionlayouts` — pane titles and fast Dev Session switching.
- `tst_appcontroller` — archiving, deleting a session and a group, and that the
  sidebar's filters never narrow the tree the controller treats as authoritative.
- `tst_models` — filtered rows, and that a terminal-state update still emits
  row-local changes instead of resetting the whole sidebar.
- `tst_sidebar` / `tst_uxshell` — the archive and delete affordances, their
  confirmations naming their subject, declining them, and the empty-state wording
  when every session is archived.
- `tst_viewers` / `tst_appsettings` — the per-file-type default handler: resolution
  order, which handlers may be assigned to which type, and that an unset mapping
  changes nothing.
- `src/web/markdown` — its own `node --test` suite, including the sanitiser cases
  (script tags, event-handler attributes, `javascript:` links, frames) and
  relative image resolution.

`-L desktop` currently holds one target, `tst_notifierlive`, which delivers a real
notification through `org.freedesktop.Notifications`. It needs a session bus AND a
running notification daemon; without them it reports **Skipped**, never Passed, and
says so loudly — so a green run can never be mistaken for "notifications proven".
It is deliberately NOT in `-L live`, whose prerequisite is the SSH fixture instead.

### Live gates

The `live` label covers both SSH-backed integration gates and a small number of local WebEngine probes. SSH-backed targets exercise a real `sshd`, a real `codeharbord` over an SSH channel, real `tmux`, the packaged Monaco/xterm pages, or the real agent hook; those targets QSKIP unless `CH_LIVE_SSH` is set. `tst_webengine_headless` and the non-SSH renderer cases in `tst_terminalpage` do not require `CH_LIVE_SSH` and instead QSKIP only when WebEngine cannot run. List them with `ctest --preset dev -L live -N`; the headline SSH-backed gate is `tst_coldstart`, the whole first-run walkthrough (add a server, accept its host key, create and open a session, get a live shell, edit and save a remote file, relaunch and find it restored).

Requirements on the remote side: `tmux`, Node 23.6+, and this repository checked out
at the path given by `CH_LIVE_REPO`. Pointing the fixture at `127.0.0.1` (your own
machine as its own "remote") is the normal way to run them.

Environment contract:

| Variable | Meaning |
|---|---|
| `CH_LIVE_SSH` | Set to `1` to enable the gates; unset means QSKIP |
| `CH_LIVE_HOST` / `CH_LIVE_PORT` | SSH endpoint (e.g. `127.0.0.1` / `2222`) |
| `CH_LIVE_USER` | SSH login user |
| `CH_LIVE_NODE` | **Absolute** path to `node` on the remote side (a non-interactive SSH session usually has no version-manager `PATH`) |
| `CH_LIVE_REPO` | Absolute path to this repo on the remote side |
| `CH_LIVE_IDENTITY` | Optional private key used by `tst_livessh` to encrypt a temporary copy and prove the passphrase callback; never committed |
| `CH_LIVE_KNOWN_HOSTS` | Optional scratch known-hosts file; when unset, each gate uses a temporary path |
| `CH_LIVE_PASSWORD` | Optional password for the multi-step SSH authentication live gate |
| `CH_LIVE_KBDINT_PASSWORD` | Optional password for the keyboard-interactive SSH live gate |

Standing up a throwaway `sshd` fixture (no root, no changes to your `~/.ssh`):

```bash
tests/live/generate-fixture.sh                  # keys + sshd_config in tests/live/.fixture/
D=tests/live/.fixture
/usr/sbin/sshd -D -e -f $PWD/$D/sshd_config &   # unprivileged; logs to stderr
ssh-agent -a $PWD/$D/agent.sock &
SSH_AUTH_SOCK=$PWD/$D/agent.sock ssh-add $D/id
```

The generator is committed; its output is git-ignored, because it holds real
private keys. Re-running it is safe, and it is also the repair for a fixture whose
config has gone stale — it rewrites `sshd_config` in place, so restart the fixture
`sshd` afterwards.

**Why a generator rather than a recipe to copy.** The config it writes contains
`SetEnv CODEHARBOR_DB=<fixture>/workspace.sqlite`, and that line is load-bearing.
The live gates run a REAL `codeharbord` as you, and its workspace database
defaults to `~/.local/share/codeharbor/codeharbor.sqlite` — the same file the
server INSTALLED on this machine uses, which is to say your own workspace.
Without the override, a live run creates its test groups and sessions in your
real sidebar AND migrates that database to whatever schema the working tree
carries, after which the installed server refuses to start (`workspace schema is
newer than the build supports`) until it is upgraded. This document used to print
the `printf` that generates the config; the line was twice omitted while copying
it by hand, and real workspace rows had to be removed afterwards. A recipe that
quietly damages real data when mistyped is a fault in the recipe.

Two guards now stand in front of it:

- `tst_live_fixture_isolation` is a CTest SETUP fixture that every `live` target
  requires. It opens a session through the fixture and asks which
  `CODEHARBOR_DB` the daemon will actually see — not merely whether a line
  appears in a file — and fails with repair instructions when the answer is empty
  or points at the real database. Because it is a setup fixture, a failure means
  the live targets do not run at all rather than running against your workspace.
  With `CH_LIVE_SSH` unset it passes quietly and needs no SSH server.
- The four targets that create workspace rows (`tst_sessionlayouts`,
  `tst_liveshell`, `tst_coldstart`, `tst_terminalpage`) delete exactly the ids
  they created, and now FAIL when that cleanup does not succeed, naming the id
  they could not remove. Cleanup errors used to be ignored, which is how
  leftovers accumulated unnoticed.

If rows have already been left behind, remove them by the id the failing test
reported. Do NOT sweep by name: "delete every group whose name looks like a test"
is how somebody eventually deletes a real session.

If the schema migration has already happened, the workspace itself is fine — the
migration only adds columns. Upgrade the installed server (`Update server` in the
client, or unpack a newer `codeharbor-remote.tar.gz` over it) and it opens again.

Then run them. `CH_LIVE_SSH` and `CH_LIVE_HOST` / `CH_LIVE_PORT` / `CH_LIVE_USER` /
`CH_LIVE_NODE` / `CH_LIVE_REPO` are required for the SSH-backed gates. Omit
`CH_LIVE_SSH` and every gate QSKIPs, which reports as a green run that proved
nothing — the exact outcome this whole section exists to avoid. Omit any of the
other required variables and the gates fail their own precondition check instead.
`CH_LIVE_IDENTITY` and `CH_LIVE_KNOWN_HOSTS` are optional; the gates use their
documented fallback paths when they are unset.

```bash
export CH_LIVE_SSH=1
export CH_LIVE_HOST=127.0.0.1 CH_LIVE_PORT=2222 CH_LIVE_USER=$(whoami)
export CH_LIVE_NODE=$(command -v node) CH_LIVE_REPO=$PWD CH_LIVE_IDENTITY=$PWD/$D/id
export SSH_AUTH_SOCK=$PWD/tests/live/.fixture/agent.sock
export CH_LIVE_KNOWN_HOSTS=$(mktemp -u /tmp/ch_kh_XXXX)
ctest --preset dev -L live --output-on-failure
```

`CH_LIVE_PORT` and `CH_LIVE_HOST` must match the `Port` and `ListenAddress` in
the `sshd_config` written above; `CH_LIVE_USER` must be the account named by
`AllowUsers`.

> These tests render QML and Chromium headlessly; each target pins its own
> environment (`QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`,
> `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox --disable-dev-shm-usage`),
> so no display is needed. Do **not** add `--single-process`: Chromium aborts with
> "Single mode supports only single profile" as soon as a second
> `QWebEngineProfile` exists, and the viewer stack creates two by design (SPEC 7.3,
> "Browser Profiles").

## Cross-platform builds & releases

### Why native, not cross-compiled

The client embeds **Qt WebEngine (Chromium)**. Qt does not support
cross-compiling WebEngine, and building Chromium's toolchain for a foreign OS is
impractical. So CodeHarbor binaries are built **natively on each target OS** —
the standard approach for Qt+WebEngine apps. There is no supported way to produce
a Windows or macOS client from a Linux box (or vice versa).

The remote service (`remote/`) is the exception: it is platform-independent JS
and runs anywhere Node 23.6+ is installed, so it is packaged once.

### Release pipeline

[`.github/workflows/release.yml`](../.github/workflows/release.yml) builds all
targets on GitHub-hosted runners. Trigger it by pushing a tag (`git tag v0.1.0 &&
git push --tags`) or via the **Run workflow** button (manual `workflow_dispatch`).

| Job | Runner | Output artifact | Deploy tool |
|---|---|---|---|
| `linux` | `ubuntu-latest` | `CodeHarbor-*.AppImage` | linuxdeploy + qt plugin |
| `windows` | `windows-latest` | `codeharbor-windows/` (`codeharbor.exe` + Qt/libssh DLLs) and `CodeHarbor-<version>-windows-x64-setup.exe` | `windeployqt`, then Inno Setup (`packaging/windows/codeharbor.iss`) |
| `macos` | `macos-latest` | `codeharbor.dmg` (bundled `.app`) | `macdeployqt` |
| `remote` | `ubuntu-latest` | `codeharbor-remote.tar.gz` (`dist/` + `sql/` + `package.json`) | `tsc` + tar |

The `publish` job zips the Windows directory into `codeharbor-windows.zip`, then
refuses to publish unless both Windows assets are present **and** the
installer's filename carries exactly the tag's version. The installer takes its
version from `project(CodeHarbor VERSION …)` in the top-level `CMakeLists.txt`,
so that second check is what stops a `v0.2.0` tag on an un-bumped tree from
publishing an installer that calls itself 0.1.8.

A raw Qt/WebEngine executable is **not** runnable off the build machine; the
deploy tools bundle the Qt libraries, plugins, and the WebEngine runtime so the
artifact is self-contained. libssh is sourced per OS: `apt` (Linux), `brew`
(macOS), and `vcpkg` (Windows, via the CMake CONFIG package — see the normalized
`ch_libssh` target in the top-level `CMakeLists.txt`). The Windows job installs
it from the in-tree overlay port pinned to 0.12.2 and FAILS the build if any
other version lands, because vcpkg's registry port is the unusable 0.12.0.

### The release drill (do not skip the dry run)

1. **Dispatch `release.yml` on `main`** ("Run workflow"). The `publish` job is
   gated on `refs/tags/v*`, so this is a genuine dry run: it exercises all three
   OS builders and publishes nothing.
2. **Cut and push the tag only after the dry run.**
   `bash .omp/skills/bump-version/bump.sh --set X.Y.Z --push` syncs the version
   files, commits them, runs the local Debug build, ctest, all workspace npm
   tests and all workspace typechecks, then creates and pushes the annotated
   tag. If any local gate fails, no tag is created.
3. To create a tag without pushing, omit `--push`; the script prints the
   explicit push command. That mode is for inspection only and does not run the
   release preflight.

This ordering is not ceremony, for two reasons.

The workflow's first real execution failed on **each** platform for a different
reason — Linux could not resolve `libQt6SerialPort.so.6` while packaging, and
Windows both failed to build the web bundle and looked for the executable in the
wrong directory. A tag that fails leaves a permanent public tag with no release
attached; a dispatch costs nothing. CI now compiles and tests the client on all
three platforms per commit, so the *build* half of that class of failure is
caught long before a tag — but packaging is still release-only, and packaging is
where two of those three failures actually were.

The dry run also **refreshes the Windows vcpkg cache** from a ref a tag can read.
GitHub only lets a run restore a cache written by its own ref or by the default
branch, so a cache written by a tag build is unreachable from every later tag.
Every push to `main` now writes that entry as part of the CI client matrix's
Windows leg, and a dispatch on `main` writes it too; a tag can only ever restore
one of those — which is why `v0.1.0`, before either existed, paid the full
~9.8 min to rebuild libssh from source.

### Building locally on each OS

The commands are identical everywhere — only dependency install differs
(§ *Other platforms*):

```bash
cmake --preset release
cmake --build --preset release
```

On Windows, run from a Developer/MSVC shell (or after `ilammy/msvc-dev-cmd`) so
the Ninja generator finds `cl.exe`, and configure with vcpkg's toolchain for
libssh: `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` (or set
`CMAKE_TOOLCHAIN_FILE` in the environment — CMake 3.21+ reads it automatically).

### What a maintainer needs to provide

- **Nothing to get unsigned artifacts.** GitHub-hosted runners cover all three
  OSes; a public repo runs the matrix for free. Just enable Actions and push a
  tag.
- **Code signing (optional, for distribution):** not wired up. macOS needs an
  Apple Developer ID cert + notarization; Windows needs an Authenticode cert.
  Both are added later as encrypted repo secrets consumed by extra deploy steps.
  Unsigned artifacts run fine for internal/dev use (with an OS "unidentified
  developer" prompt).
- **Self-hosted runners:** only needed if you cannot use GitHub-hosted runners
  (e.g. Apple Silicon-specific builds); otherwise not required.

### Local cross-OS (non-WebEngine parts only)

If you ever split out a CLI/headless component with no WebEngine dependency, that
*could* be cross-compiled (Linux→Windows via MinGW-w64, Linux→macOS via osxcross)
or built for other Linux arches via `docker buildx` + QEMU. This does **not**
apply to the GUI client and is out of scope today.

## Notes & troubleshooting

- **libssh is optional at configure time.** If `libssh-dev` is missing, CMake
  prints a warning and defines `CH_HAVE_LIBSSH=0`; the tree still configures but
  `ch_ssh` is non-functional. Install `libssh-dev` for a working build.
- **`compile_commands.json`** is emitted into `build/dev/` for clangd/IDEs;
  symlink it to the repo root if your tooling expects it there.
- **Clean reconfigure:** `rm -rf build/dev && cmake --preset dev`.
- **Qt not found:** pass `-DCMAKE_PREFIX_PATH=/path/to/qt/6.x/gcc_64` (or set
  `CMAKE_PREFIX_PATH` in the environment) when Qt is not on the default path.
