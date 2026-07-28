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
| CMake | 4.2.3 | 3.24 |
| Ninja | 1.13.2 | any |
| Qt 6 | 6.10.2 | 6.9 |
| libssh | 0.11.3 | 0.10+ |
| Node.js | 24.16 | 23.6 (native TS type-stripping) |

The CMake floor is Qt **6.9**; newer (6.10 here) works unchanged. 6.9 is not a
guess: it is the first release with `QQuickWebEngineProfile(storageName, parent)`,
and CI builds and runs the portable suite on 6.9 to keep the claim honest.

## Ubuntu / Debian (apt)

### 1. Client build dependencies

```bash
sudo apt-get update && sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config libssh-dev \
  qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools \
  qt6-webengine-dev qt6-webengine-dev-tools qt6-webchannel-dev \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-window \
  qml6-module-qtquick-layouts qml6-module-qtwebengine libqt6sql6-sqlite
```

What each group provides (maps 1:1 to the `find_package(Qt6 ... COMPONENTS ...)`
in the top-level `CMakeLists.txt`):

| Requirement | Package(s) |
|---|---|
| Compiler + make | `build-essential` |
| CMake + Ninja generator | `cmake`, `ninja-build` |
| `pkg_check_modules(libssh)` (`ch_ssh`) | `pkg-config`, `libssh-dev` |
| Qt6 Core / Gui / Network / **Sql** | `qt6-base-dev` (+ `libqt6sql6-sqlite` runtime driver) |
| Qt6 Qml / Quick / **QuickControls2** | `qt6-declarative-dev` |
| Qt6 **WebEngineQuick** | `qt6-webengine-dev` |
| Qt6 **WebChannel** | `qt6-webchannel-dev` |
| `qt_add_qml_module` / `moc` / `qmltc` tooling | `qt6-base-dev-tools`, `qt6-declarative-dev-tools`, `qt6-webengine-dev-tools` |
| Runtime QML imports for the three-region UI | `qml6-module-qtquick*`, `qml6-module-qtwebengine` |

> `qt6-tools-dev` (Qt Linguist etc.) is **not** required.

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

CI provisions the same set — see [`.github/workflows/ci.yml`](../.github/workflows/ci.yml)
(`client` job installs Qt via `jurplel/install-qt-action` + `apt` for libssh/ninja).

## Build & run

### Client (Qt / CMake)

```bash
npm install                                      # once: web-asset workspaces
npm run build --workspace src/web/editor         # Monaco bundle -> src/web/editor/dist/
cmake --preset dev            # configure (Debug, Ninja, build/dev/)
cmake --build --preset dev    # build -> build/dev/src/app/codeharbor
./build/dev/src/app/codeharbor
```

The Monaco bundle is a gitignored build artifact that CMake embeds as
`qrc:/codeharbor/web/editor/index.html` (see `src/qml/CMakeLists.txt`). CMake builds
it for you at **configure** time when it is missing or older than its sources — so
`npm install` above is the only prerequisite, and editing `src/web/editor/**`
re-triggers it on the next configure.

If the bundle is missing and cannot be built (no `npm` on `PATH`), configure
**fails** with the command to run: a client whose editor pane silently loads nothing
is never the default. Pass `-DCODEHARBOR_SKIP_WEB_BUNDLE=ON` to deliberately build
an editor-less client.

Presets are defined in [`CMakePresets.json`](../CMakePresets.json):
`dev` (Debug + tests) and `release`.

> Running the GUI needs a display. On a headless box use an X/Wayland session or
> `xvfb-run ./build/dev/src/app/codeharbor`. WebEngine may need
> `--no-sandbox` in constrained containers.

### Remote workspace (Node)

