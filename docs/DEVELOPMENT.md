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
| Qt 6 | 6.10.2 | 6.6 |
| libssh | 0.11.3 | 0.10+ |
| Node.js | 24.16 | 23.6 (native TS type-stripping) |

The CMake floor is Qt **6.6**; newer (6.10 here) works unchanged.

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
cmake --preset dev            # configure (Debug, Ninja, build/dev/)
cmake --build --preset dev    # build -> build/dev/src/app/codeharbor
./build/dev/src/app/codeharbor
```

Presets are defined in [`CMakePresets.json`](../CMakePresets.json):
`dev` (Debug + tests) and `release`.

> Running the GUI needs a display. On a headless box use an X/Wayland session or
> `xvfb-run ./build/dev/src/app/codeharbor`. WebEngine may need
> `--no-sandbox` in constrained containers.

### Remote workspace (Node)

```bash
cd remote
npm install        # dev-only deps (typescript, @types/node); runtime has none
npm test           # node --test -> 11 tests
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
