#!/bin/sh
# Generate the throwaway SSH fixture used by the live gates.
#
# The output directory is git-ignored because it contains private keys and a
# workspace database. The config is deliberately rewritten on every run: a
# hand-edited or stale sshd_config must never survive a fixture refresh without
# its CODEHARBOR_DB override.
#
# Usage: generate-fixture.sh [fixture-directory]
set -eu

if [ "$#" -gt 1 ]; then
    echo "usage: generate-fixture.sh [fixture-directory]" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fixture=${1:-"$script_dir/.fixture"}
fixture=$(CDPATH= cd -- "$fixture" 2>/dev/null && pwd || {
    mkdir -p "$fixture"
    CDPATH= cd -- "$fixture" && pwd
})

if ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "generate-fixture.sh: ssh-keygen is required to create the live fixture" >&2
    exit 1
fi

umask 077
chmod 700 "$fixture"

# A present private key is retained so rerunning the generator does not strand
# an ssh-agent that already holds the fixture key. Its public half is derived
# afresh below, which also repairs a stale or missing .pub file.
for key_name in hostkey id; do
    private="$fixture/$key_name"
    public="$fixture/$key_name.pub"
    if [ -e "$private" ] && [ ! -f "$private" ]; then
        echo "generate-fixture.sh: $private exists but is not a regular file" >&2
        exit 1
    fi
    if [ ! -e "$private" ]; then
        ssh-keygen -q -t ed25519 -f "$private" -N ''
    fi
    tmp=$(mktemp "$fixture/$key_name.pub.tmp.XXXXXX")
    if ! ssh-keygen -y -f "$private" >"$tmp"; then
        rm -f "$tmp"
        echo "generate-fixture.sh: could not read private key $private" >&2
        exit 1
    fi
    chmod 644 "$tmp"
    mv -f "$tmp" "$public"
    chmod 600 "$private"
done

# authorized_keys is regenerated rather than appended: it contains exactly the
# key created above, with the mode sshd expects when StrictModes is enabled.
tmp=$(mktemp "$fixture/authorized_keys.tmp.XXXXXX")
cat "$fixture/id.pub" >"$tmp"
chmod 600 "$tmp"
mv -f "$tmp" "$fixture/authorized_keys"

user=$(id -un)
tmp=$(mktemp "$fixture/sshd_config.tmp.XXXXXX")
cat >"$tmp" <<EOF
Port 2222
ListenAddress 127.0.0.1
HostKey $fixture/hostkey
AuthorizedKeysFile $fixture/authorized_keys
PidFile $fixture/sshd.pid
UsePAM no
StrictModes no
PasswordAuthentication no
AllowUsers $user
Subsystem sftp internal-sftp
SetEnv CODEHARBOR_DB=$fixture/workspace.sqlite
EOF
chmod 600 "$tmp"
mv -f "$tmp" "$fixture/sshd_config"

printf 'live SSH fixture generated in %s\n' "$fixture"
printf '  CODEHARBOR_DB=%s/workspace.sqlite\n' "$fixture"
