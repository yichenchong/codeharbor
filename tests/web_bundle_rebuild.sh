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
mkdir "$source/remote"
cp "$root/remote/package.json" "$source/remote/"
# CMake may run npm ci after a dependency change. Never point its installation
# directory into the checkout's real node_modules.
cp -R "$root/node_modules" "$source/node_modules"
configure() {
    cmake -S "$source" -B "$work/build" -G Ninja \
        -DQt6_DIR="$qt_dir" -DCODEHARBOR_BUILD_TESTS=OFF \
        -DCODEHARBOR_BUILD_MOBILE=OFF -DCODEHARBOR_SKIP_WEB_BUNDLE=OFF \
        -DCODEHARBOR_WEB_SOURCEMAP=OFF -DCMAKE_BUILD_TYPE=Release \
        >"$work/configure.log" 2>&1
}
configure || { cat "$work/configure.log" >&2; exit 1; }
# Losing a non-index output must be repaired just like a missing index.html.
# Without that repair qt_add_resources fails during the next configure.
for file in terminal/terminal.js editor/editor.worker.js markdown/markdown.css; do
    rm "$source/src/web/${file%/*}/dist/${file#*/}"
done
configure || { cat "$work/configure.log" >&2; exit 1; }
for file in terminal/terminal.js editor/editor.worker.js markdown/markdown.css; do
    [ -s "$source/src/web/${file%/*}/dist/${file#*/}" ]
done

# Dependencies changed without touching workspace source files. Keep an existing
# CSS output as a marker: a stale bundle leaves it untouched; a rebuild repairs it.
printf 'stale bundle marker\n' >"$source/src/web/terminal/dist/terminal.css"
# Remove a real installed dependency as a deterministic stand-in for the
# installed tree no longer matching the changed lock. The next configure must
# refresh dependencies, not merely rerun esbuild against the old installation.
rm -rf "$source/node_modules/@xterm/xterm"
# Ensure the lock is newer even on filesystems whose timestamps have low precision.
sleep 1
touch "$source/package-lock.json"
configure || { cat "$work/configure.log" >&2; exit 1; }
if [ "$(cat "$source/src/web/terminal/dist/terminal.css")" = 'stale bundle marker' ]; then
    echo "changing the dependency lock left the embedded bundle stale" >&2
    exit 1
fi

# esbuild reads each workspace's tsconfig even though it does not type-check.
# An invalid compiler configuration must not be hidden by an existing dist/.
sleep 1
cp "$source/src/web/terminal/tsconfig.json" "$work/tsconfig.saved"
printf 'not valid JSON\n' >"$source/src/web/terminal/tsconfig.json"
if configure; then
    echo "changing tsconfig left the existing bundle silently accepted" >&2
    exit 1
fi
mv "$work/tsconfig.saved" "$source/src/web/terminal/tsconfig.json"

# Failed production must preserve the complete good bundle, not publish a
# missing stylesheet or empty page and only then discover the broken output.
for bundle in terminal markdown; do
    bundle_root="$source/src/web/$bundle"
    before=$(cksum "$bundle_root"/dist/*)
    cp "$bundle_root/src/index.ts" "$work/index.saved"
    cp "$bundle_root/index.html" "$work/page.saved"
    for invalid in empty-page missing-css; do
        if [ "$invalid" = empty-page ]; then
            printf '' >"$bundle_root/index.html"
        else
            cp "$work/page.saved" "$bundle_root/index.html"
            printf 'console.log("bundle without stylesheet");\n' >"$bundle_root/src/index.ts"
        fi
        if (cd "$source" && npm run build --workspace "src/web/$bundle") >"$work/bundle.log" 2>&1; then
            echo "$bundle accepted $invalid output" >&2
            exit 1
        fi
        [ "$(cksum "$bundle_root"/dist/*)" = "$before" ] || {
            echo "$bundle discarded the good bundle after $invalid" >&2
            exit 1
        }
        [ ! -e "$bundle_root/dist.tmp" ]
        [ ! -e "$bundle_root/dist.old" ]
    done
    mv "$work/index.saved" "$bundle_root/src/index.ts"
    mv "$work/page.saved" "$bundle_root/index.html"
done
echo "web bundle rebuild tracks compiler config and preserves complete outputs"
