#!/usr/bin/env node
// Release-version tool. Nothing in this file knows anything about a particular
// project: every path, pattern and command comes from a `.bumpversion.json`
// file in the repository being operated on. If that file is missing, malformed,
// or does not describe the tree it is pointed at, every command here fails and
// changes nothing. There are deliberately no defaults that guess at a layout.
//
// Commands (all accept `--root <dir>`, default: the current directory):
//
//   paths     print the files that a bump rewrites, one per line
//   inputs    print every file a bump reads, one per line (`paths` plus any
//             manifest an npm-lock source refers to)
//   settings  print `key<TAB>value` lines: tagPrefix, remote, commitMessage,
//             and one `preflight` line per configured preflight command
//   current   print the version held by the authoritative source
//   apply X.Y.Z
//             rewrite every source to X.Y.Z and print the paths changed
//   check     verify every source agrees with the authoritative source
//
// `apply` is all-or-nothing: it reads, parses and validates every source and
// builds the complete new text of every file in memory before it writes a
// single byte. A malformed lock file or a pattern that no longer matches
// therefore leaves the whole tree untouched, instead of half-bumped.
import { chmodSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from "node:fs";
import { isAbsolute, join, relative, resolve } from "node:path";

const CONFIG_NAME = ".bumpversion.json";
const SEMVER = /^\d+\.\d+\.\d+$/;

function fail(message) {
  console.error(`bumpctl: ${message}`);
  process.exit(1);
}

// Count the capturing groups in a pattern by making the whole pattern optional
// and matching it against the empty string: the result array has one slot per
// group regardless of whether anything matched.
function countCapturingGroups(pattern) {
  return new RegExp(`${pattern}|`).exec("").length - 1;
}

// Every configured path must be repository-relative and must stay inside the
// repository. A configuration file is checked in and reviewed like code, but it
// is still data, and a tool that will happily rewrite a file outside the
// repository because a config said so is a tool that turns a bad merge into a
// bad day.
function safePath(root, value, what) {
  if (typeof value !== "string" || value === "") {
    fail(`${what} must be a non-empty string`);
  }
  if (isAbsolute(value)) {
    fail(`${what} must be a path relative to the repository root, but '${value}' is absolute`);
  }
  if (value.split(/[\\/]/).includes("..")) {
    fail(`${what} must not contain a '..' segment, but '${value}' does`);
  }
  const absolute = resolve(root, value);
  const back = relative(root, absolute);
  if (back === "" || back.startsWith("..") || isAbsolute(back)) {
    fail(`${what} points outside the repository root: '${value}'`);
  }
  return absolute;
}

function loadConfig(root) {
  const configPath = join(root, CONFIG_NAME);
  let raw;
  try {
    raw = readFileSync(configPath, "utf8");
  } catch {
    fail(
      `no ${CONFIG_NAME} at ${configPath}. This tool is driven entirely by that ` +
        `file and has no built-in knowledge of any project's layout.`,
    );
  }

  let config;
  try {
    config = JSON.parse(raw);
  } catch (error) {
    fail(`${CONFIG_NAME} is not valid JSON: ${error.message}`);
  }
  if (config === null || typeof config !== "object" || Array.isArray(config)) {
    fail(`${CONFIG_NAME} must contain a JSON object`);
  }

  const tagPrefix = config.tagPrefix ?? "v";
  if (typeof tagPrefix !== "string") {
    fail("tagPrefix must be a string");
  }

  const remote = config.remote ?? "origin";
  if (typeof remote !== "string" || remote === "") {
    fail("remote must be a non-empty string");
  }

  const commitMessage = config.commitMessage ?? "Release {tag}";
  if (typeof commitMessage !== "string" || !commitMessage.includes("{tag}")) {
    fail("commitMessage must be a string containing the placeholder {tag}");
  }

  if (!Array.isArray(config.sources) || config.sources.length === 0) {
    fail("sources must be a non-empty array");
  }

  const seenPaths = new Set();
  const sources = config.sources.map((source, index) => {
    const where = `sources[${index}]`;
    if (source === null || typeof source !== "object" || Array.isArray(source)) {
      fail(`${where} must be an object`);
    }
    safePath(root, source.path, `${where}.path`);
    if (seenPaths.has(source.path)) {
      fail(`${where}.path is listed more than once: '${source.path}'`);
    }
    seenPaths.add(source.path);

    if (source.label !== undefined && (typeof source.label !== "string" || source.label === "")) {
      fail(`${where}.label must be a non-empty string when present`);
    }

    if (source.kind === "regex") {
      if (typeof source.pattern !== "string" || source.pattern === "") {
        fail(`${where}.pattern must be a non-empty string`);
      }
      try {
        new RegExp(source.pattern, "gd");
      } catch (error) {
        fail(`${where}.pattern is not a valid regular expression: ${error.message}`);
      }
      if (countCapturingGroups(source.pattern) !== 1) {
        fail(
          `${where}.pattern must contain exactly one capturing group, wrapped ` +
            `around the version number itself`,
        );
      }
      return {
        path: source.path,
        kind: "regex",
        pattern: source.pattern,
        label: source.label ?? source.path,
      };
    }

    if (source.kind === "json") {
      const key = source.key ?? "version";
      if (typeof key !== "string" || key === "") {
        fail(`${where}.key must be a non-empty string`);
      }
      return {
        path: source.path,
        kind: "json",
        key,
        label: source.label ?? `${source.path} ${key}`,
      };
    }

    if (source.kind === "npm-lock") {
      const manifest = source.manifest ?? "package.json";
      safePath(root, manifest, `${where}.manifest`);
      return {
        path: source.path,
        kind: "npm-lock",
        manifest,
        label: source.label ?? source.path,
      };
    }

    return fail(
      `${where}.kind must be "regex", "json" or "npm-lock", but is ` +
        `${JSON.stringify(source.kind)}`,
    );
  });

  if (typeof config.primary !== "string" || config.primary === "") {
    fail("primary must name the path of the source that holds the authoritative version");
  }
  const primary = sources.find((source) => source.path === config.primary);
  if (!primary) {
    fail(`primary '${config.primary}' is not one of the configured sources`);
  }
  if (primary.kind === "npm-lock") {
    fail(
      "primary must not be an npm-lock source; name the file a human edits, " +
        "because a lock file is generated and follows the others",
    );
  }

  const preflight = config.preflight ?? [];
  if (
    !Array.isArray(preflight) ||
    preflight.some((command) => typeof command !== "string" || command.trim() === "")
  ) {
    fail("preflight must be an array of non-empty command strings");
  }

  return { root, tagPrefix, remote, commitMessage, sources, primary: config.primary, preflight };
}

function readText(root, path, what) {
  const absolute = safePath(root, path, what);
  try {
    return { absolute, text: readFileSync(absolute, "utf8") };
  } catch (error) {
    if (error.code === "ENOENT") {
      fail(`${path}: this file is named in ${CONFIG_NAME} but does not exist`);
    }
    return fail(`${path}: cannot be read: ${error.message}`);
  }
}

function checkSemver(version, where) {
  if (typeof version !== "string") {
    fail(`${where} is missing, or is not text`);
  }
  if (!SEMVER.test(version)) {
    fail(`${where} is '${version}', which is not a three-part version number like 1.2.3`);
  }
  return version;
}

function loadRegexSource(root, source) {
  const { absolute, text } = readText(root, source.path, "source path");
  const matches = [...text.matchAll(new RegExp(source.pattern, "gd"))];
  if (matches.length === 0) {
    fail(
      `${source.path}: the configured pattern matches nothing, so the version ` +
        `in this file cannot be read or updated`,
    );
  }
  if (matches.length > 1) {
    fail(
      `${source.path}: the configured pattern matches ${matches.length} times; ` +
        `it must match exactly once so there is no doubt which occurrence is ` +
        `the release version`,
    );
  }
  // Validate the captured text BEFORE reading its offsets. A pattern whose one
  // capturing group sits inside an alternative that did not participate (say
  // `LEGACY_VERSION|VERSION (\d+\.\d+\.\d+)`) matches the file, but the group
  // captured nothing: `indices[1]` is then undefined, and destructuring it
  // first crashed with a bare "Cannot destructure property" stack instead of
  // saying which configured source is at fault.
  const version = checkSemver(
    matches[0][1],
    `${source.path}: the text captured by the configured pattern`,
  );
  const [start, end] = matches[0].indices[1];
  return {
    path: source.path,
    absolute,
    entries: [{ label: source.label, version }],
    render: (next) => text.slice(0, start) + next + text.slice(end),
  };
}

function parseJsonObject(path, text) {
  let data;
  try {
    data = JSON.parse(text);
  } catch (error) {
    fail(`${path} is not valid JSON: ${error.message}`);
  }
  if (data === null || typeof data !== "object" || Array.isArray(data)) {
    fail(`${path} must contain a JSON object`);
  }
  return data;
}

function loadJsonSource(root, source) {
  const { absolute, text } = readText(root, source.path, "source path");
  const data = parseJsonObject(source.path, text);
  const keys = source.key.split(".");
  let holder = data;
  for (const key of keys.slice(0, -1)) {
    holder = holder[key];
    if (holder === null || typeof holder !== "object" || Array.isArray(holder)) {
      fail(`${source.path}: there is no object at '${source.key}'`);
    }
  }
  const last = keys[keys.length - 1];
  const version = checkSemver(holder[last], `${source.path}: '${source.key}'`);
  return {
    path: source.path,
    absolute,
    entries: [{ label: source.label, version }],
    render: (next) => {
      holder[last] = next;
      return `${JSON.stringify(data, null, 2)}\n`;
    },
  };
}

function loadNpmLockSource(root, source) {
  const { absolute, text } = readText(root, source.path, "source path");
  const lock = parseJsonObject(source.path, text);
  const manifestText = readText(root, source.manifest, "npm-lock manifest path").text;
  const manifest = parseJsonObject(source.manifest, manifestText);

  const workspaces = manifest.workspaces ?? [];
  if (!Array.isArray(workspaces) || workspaces.some((entry) => typeof entry !== "string")) {
    fail(`${source.manifest}: "workspaces" must be an array of strings`);
  }
  const glob = workspaces.find((entry) => entry.includes("*"));
  if (glob !== undefined) {
    fail(
      `${source.manifest}: workspace pattern '${glob}' uses a wildcard. This tool ` +
        `will not guess which lock entries that expands to, because a guess that ` +
        `misses one ships a package whose version disagrees with the release. ` +
        `List the workspace directories explicitly.`,
    );
  }

  if (lock.packages === null || typeof lock.packages !== "object" || Array.isArray(lock.packages)) {
    fail(`${source.path} has no "packages" object`);
  }
  const rootEntry = lock.packages[""];
  if (rootEntry === null || typeof rootEntry !== "object" || Array.isArray(rootEntry)) {
    fail(`${source.path} has no root packages[""] entry`);
  }

  const slots = [
    {
      label: `${source.path} version`,
      read: () => lock.version,
      write: (next) => {
        lock.version = next;
      },
    },
    {
      label: `${source.path} packages[""].version`,
      read: () => rootEntry.version,
      write: (next) => {
        rootEntry.version = next;
      },
    },
  ];

  for (const workspace of workspaces) {
    const entry = lock.packages[workspace];
    if (entry === null || typeof entry !== "object" || Array.isArray(entry)) {
      fail(
        `${source.path} has no packages[${JSON.stringify(workspace)}] entry, but ` +
          `${source.manifest} declares that workspace`,
      );
    }
    slots.push({
      label: `${source.path} packages[${JSON.stringify(workspace)}].version`,
      read: () => entry.version,
      write: (next) => {
        entry.version = next;
      },
    });
  }

  const entries = slots.map((slot) => ({
    label: slot.label,
    version: checkSemver(slot.read(), slot.label),
  }));

  return {
    path: source.path,
    absolute,
    entries,
    render: (next) => {
      for (const slot of slots) {
        slot.write(next);
      }
      return `${JSON.stringify(lock, null, 2)}\n`;
    },
  };
}

function loadAllSources(config) {
  return config.sources.map((source) => {
    if (source.kind === "regex") return loadRegexSource(config.root, source);
    if (source.kind === "json") return loadJsonSource(config.root, source);
    return loadNpmLockSource(config.root, source);
  });
}

function commandPaths(config) {
  for (const source of config.sources) {
    console.log(source.path);
  }
}

function commandInputs(config) {
  const seen = new Set();
  for (const source of config.sources) {
    seen.add(source.path);
    if (source.kind === "npm-lock") {
      seen.add(source.manifest);
    }
  }
  for (const path of seen) {
    console.log(path);
  }
}

function commandSettings(config) {
  console.log(`tagPrefix\t${config.tagPrefix}`);
  console.log(`remote\t${config.remote}`);
  console.log(`commitMessage\t${config.commitMessage}`);
  for (const command of config.preflight) {
    console.log(`preflight\t${command}`);
  }
}

function commandCurrent(config) {
  const loaded = loadAllSources(config);
  const primary = loaded.find((source) => source.path === config.primary);
  console.log(primary.entries[0].version);
}

function commandApply(config, version) {
  checkSemver(version, "the requested version");
  // Read and validate everything first, then build the complete replacement
  // text for every file, and only then write. Nothing is written unless all of
  // it can be written.
  const loaded = loadAllSources(config);
  const pending = loaded.map((source) => ({
    path: source.path,
    absolute: source.absolute,
    original: readFileSync(source.absolute),
    // The replacement is written to a fresh file and renamed over the target,
    // which would otherwise give it whatever the umask says rather than the
    // mode the original had. Carry the original mode across, so a version file
    // that is also an executable script does not silently lose its +x bit.
    mode: statSync(source.absolute).mode,
    content: source.render(version),
  }));

  // Stage every new file beside its target first. Nothing real has been touched
  // yet at this point, so if staging fails - no disk space, no permission -
  // deleting the staged files leaves the tree exactly as it was.
  const staged = [];
  try {
    for (const file of pending) {
      const temporary = `${file.absolute}.bumpctl-${process.pid}`;
      writeFileSync(temporary, file.content);
      chmodSync(temporary, file.mode);
      staged.push({ ...file, temporary });
    }
  } catch (error) {
    for (const file of staged) {
      rmSync(file.temporary, { force: true });
    }
    fail(`could not prepare the new version files, so nothing was changed: ${error.message}`);
  }

  // Now move them into place. Renaming a file over its neighbour in the same
  // directory is the least failure-prone step available, but if one of them
  // still fails, put back the files that were already moved. The tree then
  // holds the version it started from, instead of the half-bumped state that
  // produces a release commit nobody can tag.
  const moved = [];
  try {
    for (const file of staged) {
      renameSync(file.temporary, file.absolute);
      moved.push(file);
    }
  } catch (error) {
    for (const file of moved) {
      writeFileSync(file.absolute, file.original);
    }
    for (const file of staged.slice(moved.length)) {
      rmSync(file.temporary, { force: true });
    }
    fail(
      `could not update ${staged[moved.length].path}: ${error.message}. ` +
        `Every file already updated was put back, so no version was changed.`,
    );
  }

  for (const file of pending) {
    console.log(file.path);
  }
}

function commandCheck(config) {
  const loaded = loadAllSources(config);
  const primary = loaded.find((source) => source.path === config.primary);
  const want = primary.entries[0].version;
  const wrong = [];
  for (const source of loaded) {
    for (const entry of source.entries) {
      if (entry.version !== want) {
        wrong.push(`${entry.label}: ${entry.version}`);
      }
    }
  }
  if (wrong.length > 0) {
    console.error(`Version drift. ${config.primary} says ${want}, but:`);
    for (const line of wrong) {
      console.error(`  ${line}`);
    }
    process.exit(1);
  }
  console.log(`all version strings agree: ${want}`);
}

function main(argv) {
  let root = process.cwd();
  const positional = [];
  for (let index = 0; index < argv.length; index += 1) {
    if (argv[index] === "--root") {
      index += 1;
      if (index >= argv.length) {
        fail("--root needs a directory");
      }
      root = resolve(argv[index]);
    } else {
      positional.push(argv[index]);
    }
  }

  const [command, ...rest] = positional;
  if (command === undefined) {
    fail("no command given (paths, inputs, settings, current, apply, check)");
  }

  const config = loadConfig(root);
  switch (command) {
    case "paths":
      return commandPaths(config);
    case "inputs":
      return commandInputs(config);
    case "settings":
      return commandSettings(config);
    case "current":
      return commandCurrent(config);
    case "apply":
      if (rest.length !== 1) {
        fail("apply needs exactly one argument: the new version");
      }
      return commandApply(config, rest[0]);
    case "check":
      return commandCheck(config);
    default:
      return fail(`unknown command '${command}'`);
  }
}

main(process.argv.slice(2));
