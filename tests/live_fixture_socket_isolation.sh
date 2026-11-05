#!/usr/bin/env bash
# Resolve actual generated sshd environments and bind the production socket paths.
# Every directory/socket belongs to this test; no sshd is launched or shared socket touched.
set -euo pipefail
root=$(CDPATH= cd -- "${1:-.}" && pwd)
for tool in node ssh-keygen tmux; do
    command -v "$tool" >/dev/null || { echo "$tool not found; skipping" >&2; exit 77; }
done
sshd=$(command -v sshd || true)
if [ -z "$sshd" ] && [ -x /usr/sbin/sshd ]; then sshd=/usr/sbin/sshd; fi
[ -n "$sshd" ] || { echo "sshd not found; skipping" >&2; exit 77; }
node -e 'const [major, minor] = process.versions.node.split(".").map(Number); if (major < 23 || (major === 23 && minor < 6)) process.exit(77);' \
    || { echo "Node >=23.6 required; skipping" >&2; exit 77; }
# Keep only these tiny runtime fixtures under a short private path; build TMPDIR
# may itself be longer than a portable Unix socket pathname permits.
work=$(mktemp -d /tmp/chfix.XXXXXX)
work=$(CDPATH= cd -- "$work" && pwd -P)
cleanup() {
    for directory in "$work/other-tmux" "$work/a/tmux" "$work/b/tmux"; do
        if [ -d "$directory" ] && [ ! -L "$directory" ] && [ -O "$directory" ]; then
            tmux -S "$directory/tmux-$(id -u)/default" kill-server >/dev/null 2>&1 || true
        fi
    done
    rm -rf "$work"
}
trap cleanup EXIT
runtimes=()
for name in a b; do
    fixture="$work/$name"
    bash "$root/tests/live/generate-fixture.sh" "$fixture" >"$work/generate-$name.log" 2>&1
    "$sshd" -T -f "$fixture/sshd_config" >"$work/effective-$name"
    # Parse sshd's effective configuration, not the generated source. This test
    # uses short mktemp paths without whitespace; assignments share one directive.
    runtime=$(awk '{ for (i = 2; i <= NF; ++i) if (index($i, "XDG_RUNTIME_DIR=") == 1) { print substr($i, 17); exit } }' "$work/effective-$name")
    [ "$runtime" = "$fixture/runtime" ] || { echo "sshd did not retain the private fixture runtime" >&2; exit 1; }
    runtimes+=("$runtime")
done
node --input-type=module - "$root" "$work/home" "${runtimes[@]}" <<'JS'
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { mkdir } from "node:fs/promises";
import { createServer } from "node:net";
import { dirname, join } from "node:path";
import { pathToFileURL } from "node:url";

const root = process.argv[2];
const { resolveSocketPath } = await import(pathToFileURL(join(root, "remote/src/events.ts")).href);
const { mintControlSocketPath } = await import(pathToFileURL(join(root, "remote/src/control.ts")).href);
process.env.HOME = process.argv[3];
delete process.env.XDG_RUNTIME_DIR;
delete process.env.CODEHARBOR_AGENT_SOCKET;
const ordinary = resolveSocketPath();
const paths = [ordinary];
for (const runtime of process.argv.slice(4)) {
  process.env.XDG_RUNTIME_DIR = runtime;
  const socket = resolveSocketPath();
  assert.equal(dirname(socket), runtime);
  assert.notEqual(socket, ordinary);
  paths.push(socket);
}
assert.equal(new Set(paths).size, 3);

