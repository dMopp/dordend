# Compatibility

How rummage behaves outside Noctalia and Umbriel (ROADMAP 7.4). One column per desktop, filled
in by the author's tests; "expected" is what the protocols each compositor offers should give,
*to be verified* until tested.

## What depends on the compositor

| Need | Protocol | Without it |
|------|----------|------------|
| Title bar | `xdg-decoration` in server-side mode | rummage draws its own (client-side) |
| Cursor shapes | `cursor-shape-v1` | rummage draws them from the xcursor theme (`XCURSOR_THEME`, `XCURSOR_SIZE`) |
| Sharp fractional scaling | `fractional-scale-v1` + `viewporter` | renders at scale 1 |
| Copy and paste, drag and drop with other apps | `wl_data_device_manager` | inside rummage only |
| Focus for the apps it opens | `xdg-activation-v1` | they may open without focus |
| The desktop's colours, without `colors.toml` | the freedesktop portal, `org.freedesktop.appearance` (`color-scheme`, `accent-color`) | rummage's own default palette |

Icons need nothing: the Tabler subset is compiled into the program.

Two variables try rummage's own paths on a compositor that would not need them:
`RUMMAGE_FORCE_CSD=1` asks for client-side decorations through xdg-decoration (the compositor
has the last word), `RUMMAGE_FORCE_XCURSOR=1` draws the cursor from the xcursor theme.

## Expected

| | Umbriel | GNOME | Plasma | niri | weston |
|---|---|---|---|---|---|
| Title bar | the compositor's | rummage's | the compositor's | rummage's (niri asks for client-side unless `prefer-no-csd`) | rummage's |
| Cursor | cursor-shape-v1 | cursor-shape-v1 (recent GNOME) | cursor-shape-v1 | cursor-shape-v1 | xcursor |

## Checklist per session

For each desktop: build in the VM (or on the host for Umbriel), then `./build/rummage ~` and:

1. It starts, with the icons in the sidebar, the views and the status bar.
2. Title bar: the title shows; drag moves the window; double click maximizes and restores;
   minimize, maximize, close work; right click opens the window menu (where there is one).
3. Edges and corners resize the window, with the matching arrow; not when maximized.
4. The cursor shows, and turns into a double arrow on the column dividers.
5. Copy a file in rummage, paste it in the desktop's file manager, and back.
6. Drag a file from rummage into another app, and from another app into rummage.
7. Enter on a photo, a PDF, a text file opens them in their apps, which get the focus.
8. At a fractional scale (125 %, 150 %), text and icons are sharp.

## Results

| Step | Umbriel | GNOME | Plasma | niri | weston (headless) |
|------|---------|-------|--------|------|-------------------|
| 1 start, icons | yes | yes | yes | yes | yes (2026-10-30, screenshot) |
| 2 title bar | Umbriel's, one bar | rummage's; move, double click, buttons, window menu: yes | KWin's (screenshot), one bar | rummage's | drawn (screenshot); no pointer to try it |
| 3 resize | yes | yes | yes | yes | no pointer |
| 4 cursor | yes (cursor-shape-v1) | yes, double arrow on dividers | yes | yes | no pointer |
| 5 clipboard | yes, with foot and a second rummage | yes, both ways with Nautilus | yes, both ways with Dolphin | yes, with foot and a second rummage | no clipboard global |
| 6 drag and drop | yes, with foot and Firefox | yes, both ways with Nautilus | yes, both ways with Dolphin | yes, with foot and Firefox | no seat |
| 7 open files | yes, focused | yes, apps from `mimeapps.list` (imv, zathura, Okular), focused | yes, focused | yes, focused | — |
| 8 fractional scale | yes | yes, after enabling `scale-monitor-framebuffer` | yes | not tried (nested: the host window's scale) | no fractional-scale |

Umbriel: the author's desktop (pinned commit), tested by the author on 2026-10-31.

GNOME: 48.7 (mutter 48.7) in the Void VM `rummage-dev` (virtio without 3D, Mesa llvmpipe),
tested by the author on 2026-10-31; 249 tests pass in the VM. The colours are rummage's defaults,
not GNOME's: following the desktop's light/dark and accent is planned in 7.4. Fractional scaling
in GNOME 48 is experimental:
`gsettings set org.gnome.mutter experimental-features "['scale-monitor-framebuffer']"`, then log
in again.

Plasma: 6.7.5 (KWin 6.7.5) in the same VM, tested by the author on 2026-10-31. The title bar and
the task bar show the generic Wayland icon: no desktop entry matches the app id `rummage` yet
(to add with the 0.1.0 package, 7.8).

Desktop colours (without `colors.toml`): light/dark and the accent followed live on GNOME and
Plasma, tested by the author on 2026-10-31; Umbriel keeps Noctalia's colours.

niri: 26.04, nested in Umbriel on the host (in the VM niri needs virgl, BUG-017 of
void-noctalia), minimal config without `prefer-no-csd`, tested by the author on 2026-10-31.

Headless weston offers no `wl_seat`: it shows the window, not how it reacts to the pointer.
