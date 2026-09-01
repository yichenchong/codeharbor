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
| Qt 6 | 6.10.2 | 6.10 |
| libssh | 0.11.3 (verified runtime) | audited runtime floor 0.11.2; exactly 0.12.0 is warned about and worked around |
| Node.js | 24.16 | 23.6 (native TS type-stripping) |

The Windows release uses the separate in-tree vcpkg overlay, pinned to libssh
0.12.2. That overlay pin does not change the runtime floor for ordinary Linux
and macOS builds, which use libssh 0.11.2 or newer.

The CMake floor is Qt **6.10**, and CI builds and runs the portable suite on
6.10 to keep the claim honest. The floor was 6.9 until a crash forced it up:

- 6.9 is the first release with `QQuickWebEngineProfile(storageName, parent)`,
  the only profile-creation form that does not emit a deprecation warning the
  QML load test treats as a failure, so 6.8 and older do not build at all.
- 6.9 builds but **segfaults when a viewer pane swaps handlers while its page is
  still loading** - the ordinary "Open as" action on a slow or large file.
  Destroying a `WebEngineView` with a navigation still outstanding leaves
  Chromium to deliver the navigation decision afterwards, and
  `QQuickWebEngineViewPrivate::navigationRequested()` then wraps it for
  JavaScript through the QML engine the dead view no longer has, so
  `QJSEngine::newQObject()` dereferences null. It reproduces deterministically
  on 6.9.3 (`tst_paneidentity anUnrelatedRepublishKeepsAnOpenAsChoice`, ~1.4 s)
  and is absent on 6.10 in Debug, Release and under AddressSanitizer. No
  client-side mitigation exists: QML cannot give a dying object an engine, and
  `WebEngineView::stop()` on the still-live outgoing view was measured and does
  not prevent the callback.

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
| Qt6 Core / Gui / Network (and **Test**, when `CODEHARBOR_BUILD_TESTS=ON`) | `qt6-base-dev` |
| Qt6 Qml / Quick / **QuickControls2** | `qt6-declarative-dev` |
| Qt6 **WebEngineCore** / **WebEngineQuick** | `qt6-webengine-dev` |
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
  all three OSes. With either of those, select `qtwebengine`, `qtwebchannel` **and
  `qtpositioning`**: `Qt6WebEngineCore` hard-depends on Qt Positioning, so without
  it `find_package(Qt6 COMPONENTS WebEngineCore)` fails at configure time even
  though `Qt6WebEngineCoreConfig.cmake` is installed. Distribution packages pull
  it in as a dependency, so the `apt`/`dnf`/`pacman` lines above need nothing extra.

CI provisions the same set on **all three release platforms** — see
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml). Its `client` job is a
matrix over `ubuntu-latest`, `windows-latest` and `macos-latest`: Qt via
`jurplel/install-qt-action`, CMake and Ninja via `lukka/get-cmake`, and libssh via
`apt` on Linux, Homebrew on macOS and vcpkg (in-tree overlay port) on Windows —
the same per-OS provisioning `release.yml` uses, so a green run means the release
build compiles on every platform it ships to. What CI does *not* do for the
desktop client is package: `linuxdeploy`/`windeployqt`/`macdeployqt` and the Inno
Setup installer stay release-only.

CI also has three MOBILE jobs, which provision a deliberately smaller Qt:
`mobile shell (linux, no WebEngine)` builds and tests the mobile client on an
ordinary Linux runner with `qtwebengine` and `qtwebchannel` absent from the
image — that is the job that proves the client compiles and its logic passes,
since no phone is available in CI — while `android (arm64 APK)` and
`ios (simulator app)` cross-build the real packages. Unlike the desktop legs,
those two DO package, because for a mobile client packaging is the build.

## Build & run

### Client (Qt / CMake)

```bash
npm install                   # once: web-asset workspaces
npm run build                 # Monaco + xterm.js + Markdown bundles -> src/web/*/dist/
cmake --preset dev            # configure (Debug, Ninja, build/dev/)
cmake --build --preset dev    # build -> build/dev/src/app/codeharbor
./build/dev/src/app/codeharbor
```

That output path is the Linux/Unix one. The application target sets
`MACOSX_BUNDLE` and `WIN32_EXECUTABLE` (`src/app/CMakeLists.txt`), so macOS
produces `build/dev/src/app/codeharbor.app` and Windows produces
`codeharbor.exe` beside its DLLs in the build root (`build/dev/`) rather than
under `src/app/` — which is why the release workflow locates the Windows
executable instead of assuming a path.

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

