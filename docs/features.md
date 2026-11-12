# Nordend: every feature

Everything Nordend does today, by area. Numbers are measurements from
[benchmarks.md](benchmarks.md), taken on a Ryzen 7 5700G with an RX 6650 XT; how each feature came
to be is in the [devlog](devlog.md), and what comes next is in the
README's [What's next](../README.md#whats-next).

## What it is

- A file manager written from scratch in C++20 for Wayland: its own OpenGL ES renderer for
  boxes, text, icons and images. **No GTK, no Qt, no GLib.**
- Its own project, at home on any Wayland desktop. On Noctalia (Noctalia Shell, the Umbriel
  compositor) it wears the Noctalia theme and the same line icons, and Umbriel draws its title
  bar; elsewhere (GNOME, weston) Nordend draws its own.
- One window thread, a pool of workers for everything that touches the disk, and **zero CPU when
  nothing moves**: no frame is drawn unless something changed (0 ticks in 30 s, measured).

## Three views of the same folder

- **Columns** (Ctrl+1), as in Finder: each folder you pick opens in the next column, and the last
  column shows the selected file. Left from the first column climbs to the parent. Columns are
  resized by dragging their edge (a double click on the edge resets it), and the row slides
  sideways by itself to keep the focused column in sight.
- **Grid** (Ctrl+2): large icons and thumbnails. Ctrl+plus, Ctrl+minus or Ctrl+wheel resize the
  cells from 48 to 512 px, and Ctrl+0 goes back to the usual size. The largest sizes fit the
  available width when the window is narrow; Ctrl+I hides the information pane for more room.
- **Details** (Ctrl+3): a table of name, size, date and type. Click a title to sort by it and
  click again to reverse; drag a column's edge to widen it. By size, folders keep their name
  order; by type, unknown types come last. The order you choose is used in the other views too.
- **Zoom in the lists** too: Ctrl+plus, Ctrl+minus or Ctrl+wheel make the rows of the columns and
  the details larger or smaller (five sizes: row, icon and text grow together, the details' other
  columns with them), Ctrl+0 the usual size; the cursor's row stays where it is on screen. The
  lists share one size, the grid keeps its own. Each tab has its own zoom (a new tab starts with
  the one it comes from), remembered with the tabs.
- **Sort By** in the context menu (on a file or on empty space), in every view: name, size,
  modified or type, ascending or descending, a dot on the order in use. One order for the whole
  window: every folder in every view is sorted so, and it comes back at the next start.
- **Tabs** (Ctrl+T): each with its own folder, view and back/forward history. The bar above the
  path shows them when there are two or more: a click shows a tab, its × or a middle click
  closes it, + opens one. Ctrl+Tab and Ctrl+Shift+Tab go round them, Alt+1…9 picks one, Ctrl+W
  closes the one shown (never the last). Each tab keeps its own zoom too. A middle click on a folder, or the context menu's Open in
  New Tab (Tabs, for several folders), opens it in a tab behind the one shown; a middle click on
  a selected folder opens every selected folder. The order, hidden files, the sidebar and the info pane
  are the window's, the same in every tab.
- **One folder, three ways of looking at it.** Switching views keeps the cursor, the selection and
  the file types already detected. The status bar has a button for each view.

## Moving and selecting

- **Natural scrolling.** On a touchpad the list follows the fingers 1:1, keeps gliding when they
  lift, slows down smoothly and stops softly at the ends. A mouse wheel glides instead of jumping,
  and quick turns add up. The arrows and Page Up/Down glide; Home and End jump.
- **Keyboard first:** arrows, Page Up/Down, Home and End; Enter, Right or Alt+Down to go in;
  Backspace, Left or Alt+Up to go back. The cursor lands on the folder you came from.
- **History:** Alt+Left and Alt+Right, or the mouse's back and forward buttons, go through the
  folders opened (the last 100), each with the cursor where it was left. Opening a folder after
  going back drops the ones ahead; a folder deleted meanwhile is skipped, with a message.
- **Hidden files out of the way:** dotfiles are hidden at start; Ctrl+H shows them (dimmed) in
  every view and hides them again. Hidden, they are never selected, so Ctrl+A and Delete touch
  only what you see, and the status bar says how many there are ("12 items, 3 hidden"). A hidden
  folder you open (from a bookmark, say) stays in sight on the way to it.
- **Selection as you expect it:** click, Ctrl+click and Shift+click; Shift+arrows for a range,
  Ctrl+arrows and Ctrl+Space to pick here and there, Ctrl+A for all, Escape for one again.
- **Rubber band:** a drag from empty space (below the entries, a row away from its icon and
  name, a cell away from them in the grid) draws a rectangle that selects what it touches; with
  Ctrl it adds to the selection. Past the top or bottom of the list it scrolls, faster the
  farther the pointer; in the column view it stays in its column. A drag from an icon or a name
  still moves the files, and a click anywhere on a row still selects it.
- **The selection follows its files:** cursor and selection are kept by name, so they stay on a
  file that another program renames or that moves in the order.
- **Every shortcut on one card:** F1 or "?" lists them all, from the same table the code runs on,
  and a test checks that the card and the behaviour agree. Groups start collapsed; click a heading to expand it, or use Tab and Enter.
  Keys and descriptions share aligned rows. The card adapts to the window width;
  scroll with the wheel, touchpad, scrollbar, arrow keys, Page Up/Down, or Home/End.
  Escape, the close button, or a click outside dismisses it.

## Photos first

- **Thumbnails in a fraction of a second.** Nordend reads the previews that cameras already store
  inside their raw files instead of developing the raw. A screenful of photos is ready in
  0.15-0.4 s the first time and in 12-22 ms from the cache. One thumbnail takes 12 ms from an
  OM-1 JPEG and 36 ms from an ORF.
- **Raw files from many cameras:** OM System and Olympus ORF (including the maker-note preview),
  Canon CR2, Nikon NEF and TIFF, Sony ARW, Fujifilm RAF, Panasonic RW2 (cropped to the shot's
  aspect ratio), Samsung SRW, Epson ERF, Hasselblad 3FR and DNG (including previews stored as a
  JPEG inside a TIFF: full size in a Samsung GX20's DNG). A DNG whose previews are too small for
  where it is shown (more than 4/3 stretched: Leica M240 and M8, DJI drones, files from Adobe's
  converter) is developed by Nordend's own reader instead: lossless JPEG or uncompressed sensor
  data (8-16 bit, strips or tiles, linear raw too), black and white levels, the camera's white
  balance with highlights kept white, the file's colour matrices for the shot's light, its
  baseline exposure and tone curve (else a light curve of Nordend's), the default crop, the
  file's corrections (gain maps for vignetting and colour shading, the lens warp: a DJI drone's
  corners no longer dark; a correction Nordend does not apply keeps the preview), at half
  size or smaller, straight to the size shown; in Quick Look in about 105 ms (Canon) to 260 ms
  (M240 at 2560x1440). Each is shown upright, in the shape it
  was shot, without the black bars some cameras put on their thumbnails; a preview the camera
  stored broken is passed over for the next one. Formats with no preview Nordend can read (Sigma
  X3F, old Panasonic RAW) say so in Quick Look.
- **Images:** PNG, JPEG and GIF (wuffs), WebP (libwebp), JPEG XL (libjxl), binary PGM and PPM
  (8 and 16 bits), and **TIFF** with Nordend's own reader (BigTIFF too; uncompressed, LZW,
  Deflate, PackBits, JPEG; 1 to 16-bit and floating-point samples; grey, RGB, YCbCr, palette,
  CMYK, alpha; strips and tiles), with EXIF orientation. The format is told by the file's bytes, not its name
  (a PNG called `.jpg` still shows), and only camera files are searched for stored previews.
- **Pictures made apart:** the code that reads photos (libwebp, libjxl, wuffs, and Nordend's own
  TIFF, DNG and camera-file readers) run in small `nordend-preview` processes, not in Nordend: a file that crashes or hangs
  one costs only its preview ("Preview unavailable"), and Nordend carries on. Each one lives in
  a cage of Nordend's own (seccomp and landlock, no library): it cannot open a file, reach the
  network, start a program or signal another one, and has at most 4 GiB of memory. They start when a
  picture is first needed (at most one per worker), give their memory back after a quiet second,
  and Nordend closes them after 30 s without work.
- **Engines described by manifests:** each reader program (an *engine*; `nordend-preview` is the
  base engine, for images and text) comes with a small TOML manifest in `share/nordend/engines/`
  saying which types it shows and what it gives back. Text files too are read in its cage, never
  by Nordend. Nordend reads them at start-up (about 0.05 ms) and asks them
  who shows a file; a broken manifest or a missing program is skipped with a warning, and the
  files it would show get the information card. `nordend --about` lists the engines found.
- **Plain reasons:** a file that cannot be shown says why in one sentence, the same in Quick Look
  and everywhere else: not a kind Nordend shows, no readable preview in a raw file, too large,
  damaged, or unreadable.
- **Shared freedesktop thumbnail cache:** thumbnails go where GNOME, KDE and other apps look for
  them, byte-compatible, so they are made once for all of them. Files that fail are remembered.
- **Quick Look** (Space): the photo fills the window, raw files included. An ORF fitted to a
  2880x1620 screen takes 51-55 ms. The arrows walk the folder without closing it, and the next
  photo is already loading. Text files open on a page you can scroll, in your monospace font
  (darktable sidecars included), read in UTF-8, UTF-16 (with its byte order mark) or Latin-1
  (Windows-1252, for files from old editors: "caffè 3€" shows as written), with **syntax
  colours** for C, C++, Rust, Python, JavaScript, TypeScript, Lua, Luau, shell scripts (heredocs
  included), fish, CMake, Makefiles, JSON (with JSONC's and JSON5's comments), TOML, YAML
  (block scalars, anchors and tags included) and INI files (desktop entries, systemd units,
  `.gitconfig`, `.editorconfig`), keys set apart from their values, and `.gitignore` (and
  `.dockerignore`, `.ignore`, `.npmignore`...) and `.gitattributes`, wildcards and negations
  standing out, Markdown (highlighted, not rendered: its code blocks in their own language, a
  YAML or TOML front matter) and diffs and patches (added and removed lines, hunks, headers), the
  language found from the file's type, its name (`PKGBUILD`,
  Void's `template`, `CMakeLists.txt`, `Cargo.lock`, `.clang-format`, `.gitignore`), its extension or its `#!` line, made from your theme's colours and readable
  on its page; the info pane's first lines are coloured too; any other file shows what it is.
  On a text page you **select and copy**: drag (the page scrolls when you go past its top or
  bottom), double click a word, triple click a line, Shift+click to extend, Ctrl+A for the whole
  file, Ctrl+C copies the text (with nothing selected it copies the file, as anywhere else); a
  click beside the page still closes Quick Look. Long lines scroll sideways (Shift+wheel, or the
  touchpad), the line numbers staying put; Home and End reach the text's top and bottom, Page Up
  and Page Down a page (the arrows still walk the folder). The caption says the language, the
  encoding, CRLF when the file has it, the lines and the size. A minified file (lines over 500
  bytes on average) is shown plain, its caption saying so. A log (`.log`) larger than what is read
  shows its newest lines: its last 256 KiB, opened at the bottom, "last N lines" in the caption.
- **The info pane** beside every view shows the preview and what the file is: size, pixel size,
  dates (including the birth time), type, and for photos the shot itself (camera, lens,
  exposure, aperture, ISO, focal length, when) with your darktable rating from the `.xmp`
  sidecar. "More details" adds permissions, owner and group, link target and item count. In the
  grid and the details Ctrl+I hides it, and its edge can be dragged from 240 to 720 px.
- **darktable-aware:** sidecars get their own icon, show your rating, and follow their photo when
  it is renamed.

## Working with files

- **Opens in the right app.** Enter or a double click opens a file in its default application,
  chosen exactly as GTK apps choose it: 0 differences from `gio` on all 1 232 file types and
  aliases. Nordend implements the freedesktop specs itself, without `xdg-open`:
  - several files for the same app start one instance (five photos, one darktable);
  - terminal apps such as Helix open in a terminal (`$TERMINAL`, `xdg-terminal-exec`, foot,
    kitty, ghostty, alacritty, wezterm...);
  - the new window comes up in front, through an xdg-activation token;
  - apps run detached: no zombies, and closing Nordend leaves them open;
  - an executable is never run: it opens in the app for its type;
  - the status bar says what happened ("Opening 5 files with Darktable…"), in red when there is
    no app for a type or the app fails at once.
- **Open in a terminal.** F4 opens a terminal in the folder under the cursor, else in the folder
  shown (also from the context menu, "Open in Terminal"); the same terminal as for terminal apps
  (`$TERMINAL`, `xdg-terminal-exec`, foot...).
- **Rename in place** (F2): the name becomes a field where it is, in all three views, with the
  name selected but not its extension. Typing works with accents and compose. Enter keeps the
  name, Escape gives up, and a click elsewhere keeps it. The usual editing keys work, including
  words with Ctrl+arrows and selection with Shift; Ctrl+V pastes the clipboard's text (its first
  line); Ctrl+Z undoes and Ctrl+Shift+Z redoes (a word typed, characters deleted one after
  another, or a paste is one step; up to 100).
