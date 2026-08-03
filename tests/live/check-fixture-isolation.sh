#!/bin/sh
# Verify the environment a real sshd session gives to codeharbord.
#
# This is a CTest fixture setup test, not a config-file grep. OpenSSH's SetEnv
# directive is only useful if it reaches the session that starts the daemon.
#
# Usage: check-fixture-isolation.sh
set -eu

# The portable suite does not need an SSH server. Passing without output keeps
# this setup test harmless when the live gates are not armed.
if [ -z "${CH_LIVE_SSH:-}" ]; then
    exit 0
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
generator="$script_dir/generate-fixture.sh"

fail() {
    echo "LIVE FIXTURE ISOLATION FAILED: $*" >&2
    echo "Run $generator, then restart the fixture sshd before retrying." >&2
    echo "Proceeding without that repair can make codeharbord migrate and corrupt" >&2
    echo "the developer's real ~/.local/share/codeharbor/codeharbor.sqlite." >&2
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
set -- "$@" "$user@$host" 'printf '\''DB=%s\nHOME=%s\n'\'' "${CODEHARBOR_DB-}" "${HOME-}"'

if ! reported=$(ssh "$@" 2>&1); then
    fail "could not ask the fixture what CODEHARBOR_DB is set to: $reported"
fi

db=$(printf '%s\n' "$reported" | sed -n 's/^DB=//p' | sed -n '1p')
home=$(printf '%s\n' "$reported" | sed -n 's/^HOME=//p' | sed -n '1p')
if [ -z "$db" ]; then
    fail "the fixture session returned an empty CODEHARBOR_DB"
fi

# Reject the exact default and the equivalent path under any remote HOME. The
# latter catches a remote shell that reports a different HOME than this client.
default_db="$home/.local/share/codeharbor/codeharbor.sqlite"
case "$db" in
    "$default_db"|*/.local/share/codeharbor/codeharbor.sqlite)
        fail "the fixture session points CODEHARBOR_DB at the real workspace database ($db)"
        ;;
esac

exit 0
