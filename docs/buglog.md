> Historical notes from Nordend development, retained with their original names and measurements.
> Upstream: https://github.com/pankaspe/nordend-fm. Current application: rummage.

# Upstream bug log

Bugs found in **Noctalia Shell** and **Umbriel** while building Nordend.
Each confirmed bug becomes an upstream issue or PR (github.com/noctalia-dev).

Status: `suspected` → `confirmed` → `reported` → `pr-open` → `fixed` (or `wontfix` / `not-a-bug`)

## Template

```
### NB-000 — <short title>
- Project: noctalia | umbriel
- Version: <exact version / commit>
- Status: suspected
- Found: YYYY-MM-DD, while working on <roadmap step>
- Repro:
  1. ...
- Expected:
- Actual:
- Notes / suspected cause:
- Upstream: <issue or PR link>
```

## Entries

### NB-003 — A window without a decoration object still gets Umbriel's title bar
- Project: umbriel
- Version: commit d9497f3 (pinned in `void-noctalia/packages/srcpkgs/umbriel`)
- Status: suspected
- Found: 2026-10-30, while working on ROADMAP 7.4
- Repro:
  1. A client that binds `zxdg_decoration_manager_v1` but makes no toplevel decoration object
     for its window and draws its own title bar (Nordend at `0390fde`..`a4d34e9` with
     `NORDEND_FORCE_CSD=1` did; clients that do not know xdg-decoration do too).
  2. Look at the window.
- Expected: no server-side title bar: the protocol says "If compositor and client do not
  negotiate the use of a server-side decoration using this protocol, clients continue to
  self-decorate as they see fit."
- Actual: Umbriel draws its title bar above the client's own (two bars; screenshot by the author).
  Asking for client-side mode through the protocol works: Umbriel answers client-side and draws
  none.
- Notes / suspected cause: *to be verified* in Umbriel's source whether undecorated toplevels
  default to server-side (it may be a deliberate choice, for clients like Xwayland's).
- Upstream: none yet (only with the author's OK)

### NB-002 — Escape does not cancel a drag and drop
- Project: umbriel
- Version: commit d9497f3 (pinned in `void-noctalia/packages/srcpkgs/umbriel`)
- Status: suspected
- Found: 2026-10-30, while working on ROADMAP 7.1
- Repro:
  1. In Nordend, start dragging a file (press, move).
  2. Press Escape while still holding the button.
- Expected: the drag is cancelled (Mutter and KWin cancel it themselves; GTK sources also cancel
  on Escape when the key reaches them).
- Actual: nothing; the key does not reach the source client either, so it cannot cancel it.
- Notes / suspected cause: *to be verified* in Umbriel's source whether the drag grab keeps the
  keyboard and drops keys (wlroots leaves cancelling to the compositor). Ctrl/Shift modifiers do
  reach the client during the drag.
- Upstream: none yet (only with the author's OK)

### NB-001 — Umbriel title bars don't follow the Noctalia theme
- Project: noctalia
- Version: 5.3.0 (builtin `umbriel` template)
- Status: fixed
- Found: 2026-10-26, while working on ROADMAP 4.2
- Repro:
  1. Umbriel with `[appearance.title_bar] mode = "on"`, Nordend open (server-side decorations).
  2. Switch theme in Noctalia.
- Expected: the title bar takes the theme's colors, like the borders.
- Actual: it stays Umbriel's default dark grey; `~/.config/umbriel/noctalia.toml` has no
  `[colors.title_bar]` section.
- Notes / suspected cause: the template didn't write `[colors.title_bar]`, which Umbriel reads
  for title bar colors (`src/config/fields_general.cpp`). Already fixed upstream before we got
  there: `65b34e5` and `5f864d4`, released in Noctalia 5.4.0; our package was bumped and the
  author verified it (void-noctalia BUG-025).
- Upstream: https://github.com/noctalia-dev/noctalia/commit/65b34e5 (no issue or PR needed)