// Count actual production control socket bytes, including its random-token
// format. Exactly 103 bytes is supported on Darwin as well as Linux.
const prefix = join(dirname(process.argv[3]), "budget-");
const controlFor = (fixture) => mintControlSocketPath({ XDG_RUNTIME_DIR: join(fixture, "runtime") });
const padding = 103 - Buffer.byteLength(controlFor(prefix));
assert.ok(padding >= 2, "test requires a short temporary fixture root");
const exact = prefix + "a".repeat(padding);
const unicodeExact = prefix + "é" + "a".repeat(padding - 2);
for (const fixture of [exact, unicodeExact]) {
  const path = controlFor(fixture);
  assert.equal(Buffer.byteLength(path), 103);
  const generated = spawnSync("bash", [join(root, "tests/live/generate-fixture.sh"), fixture], { encoding: "utf8" });
  assert.equal(generated.status, 0, generated.stderr);
  paths.push(path);
}
const tooLong = exact + "b";
assert.equal(Buffer.byteLength(controlFor(tooLong)), 104);
let generated = spawnSync("bash", [join(root, "tests/live/generate-fixture.sh"), tooLong], { encoding: "utf8" });
assert.notEqual(generated.status, 0);
assert.equal(existsSync(tooLong), false, "length refusal must precede directory creation");
mkdirSync(tooLong, { mode: 0o755 });
const beforeMode = statSync(tooLong).mode;
writeFileSync(join(tooLong, "workspace.sqlite"), "do not reset this fixture\n");
generated = spawnSync("bash", [join(root, "tests/live/generate-fixture.sh"), tooLong], { encoding: "utf8" });
assert.notEqual(generated.status, 0);
assert.equal(statSync(tooLong).mode, beforeMode);
assert.equal(readFileSync(join(tooLong, "workspace.sqlite"), "utf8"), "do not reset this fixture\n");
assert.deepEqual(readdirSync(tooLong), ["workspace.sqlite"]);

