#!/usr/bin/env bash
# Generate the throwaway SSH fixture used by the live gates.
#
# The output directory is git-ignored because it contains private keys and a
# workspace database. Nothing in it is committed, so a live run can never dirty
# the checkout; but everything in it can go stale, and a refresh must not leave
# stale state behind. The sshd config is rewritten on every run (a hand-edited
# or stale one must never survive without its CODEHARBOR_DB override) and the
# workspace database is deleted on every run, for the reason given where that
# happens below.
#
# Usage: generate-fixture.sh [fixture-directory]
set -euo pipefail

if [ "$#" -gt 1 ]; then
    echo "usage: generate-fixture.sh [fixture-directory]" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fixture=${1:-"$script_dir/.fixture"}
if [ -L "$fixture" ]; then
    echo "generate-fixture.sh: refusing symlink fixture directory $fixture" >&2
    exit 1
fi
# Resolve the existing ancestor without creating anything: an overlong socket
# namespace must not reset an existing database or even create a new fixture.
parent="$fixture"
suffix=""
while [ ! -d "$parent" ]; do
    if [ -e "$parent" ] || [ -L "$parent" ]; then
        echo "generate-fixture.sh: fixture ancestor is not a directory: $parent" >&2
        exit 1
    fi
    suffix="/$(basename -- "$parent")$suffix"
    parent=$(dirname -- "$parent")
done
parent=$(CDPATH= cd -- "$parent" && pwd -P)
fixture="${parent%/}$suffix"
fixture=${fixture:-/}
runtime="${fixture%/}/runtime"
# control.ts::mintControlSocketPath uses 8 random bytes as 16 hex characters.
# Account for its longer name, not just the bridge's codeharbor.sock. Darwin
# permits 103 pathname bytes (Linux 107); use the portable ceiling, excluding NUL.
control_path="$runtime/codeharbor-control-0000000000000000.sock"
socket_bytes=$(LC_ALL=C printf '%s' "$control_path" | wc -c)
if [ "$socket_bytes" -gt 103 ]; then
    echo "generate-fixture.sh: fixture control socket needs $socket_bytes bytes; portable Unix limit is 103" >&2
    echo "Choose a shorter private fixture directory; no fixture files were changed." >&2
    exit 1
fi

# Refuse redirected inputs and outputs before changing permissions or creating
# keys. In particular, `-f` alone follows a private-key symlink, and mv treats a
# directory (including a symlink to one) as a destination to write inside.
for name in hostkey id hostkey.pub id.pub authorized_keys sshd_config; do
    path="$fixture/$name"
    if [ -L "$path" ] || { [ -e "$path" ] && [ ! -f "$path" ]; }; then
        echo "generate-fixture.sh: refusing non-regular fixture file $path" >&2
        exit 1
    fi
done

# Bridge sockets, recovery snapshots and tmux masters have independent namespaces.
# Keep them private; leave HOME and the account's shell configuration unchanged.
data="$fixture/data"
tmux="$fixture/tmux"
for directory in "$runtime" "$data" "$tmux"; do
    if [ -L "$directory" ] ||
       { [ -e "$directory" ] && { [ ! -d "$directory" ] || [ ! -O "$directory" ]; }; }; then
        echo "generate-fixture.sh: refusing redirected, non-directory, or unowned private directory $directory" >&2
        exit 1
    fi
done

if ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "generate-fixture.sh: ssh-keygen is required to create the live fixture" >&2
    exit 1
fi

umask 077
mkdir -p "$fixture"
chmod 700 "$fixture"
mkdir -p "$runtime" "$data" "$tmux"
chmod 700 "$runtime" "$data" "$tmux"

tmp=""
cleanup() {
    if [ -n "$tmp" ]; then
        rm -f "$tmp"
    fi
}
trap cleanup EXIT

# A present private key is retained so rerunning the generator does not strand
# an ssh-agent that already holds the fixture key. Its public half is derived
# afresh below, which also repairs a stale or missing .pub file.
for key_name in hostkey id; do
    private="$fixture/$key_name"
    public="$fixture/$key_name.pub"
    if [ ! -e "$private" ]; then
        ssh-keygen -q -t ed25519 -f "$private" -N ''
    fi
    tmp=$(mktemp "$fixture/$key_name.pub.tmp.XXXXXX")
    if ! ssh-keygen -y -f "$private" >"$tmp"; then
        echo "generate-fixture.sh: could not read private key $private" >&2
        exit 1
    fi
    chmod 644 "$tmp"
    mv -f "$tmp" "$public"
    tmp=""
    chmod 600 "$private"
