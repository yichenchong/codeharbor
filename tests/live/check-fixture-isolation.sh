#!/usr/bin/env bash
# Verify the environment a real sshd session gives to codeharbord.
#
# This is a CTest fixture setup test, not a config-file grep. OpenSSH's SetEnv
# directive is only useful if it reaches the session that starts the daemon.
#
# Usage: check-fixture-isolation.sh
set -euo pipefail

# The portable suite does not need an SSH server. Passing without output keeps
# this setup test harmless when the live gates are not armed.
if [ -z "${CH_LIVE_SSH:-}" ]; then
    exit 0
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
generator="$script_dir/generate-fixture.sh"

fail() {
    echo "LIVE FIXTURE ISOLATION FAILED: $*" >&2
    echo "Stop only this fixture's sshd/daemon/bridge/private tmux, run $generator," >&2
    echo "then restart its sshd before retrying." >&2
    echo "Unsafe environments can migrate the developer's real workspace database" >&2
    echo "or collide with their ordinary bridge socket. Never remove a shared socket." >&2
    echo "Recovery snapshots and Bash history must also remain isolated." >&2
    exit 1
}

host=${CH_LIVE_HOST:-}
port=${CH_LIVE_PORT:-}
user=${CH_LIVE_USER:-}
identity=${CH_LIVE_IDENTITY:-}

if [ -z "$host" ] || [ -z "$port" ] || [ -z "$user" ]; then
    fail "CH_LIVE_HOST, CH_LIVE_PORT, and CH_LIVE_USER are required when CH_LIVE_SSH is set"
fi
if ! command -v ssh >/dev/null 2>&1; then
    fail "the ssh client is not available"
fi

# Match the live client's unknown-host handling: this is a throwaway fixture,
# and the gate must ask the server rather than trust a local known_hosts file.
set -- -o BatchMode=yes \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o LogLevel=ERROR \
    -o ConnectTimeout=10 \
    -p "$port"
if [ -n "$identity" ]; then
    set -- "$@" -i "$identity"