The mobile presets — `mobile-desktop`, `android-arm64`, `android-arm64-debug`,
`ios-arm64` and `ios-simulator` — are covered in
§ *Mobile clients (Android / iOS)*. The iOS pair carries a Darwin `condition`,
so `cmake --list-presets` does not offer them on Linux or Windows.

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
`remote/src/codeharbord.ts` (currently 8 — 6 → 7 added the agent viewer control
channel, SPEC 6.8; 7 → 8 added `tmux.paneActivity`, the per-pane last-output time
that keeps a detached pane's sidebar status honest, SPEC 6.6) against
`AppController::kMinimumServerSchemaVersion`. A client refuses a server whose RPC
schema is older than it needs, which is the "Server too old" path.

## Mobile clients (Android / iOS)

The Android and iOS clients are a second presentation layer over the same
backend, not a second application. `ch_models`, `ch_remote`, `ch_persistence`,
`ch_ssh`, `ch_terminal`, `ch_editor`, `ch_agent` and `ch_app` are linked
unchanged; the daemon protocol is unchanged; `codeharbord` cannot tell the two
clients apart. What is replaced is everything the desktop draws with Qt
WebEngine — Qt ships **no WebEngine for Android and none for iOS**, so the
terminal, the editor and every viewer are native Qt Quick in `src/vt` and
`src/mobile` instead of Chromium pages.

Two CMake options decide what a tree contains:

| Option | Default | Builds |
|---|---|---|
| `CODEHARBOR_BUILD_DESKTOP` | `ON`, forced `OFF` on Android/iOS | `src/qml`, the WebEngine `ch_viewers`, the `codeharbor` executable |
| `CODEHARBOR_BUILD_MOBILE` | `ON` everywhere | `src/vt`, `src/mobile`, the `CodeHarbor.Mobile` (QML) and `CodeHarbor.Mobile.Core` (C++ types) QML modules, `codeharbor_mobile` |

`CODEHARBOR_BUILD_MOBILE` is on for desktop Linux/macOS/Windows **on purpose**.
The mobile shell is plain Qt Quick with no device-only API, so it compiles and
its `tst_*` targets run headless on a workstation; needing an NDK or an iPhone
to compile it would mean it only ever got compiled on a release runner. It is on
in the ordinary `dev` preset too, so `cmake --build --preset dev` already
produces `codeharbor_mobile` and `ctest --preset dev` already runs the mobile
and `src/vt` suites — the `mobile-desktop` preset below is the same content in
its own build tree, which is what lets a mobile experiment run without
disturbing `build/dev/`.

A third option interacts with these: `CODEHARBOR_BUILD_TESTS` is **forced
`OFF`** for an Android or iOS configure, with a status line saying so. Every
`tst_*` target is a host executable `ctest` launches locally, so a
cross-compiled one cannot run; on Android it would additionally get its own
`androiddeployqt` package target and join the global `apk` target, so building
the APK would build a dozen packages that all deploy into the same
`android-build` directory. Test the mobile client on the host, with
`mobile-desktop`.

### Developing the mobile shell without a device

```bash
cmake --preset mobile-desktop        # Debug, tests ON, BOTH clients (build/mobile-desktop/)
cmake --build --preset mobile-desktop
ctest --preset mobile-desktop
./build/mobile-desktop/src/mobile/codeharbor_mobile
```

This is the loop to use for anything that is not packaging. It builds the
desktop client too, which is what catches the mistake that matters most here:
a change to a shared backend module that suits one client and breaks the other.
To prove the mobile client has no WebEngine dependency at all — with the option
off, no `find_package` for it is issued anywhere, and no web bundle is built
because `src/qml` is never added — configure the same tree without the desktop
half:

```bash
cmake --preset mobile-desktop -DCODEHARBOR_BUILD_DESKTOP=OFF
cmake --build build/mobile-desktop
ctest --test-dir build/mobile-desktop
```

Tests stay ON there and still run. The test directories that drive the real
Chromium panes link `ch_viewers` and `codeharbor_qmlplugin` by name, so they are
desktop-only by construction and disappear with the desktop half:
`src/app/tests`, `src/editor/tests` and `src/terminal/tests` are each gated on
`CODEHARBOR_BUILD_DESKTOP` alongside `CODEHARBOR_BUILD_TESTS`;
`src/viewers/tests` sits inside the `ch_viewers` branch of
`src/viewers/CMakeLists.txt`, and `src/qml/tests` needs no gate of its own
because `src/qml` itself is only added for a desktop build.
Everything else, the mobile suites included, is unaffected. This is the
configuration CI uses to prove the mobile client links without WebEngine, and it
costs minutes rather than the desktop client's hour.

### Environment

Cross-compiling needs the SDKs on the machine; the presets read them from the
environment rather than hardcoding a layout:

| Variable | Used by | What it points at |
|---|---|---|
| `ANDROID_NDK_ROOT` | Android | NDK root. Qt's own toolchain file reads it and chainloads `$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake` for you; the preset does **not** name the NDK toolchain directly, because that file restricts `find_package` to the device sysroot and Qt would then be invisible (see below) |
| `ANDROID_SDK_ROOT` | Android | SDK root. Needed at **configure** time, not just at package time: the preset copies it into the cache variable of the same name, and Qt's deployment-settings generation globs `<sdk>/build-tools/*` while CMake is still running — an empty value aborts the configure with "Could not locate Android SDK build tools" |
| `QT_HOST_PATH` | both | the **host** Qt (`.../6.10.0/gcc_64`, `.../macos`). `moc`, `rcc`, `qmlcachegen` and `androiddeployqt` are host binaries |
| `QT_ANDROID_PATH` | Android | the Qt-for-Android prefix, e.g. `.../6.10.0/android_arm64_v8a` |
| `QT_IOS_PATH` | iOS | the Qt-for-iOS prefix, e.g. `.../6.10.0/ios` |
| `CH_LIBSSH_PREFIX` | both | a libssh built for that ABI (see below). On Android it is passed as `QT_ADDITIONAL_PACKAGES_PREFIX_PATH`, on iOS appended to `CMAKE_PREFIX_PATH` |

`QT_HOST_PATH` and the target Qt must be the **same Qt version**; a mismatch
fails at `qmlcachegen` with an unhelpful bytecode error.

The Android build uses the shared C++ runtime (`c++_shared`). Qt for Android is
built against it and `androiddeployqt` bundles `libc++_shared.so`, while the
NDK's own toolchain file defaults to `c++_static`; taking that default would load
two C++ runtimes into one process, which makes exceptions and `dynamic_cast`
across the app/Qt boundary undefined and crashes only on a device. Nothing in the
preset pins it, because Qt's `qt.toolchain.cmake` — which the preset does use —
sets it before chainloading the NDK toolchain. That is one of two reasons the Qt
toolchain is used rather than the NDK one directly; the other, and the one that
makes it mandatory, is that the NDK toolchain restricts `find_package` to the
device sysroot, so Qt itself cannot be found through it.

### Android

```bash
export ANDROID_NDK_ROOT=$HOME/Android/Sdk/ndk/27.2.12479018
export ANDROID_SDK_ROOT=$HOME/Android/Sdk
export QT_HOST_PATH=$HOME/Qt/6.10.0/gcc_64
export QT_ANDROID_PATH=$HOME/Qt/6.10.0/android_arm64_v8a
export CH_LIBSSH_PREFIX=$HOME/vcpkg/installed/arm64-android

cmake --preset android-arm64                       # Release, build/android-arm64/
cmake --build --preset android-arm64-apk           # -> APK
cmake --build --preset android-arm64-aab           # -> AAB, for Play upload
```

`android-arm64-debug` is the same toolchain in Debug, in its own binary
directory so a debuggable and a release package can coexist. The artefacts land
where `androiddeployqt` puts them:

```text
build/android-arm64/src/mobile/android-build/build/outputs/apk/release/android-build-release-unsigned.apk
build/android-arm64/src/mobile/android-build/build/outputs/bundle/release/android-build-release.aab
build/android-arm64-debug/src/mobile/android-build/build/outputs/apk/debug/android-build-debug.apk
```

`androiddeployqt` also copies whichever APK it just built to the path CMake asked
it for, so the stable name to hand to `adb install` is
`build/android-arm64/src/mobile/android-build/codeharbor_mobile.apk` (the Gradle
paths above are named after the `android-build` directory, not after the target).

Note the `-unsigned` in that release filename: Gradle does not sign a release
build, and `adb install` will refuse it with *"package appears to be invalid"*.
For a package to put on a device, either use the **debug** preset (Gradle signs
those with its own debug key) or sign the release one yourself:

```bash
CH_KEYSTORE_PASSWORD=... .github/scripts/sign-android.sh \
  ~/keys/codeharbor-release.jks codeharbor 28 \
  build/android-arm64/src/mobile/android-build/codeharbor_mobile.apk
```

That is the same script the release job runs, so it verifies the signature the
same way. Remember an app signed with a different key cannot upgrade one already
installed - uninstall first when switching between debug and release keys.

Only `arm64-v8a` is configured. Every Android device shipped in the last several
years is arm64, and a multi-ABI package multiplies build time by the number of
ABIs; add one by copying the preset rather than by making the existing one
multi-ABI.

That NDK revision is Qt 6.10's own, and it is worth pinning rather than taking
whatever is installed: Qt for Android is built against one NDK, and a different
one links but produces subtle STL and unwinder mismatches. `ubuntu-latest`
preinstalls a *newer* revision (27.3.13750724), so CI installs the pinned one
through `sdkmanager` and points `ANDROID_NDK_ROOT` at it. Note also that Qt 6.10
for Android is published under aqt's `all_os` host, not `linux`, whose Android
builds stop at 6.7.3, and that its Gradle plugin needs JDK 17.

Qt PDF is **not** an installable module for `android_arm64_v8a` in 6.10.0 (it is
for iOS), so `CH_HAVE_QTPDF` is 0 on Android. That is a clean capability-off,
never a build error: the PDF page reports that the platform cannot render one
and offers the file as bytes.

`packaging/android/` is the `ANDROID_PACKAGE_SOURCE_DIR` the `codeharbor_mobile`
target points at. It holds `AndroidManifest.xml` (application id
`dev.codeharbor.mobile`, `INTERNET` and no other permission, min SDK 28 —
Qt 6.10's floor and the preset's `ANDROID_PLATFORM` — target SDK 35) and
`res/`, whose adaptive launcher icon is a vector transcription of
`packaging/codeharbor.svg`.

It contains **no `build.gradle` and no `gradle.properties`**, and that is
deliberate rather than an omission. Qt copies its own `build.gradle` template
and `androiddeployqt` writes `gradle.properties` from the deployment settings on
every build; the only reason to add either file would be to override a default,
and this client overrides none. The two values a project usually reaches for
them for — min and target SDK — are target properties
(`QT_ANDROID_MIN_SDK_VERSION`, `QT_ANDROID_TARGET_SDK_VERSION`), set in
`src/mobile/CMakeLists.txt`, and Qt's template already puts our `res/` on
`res.srcDirs` alongside its own. Adding a stale copy of Qt's `build.gradle`
would silently pin the Android Gradle Plugin version to whatever Qt shipped the
day it was copied.

### iOS

```bash
export QT_HOST_PATH=$HOME/Qt/6.10.0/macos
export QT_IOS_PATH=$HOME/Qt/6.10.0/ios
export CH_LIBSSH_PREFIX=$HOME/vcpkg/installed/x64-ios      # simulator; arm64-ios for a device build

cmake --preset ios-simulator                       # build/ios-simulator/
cmake --build --preset ios-simulator

cmake --preset ios-arm64                           # device build
cmake --build --preset ios-arm64
```

Both presets use the **Xcode** generator, not Ninja: Qt's iOS deployment, bundle
assembly and code signing are all Xcode build settings, and a single-config
Ninja tree cannot express the per-configuration SDK the simulator needs. Both
carry a `condition` on a Darwin host, so they simply do not appear in
`cmake --list-presets` on Linux.

The simulator build is **x86_64**, and that is not a choice. The official Qt for
iOS package ships fat binaries carrying a device `arm64` slice and an `x86_64`
simulator slice, and no `arm64` simulator slice at all; asking for `arm64` with
the simulator SDK therefore selects the device slice and the link fails with
`building for 'iOS-simulator', but linking in object file ... built for 'iOS'`.
So `ios-simulator` sets `CMAKE_OSX_ARCHITECTURES=x86_64` and `ios-arm64` sets
`arm64`, which is also why the simulator's libssh comes from vcpkg's `x64-ios`
triplet (vcpkg selects the `iphonesimulator` sysroot for its x64/x86 iOS
architectures) while a device build uses `arm64-ios`. On an Apple silicon Mac the
resulting simulator app runs under Rosetta. A native `arm64` simulator build
needs a Qt built from source with `-sdk iphonesimulator`, which this project does
not require of anyone.

`ios-simulator` turns code signing off outright — the simulator does not check
it, and requiring it would stop an unsigned CI machine from compiling the client
at all. `ios-arm64` leaves signing to the machine that runs it: pass
`-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<team id>` or set it in Xcode, so no
team identifier is committed. Bundles land at
`build/ios-simulator/src/mobile/Release-iphonesimulator/codeharbor_mobile.app` and
`build/ios-arm64/src/mobile/Release-iphoneos/codeharbor_mobile.app`.

`packaging/ios/Info.plist.in` is a `configure_file` template
(`@ONLY`, so Xcode's own `$(VAR)` settings survive):
`CFBundleIdentifier` comes from the `CODEHARBOR_MOBILE_BUNDLE_ID` cache variable
(the same string the Android manifest spells literally — an Android configure
fails if the two disagree), and both
`CFBundleShortVersionString` and `CFBundleVersion` come from `PROJECT_VERSION`,
so the bump-version drill covers iOS with no extra step.
`ITSAppUsesNonExemptEncryption` is declared **true**, honestly: this client's
whole purpose is an SSH connection. An App Store submission therefore needs
export compliance documentation (a self-classification report under US EAR
740.17) — a release-time task, and the reason the key is answered in the plist
rather than left for App Store Connect to ask on every upload.

The plist also declares the launch screen (an empty `UILaunchScreen`
dictionary — the key-only form, no storyboard), and `src/mobile/CMakeLists.txt`
sets `QT_NO_SET_DEFAULT_IOS_LAUNCH_SCREEN` so Qt does not additionally copy its
own `LaunchScreen.storyboard` into a bundle that never references it. There is
no asset catalog and no app-icon set here — Android's `res/` has an adaptive
launcher icon, iOS has no counterpart — so an iOS build shows Xcode's
placeholder icon, which App Store review rejects; that is a release-time gap,
not a build one.

### libssh per mobile ABI

There is no `apt` or `brew` inside a cross-compile, and neither the NDK nor the
Qt-for-Android/iOS prefix carries libssh. Build it from the in-tree overlay,
which is already mobile-aware: `packaging/vcpkg-ports/libssh` carries
`android-glob-tilde.diff` (Bionic has no `GLOB_TILDE`) and
`0004-file-permissions-constants.patch` (Bionic has no `S_IWRITE`), and its
`vcpkg.json` drops the `pcap` and `server` default features on Android.

```bash
$VCPKG_ROOT/vcpkg install libssh \
    --overlay-ports=packaging/vcpkg-ports \
    --triplet arm64-android          # or arm64-ios
export CH_LIBSSH_PREFIX=$VCPKG_ROOT/installed/arm64-android
```

vcpkg's own `arm64-android` triplet needs `ANDROID_NDK_HOME` exported (it is the
same path as `ANDROID_NDK_ROOT`). On **Android** the preset hands
`CH_LIBSSH_PREFIX` to Qt as `QT_ADDITIONAL_PACKAGES_PREFIX_PATH` rather than
putting it on `CMAKE_PREFIX_PATH`: the NDK toolchain that Qt's toolchain
chainloads sets `CMAKE_FIND_ROOT_PATH` to the device sysroot and
`CMAKE_FIND_ROOT_PATH_MODE_PACKAGE` to `ONLY`, so a package outside that sysroot
is not found however `CMAKE_PREFIX_PATH` is spelled. Qt's toolchain prepends
every entry of `QT_ADDITIONAL_PACKAGES_PREFIX_PATH` to both variables, which is
what makes an out-of-sysroot libssh findable. On **iOS** there is no such
restriction, so `CMAKE_PREFIX_PATH` is used directly.

Leaving `CH_LIBSSH_PREFIX` unset is not a configure error — it degrades to the
usual `CH_HAVE_LIBSSH=0` warning, plus an Android/iOS-specific `-- libssh:
cross-compiling for …` status line pointing back here. That combination builds,
installs and launches, and then cannot connect to anything, which on a device is
a slow thing to diagnose; read the configure output.

### The single-pane UX contract

The desktop client shows three regions and arbitrary splits. The mobile client
shows **exactly one pane at a time — one viewer or one terminal** — and reaches
it through a two-step selection: pick a Dev Session, then pick the pane inside
it. There is no split, no region resize, and no window chrome.

This is not a subset that will be filled in later. It follows from the same
server-authoritative layout tree the desktop reads: the mobile client flattens
`viewerTree` and `terminalTree` depth-first into a flat list
(`ch::PaneListModel`) and shows one leaf, so a pane keeps its server-minted
identity (`terminalPaneId` for terminals) and both clients can be attached to
the same Dev Session at once without either one rewriting the other's layout.

Two rules constrain every mobile QML file, and both are security rules
(SPEC 7.5, 2.4, 7.4):

- Every server-controlled string is rendered with `textFormat: Text.PlainText`.
  `Text.MarkdownText`, `Text.StyledText`, `Text.RichText` and `Text.AutoText`
  are **banned** in `src/mobile/qml`. Markdown is rendered by parsing it into a
  block model (`ch::MarkdownModel`) and drawing plain-text blocks; raw HTML in a
  document is shown as literal text and never interpreted.
- A remote `file://` or internal URL is never handed to `QDesktopServices`, an
  OS handler, or any other app, and no mobile surface performs network access on
  behalf of remote content. Remote bytes reach the screen only through the
  daemon's `file.readFile`.

Two viewer backends are optional, discovered at configure time exactly as
libssh and QtDBus are, and reported as `CH_HAVE_QTPDF` / `CH_HAVE_QTWEBVIEW`
(visible to QML through `ch::MobileCapabilities`): Qt PDF for the PDF page and
Qt WebView for the web page. Neither is in a default Qt install. Without them
the corresponding page says so; nothing is silently opened elsewhere, and the
page's `.qml` file is not even added to the QML module (an `import QtQuick.Pdf`
in a compiled module fails the whole module, not just that file).