done

# authorized_keys is regenerated rather than appended: it contains exactly the
# key created above, with the mode sshd expects when StrictModes is enabled.
tmp=$(mktemp "$fixture/authorized_keys.tmp.XXXXXX")
cat "$fixture/id.pub" >"$tmp"
chmod 600 "$tmp"
mv -f "$tmp" "$fixture/authorized_keys"
tmp=""

# The port the generated sshd_config listens on. Named once so the config below
# and the environment printed at the end cannot drift: check-fixture-isolation.sh
# connects to CH_LIVE_PORT, and a config that says 2222 next to instructions
# that say something else sends the live gates at the developer's real sshd.
port=2222
user=$(id -un)
tmp=$(mktemp "$fixture/sshd_config.tmp.XXXXXX")
cat >"$tmp" <<EOF
Port $port
ListenAddress 127.0.0.1
HostKey "$fixture/hostkey"
AuthorizedKeysFile "$fixture/authorized_keys"
PidFile "$fixture/sshd.pid"
UsePAM no
StrictModes no
PasswordAuthentication no
AllowUsers $user
Subsystem sftp internal-sftp
SetEnv "CODEHARBOR_DB=$fixture/workspace.sqlite" "XDG_RUNTIME_DIR=$runtime" "XDG_DATA_HOME=$data" "HISTFILE=/dev/null" "TMUX_TMPDIR=$tmux"
EOF
chmod 600 "$tmp"
mv -f "$tmp" "$fixture/sshd_config"
tmp=""

# The workspace database, deleted rather than reused.
#
# codeharbord opens $fixture/workspace.sqlite IN PLACE and migrates whatever
# schema it finds. A database left by an older build therefore silently decides
# what the next live run tests against: as of today the server drops the
# obsolete index `idx_dev_sessions_group_id` on first open
# (remote/sql/indexes.sql), so an old fixture is quietly migrated the moment a
# live test connects, and the run is no longer exercising a fresh install.
#
# The private keys above are deliberately RETAINED on a refresh, because an
# ssh-agent may already hold them. The database is the opposite case: nothing
# outside this directory refers to it, the live tests are written not to depend
# on state from a previous run, and a stale one is actively misleading. The
# write-ahead log and shared-memory sidecars go with it - leaving a 4 MB -wal
# beside a deleted database is how a "fresh" fixture comes back to life.
rm -f "$fixture/workspace.sqlite" \
      "$fixture/workspace.sqlite-wal" \
      "$fixture/workspace.sqlite-shm" \
      "$fixture/workspace.sqlite-journal"

printf 'live SSH fixture generated in %s\n' "$fixture"
printf 'the workspace database was reset; codeharbord will create a fresh one\n'
printf 'stop this fixture sshd, daemon/bridge and private tmux master before any future refresh\n'
printf '\nStart it with:\n'
printf '  /usr/sbin/sshd -f %s/sshd_config -D\n' "$fixture"
printf '\nThen arm the live gates with:\n'
printf '  export CH_LIVE_SSH=1\n'
printf '  export CH_LIVE_HOST=127.0.0.1\n'
printf '  export CH_LIVE_PORT=%s\n' "$port"
printf '  export CH_LIVE_USER=%s\n' "$user"
printf '  export CH_LIVE_IDENTITY=%s/id\n' "$fixture"
printf '\nThe session will see CODEHARBOR_DB=%s/workspace.sqlite,\n' "$fixture"
printf 'never the developer'"'"'s real ~/.local/share/codeharbor/codeharbor.sqlite.\n'
printf 'Its private bridge/hook runtime is %s (XDG_RUNTIME_DIR).\n' "$runtime"
printf 'Runtime contents are retained; no shared user socket is removed.\n'
printf 'Recovery snapshots stay under %s (XDG_DATA_HOME); contents are retained.\n' "$data"
printf 'HISTFILE=/dev/null suppresses ordinary Bash history writes; this is not a shell sandbox.\n'
printf 'The tmux master namespace is %s (TMUX_TMPDIR); contents are retained.\n' "$tmux"
