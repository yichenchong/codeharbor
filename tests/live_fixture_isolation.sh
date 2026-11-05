#!/usr/bin/env bash
# Exercise the live preflight with its remote shell probe executed locally.
# No sshd, daemon, or developer database is used or modified.
set -euo pipefail
root=$(CDPATH= cd -- "${1:-.}" && pwd)
probe="$root/tests/live/check-fixture-isolation.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/home/.local/share/codeharbor" "$work/fixture"
mkdir "$work/fixture/runtime" "$work/fixture/data" "$work/fixture/tmux"
chmod 700 "$work/fixture/runtime" "$work/fixture/data" "$work/fixture/tmux"
export XDG_RUNTIME_DIR="$work/fixture/runtime" XDG_DATA_HOME="$work/fixture/data"
export HISTFILE=/dev/null
export TMUX_TMPDIR="$work/fixture/tmux"
cat >"$work/bin/ssh" <<'SH'
#!/usr/bin/env bash
# SSH's final argument is the remote command; run exactly that command.
exec /bin/sh -c "${!#}"
SH
chmod +x "$work/bin/ssh"
export PATH="$work/bin:$PATH"
export HOME="$work/home" CH_LIVE_SSH=1 CH_LIVE_HOST=127.0.0.1
export CH_LIVE_PORT=2222 CH_LIVE_USER=fixture CH_LIVE_IDENTITY=""
default="$HOME/.local/share/codeharbor/codeharbor.sqlite"
printf 'untouched developer data\n' >"$default"

check() {
    local expected=$1 candidate=$2 status=0
    CODEHARBOR_DB="$candidate" bash "$probe" >"$work/out" 2>&1 || status=$?
    if { [ "$expected" = accept ] && [ "$status" -ne 0 ]; } ||
       { [ "$expected" = reject ] && [ "$status" -eq 0 ]; }; then
        cat "$work/out" >&2
        echo "expected $expected for '$candidate', got exit $status" >&2
        exit 1
    fi
}

check accept "$work/fixture/workspace.sqlite"
printf 'isolated data\n' >"$work/fixture/workspace.sqlite"
check accept "$work/fixture/workspace.sqlite"
check reject ""
check reject "workspace.sqlite"
check reject "$default"
check reject "$HOME/.local/share/codeharbor/../codeharbor/codeharbor.sqlite"
ln -s "$default" "$work/fixture/symlink.sqlite"
check reject "$work/fixture/symlink.sqlite"
ln "$default" "$work/fixture/hardlink.sqlite"
check reject "$work/fixture/hardlink.sqlite"
ln -s "$HOME/.local/share/codeharbor" "$work/alias"
check reject "$work/alias/codeharbor.sqlite"

# Canonicalize the default parent too: its conventional directory can itself
# be a symlink to a location whose name has no recognizable default suffix.
mv "$HOME/.local/share/codeharbor" "$work/real-data"
ln -s "$work/real-data" "$HOME/.local/share/codeharbor"
check reject "$work/real-data/codeharbor.sqlite"
rm "$work/real-data/codeharbor.sqlite"
check reject "$work/real-data/codeharbor.sqlite"
check reject "$HOME/.local/share/codeharbor/../codeharbor/codeharbor.sqlite"
check accept "$work/fixture/new.sqlite"

# A private database alone is insufficient: the bridge must not bind a normal
# user's shared socket or reuse another fixture's runtime directory.
safe_db="$work/fixture/workspace.sqlite"
XDG_RUNTIME_DIR="" check reject "$safe_db"
XDG_RUNTIME_DIR="runtime" check reject "$safe_db"
XDG_RUNTIME_DIR="$work/missing-runtime" check reject "$safe_db"
mkdir -p "$HOME/.cache/codeharbor" "$work/other/runtime"
chmod 700 "$HOME/.cache/codeharbor" "$work/other/runtime"
XDG_RUNTIME_DIR="$HOME/.cache/codeharbor" check reject "$safe_db"
XDG_RUNTIME_DIR="$work/other/runtime" check reject "$safe_db"
ln -s "$work/fixture/runtime" "$work/runtime-alias"
XDG_RUNTIME_DIR="$work/runtime-alias" check reject "$safe_db"
printf 'not a directory\n' >"$work/runtime-file"
XDG_RUNTIME_DIR="$work/runtime-file" check reject "$safe_db"
chmod 755 "$work/fixture/runtime"
check reject "$safe_db"
chmod 700 "$work/fixture/runtime"
check accept "$safe_db"

# Recovery snapshots use XDG_DATA_HOME, not CODEHARBOR_DB or the socket runtime.
XDG_DATA_HOME="" check reject "$safe_db"
XDG_DATA_HOME="data" check reject "$safe_db"
XDG_DATA_HOME="$work/missing-data" check reject "$safe_db"
XDG_DATA_HOME="$HOME/.local/share" check reject "$safe_db"
mkdir "$work/other/data"
chmod 700 "$work/other/data"
XDG_DATA_HOME="$work/other/data" check reject "$safe_db"
ln -s "$work/fixture/data" "$work/data-alias"
XDG_DATA_HOME="$work/data-alias" check reject "$safe_db"
XDG_DATA_HOME="$work/runtime-file" check reject "$safe_db"
chmod 755 "$work/fixture/data"
check reject "$safe_db"
chmod 700 "$work/fixture/data"
HISTFILE="" check reject "$safe_db"
HISTFILE="$HOME/.bash_history" check reject "$safe_db"
check accept "$safe_db"

# Default tmux must not reuse a user master whose environment predates this fixture.
TMUX_TMPDIR="" check reject "$safe_db"
TMUX_TMPDIR="tmux" check reject "$safe_db"
TMUX_TMPDIR="$work/missing-tmux" check reject "$safe_db"
TMUX_TMPDIR="/tmp" check reject "$safe_db"
mkdir "$work/other/tmux"
chmod 700 "$work/other/tmux"
TMUX_TMPDIR="$work/other/tmux" check reject "$safe_db"
ln -s "$work/fixture/tmux" "$work/tmux-alias"
TMUX_TMPDIR="$work/tmux-alias" check reject "$safe_db"
TMUX_TMPDIR="$work/runtime-file" check reject "$safe_db"
chmod 755 "$work/fixture/tmux"
check reject "$safe_db"
chmod 700 "$work/fixture/tmux"
check accept "$safe_db"

# The portable suite must still pass without any live connection configured.
CH_LIVE_SSH="" bash "$probe"
echo "live fixture isolation: safe paths accepted; default database aliases rejected"