- **New folder** (Ctrl+Shift+N): "New Folder" (or "New Folder 2"...), ready to be named.
- **Never overwrites:** renames use `RENAME_NOREPLACE`, so an existing file is never replaced,
  even one that appears in the meantime. A taken or invalid name is refused with a message and
  the field stays open for another try.
- **Photo-safe renames:** renaming `P1.ORF` also renames `P1.ORF.xmp` and the duplicates
  `P1_01.ORF.xmp`, `P1_02.ORF.xmp`… If any of their new names is taken, nothing moves.
- **Copy and move** (Ctrl+C or Ctrl+X, then Ctrl+V), in any of the three views:
  - a move on one disk is a rename: instant, whatever the size;
  - a copy runs as fast as `cp` (3.1 GB of photos in 1.39 s, warm cache, against 1.39 s), by
    the kernel's `copy_file_range`, on a thread of its own, so the window never waits;
  - the status bar shows "Copying 163 files · 2.1 GB of 3.4 GB · 180 MB/s · 8 s left", a
    progress line and an × to stop; what arrived is selected afterwards;
  - nothing half-copied ever carries a real name: each file is written under a hidden temporary
    name and renamed when complete, and a folder is built whole the same way. Stopping leaves
    nothing behind;
  - moving off another disk (an SD card, a USB drive) deletes the original only after its copy is
    flushed to disk;
  - dates (to the nanosecond), permissions and symbolic links are kept, and darktable sidecars
    travel with their photo, even under a new name;
  - cut files fade until pasted (Escape keeps them where they are);
  - a taken name asks on a card: Replace (the old file goes only once the new one is complete;
    folders are merged), Skip, Keep both ("P1 2.ORF", sidecars included), for this one or for
    the rest; pasting into the same folder keeps both by itself;
  - refused before anything moves: a folder into itself, or more than the free space.