Each capability needs BOTH halves of its module, and the configure requires
both: `Qt6::Pdf` **and** `Qt6::PdfQuick`, `Qt6::WebView` **and**
`Qt6::WebViewQuick`. The plain target is the C++ library; the `*Quick` target is
the one that carries the QML module (`QtQuick.Pdf`, `QtWebView`) the page
imports, and on a static Qt — which is what Qt for iOS is — an import whose
module was never linked is a blank page at runtime. On Ubuntu/Debian the
packages are `qt6-pdf-dev` and `qt6-webview-dev`; they are not in the apt list
above because a desktop build never links either, so a stock workstation
configure reports both capabilities off.

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
| `tst_viewercommands` | The client end of the agent viewer control channel (SPEC 6.8): a `viewer.command` notification off the wire becoming a validated signal, an answer becoming one `viewer.commandResult` request, a double answer dropped, and the in-flight bound refusing past it |

Existing targets that gained coverage in the same period:

- `tst_paneidentity` — viewer navigation (history truncation, disabled ends,
  reload of non-web content, Home with and without a session), per-pane header
  actions naming their own pane, the kill confirmation, and restoring the pane the
  user was last working in (including the stale-stamp drop and the missing-pane
  fallback).
  It also covers the host's `openPaneTarget()` letting the handler registry choose
  when the caller names no handler — the "Open as" override refuses an empty one,
  which used to answer a plain "open this file" with a refusal.
