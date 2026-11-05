#!/usr/bin/env bash
# Configure the real project in a disposable source copy, never its shared dist/.
set -euo pipefail
root=$(CDPATH= cd -- "${1:-.}" && pwd)
qt_dir=${2:?Qt6 package directory is required}
for tool in cmake npm ninja; do
    command -v "$tool" >/dev/null || { echo "$tool not found; skipping" >&2; exit 77; }
done
[ -d "$root/node_modules/esbuild" ] || { echo "web dependencies not installed; skipping" >&2; exit 77; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
source="$work/source"
mkdir "$source"
cp "$root/CMakeLists.txt" "$root/package.json" "$root/package-lock.json" "$source/"
cp -R "$root/src" "$root/packaging" "$source/"
ln -s "$root/node_modules" "$source/node_modules"
configure() {
    cmake -S "$source" -B "$work/build" -G Ninja \
        -DQt6_DIR="$qt_dir" -DCODEHARBOR_BUILD_TESTS=OFF \
        -DCODEHARBOR_BUILD_MOBILE=OFF -DCODEHARBOR_SKIP_WEB_BUNDLE=OFF \
        -DCODEHARBOR_WEB_SOURCEMAP=OFF -DCMAKE_BUILD_TYPE=Release \
        >"$work/configure.log" 2>&1 || { cat "$work/configure.log" >&2; exit 1; }
}
configure
# Losing a non-index output must be repaired just like a missing index.html.
# Without that repair qt_add_resources fails during the next configure.
for file in terminal/terminal.js editor/editor.worker.js markdown/markdown.css; do
    rm "$source/src/web/${file%/*}/dist/${file#*/}"
done
configure
for file in terminal/terminal.js editor/editor.worker.js markdown/markdown.css; do
    [ -s "$source/src/web/${file%/*}/dist/${file#*/}" ]
done

# Dependencies changed without touching workspace source files. Keep an existing
# CSS output as a marker: a stale bundle leaves it untouched; a rebuild repairs it.
printf 'stale bundle marker\n' >"$source/src/web/terminal/dist/terminal.css"
# Ensure the lock is newer even on filesystems whose timestamps have low precision.
sleep 1
touch "$source/package-lock.json"
configure
if [ "$(cat "$source/src/web/terminal/dist/terminal.css")" = 'stale bundle marker' ]; then
    echo "changing the dependency lock left the embedded bundle stale" >&2
    exit 1
fi
echo "web bundle configure repairs missing outputs and changed dependencies"
