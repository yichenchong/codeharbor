#!/usr/bin/env bash
# Fake Oh My Pi agent for the live agent-awareness gate (src/agent/tests/
# tst_liveagent.cpp).
#
# Oh My Pi is not installed on the fixture host, so this script stands in for
# the harness process: it fires ONE native lifecycle hook exactly the way the
# harness would (SPEC 6.2), by invoking the REAL hook script. Nothing about the
# hook, the bridge, or the wire format is simulated here — this file only
# supplies the argv/env a harness would supply.
#
#   usage: fake-omp-agent.sh <native-event> [tool]
#
#   env (required):
#     CH_HOOK_NODE        node binary (>= 23.6; runs the TypeScript directly)
#     CH_HOOK_REPO        CodeHarbor checkout root holding remote/src
#     OMP_DEV_SESSION_ID  Dev Session the event belongs to
#     OMP_TERMINAL_ID     terminal pane the agent runs in
#   env (optional):
#     XDG_RUNTIME_DIR     selects the bridge socket, <dir>/codeharbor.sock
#                         (SPEC 6.3; defaults to ~/.cache/codeharbor)
#     OMP_SUMMARY         human-readable summary carried to the notification
#     OMP_ERROR=1         mark the firing as an agent/hook error
#
# A tool name is a positional argument rather than an env var so the caller
# cannot leave a stale OMP_TOOL set across firings: `tool_call` with tool `ask`
# is waiting_input, `tool_call` without a tool is running (SPEC 6.5).
set -euo pipefail

event=${1:?usage: fake-omp-agent.sh <native-event> [tool]}
tool=${2:-}
node=${CH_HOOK_NODE:?CH_HOOK_NODE must point at the remote node binary}
repo=${CH_HOOK_REPO:?CH_HOOK_REPO must point at the CodeHarbor checkout root}

if [ -n "$tool" ]; then
    OMP_TOOL=$tool
    export OMP_TOOL
else
    unset OMP_TOOL || true
fi

exec "$node" "$repo/remote/src/hooks/oh-my-pi-hook.ts" "$event"
