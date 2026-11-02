#!/usr/bin/env bash
# Exercise the live preflight with its remote shell probe executed locally.
# No sshd, daemon, or developer database is used or modified.
set -euo pipefail
root=$(CDPATH= cd -- "${1:-.}" && pwd)
probe="$root/tests/live/check-fixture-isolation.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/home/.local/share/codeharbor" "$work/fixture"
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

# The portable suite must still pass without any live connection configured.
CH_LIVE_SSH="" bash "$probe"
echo "live fixture isolation: safe paths accepted; default database aliases rejected"