- **Undo** (Ctrl+Z, in any view and in Quick Look) the last operation on files, and redo it
  (Ctrl+Shift+Z):
  - a rename gets its old name back, darktable sidecars included; a move goes back where it came
    from, across disks too; a Delete is restored; a restore goes back to the trash;
  - copies and new folders or files go to the trash, never deleted, so even an undo can be
    undone; a new folder only while it is empty, a new file only while it is as it was made;
  - a permanent delete cannot be undone, and the status bar says so; what changed meanwhile
    (a file another program moved) is skipped and said;
  - the status bar says what happened: "Undone: “Lake.ORF” is “P1.ORF” again", "Undone: 3
    items moved to the trash", "Redone: “P1.ORF” back in album".
- **Drag and drop:** a press on an entry's icon or name moved a few pixels drags it (the whole selection when it
  is in it), with its icon and name under the pointer. It drops onto a folder in any view (marked),
  the empty part of a column or view (that folder), a step of the path above or a place in the
  sidebar; the Trash trashes. On the same disk it moves, onto another it copies; Ctrl copies,
  Shift moves. It goes through the same engine as a paste: sidecars, questions, progress, Ctrl+Z.
  A folder never goes into itself, and dropping where the files already are does nothing. Other
  programs receive the files as file URIs and paths (Firefox, terminals), only to copy: Nordend
  never deletes its files because another program says it took them. Files dragged from another
  program (Firefox's downloads) are copied; a web address is not a file. Near the top or bottom of
  a list the drag scrolls it. Opening a folder by hovering over it comes later.
- **The system clipboard**, with other programs:
  - files copied or cut in Nordend paste in other programs: a file manager, Firefox (an upload, a
    message), a terminal or an editor (their paths); Nordend offers them as
    `x-special/gnome-copied-files`, `text/uri-list` (each file URI as GLib writes it) and text,
    and says "cut" the GNOME and the KDE way;
  - files copied or cut in another program paste in Nordend with Ctrl+V: copied, or moved when
    the other program cut them, through the same engine (sidecars, conflicts, progress); only
    files on this computer are taken;
  - read and written through pipes the window's loop watches: a program that does not answer
    never freezes Nordend (it is given up after 3 s);
  - when another program copies something, the files Nordend cut stop fading: Ctrl+V pastes what
    is on the clipboard now. A cut pasted once empties the clipboard.
- **The Trash** in the sidebar (its icon full or empty):
  - every trash as one folder: the home's and each disk's (`.Trash-1000` on a data disk), by
    original name; the same name trashed twice shows as "a.txt" and "a.2.txt";
  - the info pane adds where it came from and when it was deleted; previews, thumbnails and
    Quick Look work as anywhere else; it follows what other programs trash or restore, live;
  - Restore (right click) puts things back where they were, makes their folder again if it is
    gone, asks on the card when the name is taken (a folder is never merged), and brings a
    photo's darktable sidecars back with it;
  - Delete (or "Delete Permanently…" in the menu) deletes the selection for good, a photo with
    its trashed darktable sidecars; "Empty Trash…" (on the trash's empty space, or a right click
    on Trash in the sidebar) empties every trash at once, orphaned `.trashinfo` files and
    `directorysizes` included; both after the confirmation card;
  - nothing is made, renamed or pasted there, and files are not opened from the trash.
- **Context menu**: a right click (or a two-finger tap on the touchpad) in any of the three views
  or on the info pane, the Menu key or Shift+F10:
  - on files and folders: Open, Open With, Copy, Cut, Paste, Rename (one entry), Move to Trash,
    Delete Permanently; on a folder also Open in Terminal and Add to Sidebar;
    on a folder's empty space: New Folder, New File, Paste, Add to Sidebar, Open in Terminal; what cannot be done now is dimmed,
    and each item shows its shortcut;
  - a right click on an entry selects it first, unless it is part of the selection already;
  - Open With lists the applications for the file's type exactly as GTK apps do (0 differences
    from `gio mime` on all 1 232 types and aliases), the default one first, and opens every
    selected file with the one chosen (on a folder: the applications for folders, such as an
    editor, and the selected folders);
  - New File makes an empty file, or a copy of one of your templates (`XDG_TEMPLATES_DIR`),
    never over an existing file, with the name ready to edit;
  - drawn by Nordend in your theme's colors, it stays whole near the window's edges, and the
    keyboard works in it: arrows, Enter, → into a submenu, ← and Escape back.
