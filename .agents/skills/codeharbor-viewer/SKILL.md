---
name: codeharbor-viewer
description: Show files, diffs, directories and web pages in the user's CodeHarbor viewer panes, and split, focus, close or reload those panes. Use whenever the user asks to see, open, show, display or pull up something, or asks to rearrange the viewer region — instead of pasting file contents into the terminal.
---

# Controlling CodeHarbor viewer panes

You are running in a terminal pane inside CodeHarbor: a remote-first desktop
client whose window has a sessions sidebar, a **viewer region** of browser panes,
and the terminal region you live in. The viewer panes are the user's screen. Put
things there rather than printing them.

This file is read by Codex and by Oh My Pi (both scan `.agents/skills`).

## Tools

They come from the `codeharbor` MCP server: `viewer_list`, `viewer_open`,
`viewer_split`, `viewer_focus`, `viewer_close`, `viewer_reload`.

| tool | what it does |
| --- | --- |
| `viewer_list` | pane ids, what each shows, which has focus |
| `viewer_open` | show a file / directory / URL in a pane, optionally a new one |
| `viewer_split` | split a pane, leaving both halves as they were |
| `viewer_focus` | make a pane the active one |
| `viewer_close` | close a pane |
| `viewer_reload` | re-fetch what a pane is showing |

## Addressing content

`viewer_open`'s `url` accepts:

- a remote path — `README.md`, `docs/SPEC.md`, `/etc/hosts`. Relative paths
  resolve against the Dev Session's repository root.
- a directory, with a **trailing slash**: `src/qml/` gives the directory listing;
  without the slash CodeHarbor treats it as a file.
- `file:///absolute/path` — always a file on the **server**, never on the user's
  desktop machine.
- `http://` or `https://`.

`codeharbor-internal://` addresses are an internal detail and are refused.

## Rules

- **Call `viewer_list` before naming a pane.** Ids look like `viewer-1` and are
  per Dev Session; omitting `pane` targets the focused pane.
- **`viewer_open` with `newPane: true`** when the user should see the new thing
  alongside what is already open.
- **Never close a pane you did not open** without asking: `viewer_close` does not
  confirm unsaved editor changes.
- **After writing a file the user has open**, `viewer_reload` that pane.
- Two or three panes is plenty; the region has a fixed width.

## Refusal codes

`not_active_session` (the user is on a different Dev Session — say so, do not
retry), `unknown_pane` (re-list), `bad_request` (the message says what was
wrong), `timeout` / `failed` (CodeHarbor is not reachable).

## One-time setup

### Codex

Codex does not read a project MCP file by default. Register the server once:

```bash
codex mcp add codeharbor -- node /absolute/path/to/codeharbor/remote/src/mcp/server.ts
```

which writes to `~/.codex/config.toml`:

```toml
[mcp_servers.codeharbor]
command = "node"
args = ["/absolute/path/to/codeharbor/remote/src/mcp/server.ts"]
```

Sandboxing note: Codex launches a local stdio MCP server **outside** its command
sandbox, so the MCP path needs no `sandbox_mode` change. The shell fallback below
is a sandboxed command, and under `sandbox_mode = "workspace-write"` with the
network proxy enabled it needs the socket allowed explicitly:

```toml
[features.network_proxy]
enabled = true
unix_sockets = { "/run/user/1000/codeharbor-control-<token>.sock" = "allow" }
```

The socket name carries a per-window token, so this allowlist entry has to be the
path from `echo "$CODEHARBOR_CONTROL_SOCKET"` in the pane. Prefer the MCP tools and
none of this comes up.

### Oh My Pi

`.omp/mcp.json` in this repository already registers the server; nothing to do.

## Shell fallback

```bash
node <repo>/remote/src/tools/viewctl.ts list
node <repo>/remote/src/tools/viewctl.ts open --url README.md --new-pane
node <repo>/remote/src/tools/viewctl.ts reload --pane viewer-2
```

One JSON line on stdout; exit 0 applied, 1 refused, 2 usage.

## If nothing works at all

Every path needs three variables, exported into the tmux session of each pane
CodeHarbor creates: `OMP_DEV_SESSION_ID` and `OMP_TERMINAL_ID` (which pane) and
`CODEHARBOR_CONTROL_SOCKET` (which CodeHarbor window owns it). A shell that was
already running before CodeHarbor attached has none of them. Check with
`echo "$CODEHARBOR_CONTROL_SOCKET"`; if empty, ask the user to start the agent in
a fresh CodeHarbor terminal pane. The socket is never guessed — without it a
command could drive a different window — so a missing one is refused.