```bash
cd remote
npm install        # dev-only deps (typescript, @types/node); runtime has none
npm test           # node --test -> 99 tests
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

## Test suites

```bash
ctest --preset dev                 # default: unit + integration, no external deps
ctest --preset dev -L live         # live gates (need a real SSH server, see below)
ctest --preset dev -L desktop      # desktop-only proofs (need a session bus)
```

`-L desktop` currently holds one target, `tst_notifierlive`, which delivers a real
notification through `org.freedesktop.Notifications`. It needs a session bus AND a
running notification daemon; without them it reports **Skipped**, never Passed, and
says so loudly — so a green run can never be mistaken for "notifications proven".
It is deliberately NOT in `-L live`, whose prerequisite is the SSH fixture instead.

### Live gates

The `live`-labelled targets exercise the real thing: a real `sshd`, a real
`codeharbord` over an SSH channel, real `tmux`, the real Monaco and xterm.js pages,
and the real agent hook. They **QSKIP** unless `CH_LIVE_SSH` is set, so the default
suite stays portable. List them with `ctest --preset dev -L live -N`; the headline
one is **`tst_coldstart`**, the whole first-run walkthrough (add a server, accept its
host key, create and open a session, get a live shell, edit and save a remote file,
relaunch and find it restored).

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
| `CH_LIVE_KNOWN_HOSTS` | Scratch known-hosts file; the first-use host key is persisted here |
| `SSH_AUTH_SOCK` | Agent socket holding the key — the pool authenticates via ssh-agent first |

Standing up a throwaway `sshd` fixture (no root, no changes to your `~/.ssh`):

```bash
D=tests/live/.fixture && mkdir -p $D          # gitignored: it holds real private keys
ssh-keygen -q -t ed25519 -f $D/hostkey -N ''
ssh-keygen -q -t ed25519 -f $D/id -N ''
cat $D/id.pub > $D/authorized_keys && chmod 600 $D/authorized_keys $D/id
printf 'Port 2222\nListenAddress 127.0.0.1\nHostKey %s/hostkey\nAuthorizedKeysFile %s/authorized_keys\nPidFile %s/sshd.pid\nUsePAM no\nStrictModes no\nPasswordAuthentication no\nAllowUsers %s\nSubsystem sftp internal-sftp\n' \
  "$PWD/$D" "$PWD/$D" "$PWD/$D" "$(whoami)" > $D/sshd_config

/usr/sbin/sshd -D -e -f $PWD/$D/sshd_config &   # unprivileged; logs to stderr
ssh-agent -a $PWD/$D/agent.sock &
SSH_AUTH_SOCK=$PWD/$D/agent.sock ssh-add $D/id
```

Then run them:

```bash
export CH_LIVE_NODE=$(command -v node) CH_LIVE_REPO=$PWD CH_LIVE_IDENTITY=$PWD/$D/id
export SSH_AUTH_SOCK=$PWD/tests/live/.fixture/agent.sock
export CH_LIVE_KNOWN_HOSTS=$(mktemp -u /tmp/ch_kh_XXXX)
ctest --preset dev -L live --output-on-failure
```

> These tests render QML and Chromium headlessly; each target pins its own
> environment (`QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`,
> `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox --disable-dev-shm-usage`),
> so no display is needed. Do **not** add `--single-process`: Chromium aborts with
> "Single mode supports only single profile" as soon as a second
> `QWebEngineProfile` exists, and the viewer stack creates two by design (SPEC 7.2).

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
| `windows` | `windows-latest` | `codeharbor.exe` + Qt/libssh DLLs | `windeployqt` |
| `macos` | `macos-latest` | `codeharbor.dmg` (bundled `.app`) | `macdeployqt` |
| `remote` | `ubuntu-latest` | `codeharbor-remote.tar.gz` (`dist/` + `package.json`) | `tsc` + tar |

A raw Qt/WebEngine executable is **not** runnable off the build machine; the
deploy tools bundle the Qt libraries, plugins, and the WebEngine runtime so the
artifact is self-contained. libssh is sourced per OS: `apt` (Linux), `brew`
(macOS), and `vcpkg` (Windows, via the CMake CONFIG package — see the normalized
`ch_libssh` target in the top-level `CMakeLists.txt`).

### The release drill (do not skip the dry run)

1. **Dispatch `release.yml` on `main`** ("Run workflow"). The `publish` job is
   gated on `refs/tags/v*`, so this is a genuine dry run: it exercises all three
   OS builders and publishes nothing.
2. **Then push the tag.** `bash .omp/skills/bump-version/bump.sh --set X.Y.Z`.

This ordering is not ceremony, for two reasons.

The workflow's first real execution failed on **each** platform for a different
reason — Linux could not resolve `libQt6SerialPort.so.6` while packaging, and
Windows both failed to build the web bundle and looked for the executable in the
wrong directory. A tag that fails leaves a permanent public tag with no release
attached; a dispatch costs nothing.

The dry run also **seeds the Windows vcpkg cache**. GitHub only lets a run
restore a cache written by its own ref or by the default branch, so a cache
written by a tag build is unreachable from every later tag. Seeding happens on
`main` or not at all — which is why `v0.1.0` paid the full ~9.8 min to rebuild
libssh from source.

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