- **Empty files open as their name says**: a new, empty `notes.txt` opens in your text editor
  (GLib calls any empty file `application/x-zerosize`, which no application opens).
- **Trash** (Delete), the freedesktop Trash specification 1.0, in any of the three views and in
  Quick Look:
  - files on the home's disk go to `~/.local/share/Trash`, files on other disks to that disk's
    own trash (`.Trash-1000` at its top, or `.Trash/1000`): a rename, never a copy, so trashing
    is instant (1 001 items in 25 ms, `gio trash` 70 ms); where no trash can be made, the files
    stay and the status bar says why;
  - other trash programs (`trash-list`, `trash-restore`, gio, file managers) see what Nordend
    trashed and restore it to its place: the `.trashinfo` files are the same as GLib's, odd
    names included;
  - darktable sidecars go with their photo, each restorable on its own;
  - the cursor moves on to the next entry; the status bar says what went ("“P2.ORF” with 1
    darktable sidecar moved to the trash") or what could not;
  - trashed folders are listed in `directorysizes` with their size, so trash tools need not
    measure them again.
- **Delete permanently** (Shift+Delete, in any view and in Quick Look; Delete in the Trash):
  - first a card says what goes: "Delete “P1.ORF” permanently?", "With its darktable sidecar ·
    21.0 MB", and for folders how many files and folders they hold in all; the size is added up
    on a worker, the card is there at once; Enter (or the red Delete) deletes, Escape (or Cancel)
    keeps everything;
  - on the transfer thread, with progress in the status bar and its × to stop; links are never
    followed, read-only folders of yours are opened up to be emptied; darktable sidecars go with
    their photo, and a photo that stays keeps them;
  - 10 101 files and folders in 0.20 s on ext4 (`rm -rf` 0.17 s: the difference is the first
    pass that counts them for the progress bar).
- **Clear errors:** a read-only folder, permission denied or a missing file shows a message in the
  status bar, never a crash.

## Finding your way

- **Command palette** (Ctrl+K): a card at the top of the window; type a few letters of anything
  Nordend can do and Enter runs it. Every command is there, the ones with a shortcut (shown at the
  right) and the ones only in menus (Sort by…, Open With…, New Empty File, Empty Trash, Eject,
  Zoom, Select All, Quit); only what can be used at that moment is offered ("Paste" with
  something to paste, "Rename…" with one entry). The sidebar's places are there as "Go to …" and
  the other tabs as "Tab 2: …". The letters are matched in order anywhere (fzy, as Noctalia's
  launcher): "nf" finds New Folder, "gdo" Go to Downloads; the matched letters show in the accent.
  With nothing typed, the ten most recently used come first, remembered in `state.toml`. ↑ ↓
  and Page Up/Down move, Enter runs, Escape or Ctrl+K again closes; the mouse works too. Each item
  goes the same way as its key or menu item, so they never differ. "Open With…" lists the
  applications in the palette itself.
- **Find files** (Ctrl+F): the same card, on every file and folder under the folder you are in,
  at any depth. They are listed once, nearest first, on a worker (273 000 entries in under half a
  second), and what you type is matched against their paths on all the workers, a new letter
  cancelling the search before: the first results show in about 8 ms. A match in a file's name
  ranks above one that needs its folders, so "vinc17" finds `Piramide Vincent/_7230017.ORF`;
  spaces in what you type do not count. Enter shows the file in the view (the columns unfold down
  to it), Space opens it in Quick Look, Shift+Enter in its application. Hidden files are left out
  unless Ctrl+H shows them; links are not followed and other disks mounted inside are not entered;
  the search stops at 300 000 entries and says so. The memory the paths took (about 85 bytes
  each) goes back to the system when the card closes.
- **Sidebar:** your home and its folders (Downloads, Pictures… in your language, from
  `user-dirs.dirs`), the file system, your GTK bookmarks (shared with other file managers) and
  the mounted disks. A disk appears as soon as it is mounted and disappears when it is unmounted.
  Ctrl+B hides the sidebar, and F6 moves the keyboard into it and back.
- **Disks:** through udisks2, a disk plugged in appears among the devices, quieter, without
  mounting itself (encrypted ones come later); a click mounts it and opens it, and a disk that
  mounts read-only says why (an NTFS disk not closed properly) and what fixes it. The eject icon of
  an external disk unmounts every partition on it and powers it off, taking the view home first if
  it shows that disk; "in use" says to close what is open there. The right click mounts, unmounts
  and ejects. The list follows disks coming and going; nothing waits on udisks2.
- **Network folders:** `smb://` and `sftp://` bookmarks (the ones Nautilus and other GTK file
  managers keep in the same file) are listed too, quieter until mounted. A click mounts one through
  gvfs on the session bus, with no GLib in Nordend, and opens it through gvfs's FUSE folder. A
  server that wants a name and a password gets them on a card: user, domain, a password drawn as
  dots, "Anonymous" when the server allows it, and "Remember the password" (gvfs keeps it in the
  keyring). A mounted share has an eject icon that unmounts it; the right click mounts, unmounts and
  removes the bookmark. Without gvfs a message says so.
- **Go to (Ctrl+L):** a card with one line for where to go: a folder (`/data`, `~/Bilder`,
  `file://…`) or a server (`smb://server/share`, `sftp://user@host/path`, or a Windows share
  `\\server\share`). A server is added to the sidebar too (unless unticked) and mounted. A wrong
  address says what is wrong and the card stays open. Also in the command palette.
- **Favourites:** Ctrl+D adds the folder shown, "Add to Sidebar" the folders selected (or the
  folder, from its empty part), and folders dragged onto the "BOOKMARKS" title or below the last
  place are added at the end, a line showing where; dropping onto a place still moves into it.
  A right click on a favourite removes it. They are the GTK bookmarks: other file managers and
  the Open dialogs of other apps (Firefox's) show them too. The file is written all at once on a
  worker, keeping what Nordend does not touch (remote bookmarks, labels, order); a folder already
  there is not added twice. Reordering and renaming come later.
- **Breadcrumb:** the folder's path as a row of clickable steps, starting from your home. A long
  path is shortened from the left.
- **Status bar:** item count, selection, free space on the disk (updated after a copy into the
  folder), the three view buttons and "?". Messages appear here for a few seconds.

## Always current

- **Live folders:** files created, removed, changed or renamed by any program show up at once
  (inotify). A thousand-file copy arrives in a few smooth updates, not a thousand.
- **File types exactly as GTK sees them:** the freedesktop shared-mime-info database, read by
  name first and by content only when needed (and only for what is on screen). 0 differences from
  `gio` on about 4 750 files.
- **Live theme:** change the palette in Noctalia and Nordend follows instantly (with the
  `nordend` template enabled in Noctalia); elsewhere, switch
  the desktop between light and dark or pick another accent colour and Nordend follows too.

## Looks

- Its own icon: a folder with the Nordend's snowy summit, in one ice-blue hue
  (`share/icons/hicolor/scalable/apps/nordend.svg`, named after the app id), shown by docks,
  launchers and menus on any desktop.
- **The icon follows the accent:** Nordend writes its icon in the desktop's accent (Noctalia's
  primary colour, else GNOME's or Plasma's accent from the portal) to
  `~/.local/share/icons/hicolor/scalable/apps/nordend.svg`, which docks and launchers read before
  the installed one, and rewrites it when the accent changes, only when it changes. The three
  tones keep the ice icon's lightness and steps in OKLCH, with the accent's hue and chroma (held
  to what the icon can carry); the snow stays white. No accent, or `icon.follow_accent = false`:
  the copy is taken away (installed under `~/.local`, the ice icon is written back). An icon not
  written by Nordend at that path is never touched.
- Noctalia's colors and the same line icons as Noctalia (Tabler), tinted by the theme; camera
  raw files and darktable sidecars have their own icons. The icons are compiled into the
  program (a 14 KB subset of the official Tabler font), so they show on any desktop.
- Sharp at any scale: fractional scaling (1, 1.25, 1.5, 2…), boxes snapped to the pixel grid.
- Text shaped by HarfBuzz, with font fallback for accents, emoji and CJK. Your font family and
  monospace font come from `config.toml`.
- **Its own title bar where the compositor draws none** (GNOME, weston): the title in the middle,
  minimize, maximize (restore when maximized) and close at the right, in the theme's colors;
  drag it to move the window, double-click to maximize, right-click for the compositor's window
  menu; the window's edges and corners resize it, with the matching arrows (not when maximized,
  nor on an edge tiled against the screen). Buttons the compositor cannot honour are left out.
  `NORDEND_FORCE_CSD=1` asks any compositor for it (the compositor may still decorate).