- `tst_terminalcontroller` — the viewer control socket exported into a pane's tmux
  environment: set at session creation, refreshed on every attach (a reconnect is a
  new daemon with a new socket), unset when the server reports none, and never
  exported for a pane with no identity.
- `tst_agentmonitor` — the sidebar status of a pane the user is NOT looking at: a
  detached `generic` pane reads `unknown` rather than the false `idle` it used to
  settle on, a server-observed activity age drives it back to `running` and on to
  `idle`, that age refutes the silence timeout for an adapter-backed pane too, and
  an unseen completion, `waiting_input`, `error` and `stopped` all survive both a
  detach and a remote observation.
- `tst_tmuxactivity` — the poller: the age is computed from the SERVER's clock, a
  pane tmux could not date reports nothing at all, and a tick while a request is
  still in flight issues no second request.
- `tst_terminalpage` — the inverted mouse policy: a plain drag makes a local
  selection that survives, and a modifier drag reaches the remote program instead.
- `tst_uxshell` — `theSheetInFrontKeepsTheKeyboard`: the connect sheet and the
  settings window are both keyboard-containing surfaces and Main.qml can have both
  up at once, so each has to yield to the one in front. They used to pull against
  each other — every pull emits `activeFocusItemChanged` synchronously, so the
  other sheet's handler ran nested inside the first's — until the JS stack was
  exhausted. That reported as `RangeError: Maximum call stack size exceeded`
  against whatever unrelated binding the cascade happened to reach, which is why
  the failure named `LogView.qml` and `SettingsWindow.qml` at line numbers that
  were not line numbers at all. It took `tst_coldstart` down with it, and only
  under `-L live`, because a cold start is what puts both surfaces on screen.
