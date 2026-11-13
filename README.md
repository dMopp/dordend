<p align="center"><img src="data/rummage.svg" width="96" height="96" alt=""></p>

<h1 align="center">rummage</h1>

rummage is a fork of [Nordend](https://github.com/pankaspe/nordend-fm), created by
[pankaspe](https://github.com/pankaspe). It builds on Nordend's native Wayland file manager
and retains the upstream MIT license and copyright notice in [LICENSE](LICENSE).
Bundled dependencies and their licenses are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
The repository is [barrulus/rummage](https://github.com/barrulus/rummage).


<p align="center">
  <b>A native file manager for Wayland: fast, light and safe.</b><br>
  No GTK, no Qt, no GLib: C++20 straight on Wayland and OpenGL ES.<br>
  At home on any Wayland desktop, and right at home on <a href="https://github.com/noctalia-dev">Noctalia</a>.
</p>

<p align="center">
  <a href="https://github.com/barrulus/rummage/releases/tag/development"><img src="https://img.shields.io/badge/build-development-2a6a9e" alt="development build"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/Wayland-native-FFBC00?logo=wayland&logoColor=black" alt="Wayland native">
  <img src="https://img.shields.io/badge/tests-420%20%C2%B7%20ASan%20%C2%B7%20TSan-4B32C3" alt="420 tests, ASan, TSan">
  <img src="https://img.shields.io/badge/license-MIT-3DA639" alt="MIT license">
</p>

> [!NOTE]
> **Young software, used every day by its author.** 0.5.0 opens **text files** in Quick Look
> with **syntax colours** for over twenty languages (code, configuration, Markdown, diffs), to
> select and copy, logs shown from their newest lines; read, like pictures since 0.4.0, by
> rummage's own code in a **caged helper process**.
> On Void Linux, [install it as a package](#on-void-linux); anywhere else,
> [build it from source](#from-source) in about a minute.

<p align="center">
  <img src="docs/images/readme-screenshot.webp" alt="rummage in a light theme: the column view in a folder of photos exported from darktable, the context menu open on one with Open With, its preview and EXIF in the info pane" width="100%">
  <br>
  <sub>The columns in a folder of photos exported from darktable, two tabs open, the context menu and the info pane with the photo's EXIF.</sub>
</p>

## Why rummage

rummage talks to the compositor directly, with **no toolkit at all**: text, icons, scrolling and
animations come from its own renderer. It takes your desktop's light or dark style and accent
colour; on Noctalia (Noctalia Shell, the Umbriel compositor) it wears your Noctalia theme, live,
once Noctalia is told to write it ([two steps](#on-noctalia)).

- **Fast.** The first frame in about 65 ms, a 10 000-file folder in 0.7 ms, a raw photo in
  Quick Look in under 100 ms. Every figure is measured, with the tool to measure it again.
- **Light.** A 3 MB binary, 17 libraries, about 25 MB of memory, and no CPU at all while
  nothing moves.
- **Safe with what it reads.** Files that come from outside are read by `rummage-preview`, a
  small helper process in a cage of rummage's own (seccomp and landlock: no files, no network,
  no programs). A broken or hostile file costs at most its preview; rummage carries on.
- **Its own readers.** TIFF (LZW, Deflate, PackBits, JPEG, tiles, BigTIFF), PGM and PPM are
  read by code written for rummage, checked pixel for pixel against libtiff and fuzzed; PNG, JPEG
  and GIF by [wuffs](https://github.com/google/wuffs), memory-safe by construction. Raw files
  show the JPEG their camera stored, so the camera's own rendering; a DNG whose stored previews
  are too small is developed by rummage's own reader, with the file's own colour.
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

Everything it does is in [docs/features.md](docs/features.md); inside rummage, <kbd>F1</kbd> lists
every shortcut.

## Features

- **Views:** columns, thumbnail grid, details table; tabs; zoom; sort by name, size, date or type
- **Quick Look** (<kbd>Space</kbd>): photos fill the window, raw files included; text files on a
  scrollable page; the arrows walk the folder
- **Raw files:** the previews cameras embed in ORF, CR2, NEF, ARW, RAF, RW2, SRW, DNG, 3FR
  (found by rummage's own reader), upright and cropped to the shot; DNGs with only a small
  preview (Leica, drones) developed by rummage itself; EXIF in the info pane
- **Images:** PNG, JPEG, GIF (wuffs), WebP, JPEG XL, and TIFF, PGM and PPM with rummage's own
  readers; thumbnails in the shared freedesktop cache
- **Isolated reading:** every picture is made in a caged helper process, never in rummage
- **darktable:** sidecars follow their photo everywhere; ratings shown
- **Command palette** (<kbd>Ctrl</kbd>+<kbd>K</kbd>): every command, place and tab, fuzzy-matched
  (fzy), the recently used first
- **Find files** (<kbd>Ctrl</kbd>+<kbd>F</kbd>): fuzzy search of every file under the folder,
  first results in milliseconds even among 270 000; Enter shows it, Space opens Quick Look
- **Files:** copy and move with progress and conflict questions, rename, new folder or file
  from templates, undo and redo, freedesktop trash with restore, delete permanently
- **Properties** (Alt+Enter or right-click): file and folder details, with a searchable folder
  icon picker. Choices are shared with Dolphin through `.directory` files.
- **Drag and drop** within rummage and with other apps; rubber-band selection; the system
  clipboard
- **Sidebar:** places, GTK bookmarks (also `smb://` and `sftp://`, mounted through gvfs), disks
  through udisks2 (mount, unmount, eject)
- **Actions:** Copy Location, archive extraction through File Roller, 7z, unzip, gzip or tar, and configurable commands
  in the context menu, command palette and shortcuts ([examples](docs/config.example.toml))
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
| Binary | **3.95 MB** (3.28 MB stripped), x86-64; the decoders and text readers in `rummage-preview`, 1.13 MB (1.01 stripped), a separate caged helper |
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

## Upgrading from Nordend

The command, desktop entry, portal backend, packages, Nix module, and environment variables
now use `rummage` / `RUMMAGE` (including `rummage-media` and `programs.rummage`).
Update launcher commands, `xdg-mime` defaults, compositor rules, portal preferences, and
Noctalia templates accordingly; the old executable and portal names are not installed as aliases.

On the first normal launch without `--config`, rummage copies existing Nordend `config.toml`,
`colors.toml`, and session `state.toml` into the corresponding `rummage` XDG directories,
only where the destination is absent. Originals and existing rummage files are left intact.
Custom commands and paths inside copied settings are preserved; update any that invoke Nordend.
Explicit `--config`, config checks, and picker/portal-only launches do not migrate files.
Screenshots and historical development notes may still show the upstream name; a separate
documentation overhaul will follow the rename.


## Install

rummage needs Linux with a Wayland compositor that offers xdg-shell (developed on Umbriel, also
tried on GNOME, Plasma and niri) and a GPU driver with OpenGL ES 3.0 (Mesa).

### Development downloads

<!-- development-downloads:start -->
The [latest development build](https://github.com/barrulus/rummage/releases/tag/development)
is refreshed automatically after **all checks pass on `main`**. These are unsigned Linux
x86_64 development snapshots. Choose your exact distribution; the Fedora RPM is **not
validated for Leap or RHEL**, and the DEB is not validated for Ubuntu or older Debian.

| System | Download | Install after downloading |
|---|---|---|
| Debian 13 (trixie), amd64 | [DEB](https://github.com/barrulus/rummage/releases/download/development/rummage-debian13-amd64.deb) | `sudo apt install ./rummage-debian13-amd64.deb` |
| Fedora 44, x86_64 | [RPM](https://github.com/barrulus/rummage/releases/download/development/rummage-fedora44-x86_64.rpm) | `sudo dnf install ./rummage-fedora44-x86_64.rpm` |
| Arch Linux, x86_64 | [Arch package](https://github.com/barrulus/rummage/releases/download/development/rummage-arch-x86_64.pkg.tar.zst) | `sudo pacman -Syu`, then `sudo pacman -U ./rummage-arch-x86_64.pkg.tar.zst` |
| openSUSE Tumbleweed, x86_64 | [Tumbleweed RPM](https://github.com/barrulus/rummage/releases/download/development/rummage-tumbleweed-x86_64.rpm) | `sudo zypper dup`, then `sudo zypper install --allow-unsigned-rpm ./rummage-tumbleweed-x86_64.rpm` |
| Void glibc, x86_64 | [XBPS bundle](https://github.com/barrulus/rummage/releases/download/development/rummage-void-glibc-x86_64.tar.gz) | See the two commands below. |

For Void, extract the bundle and keep its package and repository index together:

```sh
tar -xzf rummage-void-glibc-x86_64.tar.gz
sudo xbps-install -R "$PWD/rummage-void-x86_64/packages" rummage
```

For Nix, with flakes enabled, run or install the same tested source revision through the
moving `development` tag (this builds rummage if no cached binary is available):

```sh
nix run github:barrulus/rummage/development
nix profile install github:barrulus/rummage/development
```

[Checksums](https://github.com/barrulus/rummage/releases/download/development/SHA256SUMS)
and [source identity](https://github.com/barrulus/rummage/releases/download/development/source.json)
are provided with every build. To verify a download, save `SHA256SUMS` beside it and run
`sha256sum --check --ignore-missing SHA256SUMS`. Downloads are replaced as builds pass;
use the full commit from `source.json` instead of `development` in the Nix reference to
pin a version. These downloads are individual packages, not an automatic-update repository.

For a **Debian source package**, download the
[Debian source bundle](https://github.com/barrulus/rummage/releases/download/development/rummage-debian-source.tar.gz).
It contains the matching `.dsc`, `.orig.tar.gz`, `.debian.tar.xz`, source `.changes` and
`.buildinfo`, with their versioned filenames preserved. CI builds the published DEB from
this source package. Rebuild on Debian 13:

```sh
sudo apt update
sudo apt install build-essential
tar -xzf rummage-debian-source.tar.gz
cd rummage-debian-source
dpkg-source -x rummage_*.dsc source
sudo apt build-dep ./source
cd source
dpkg-buildpackage -b -us -uc
```

These source packages are unsigned development snapshots. The
[generic source archive](https://github.com/barrulus/rummage/releases/download/development/rummage-source.tar.gz)
is also available; it is distinct from the Debian source package.
<!-- development-downloads:end -->

### On Void Linux

Use the [development XBPS bundle](#development-downloads) or build a package from this
fork's exact checkout with the [CI package builder](docs/ci-packaging.md#void-linux).
The template in [`packaging/void/rummage`](packaging/void/rummage) is populated by that
builder with a verified source archive and checksum; it does not fetch an upstream Nordend release.

### Optional video previews

Install the matching `rummage-media` package for video thumbnails and still previews in the
information pane and Space Quick Look. The grid draws cinema filmstrip borders; cached images
and larger previews stay undecorated. This is still-frame preview, not video playback.

The helper uses system FFmpeg 6.1 or newer shared libraries (`libavformat`, `libavcodec`,
`libavutil`, `libswscale`); it does not execute `ffmpeg`, `ffprobe`, or `ffmpegthumbnailer`.
Your package manager reuses compatible installed libraries. Some distributions package these
libraries together with the FFmpeg commands. Codec availability follows the distribution build.

The main package has no FFmpeg dependency. Without the media helper, videos retain their file
icons and information cards; existing valid cached thumbnails remain usable. Enter opens the
selected video in its default application. Missing helpers/libraries are retried after restarting
rummage and do not create permanent failed-thumbnail records.

For a source build, add `-DRUMMAGE_MEDIA=ON` with the four libraries' development packages
installed. It defaults to OFF. `cmake --install build --component media` installs only the helper
and its manifest; `--component Unspecified` installs the core application. The helper uses the
existing seccomp/Landlock sandbox, reads only the supplied descriptor, and has bounded decoding,
output dimensions, concurrency and time. Thumbnail selection tries beyond dark opening frames.

For Nix, `nix build .#rummage-with-media` builds the application configured with the optional
helper; `.#rummage-media` selects its separate helper output. The normal default remains free of
FFmpeg. Separate custom prefixes can be registered through `RUMMAGE_ENGINE_PATH`, a colon-separated
list of absolute `share/rummage/engines` directories. Restart after changing the installed engines.

### With Nix

From a checkout, with Nix flakes enabled:

```sh
nix build                 # package in ./result, including the preview helper and desktop entry
nix run . -- ~/Pictures    # run rummage in your Wayland session
nix flake check           # check the installed package and command-line entry points
```

The flake provides packages and development shells for `x86_64-linux` and `aarch64-linux`.
For development, enter the shell and use the usual CMake workflow:

```sh
nix develop
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/rummage
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
git clone https://github.com/barrulus/rummage rummage
cd rummage
cmake -B build -G Ninja
cmake --build build
./build/rummage            # your home folder, or ./build/rummage ~/Pictures
cmake --install build --prefix ~/.local --strip   # rummage, its desktop entry and icon
```

### Then

Make it the default for folders (a browser's "Show in folder", `xdg-open`) and give it a key:

```sh
xdg-mime default rummage.desktop inode/directory
```

```toml
# Umbriel's config.toml; other compositors have their own way
"Mod+E" = { action = "spawn rummage", repeat = false }
```

### As a file picker

rummage can handle Open/Save dialogs through the XDG FileChooser portal, alongside Umbriel's
screenshot and screen-sharing backend. On NixOS, import this flake's module and enable both:

```nix
imports = [ inputs.rummage.nixosModules.default ];
programs.rummage.enable = true;
programs.rummage.filePicker.enable = true;
```

The picker defaults to Umbriel; keep your existing portal backends and other preferences.
See [file-picker setup](docs/file-picker.md) for the flake input, other desktops and manual
installation, or try `rummage --pick ~/Pictures` without changing desktop defaults.

### On Noctalia

rummage takes its colours from `~/.config/rummage/colors.toml`, and Noctalia writes that file
through a template, on every theme change. Noctalia does not have the template yet, so tell it
once:

```sh
mkdir -p ~/.config/noctalia/templates ~/.config/rummage
cp /usr/share/rummage/noctalia/rummage ~/.config/noctalia/templates/
# installed in ~/.local or not at all: ~/.local/share/rummage/noctalia/rummage, or
# data/noctalia/rummage in this repository
```

```toml
# ~/.config/noctalia/config.toml
[theme.templates.user.rummage]
input_path  = "templates/rummage"
output_path = "~/.config/rummage/colors.toml"
```

Then pick a theme in Noctalia (or the same one again): rummage changes with it, live. Without
`colors.toml`, rummage follows the desktop's light or dark style and accent instead, where the
desktop's portal offers them (GNOME, Plasma), else its own default palette.

Configuration: every key, with its default, in [docs/config.example.toml](docs/config.example.toml)
(copy it to `~/.config/rummage/config.toml`; `rummage --check-config` reports any problem). The
command line: `rummage [DIR]`, `--config FILE`, `--version`, and `--about` (what rummage runs on,
for a bug report).

To enlarge the status bar text, add `[status_bar]` with `text_size = 15` to `config.toml`.
Sizes from 8 to 32 logical pixels apply live, independently of list/sidebar zoom; the bar grows
to fit larger text.

For photo folders, switch to the grid with **Ctrl+2** and enlarge thumbnails with
**Ctrl+mouse wheel** or **Ctrl++**; **Ctrl+-** shrinks them and **Ctrl+0** restores the usual size.
The size is remembered per tab. The left sidebar zooms independently: use **Ctrl+mouse wheel**
over it, or press **F6** to focus it and use **Ctrl++**, **Ctrl+-**, or **Ctrl+0**. Its size is
remembered across restarts. **Ctrl+I** hides the information pane to give large thumbnails
more room. Grid thumbnails range from 48 to 512 px and fit the available window width.

Use **F3** for two persistent panes, each with its own tabs. **Tab** switches the active pane;
**Ctrl+F5** copies its selected files to the other pane and **Ctrl+F6** moves them. The pane
headers show focus, and the command palette's transfer commands name the destination. Drag the
divider to resize (double-click to equalize), or use **Top / bottom** and **Swap panes** above the
views. F3 shows the active pane full-width without discarding the other pane's tabs. Both tab
sets and the layout return when rummage starts without an explicit folder. See
[the pane workflow](docs/features.md#two-persistent-browsing-panes) for details.

For a transparent background with solid text, icons and thumbnails, add to rummage's config:

```toml
[appearance]
background_opacity = 0.70
```

Transparency is disabled by default (`background_opacity = 1.0`). This reloads live; `1.0` is fully opaque and `0.0` fully transparent. Keep the compositor's
**window opacity at `1.0`** (or remove its opacity rule), since that fades the entire window.
Compositor blur can remain enabled; rummage supplies transparency but does not apply blur itself.

rummage uses the desktop icon theme by default. To select Candy explicitly, put this in
`~/.config/rummage/config.toml` (or add the key to an existing `[icon]` section):

```toml
[icon]
theme = "candy-icons"
```

Use `theme = "system"` for desktop settings or `theme = "builtin"` for the original line icons.

Choose individual Places icons in the same config; changes reload live:

```toml
[icon.places]
home = "builtin:home"
documents = "folder-documents"
pictures = "/home/you/.local/share/icons/my-pictures.svg"
```

Values accept names from the selected icon theme, `builtin:<name>` for bundled icons, or absolute
SVG/PNG paths. Missing icons fall back to the defaults. All place keys and examples are in
[config.example.toml](docs/config.example.toml).
Theme changes in rummage's config apply immediately; desktop theme changes are picked up at
startup. SVG theme icons are rendered in-process using FreeType.

On first normal launch, rummage creates a complete `config.toml` at
`$XDG_CONFIG_HOME/rummage/config.toml` (or `~/.config/rummage/config.toml`). It includes
all settings, commented default shortcuts and context-menu layouts, and custom-action
examples. Existing files, including
symlinks, are never replaced. `--check-config`, `--test-scene`, and explicit `--config`
paths do not create a config. If creation fails, rummage reports it and continues.
The same reference is installed at `share/rummage/config.example.toml` under the
installation prefix; upgrades update that reference without changing your settings.

Keyboard commands can be overridden in the same file and reload live:

```toml
[keybindings]
rename = "Ctrl+r"
select_all = "Ctrl+g"
toggle_selection = "Ctrl+j"
```

Each entry replaces that action's default keys; `""` disables it. F1 shows the current
bindings. `rummage --check-config` reports invalid or conflicting bindings. See
[the example config](docs/config.example.toml) for action names and key syntax.
Text-editing, navigation and range-selection keys retain their standard behavior.

### Custom actions and context menus

Add `[[actions]]` tables to `~/.config/rummage/config.toml`. Actions appear in the context
menu and command palette (**Ctrl+K**) when their selection and MIME rules match. Programs
are optional dependencies: install the ones your actions use. Missing programs are dimmed
in the context menu and omitted from the palette.

This example compresses one or more selected files or folders. rummage asks for the ZIP
filename and destination, calls **7-Zip directly**, then notifies you when the command exits:

```toml
[[actions]]
id = "compress"
label = "Compress to ZIP…"
argv = ["7z", "a", "-tzip", "-spd", "{destination}", "--", "{files}"]
selection = "many"
placement = "menu"
destination = "file"
destination_name = "Archive.zip"
notify_on_completion = true
```

Use `7zz` instead of `7z` if that is your installed executable. `-spd` treats wildcard
characters in selected filenames literally. `7z a` updates an existing archive; choose a
new filename to create a separate archive. No Python or shell wrapper is needed.

The same picker works with other commands. These examples require **ImageMagick** and
**GNU cp**, respectively:

```toml
[[actions]]
id = "resize-image"
label = "Resize Image to Half Size…"
argv = ["magick", "{file}", "-resize", "50%", "{destination}"]
mime = ["image/*"]
selection = "one"
destination = "file"
destination_name = "resized.png"
notify_on_completion = true

[[actions]]
id = "copy-to"
label = "Copy To…"
argv = ["cp", "--archive", "--", "{files}", "{destination}"]
selection = "many"
destination = "directory"
notify_on_completion = true
```

`destination = "file"` opens rummage's save dialog; `"directory"` selects an existing
folder. `destination_name` is an optional literal suggested filename for the file picker.
The picker starts in the current folder, uses the action's label as its title, and passes
the selected absolute path as `{destination}`. Cancelling launches nothing. The selection
and working directory are captured before the picker opens. rummage does not create the
output: the command does, with its own overwrite behavior. The save dialog confirms an
existing file; commands such as `cp` and `magick` can replace output. External operations
are not part of rummage's Undo history.

Omit `destination` and `{destination}` when the application already supplies its own dialog;
for example, File Roller can use
`argv = ["file-roller", "--default-dir", "{directory}", "--add", "{files}"]`.
A destination picker and `{destination}` must be configured together.

`argv` is an argument array, with no shell interpretation, globbing, or environment/tilde
expansion. Placeholders must occupy whole arguments after the executable; do not add shell
quotes around them. Filenames containing spaces remain single arguments.

| Placeholder | Value |
| --- | --- |
| `{file}` | One absolute selected path; requires `selection = "one"` |
| `{files}` | Each selected absolute path as a separate argument; requires `"one"` or `"many"` |
| `{directory}` | The current folder, also used as the command's working directory |
| `{destination}` | The absolute path chosen by rummage's destination picker |

`selection` defaults to `"one"` (exactly one item). `"many"` allows one or more items;
`"background"` requires none; `"any"` allows any number, including none. Use `{directory}`
or fixed arguments with `"background"`/`"any"`, not `{file}` or `{files}`. Without `mime`,
all file types are allowed, but multi-selection still requires `selection = "many"`.
MIME filters accept exact types and patterns such as `"image/*"`; **every selected item**
must match at least one filter. Folders use `"inode/directory"`.

`notify_on_completion = true` enables a desktop notification and status message on success
or failure; it defaults to false. rummage sends notifications itself, without `notify-send`.
The option applies to external actions: exit code 0 means success, other exit codes or
signals mean failure. Cancelling the picker or failing to launch sends no completion notice;
launch errors still appear in the status bar. Keep rummage open to receive notifications.
A desktop notification service is needed for popups; the status message still works without
one. Completion follows the launched process, so a GUI launcher that delegates work and
exits early cannot report when that delegated work finishes.

#### Arrange and nest menus

Actions default to the **Actions** submenu. `placement = "menu"` puts an action directly
above that submenu. A `[context_menu]` layout can instead arrange built-in commands and
custom actions into nested groups, in the order you choose. For the three actions above:

```toml
[context_menu]
items = [
  { id = "open" },
  { id = "open_with" },
  { id = "action:compress" },
  { label = "My Actions", items = [
    { label = "Images", items = [ { id = "action:resize-image" } ] },
    { label = "Files", items = [ { id = "action:copy-to" } ] },
  ] },
  { separator = true },
  { remaining = true },
]
```

Use `action:<id>` for actions and built-in IDs such as `copy`, `rename`, `sort`, and
`open_with` for other commands. A group has `label` and nested `items`; groups can nest
further. Add `label` to an ID reference to rename its menu entry, or `separator = true`
to place a line above it. Inapplicable items and empty groups disappear.

`{ remaining = true }` keeps unlisted items in their default grouping. Use it at most once
per layout, at any depth; omit it to hide unlisted items. Referencing `open_with`, `new_file`,
`sort`, or `actions` keeps the parent's remaining children, including dynamically discovered
applications/templates. Moving a child elsewhere removes it from the original group. Each
ID can appear only once per layout. Explicit layout references override action placement;
menu customization does not change command-palette or shortcut availability.

| Layout | Context |
| --- | --- |
| `items` | Optional common fallback for contexts without a specific override |
| `entry` | Files or folders targeted in an ordinary folder |
| `background` | Empty space in an ordinary folder, without a target |
| `trash` | Trash entries or empty space in Trash |
| `sidebar` | Devices, bookmarks, and the Trash sidebar item |

Specific arrays override `items`. Without either, rummage uses that context's built-in menu;
an empty array hides its menu. The [example config](docs/config.example.toml) includes the
**complete default layouts for all four contexts**, their separators and submenu contents,
as well as the available IDs and additional custom-action examples. Default reference arrays
and the custom `items` example are alternatives: keep only the overrides you intend to use
and only one `[context_menu]` table.

Copy Location, Extract Here, and Extract To are built-in actions. Extraction is filtered to
applicable archive selections; missing backends are dimmed. Supported backends include
File Roller, unzip, gzip, tar, and 7-Zip. Copy Location cannot be replaced. Override an extraction preset by its ID to change
its label, placement, or enabled state; providing `argv` makes it an external command.
Actions are unavailable in Trash, and external actions are unavailable in file-picker mode.

Action IDs can also be bound in the existing `[keybindings]` table, for example
`"action:compress" = "Ctrl+Alt+z"`; define the action before enabling its binding. Actions,
keybindings, and menu layouts reload together. Invalid changes retain the previous valid
set on reload. Validate edits with `rummage --check-config` or
`rummage --config /path/to/config.toml --check-config`.

The installed reference at `share/rummage/config.example.toml` updates with rummage.
Existing personal files, including a copied `~/.config/rummage/config.example.toml`, are
not refreshed automatically. Use the current reference when adding new features to an
existing configuration.

### CI and package checks

Pull requests run the full Nix test suite, NixOS module checks, installed-package checks,
and native Void XBPS, Debian 13 DEB and Fedora 44 RPM build/install checks against the PR
checkout. Arch and openSUSE Tumbleweed are also built and installed in fresh containers.
All installed packages must
render a window under headless Weston. Initial coverage is x86_64 Linux/glibc; ARM, musl,
and other distribution packages are not yet covered by this workflow.

See [CI and packaging](docs/ci-packaging.md) for local commands, downloadable Void/DEB/RPM snapshots,
required branch checks, pinned build tools, and the distinction between development snapshots
and release packages.

## Upstream Nordend versions

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

After this rename, the documentation will be overhauled to cover the combined feature set,
installation options, configuration, and workflows. Historical roadmap notes and measurements
remain in the upstream development notes linked below.

## Documentation

[Features](docs/features.md) ·
[Devlog](docs/devlog.md) ·
[Benchmarks](docs/benchmarks.md) ·
[Compatibility](docs/compatibility.md) ·
[Upstream bug log](docs/buglog.md)

## Development

rummage continues the work of [Nordend](https://github.com/pankaspe/nordend-fm) by pankaspe.
Historical development notes retain their original attribution. Step numbers in inherited
code comments ("ROADMAP 6.3") refer to Nordend's original roadmap.

## License

[MIT](LICENSE). Third-party code and data (the wuffs decoder, the Tabler icons, Noctalia's default
palette) are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