- Pointer shapes from the compositor's cursor theme (cursor-shape-v1): a resize arrow on every
  draggable edge, a hand on what a click acts on (the status bar's buttons, a transfer's ×, the
  path's crumbs, a disk's eject icon, "More details", the buttons of a question), the text cursor
  over a name being edited. Where the compositor has no cursor-shape-v1 (GNOME, weston), Nordend
  draws them itself from the xcursor theme (`XCURSOR_THEME`, `XCURSOR_SIZE`, sharp at any
  scale); `NORDEND_FORCE_XCURSOR=1` does so on any compositor.

## Fast, and measured

| What | Time |
|------|-----:|
| First entries of a 10 000-file folder (warm / cold cache) | 0.8 ms / 1.7 ms |
| The whole folder loaded and sorted (warm / cold) | 19.5 ms / 81 ms |
| A scrolling frame at 2880x1620 (columns / grid / details) | 0.26 / 0.30 / 0.36-0.50 ms |
| Sorting 10 000 entries by size, date or type | 1-3 ms |
| Quick Look of an ORF, fitted to 2880x1620 | 51-55 ms |
| Reading every installed app to choose one | 0.5 ms |
| Copying 342 photos, 3.1 GB, warm cache (Nordend / `cp -a`) | 1.39 s / 1.39 s |
| CPU while nothing moves | 0 |