// Apply actual effective sshd environments to the real RPC handlers. HOME is
// synthetic only inside this test process: generated fixture configs do not
// replace the account's HOME or shell startup semantics.
const { dispatch } = await import(pathToFileURL(join(root, "remote/src/codeharbord.ts")).href);
const { closeDefaultWorkspace } = await import(pathToFileURL(join(root, "remote/src/workspace.ts")).href);
const tmuxEnvironment = (environment) => {
  const result = { ...environment };
  delete result.TMUX;
  delete result.TMUX_PANE;
  return result;
};
const tmux = (environment, args) => {
  const child = spawnSync("tmux", args, {
    env: tmuxEnvironment(environment), encoding: "utf8", timeout: 10000,
  });
  assert.equal(child.status, 0, `${args.join(" ")}: ${child.stderr || child.error || ""}`);
  return child.stdout.trim();
};
const otherRoot = join(dirname(process.argv[3]), "other-tmux");
mkdirSync(otherRoot, { mode: 0o700 });
const otherHistory = join(dirname(process.argv[3]), "other-history");
writeFileSync(otherHistory, "ordinary history must remain untouched\n");
const otherEnvironment = {
  ...process.env, TMUX_TMPDIR: otherRoot,
  XDG_RUNTIME_DIR: join(otherRoot, "other-runtime"),
  XDG_DATA_HOME: join(otherRoot, "other-data"),
  HISTFILE: otherHistory,
};
tmux(otherEnvironment, ["-f", "/dev/null", "new-session", "-d", "-s", "keeper", "/bin/sleep", "300"]);
const otherPid = tmux(otherEnvironment, ["display-message", "-p", "#{pid}"]);
try {
  for (const name of ["a", "b"]) {
    const fixture = join(dirname(process.argv[3]), name);
    const effective = readFileSync(join(dirname(process.argv[3]), `effective-${name}`), "utf8");
    const entries = effective.split("\n")
      .filter((line) => line.startsWith("setenv "))
      .flatMap((line) => line.slice(7).trim().split(/\s+/))
      .filter((entry) => entry.includes("="))
      .map((entry) => [entry.slice(0, entry.indexOf("=")), entry.slice(entry.indexOf("=") + 1)]);
    const environment = Object.fromEntries(entries);
    assert.equal(environment.CODEHARBOR_DB, join(fixture, "workspace.sqlite"));
    assert.equal(environment.XDG_RUNTIME_DIR, join(fixture, "runtime"));
    assert.equal(environment.XDG_DATA_HOME, join(fixture, "data"));
    assert.equal(environment.HISTFILE, "/dev/null");
    assert.equal(environment.TMUX_TMPDIR, join(fixture, "tmux"));
    assert.equal(Object.hasOwn(environment, "HOME"), false);
    Object.assign(process.env, environment);
    const fixtureEnvironment = { ...process.env, ...environment };
    try {
      const info = await dispatch({ jsonrpc: "2.0", id: 1, method: "server.info", params: {} });
      assert.ok(info && "result" in info, JSON.stringify(info));
      const recovery = join(fixture, "data", "codeharbor", "recovery");
      assert.equal(info.result.recoveryDir, recovery);
      const snapshot = join(recovery, "fixture-recovery.json");
      const content = JSON.stringify({ content: "unsaved fixture text" });
      const written = await dispatch({
        jsonrpc: "2.0", id: 2, method: "file.writeFile",
        params: { path: snapshot, content, expectedRevision: "", mode: 0o600 },
      });
      assert.ok(written && "result" in written, JSON.stringify(written));
      assert.equal(readFileSync(snapshot, "utf8"), content);
      assert.equal(statSync(snapshot).mode & 0o777, 0o600);
      assert.equal(statSync(recovery).mode & 0o777, 0o700);
      assert.equal(existsSync(join(process.env.HOME, ".local/share/codeharbor/recovery")), false);

      // No -L/-S here: real default-server selection must use TMUX_TMPDIR, even
      // with the older differently configured private master already running.
      tmux(fixtureEnvironment, ["-f", "/dev/null", "new-session", "-d", "-s", "keeper", "/bin/sleep", "300"]);
      assert.notEqual(tmux(fixtureEnvironment, ["display-message", "-p", "#{pid}"]), otherPid);
      const environmentFile = join(fixture, "tmux-child-environment");
      const command = 'set -e; printf "%s\\n" "$XDG_RUNTIME_DIR" "$XDG_DATA_HOME" "$HISTFILE" "$TMUX_TMPDIR" "$CODEHARBOR_DB" > "$1"; history -s fixture-history-sentinel; history -w; tmux wait-for -S fixture-environment-ready';
      tmux(fixtureEnvironment, ["new-window", "-d", "-t", "keeper", "-n", "probe",
        "/bin/bash", "--noprofile", "--norc", "-i", "-c", command, "fixture-bash", environmentFile]);
      tmux(fixtureEnvironment, ["wait-for", "fixture-environment-ready"]);
      assert.deepEqual(readFileSync(environmentFile, "utf8").trim().split("\n"), [
        environment.XDG_RUNTIME_DIR, environment.XDG_DATA_HOME, environment.HISTFILE,
        environment.TMUX_TMPDIR, environment.CODEHARBOR_DB,
      ]);
      assert.equal(existsSync(join(process.env.HOME, ".bash_history")), false);
      assert.equal(tmux(otherEnvironment, ["show-environment", "-g", "XDG_RUNTIME_DIR"]),
        `XDG_RUNTIME_DIR=${otherEnvironment.XDG_RUNTIME_DIR}`);
      assert.equal(tmux(otherEnvironment, ["show-environment", "-g", "XDG_DATA_HOME"]),
        `XDG_DATA_HOME=${otherEnvironment.XDG_DATA_HOME}`);
      assert.equal(tmux(otherEnvironment, ["show-environment", "-g", "HISTFILE"]),
        `HISTFILE=${otherHistory}`);
      assert.equal(readFileSync(otherHistory, "utf8"), "ordinary history must remain untouched\n");
    } finally {
      closeDefaultWorkspace();
      spawnSync("tmux", ["kill-server"], { env: tmuxEnvironment(fixtureEnvironment), timeout: 5000 });
    }
  }
} finally {
  spawnSync("tmux", ["kill-server"], { env: tmuxEnvironment(otherEnvironment), timeout: 5000 });
}
const servers = [];
try {
  for (const path of paths) {
    await mkdir(dirname(path), { recursive: true });
    const server = createServer();
    servers.push(server);
    await new Promise((resolve, reject) => {
      server.once("error", reject);
      server.listen(path, resolve);
    });
  }
  assert.equal(servers.filter((server) => server.listening).length, paths.length);
} finally {
  await Promise.all(servers.filter((server) => server.listening).map((server) =>
    new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()))));
}
console.log("generated SSH environments isolate both fixture bridge sockets from the ordinary default");
JS
