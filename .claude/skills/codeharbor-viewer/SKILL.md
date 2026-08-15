---
name: codeharbor-viewer
description: Show files, diffs, directories and web pages in the user's CodeHarbor viewer panes, and split, focus, close or reload those panes. Use whenever the user asks to see, open, show, display or pull up something, or asks to rearrange the viewer region — instead of pasting file contents into the terminal.
---

# Controlling CodeHarbor viewer panes

You are running in a terminal pane inside CodeHarbor: a remote-first desktop
client whose window has a sessions sidebar, a **viewer region** of browser panes,
and the terminal region you live in. The viewer panes are the user's screen. Put
things there rather than printing them.

The tools come from the `codeharbor` MCP server, namespaced by Claude Code as
`mcp__codeharbor__<tool>`:

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

Do not pass `codeharbor-internal://` addresses; they are an internal detail and
are refused.

## Rules

- **Call `viewer_list` before naming a pane.** Pane ids look like `viewer-1` and
  are per Dev Session; omitting `pane` targets the focused pane, which is usually
  what you want.
- **`viewer_open` with `newPane: true`** when the user should see the new thing
  *alongside* what is already open. Replacing a pane's content silently takes
  away whatever they were reading.
- **Never close a pane you did not open** without asking. `viewer_close` does not
  confirm unsaved editor changes.
- **After you write a file the user has open**, `viewer_reload` that pane.
- Do not open more than two or three panes; the region is a fixed width and every
  extra pane makes all of them narrower.

## When a call is refused

The tool result names a reason:

- `not_active_session` — the user is looking at a different Dev Session. Say so;
  do not retry. Only the session on screen can be driven.
- `unknown_pane` — re-run `viewer_list`; the pane was closed or the layout moved.
- `bad_request` — the address or argument was rejected; the message says which.
- `timeout` / `failed` — CodeHarbor is not reachable (the user disconnected, or
  the desktop is busy). Fall back to the terminal and mention it once.

## Shell fallback

If the MCP server is not connected, the same operations are available from the
pane's shell — the CodeHarbor checkout on this server ships them:

```bash
node <repo>/remote/src/tools/viewctl.ts list
node <repo>/remote/src/tools/viewctl.ts open --url README.md --new-pane
node <repo>/remote/src/tools/viewctl.ts reload --pane viewer-2
```

It prints one JSON line and exits 0 on success, 1 on refusal. Prefer the MCP
tools: they need no quoting and no path to the checkout.

## If nothing works at all

Every path needs `OMP_DEV_SESSION_ID` and `OMP_TERMINAL_ID`, which CodeHarbor
exports into the tmux session of each pane it creates. A shell that was already
running before CodeHarbor attached does not have them. Check with
`echo "$OMP_DEV_SESSION_ID"`; if it is empty, tell the user to start the agent in
a fresh CodeHarbor terminal pane.