A 3.5 MB binary (2.9 MB stripped) with 21 direct libraries; 25 MB of its own memory at start,
54 MB after browsing 163 raw photos (143 MB resident with the shared libraries): decoded photos
go back to the system.

## Settings

- `~/.config/nordend/config.toml`: font family, monospace font, double-click time and distance,
  the icon in the accent ([example](config.example.toml)). `nordend --check-config` reports every problem with its
  line.
- No settings window: the defaults are the system's fonts (fontconfig's `sans-serif` and
  `monospace`) and the desktop's colours, and the few settings live in the file.
  `config.toml` is followed while Nordend runs: the double click and the icon change at once, the fonts at
  the next start; a file with problems keeps the settings in use, with the problems in the log.
- **`nordend --about`**: Nordend's version and commit, the distribution, the kernel, the
  desktop, the versions of Noctalia and Umbriel when they are running (asked of the very
  programs running, never of another copy) and the GPU (from a surfaceless OpenGL context), one
  "Label: value" line each, ready to paste into a bug report; and the engines found, with
  their programs.
- Colors come from Noctalia (`~/.config/nordend/colors.toml`, written by Noctalia from the
  `nordend` template, installed at `share/nordend/noctalia/nordend`; the README's "On Noctalia"
  says how to enable it). Without the file, Nordend follows the desktop through the freedesktop portal (GNOME,
  Plasma): light or dark, and the accent colour for the selection, icons and links, with
  neutral surfaces and text kept readable (WCAG AA contrast, tested for 15 accents). Without a
  portal either, a built-in default palette is used.
