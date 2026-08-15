---
name: codeharbor-viewer
description: Show files, diffs, directories and web pages in the user's CodeHarbor viewer panes, and split, focus, close or reload those panes. Use whenever the user asks to see, open, show, display or pull up something, or asks to rearrange the viewer region — instead of pasting file contents into the terminal.
---

# Controlling CodeHarbor viewer panes

You are running in a terminal pane inside CodeHarbor: a remote-first Qt desktop
client whose window is a sessions sidebar, a **viewer region** of browser panes,
and the terminal region you live in. The viewer panes are the user's screen. Put
things there rather than printing them.

## Tools

The `codeharbor` MCP server (registered by `.omp/mcp.json` in this repository)
provides:

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
  desktop machine (SPEC 2.4).
- `http://` or `https://`.

`codeharbor-internal://` is an implementation detail of the privileged handlers
(SPEC 7.4) and is refused.

## Rules

- **`viewer_list` before naming a pane.** Ids look like `viewer-1`, are per Dev
  Session, and are recycled; omitting `pane` targets the focused pane.
- **`viewer_open` with `newPane: true`** when the user should see the new thing
  alongside what is already open.
- **Never close a pane you did not open** without asking: `viewer_close` takes the
  palette's unconfirmed close path and will not warn about unsaved editor changes.
- **After writing a file the user has open**, `viewer_reload` that pane — the
  server-side watch refreshes clean buffers, but a rendered view that missed it
  needs the nudge.
- Two or three panes is plenty; the region has a fixed width.

## Refusal codes

`not_active_session` (the user is looking at a different Dev Session — say so, do
not retry; only the session on screen can be driven), `unknown_pane` (re-list),
`busy` (too many commands in flight — slow down), `bad_request` (the message names
the bad field), `timeout` / `failed` (CodeHarbor is not reachable).

## Shell fallback

```bash
# from the repository root on the server
node remote/src/tools/viewctl.ts list
node remote/src/tools/viewctl.ts open --url README.md --new-pane
node remote/src/tools/viewctl.ts split --orientation vertical
node remote/src/tools/viewctl.ts reload --pane viewer-2
```

One JSON line on stdout; exit 0 applied, 1 refused, 2 usage.

## If nothing works at all

Every path needs `OMP_DEV_SESSION_ID` and `OMP_TERMINAL_ID`, which CodeHarbor
exports into the tmux session of each pane it creates
(`TerminalController::tmuxNewSessionCommand`). A shell that was already running
before CodeHarbor attached to its tmux session does not have them. Check with
`echo "$OMP_DEV_SESSION_ID"`; if empty, ask the user to start the agent in a fresh
CodeHarbor terminal pane.

## How it works, if you need to debug it

```
this pane's agent
  -> $XDG_RUNTIME_DIR/codeharbor-control.sock  (remote/src/control.ts)
  -> codeharbord relays a `viewer.command` JSON-RPC notification to the desktop
  -> ch::ViewerCommandService -> Main.qml -> the real ViewerRegion
  -> `viewer.commandResult` back to the daemon -> the answer on your socket
```

If the socket is missing, CodeHarbor is not currently connected to this server, or
a second CodeHarbor window took the socket first (its daemon prints
`viewer control disabled: control address already in use` on stderr).
