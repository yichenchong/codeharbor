#!/usr/bin/env bash
# Run the real generator only inside a private temporary directory.
set -euo pipefail
root=$(CDPATH= cd -- "${1:-.}" && pwd)
command -v ssh-keygen >/dev/null || { echo "ssh-keygen not found; skipping" >&2; exit 77; }
generator="$root/tests/live/generate-fixture.sh"
# These small key/socket fixtures need short Unix socket names, independent of
# a potentially long build TMPDIR. No bulk build data is written under /tmp.
work=$(mktemp -d /tmp/chfix.XXXXXX)
work=$(CDPATH= cd -- "$work" && pwd -P)
trap 'rm -rf "$work"' EXIT
fixture="$work/fixture"
bash "$generator" "$fixture" >"$work/out" 2>&1
host_before=$(cksum <"$fixture/hostkey")
id_before=$(cksum <"$fixture/id")
for name in runtime data tmux; do
    [ -d "$fixture/$name" ] && [ -O "$fixture/$name" ]
    [ "$(find "$fixture/$name" -prune -perm 0700 | wc -l)" -eq 1 ]
    printf 'retained fixture %s state\n' "$name" >"$fixture/$name/sentinel"
    chmod 750 "$fixture/$name"
done
for name in workspace.sqlite workspace.sqlite-wal workspace.sqlite-shm workspace.sqlite-journal; do
    printf 'stale fixture state\n' >"$fixture/$name"
done
printf 'stale public key\n' >"$fixture/id.pub"
bash "$generator" "$fixture" >"$work/out" 2>&1
[ "$(cksum <"$fixture/hostkey")" = "$host_before" ]
[ "$(cksum <"$fixture/id")" = "$id_before" ]
ssh-keygen -y -f "$fixture/id" >"$work/expected.pub"
cmp "$work/expected.pub" "$fixture/id.pub"
cmp "$fixture/id.pub" "$fixture/authorized_keys"
for name in workspace.sqlite workspace.sqlite-wal workspace.sqlite-shm workspace.sqlite-journal; do
    [ ! -e "$fixture/$name" ]
done
for name in runtime data tmux; do
    [ "$(find "$fixture/$name" -prune -perm 0700 | wc -l)" -eq 1 ]
    [ "$(cat "$fixture/$name/sentinel")" = "retained fixture $name state" ]
done

# Exercise every input/output redirection, including dangling symlinks and
# symlink-to-directory mv destinations. Neither outside files nor the fixture's
# other files may change when validation fails.
mkdir "$work/outside"
cp "$fixture/id" "$work/outside/sentinel"
chmod 400 "$work/outside/sentinel"
outside_before=$(cksum <"$work/outside/sentinel")
for name in hostkey id hostkey.pub id.pub authorized_keys sshd_config; do
    mv "$fixture/$name" "$work/saved"
    for target in "$work/outside/sentinel" "$work/outside" "$work/missing"; do
        ln -s "$target" "$fixture/$name"
        if bash "$generator" "$fixture" >"$work/out" 2>&1; then
            cat "$work/out" >&2
            echo "generator accepted redirected $name ($target)" >&2
            exit 1
        fi
        [ -L "$fixture/$name" ]
        [ "$(cksum <"$work/outside/sentinel")" = "$outside_before" ]
        [ "$(find "$work/outside" -type f | wc -l)" -eq 1 ]
        [ ! -e "$work/missing" ]
        [ "$(find "$work/outside/sentinel" -perm 0400 | wc -l)" -eq 1 ]
        rm "$fixture/$name"
    done
    mkdir "$fixture/$name"
    if bash "$generator" "$fixture" >"$work/out" 2>&1; then
        echo "generator accepted directory at $name" >&2
        exit 1
    fi
    rmdir "$fixture/$name"
    mv "$work/saved" "$fixture/$name"
done

# Private-directory redirection must be refused before chmod or refresh writes.
for name in runtime data tmux; do
    mv "$fixture/$name" "$work/directory-saved"
    chmod 755 "$work/outside"
    for target in "$work/outside" "$work/outside/sentinel" "$work/missing-directory"; do
        ln -s "$target" "$fixture/$name"
        if bash "$generator" "$fixture" >"$work/out" 2>&1; then
            echo "generator accepted redirected $name ($target)" >&2
            exit 1
        fi
        [ -L "$fixture/$name" ]
        [ "$(find "$work/outside" -prune -perm 0755 | wc -l)" -eq 1 ]
        [ "$(cksum <"$work/outside/sentinel")" = "$outside_before" ]
        [ ! -e "$work/missing-directory" ]
        rm "$fixture/$name"
    done
    printf 'not a private directory\n' >"$fixture/$name"
    if bash "$generator" "$fixture" >"$work/out" 2>&1; then
        echo "generator accepted a file as $name directory" >&2
        exit 1
    fi
    rm "$fixture/$name"
    mv "$work/directory-saved" "$fixture/$name"
done
echo "live fixture generation: refresh preserves keys; redirected files rejected"