- **Remembered from one start to the next** (`~/.local/state/nordend/state.toml`, written by
  Nordend): the view, the details' order and column widths, hidden files shown or not, the
  sidebar and the info pane (shown, width, "More details"), the window's size and whether it was
  maximized; with two tabs or more, the tabs (each one's folder, view, cursor and, in the columns,
  where they start) and which was shown, back when Nordend starts without a folder (Super+E;
  folders gone meanwhile are left out). With one tab, not the last folder: Nordend opens Home, or
  the folder given. Not the back/forward history. A broken file only means the defaults.

## Standards it follows

freedesktop thumbnails, shared-mime-info, MIME applications associations, Desktop Entry, XDG
base and user directories, GTK bookmarks; Wayland xdg-shell, xdg-decoration, fractional-scale,
viewporter, cursor-shape and xdg-activation.

## Next

0.5.0 is out (text previews with syntax colours, selecting and copying, logs from their end; the
helper became the first engine), after 0.4.0 (image previews read by Nordend's own code in caged
helpers: DNG, TIFF, raw previews and EXIF without exiv2), 0.3.1 (more raw previews), 0.3.0 (the
command palette, finding files, the icon in the accent), 0.2.0 (Sort By, tabs, zoom in the
lists) and the first release, 0.1.0. Coming, roughly in this order: other common formats, colour
labels and ratings, a Quick Look for photographers, a dual pane. See the README's
[What's next](../README.md#whats-next).

### Expandable folders in details view

In the main window's details view (**Ctrl+3**), click a folder's chevron to show its
contents inline. **Right** expands the selected folder, then moves into its first
child; **Left** collapses it or selects its parent row. **Enter** or a double click
opens the folder as the new location. **Backspace** goes above the tree's root.
Expanded folders load on demand and follow filesystem changes, sorting and hidden-file
settings. Selections (including Shift ranges, Ctrl+A and rubber bands) stay within
one parent folder. Expansion is retained while switching tabs, but resets when
navigating or switching views and is not saved across restarts.

## File picker

An optional XDG FileChooser portal backend for OpenFile, SaveFile and SaveFiles: single or
multiple files and folders, filename entry, glob/MIME filters, application choices, overwrite
confirmation and cancellation. Picker windows reuse the three views and previews without
changing the file manager's saved tabs. See [setup and usage](file-picker.md).