fi
remote_probe=$(cat <<'SH'
db=${CODEHARBOR_DB-}
home=${HOME-}
default_db="$home/.local/share/codeharbor/codeharbor.sqlite"
# Resolve on the server, not this client's filesystem. The fixture directory
# must exist, but codeharbord is allowed to create the database on first use.
case "$db" in
    /*)
        if [ -L "$db" ]; then
            echo "refusing a symlink CODEHARBOR_DB ($db)" >&2
            exit 1
        fi
        if [ -e "$default_db" ] && [ "$db" -ef "$default_db" ]; then
            echo "CODEHARBOR_DB aliases the real workspace database ($db)" >&2
            exit 1
        fi
        parent=$(CDPATH= cd -- "$(dirname -- "$db")" && pwd -P) || exit 1
        db="$parent/$(basename -- "$db")"
        ;;
esac
if parent=$(CDPATH= cd -- "$(dirname -- "$default_db")" 2>/dev/null && pwd -P); then
    default_db="$parent/$(basename -- "$default_db")"
fi
printf 'DB=%s\nHOME=%s\nDEFAULT_DB=%s\n' "$db" "$home" "$default_db"
runtime=${XDG_RUNTIME_DIR-}
case "$runtime" in
    /*) ;;
    *) echo "XDG_RUNTIME_DIR must name the fixture's absolute private runtime" >&2; exit 1 ;;
esac
if [ -L "$runtime" ] || [ ! -d "$runtime" ]; then
    echo "refusing redirected or missing XDG_RUNTIME_DIR ($runtime)" >&2
    exit 1
fi
private=$(find "$runtime" -prune -type d -user "$(id -u)" -perm 0700 -print) || exit 1
if [ -z "$private" ]; then
    echo "XDG_RUNTIME_DIR must be owned by this account with mode 0700 ($runtime)" >&2
    exit 1
fi
runtime=$(CDPATH= cd -- "$runtime" && pwd -P) || exit 1
if [ "$runtime" != "$(dirname -- "$db")/runtime" ]; then
    echo "XDG_RUNTIME_DIR is not the database fixture's reserved runtime ($runtime)" >&2
    exit 1
fi
printf 'RUNTIME=%s\n' "$runtime"
data=${XDG_DATA_HOME-}
case "$data" in
    /*) ;;
    *) echo "XDG_DATA_HOME must name the fixture's absolute private data directory" >&2; exit 1 ;;
esac
if [ -L "$data" ] || [ ! -d "$data" ]; then
    echo "refusing redirected or missing XDG_DATA_HOME ($data)" >&2
    exit 1
fi
private=$(find "$data" -prune -type d -user "$(id -u)" -perm 0700 -print) || exit 1
if [ -z "$private" ]; then
    echo "XDG_DATA_HOME must be owned by this account with mode 0700 ($data)" >&2
    exit 1
fi
data=$(CDPATH= cd -- "$data" && pwd -P) || exit 1
if [ "$data" != "$(dirname -- "$db")/data" ]; then
    echo "XDG_DATA_HOME is not the database fixture's reserved data directory ($data)" >&2
    exit 1
fi
if [ "${HISTFILE-}" != /dev/null ]; then
    echo "HISTFILE must be /dev/null in the live fixture session" >&2
    exit 1
fi
printf 'DATA=%s\n' "$data"
tmux=${TMUX_TMPDIR-}
case "$tmux" in
    /*) ;;
    *) echo "TMUX_TMPDIR must name the fixture's absolute private tmux directory" >&2; exit 1 ;;
esac
if [ -L "$tmux" ] || [ ! -d "$tmux" ]; then
    echo "refusing redirected or missing TMUX_TMPDIR ($tmux)" >&2
    exit 1
fi
private=$(find "$tmux" -prune -type d -user "$(id -u)" -perm 0700 -print) || exit 1
if [ -z "$private" ]; then
    echo "TMUX_TMPDIR must be owned by this account with mode 0700 ($tmux)" >&2
    exit 1
fi
tmux=$(CDPATH= cd -- "$tmux" && pwd -P) || exit 1
if [ "$tmux" != "$(dirname -- "$db")/tmux" ]; then
    echo "TMUX_TMPDIR is not the database fixture's reserved tmux directory ($tmux)" >&2
    exit 1
fi
printf 'TMUX=%s\n' "$tmux"
SH
)
set -- "$@" "$user@$host" "$remote_probe"

if ! reported=$(ssh "$@" 2>&1); then
    fail "could not verify the fixture database/runtime/recovery/history/tmux environment: $reported"
fi

# A here-string, not `printf ... | awk`. `set -o pipefail` is on and the awk
# programs stop at the first match, so on a long enough reply printf would be
# killed by SIGPIPE, the assignment would inherit 141, and `set -e` would abort
# this gate with no message at all - a live-fixture check that silently stops
# checking is worse than one that fails.
db=$(awk 'index($0, "DB=") == 1 { print substr($0, 4); exit }' <<<"$reported")
default_db=$(awk 'index($0, "DEFAULT_DB=") == 1 { print substr($0, 12); exit }' <<<"$reported")
if [ -z "$db" ]; then
    fail "the fixture session returned an empty CODEHARBOR_DB"
fi
case "$db" in
    /*) ;;
    *)
        fail "the fixture session returned a relative CODEHARBOR_DB path ($db); a relative path can resolve to the real workspace database"
        ;;
esac

# The remote probe canonicalizes both parents, catching ../ and directory
# symlink aliases even before either database exists. The suffix check also
# rejects another account's conventional workspace path.
if [ -z "$default_db" ]; then
    fail "the fixture session did not report its default workspace database"
fi
case "$db" in
    "$default_db"|*/.local/share/codeharbor/codeharbor.sqlite)
        fail "the fixture session points CODEHARBOR_DB at the real workspace database ($db)"
        ;;
esac

exit 0