- `tst_terminalpage` — `x10MouseReportsReachTheRemoteSideAsBytes`: a drag with
  ONLY `?1000h` enabled, i.e. the X10 encoding whose reports xterm.js emits as a
  binary event. Those were dropped outright before; the case asserts the encoded
  bytes arrive on the transport.
- `tst_liveterminalfactory` — `anUncleanNetworkDropKeepsTheSessionItsWorkAndSaysNothing`:
  the fixture sshd's descendants are killed so the transport dies with no orderly
  close, then the client reconnects. Asserts tmux's `session_created` is unchanged
  (same session), that a marker process inside the pane kept writing across the
  whole drop, and that no lost-session notice was raised. The marker assertion
  WAITS for the count to grow rather than sampling once: it writes a line a
  second, a reconnect can finish inside that second, and a one-shot read would
  fail at random by claiming the user's work had died.
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

**Never separate tmux `-F` fields with a non-printable byte.** tmux passes a
non-printable character through a format only when the tmux CLIENT invoking it
considers itself UTF-8-capable; otherwise `utf8_sanitize()` rewrites it to `_`. A
client is non-UTF-8 when its locale is non-UTF-8 AND it is not itself inside a
tmux session, and that is exactly how `codeharbord` runs: an SSH exec channel with
no `LANG` and no `TMUX`. Measured on one private server, one command: client under
`C` with `TMUX` unset gives `M_1786814684_mx`, the same client under `C` with
`TMUX` set gives `M\t1786814684\tmx`, and `C.UTF-8` gives the tabs too.

