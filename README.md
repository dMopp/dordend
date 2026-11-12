<p align="center"><img src="data/nordend.svg" width="96" height="96" alt=""></p>

<h1 align="center">Nordend</h1>

<p align="center">
  <b>A native file manager for Wayland: fast, light and safe.</b><br>
  No GTK, no Qt, no GLib: C++20 straight on Wayland and OpenGL ES.<br>
  At home on any Wayland desktop, and right at home on <a href="https://github.com/noctalia-dev">Noctalia</a>.
</p>

<p align="center">
  <a href="https://github.com/pankaspe/nordend-fm/releases/tag/v0.5.0"><img src="https://img.shields.io/badge/release-v0.5.0-2a6a9e" alt="release v0.5.0"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/Wayland-native-FFBC00?logo=wayland&logoColor=black" alt="Wayland native">
  <img src="https://img.shields.io/badge/tests-420%20%C2%B7%20ASan%20%C2%B7%20TSan-4B32C3" alt="420 tests, ASan, TSan">
  <img src="https://img.shields.io/badge/license-MIT-3DA639" alt="MIT license">
</p>

> [!NOTE]
> **Young software, used every day by its author.** 0.5.0 opens **text files** in Quick Look
> with **syntax colours** for over twenty languages (code, configuration, Markdown, diffs), to
> select and copy, logs shown from their newest lines; read, like pictures since 0.4.0, by
> Nordend's own code in a **caged helper process**.
> On Void Linux, [install it as a package](#on-void-linux); anywhere else,
> [build it from source](#from-source) in about a minute.

<p align="center">
  <img src="docs/images/readme-screenshot.webp" alt="Nordend in a light theme: the column view in a folder of photos exported from darktable, the context menu open on one with Open With, its preview and EXIF in the info pane" width="100%">
  <br>
  <sub>The columns in a folder of photos exported from darktable, two tabs open, the context menu and the info pane with the photo's EXIF.</sub>
</p>

## Why Nordend

Nordend talks to the compositor directly, with **no toolkit at all**: text, icons, scrolling and
animations come from its own renderer. It takes your desktop's light or dark style and accent
colour; on Noctalia (Noctalia Shell, the Umbriel compositor) it wears your Noctalia theme, live,
once Noctalia is told to write it ([two steps](#on-noctalia)).

- **Fast.** The first frame in about 65 ms, a 10 000-file folder in 0.7 ms, a raw photo in
  Quick Look in under 100 ms. Every figure is measured, with the tool to measure it again.
- **Light.** A 3 MB binary, 17 libraries, about 25 MB of memory, and no CPU at all while
  nothing moves.
- **Safe with what it reads.** Files that come from outside are read by `nordend-preview`, a
  small helper process in a cage of Nordend's own (seccomp and landlock: no files, no network,
  no programs). A broken or hostile file costs at most its preview; Nordend carries on.
- **Its own readers.** TIFF (LZW, Deflate, PackBits, JPEG, tiles, BigTIFF), PGM and PPM are
  read by code written for Nordend, checked pixel for pixel against libtiff and fuzzed; PNG, JPEG
  and GIF by [wuffs](https://github.com/google/wuffs), memory-safe by construction. Raw files
  show the JPEG their camera stored, so the camera's own rendering; a DNG whose stored previews
  are too small is developed by Nordend's own reader, with the file's own colour.
- **Photos and their editors.** Thumbnails, the info pane and Quick Look with the shot's EXIF
  and rating; darktable sidecars travel with their photo through every rename, copy, move and
  trip to the trash.
- **Three views of the same folder.** Columns, a thumbnail grid and a details table
  (<kbd>Ctrl</kbd>+<kbd>1</kbd> <kbd>2</kbd> <kbd>3</kbd>); switching keeps your place and your
  selection. Sort by name, size, date or type from the context menu, in every view.
- **Tabs.** <kbd>Ctrl</kbd>+<kbd>T</kbd>, or a middle click on a folder to open it behind the
  one you are in; each tab keeps its folder, view and history, and they all come back at the next
  start.
- **Careful with your files.** Nothing overwritten without asking, nothing half-copied under a
  real name, a real freedesktop trash, and <kbd>Ctrl</kbd>+<kbd>Z</kbd>.
- **Keyboard first, mouse always.** Every action has a key (<kbd>F1</kbd> lists them) and a click.

Everything it does is in [docs/features.md](docs/features.md); inside Nordend, <kbd>F1</kbd> lists
every shortcut.

## Features

- **Views:** columns, thumbnail grid, details table; tabs; zoom; sort by name, size, date or type
- **Quick Look** (<kbd>Space</kbd>): photos fill the window, raw files included; text files on a
  scrollable page; the arrows walk the folder
- **Raw files:** the previews cameras embed in ORF, CR2, NEF, ARW, RAF, RW2, SRW, DNG, 3FR
  (found by Nordend's own reader), upright and cropped to the shot; DNGs with only a small
  preview (Leica, drones) developed by Nordend itself; EXIF in the info pane
- **Images:** PNG, JPEG, GIF (wuffs), WebP, JPEG XL, and TIFF, PGM and PPM with Nordend's own
  readers; thumbnails in the shared freedesktop cache
- **Isolated reading:** every picture is made in a caged helper process, never in Nordend
- **darktable:** sidecars follow their photo everywhere; ratings shown
- **Command palette** (<kbd>Ctrl</kbd>+<kbd>K</kbd>): every command, place and tab, fuzzy-matched
  (fzy), the recently used first
- **Find files** (<kbd>Ctrl</kbd>+<kbd>F</kbd>): fuzzy search of every file under the folder,
  first results in milliseconds even among 270 000; Enter shows it, Space opens Quick Look
- **Files:** copy and move with progress and conflict questions, rename, new folder or file
  from templates, undo and redo, freedesktop trash with restore, delete permanently
- **Drag and drop** within Nordend and with other apps; rubber-band selection; the system
  clipboard
- **Sidebar:** places, GTK bookmarks (also `smb://` and `sftp://`, mounted through gvfs by the
  `remotes` plugin), disks through udisks2 (mount, unmount, eject)
- **Plugins:** features outside the core, compiled in and switched off per build or in
  `config.toml` ([docs/plugins.md](docs/plugins.md))
- **Opening:** default applications and "Open With", resolved exactly as GLib does; a terminal
  in any folder (<kbd>F4</kbd>)
- **Always current:** folders follow changes on disk (inotify); history with
  <kbd>Alt</kbd>+<kbd>←</kbd> <kbd>→</kbd> and the mouse's side buttons
- **Looks:** Noctalia's theme live (through [a template](#on-noctalia)), or the desktop's
  light/dark style and accent (GNOME, Plasma);
  its icon follows the accent; fractional scaling; its own title bar where needed
- **Remembers** views, order, tabs and window size; a short `config.toml`, reloaded live

## At a glance

| | |
|---|---|
| Binary | **3.95 MB** (3.28 MB stripped), x86-64; the decoders and text readers in `nordend-preview`, 1.13 MB (1.01 stripped), a separate caged helper |
| Libraries | 17 direct (the helper: 8); no GTK, no Qt, no GLib |
| Code | about 51 300 lines of C++20, 17 400 of tests (420 test cases, under ASan and TSan too) |
| Memory | **25 MB** of its own at start; pictures are decoded in the helpers, which give their memory back after a quiet second |
| Idle | **0 CPU**: nothing is drawn while nothing moves |

## Benchmarks

The design's four targets, measured on 2026-11-01 (Ryzen 7 5700G, RX 6650 XT, SATA SSD):

| Target | Measured |
|--------|---------:|
| Cold start to the first frame on screen: < 150 ms | **128 ms** (warm: **64 ms**) |
| A 10 000-file folder: first frame < 50 ms | first entries in **0.7 ms**, all in **17 ms** |
| Quick Look of an ORF raw photo: < 100 ms | **82-96 ms** |
| Scrolling with no dropped frame | worst frame **3.7 ms** of 13.3 (75 Hz) |

Every measurement and its method is in [docs/benchmarks.md](docs/benchmarks.md).

## Install

Nordend needs Linux with a Wayland compositor that offers xdg-shell (developed on Umbriel, also
tried on GNOME, Plasma and niri) and a GPU driver with OpenGL ES 3.0 (Mesa).

### On Void Linux

Nordend is developed on Void, and comes with an xbps-src template in
[`packaging/void`](packaging/void). It builds the latest release into a package that puts
`nordend`, its desktop entry and its icon under `/usr`:

```sh
# 1. void-packages, once (skip if you already have a checkout)
git clone --depth 1 https://github.com/void-linux/void-packages
cd void-packages
./xbps-src binary-bootstrap

# 2. Nordend's template, then the package
git clone --depth 1 https://github.com/pankaspe/nordend-fm /tmp/nordend-fm
cp -r /tmp/nordend-fm/packaging/void/nordend srcpkgs/
./xbps-src pkg nordend
sudo xbps-install -R hostdir/binpkgs nordend
```

The package is the latest release, **0.5.0**. What `main` gains after it comes with the next
release; to have it at once, build from source.

### With Nix

From a checkout, with Nix flakes enabled:

```sh
nix build                 # package in ./result, including the preview helper and desktop entry
nix run . -- ~/Pictures    # run Nordend in your Wayland session
nix flake check           # check the installed package and command-line entry points
```

The flake provides packages and development shells for `x86_64-linux` and `aarch64-linux`.
For development, enter the shell and use the usual CMake workflow:

```sh
nix develop
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/nordend
```

The shell includes the build dependencies, doctest, clang tools, GDB and the optional JPEG
benchmark dependency. Package builds omit unit tests; development-shell tests use the host
environment. On non-NixOS systems, running the GUI also requires configuring Nix's access to
the host's graphics drivers.

### From source

On Void Linux the packages are these; elsewhere, the development packages of the same libraries
(their names differ and have not been tried yet):

```sh
sudo xbps-install -S cmake ninja pkgconf wayland-devel wayland-protocols \
    libxkbcommon-devel libglvnd-devel MesaLib-devel \
    harfbuzz-devel freetype-devel fontconfig-devel tomlplusplus-devel \
    libwebp-devel libjxl-devel zlib-devel sdbus-c++-devel
git clone https://github.com/pankaspe/nordend-fm
cd nordend-fm
cmake -B build -G Ninja
cmake --build build
./build/nordend            # your home folder, or ./build/nordend ~/Pictures
cmake --install build --prefix ~/.local --strip   # nordend, its desktop entry and icon
```

### Then

Make it the default for folders (a browser's "Show in folder", `xdg-open`) and give it a key:

```sh
xdg-mime default nordend.desktop inode/directory
```

```toml
# Umbriel's config.toml; other compositors have their own way
"Mod+E" = { action = "spawn nordend", repeat = false }
```

### As a file picker

Nordend can handle Open/Save dialogs through the XDG FileChooser portal, alongside Umbriel's
screenshot and screen-sharing backend. On NixOS, import this flake's module and enable both:

```nix
imports = [ inputs.nordend.nixosModules.default ];
programs.nordend.enable = true;
programs.nordend.filePicker.enable = true;
```

The picker defaults to Umbriel; keep your existing portal backends and other preferences.
See [file-picker setup](docs/file-picker.md) for the flake input, other desktops and manual
installation, or try `nordend --pick ~/Pictures` without changing desktop defaults.

### On Noctalia

Nordend takes its colours from `~/.config/nordend/colors.toml`, and Noctalia writes that file
through a template, on every theme change. Noctalia does not have the template yet, so tell it
once:

```sh
mkdir -p ~/.config/noctalia/templates ~/.config/nordend
cp /usr/share/nordend/noctalia/nordend ~/.config/noctalia/templates/
# installed in ~/.local or not at all: ~/.local/share/nordend/noctalia/nordend, or
# data/noctalia/nordend in this repository
```

```toml
# ~/.config/noctalia/config.toml
[theme.templates.user.nordend]
input_path  = "templates/nordend"
output_path = "~/.config/nordend/colors.toml"
```

Then pick a theme in Noctalia (or the same one again): Nordend changes with it, live. Without
`colors.toml`, Nordend follows the desktop's light or dark style and accent instead, where the
desktop's portal offers them (GNOME, Plasma), else its own default palette.

Configuration: every key, with its default, in [docs/config.example.toml](docs/config.example.toml)
(copy it to `~/.config/nordend/config.toml`; `nordend --check-config` reports any problem). The
command line: `nordend [DIR]`, `--config FILE`, `--version`, and `--about` (what Nordend runs on,
for a bug report).

For photo folders, switch to the grid with **Ctrl+2** and enlarge thumbnails with
**Ctrl+mouse wheel** or **Ctrl++**; **Ctrl+-** shrinks them and **Ctrl+0** restores the usual size.
The size is remembered per tab. The left sidebar zooms independently: use **Ctrl+mouse wheel**
over it, or press **F6** to focus it and use **Ctrl++**, **Ctrl+-**, or **Ctrl+0**. Its size is
remembered across restarts. **Ctrl+I** hides the information pane to give large thumbnails
more room. Grid thumbnails range from 48 to 512 px and fit the available window width.

For a transparent background with solid text, icons and thumbnails, add to Nordend's config:

```toml
[appearance]
background_opacity = 0.70
```

Transparency is disabled by default (`background_opacity = 1.0`). This reloads live; `1.0` is fully opaque and `0.0` fully transparent. Keep the compositor's
**window opacity at `1.0`** (or remove its opacity rule), since that fades the entire window.
Compositor blur can remain enabled; Nordend supplies transparency but does not apply blur itself.

Nordend uses the desktop icon theme by default. To select Candy explicitly, put this in
`~/.config/nordend/config.toml` (or add the key to an existing `[icon]` section):

```toml
[icon]
theme = "candy-icons"
```

Use `theme = "system"` for desktop settings or `theme = "builtin"` for the original line icons.
Theme changes in Nordend's config apply immediately; desktop theme changes are picked up at
startup. SVG theme icons are rendered in-process using FreeType.

On first normal launch, Nordend creates a complete `config.toml` at
`$XDG_CONFIG_HOME/nordend/config.toml` (or `~/.config/nordend/config.toml`). It includes
all settings and a commented list of default shortcuts. Existing files, including
symlinks, are never replaced. `--check-config`, `--test-scene`, and explicit `--config`
paths do not create a config. If creation fails, Nordend reports it and continues.
The same reference is installed at `share/nordend/config.example.toml` under the
installation prefix; upgrades update that reference without changing your settings.

Keyboard commands can be overridden in the same file and reload live:

```toml
[keybindings]
rename = "Ctrl+r"
select_all = "Ctrl+g"
toggle_selection = "Ctrl+j"
```

Each entry replaces that action's default keys; `""` disables it. F1 shows the current
bindings. `nordend --check-config` reports invalid or conflicting bindings. See
[the example config](docs/config.example.toml) for action names and key syntax.
Text-editing, navigation and range-selection keys retain their standard behavior.

## Versions

Nordend stays at 0.x while it is young. A new **minor** version (0.2.0, 0.3.0…) brings features
you can see; a **patch** (0.2.1) only fixes things. 1.0.0 will come once Nordend has been its
author's everyday file manager for a while and its files (`config.toml`, `state.toml`) have
settled.

| Version | What it brought |
|---|---|
| on `main`, next | — |
| [0.5.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.5.0), 2026-11-09 | Text previews: text files in Quick Look and the info pane, read in the caged helper (UTF-8, UTF-16, Latin-1) with syntax colours from your theme for C, C++, Rust, Python, JavaScript, TypeScript, Lua, Luau, shell (heredocs included), fish, CMake, Makefiles, JSON, TOML, YAML, INI files and desktop entries, `.gitignore` and `.gitattributes`, Markdown (code blocks in their own language) and diffs; select with the pointer, Ctrl+A, Ctrl+C; long lines scroll sideways; Home, End, Page Up and Page Down; a caption with the language, encoding and line ends; minified files shown plain; logs opened at their newest lines. The helper became the first engine, described by a manifest |
| [0.4.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.4.0), 2026-11-08 | Image previews read by Nordend's own code: DNG developed when its previews are too small (Leica M240 and M8, DJI drones, Adobe-converted files) with the file's colour, tone curve, vignetting and lens corrections, read identically to LibRaw and 2-4 times faster; TIFF with its own reader (BigTIFF, LZW, Deflate, PackBits, JPEG, 8/16-bit and float, CMYK, tiles, a scanner's scan in a SubIFD); raw files' previews and the info pane's EXIF without exiv2 (the same results, 3 to 100 times quicker); pictures made in small `nordend-preview` processes, each in a cage of Nordend's own (seccomp and landlock), so a file that crashes or hangs one costs only its preview; one path for every picture, told by the file's bytes; a plain sentence when a file cannot be shown. Canon CRW and Minolta MRW get that sentence; darktable sidecars' ratings are no longer read (Nordend's own ratings come next) |
| [0.3.1](https://github.com/pankaspe/nordend-fm/releases/tag/v0.3.1), 2026-11-07 | Fixes: DNG previews stored as a JPEG inside a TIFF (Leica M240, Samsung GX20 at full size), broken previews passed over for the next one (Samsung SRW), PGM and PPM images, a plain message for raw formats with no readable preview (Sigma X3F, old Panasonic RAW) |
| [0.3.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.3.0), 2026-11-06 | The command palette (<kbd>Ctrl</kbd>+<kbd>K</kbd>, fuzzy matching with fzy); finding files under a folder (<kbd>Ctrl</kbd>+<kbd>F</kbd>); Nordend's icon follows the desktop's accent colour (`icon.follow_accent`) |
| [0.2.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.2.0), 2026-11-06 | Sort By in the context menu; tabs (a folder, a view, a history and a zoom each, remembered); zoom in the columns and the details |
| [0.1.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.1.0), 2026-11-06 | The first release: three views, raw previews and Quick Look, file operations with undo, trash, drag and drop, disks, the desktop's look |

## What's next

Nordend stays what it is: a native C++ file manager, made for Noctalia Shell, with no GTK or Qt;
very light, safe and fast. Everything else is a feature on top, added only when it keeps those
three.

0.1.0 came out after a week of daily use by the author; 0.2.0 added Sort By, tabs and zoom in the
lists; 0.3.0 the command palette (<kbd>Ctrl</kbd>+<kbd>K</kbd>) and finding files
(<kbd>Ctrl</kbd>+<kbd>F</kbd>); 0.3.1 more raw previews; 0.4.0 image previews read by Nordend's own
code in caged helpers; 0.5.0 text previews with syntax colours, selecting and copying. Coming,
roughly in this order: **previews, one family at a time**: other common formats one by one (SVG,
AVIF, fonts, archive contents, audio tags).
After that: colour labels and star ratings of your own, not tied to any raw editor; a Quick Look
for photographers (100 % zoom, EXIF, histogram, comparison); a dual pane, a queue of operations
and batch renaming; translations, plugins in Luau (one
system, each plugin caged; heavy formats such as PDF and video, and local AI search, as separate
engines).

## Documentation

[Features](docs/features.md) ·
[Devlog](docs/devlog.md) ·
[Benchmarks](docs/benchmarks.md) ·
[Compatibility](docs/compatibility.md) ·
[Upstream bug log](docs/buglog.md)

## Development

Nordend is copiloted by **Claude Opus** (Anthropic), which assists with code writing, code analysis,
bug hunting, testing and documentation. Design decisions, testing on real hardware and every release
are the author's. Step numbers in code comments ("ROADMAP 6.3") refer to the internal roadmap.

## License

[MIT](LICENSE). Third-party code and data (the wuffs decoder, the Tabler icons, Noctalia's default
palette) are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