This shipped as a real defect: both listing formats used a TAB, so on any normally
configured host `tmux.listSessions` answered `[]` and `tmux.paneActivity` dated no
pane. It hid for a release because every unit fixture hard-coded tab-separated
bytes that real tmux never produces. The separator is now one exported constant,
`LIST_FIELD_SEPARATOR` in `remote/src/tmux.ts`, used by both formats, both marker
anchors and both parsers; the fixtures build their lines from it rather than
spelling it; and `remote/test/tmux-live.test.ts` drives a REAL tmux on a private
`-L` server with `LC_ALL=C` and `TMUX` deleted, which is the only guard that
reproduces the daemon's own environment. Reverting the constant to a tab fails
that file with `real tmux listing did not parse; got []` — the production symptom.
Test it by hand from a UTF-8 shell, or from inside tmux, and it will look fine.

**The producer at the head of a chain needs its own gate.** The oh-my-pi status
integration shipped unusable because every test drove the CLI script directly, so
the whole chain was green while the harness itself invoked nothing. Two tests now
run the REAL binaries: `remote/test/oh-my-pi-live.test.ts` runs an actual `omp`
with the extension loaded and asserts the bridge receives
`starting → running → idle_unseen → stopped`, and `remote/test/tmux-live.test.ts`
does the same job for tmux. Both skip cleanly when the binary is absent. Prefer
one of these over another fixture-fed unit test whenever the thing that could be
wrong is an assumption about an external program's interface.

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
| `linux` | `ubuntu-latest` | `CodeHarbor-<version>-x86_64.AppImage` | linuxdeploy + qt plugin |
| `windows` | `windows-latest` | `codeharbor-windows/` (`codeharbor.exe` + Qt/libssh DLLs) and `CodeHarbor-<version>-windows-x64-setup.exe` | `windeployqt`, then Inno Setup (`packaging/windows/codeharbor.iss`) |
| `macos` | `macos-latest` | `CodeHarbor-<version>-macos-<arch>.dmg` (bundled `.app`; `<arch>` is the runner's `uname -m`) | `macdeployqt` |
| `remote` | `ubuntu-latest` | `codeharbor-remote.tar.gz` (`dist/` + `sql/` + `package.json`) | `tsc` + tar |
| `android` | `ubuntu-latest` | `CodeHarbor-<version>-android-arm64.apk` and `…-android-arm64.aab` | `androiddeployqt` (Qt's `apk`/`aab` targets), then `.github/scripts/sign-android.sh` |
| `ios-simulator` | `macos-latest` | `CodeHarbor-<version>-ios-simulator.zip` (a Release `.app` bundle, `ditto`-archived) | Xcode, no code signing |

#### Android signing (required to publish)

The Android assets are **signed** by the release job, and this is not optional:
Android's installer rejects a package with no signature, so an unsigned APK
fails on every device with *"App not installed as package appears to be
invalid"*. v0.4.0 shipped exactly that - a 72 MB asset nobody could install -
because the workflow checked that assets were present, named and checksummed,
and never that one could be installed.

Signing needs four repository secrets (**Settings -> Secrets and variables ->
Actions**):

| Secret | Contents |
|---|---|
| `ANDROID_KEYSTORE_BASE64` | the keystore file, base64-encoded |
| `ANDROID_KEYSTORE_PASSWORD` | store password |
| `ANDROID_KEY_ALIAS` | key alias inside the keystore |
| `ANDROID_KEY_PASSWORD` | key password (optional; defaults to the store password) |

Create the key **once** and keep it forever. Android identifies an installed app
by its signing certificate, so a later release signed with a different key
cannot upgrade an installed one in place - users would have to uninstall first,
losing their data. Back the file up somewhere you will still have in five years;
losing it is unrecoverable for anyone who has the app installed.

```bash
keytool -genkeypair -v \
  -keystore codeharbor-release.jks -alias codeharbor \
  -keyalg RSA -keysize 4096 -validity 10000 \
  -dname "CN=CodeHarbor, O=CodeHarbor, C=GB"

base64 -w0 codeharbor-release.jks   # the value for ANDROID_KEYSTORE_BASE64
```

`-validity 10000` (~27 years) matters: Play rejects an upload key expiring
before 2033, and an expired key cannot sign an update.

On a **tag** run the secrets are mandatory - the job fails rather than publish
an uninstallable asset. A `workflow_dispatch` dry run without them still builds
and appends `-unsigned` to both names, so the drill works on a fork with no
credentials. `publish` independently re-checks the uploaded APK for an APK
Signing Block and refuses to release one without it.

The **iOS** asset is still unsigned, and its name says so: it is the simulator
slice, which checks no signature. A device build (`ios-arm64`) and any App Store
submission need a provisioning profile and a distribution certificate, so they
happen on a machine that owns those credentials and are deliberately not part of
this workflow.

The `publish` job zips the Windows directory into `codeharbor-windows.zip`, then
refuses to publish unless **every** expected asset is present exactly once and
every versioned filename carries exactly the tag's version. Those names are
stamped from `project(CodeHarbor VERSION …)` in the top-level `CMakeLists.txt`
(one parser, `.github/scripts/release-version.sh`, shared by all five build
jobs), so that second check is what stops a `v0.2.0` tag on an un-bumped tree
from publishing an installer that calls itself 0.1.8.

A raw Qt/WebEngine executable is **not** runnable off the build machine; the
deploy tools bundle the Qt libraries, plugins, and the WebEngine runtime so the
artifact is self-contained. libssh is sourced per OS: `apt` (Linux), `brew`
(macOS), and `vcpkg` (Windows, via the CMake CONFIG package — see the normalized
`ch_libssh` target in the top-level `CMakeLists.txt`). The Windows job installs
it from the in-tree overlay port pinned to 0.12.2 and FAILS the build if any
other version lands, because vcpkg's registry port is the unusable 0.12.0.

### The release drill (do not skip the dry run)

1. **Dispatch `release.yml` on `main`** ("Run workflow"). `publish` is gated on
   `github.event_name == 'push'` **and** `refs/tags/v*`, so this is a genuine
   dry run: it exercises all five builders — three desktop, Android, iOS
   simulator — and publishes nothing. Signing secrets ARE available to a
   dispatch, so the Android packages get really signed here; that is the point,
   because a wrong keystore password should surface before a tag exists.

   Dispatch on `main`, **never** `--ref <a tag>`. GitHub runs the workflow file
   *as defined at the ref you dispatch*, so aiming a dispatch at an old tag
   silently runs that tag's workflow and tests nothing you just changed - a
   green result there is vacuous. To dry-run the CURRENT workflow against an
   older tree, dispatch on `main` and pass the tag through the `ref` input
   (`gh workflow run release.yml --ref main -f ref=refs/tags/v0.4.0`), which is
   what the build jobs check out.
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
three desktop platforms per commit, and cross-builds the Android and iOS
packages, so the *build* half of that class of failure is caught long before a
tag. Desktop packaging (`linuxdeploy`/`windeployqt`/`macdeployqt` and the Inno
Setup installer) is still release-only, and that is where two of those three
failures actually were.

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
- **Remote sessions that vanish on disconnect.** SPEC 2.2 promises terminal
  processes outlive a disconnect, and tmux delivers that — but two things on the
  REMOTE host can break it, neither of them visible from the client. A session
  that dies this way is not reported: the next attach runs
  `tmux new-session -A`, which silently creates a fresh empty session under the
  same name, so the pane simply comes back blank.
  - `destroy-unattached on` in the user's `~/.tmux.conf` destroys a session as
    soon as its last client leaves. CodeHarbor turns it off per session at
    creation (`tmuxNewSessionCommand`), which covers every session it makes and
    any surviving session it re-attaches to. A session created BEFORE that guard
    existed is not covered: it inherits the global `on` and dies at its next
    detach, before anything can be set on it. Those can be guarded by hand,
    while they are still alive, on the remote host:

    ```bash
    tmux ls -F '#{session_name}' | grep '^ch_' \
      | xargs -I{} tmux set-option -t '={}:' destroy-unattached off
    ```
  - `systemd-logind` with `KillUserProcesses=yes` kills the processes in a
    user's SESSION SCOPE when their last login session ends — a tmux server
    started from an SSH session is in that scope, so it dies with it. Nothing in
    the client prevents this, and `loginctl enable-linger` alone does NOT fix
    it: lingering keeps a user manager running, it does not move an
    already-running tmux out of the session scope it was started in. The
    remedies are, on the host:
    - the administrator setting `KillUserProcesses=no` in `logind.conf`, or
      naming the user in `KillExcludeUsers=`; or
    - starting tmux under the user manager instead of the session scope, e.g.
      `systemd-run --user --scope tmux …`, which places it in the user slice.
      CodeHarbor does not do this today — it runs plain `tmux new-session`, so
      on such a host the session's lifetime is the login session's.

  To tell them apart after it happens: if the user's own unrelated tmux sessions
  died too, it was logind (one tmux server holds them all — the command passes
  no `-L`/`-S`); if only CodeHarbor's died, it was `destroy-unattached`.
