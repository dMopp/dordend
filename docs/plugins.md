# Plugins

How Nordend's plugins work, how to add one, and what a plugin must bring before it is merged.

- [Why plugins](#why-plugins)
- [How they work](#how-they-work)
- [Switching plugins on and off](#switching-plugins-on-and-off)
- [Adding a plugin, step by step](#adding-a-plugin-step-by-step)
- [The hooks](#the-hooks)
- [Adding a hook](#adding-a-hook)
- [Requirements](#requirements)
- [Checklist](#checklist)

---

## Why plugins

Nordend's core is a fast, small file manager: folders, files, previews, the sidebar. Everything
that is useful to some people but not part of that idea (network folders today) is a plugin, so
the core does not grow with every wish and each feature keeps its dependencies to itself.

**Belongs in a plugin:** talking to an outside service (gvfs, a cloud, a VCS), a feature with its
own libraries, anything a person might reasonably want to switch off.

**Stays in the core:** what every plugin of a kind needs and what makes sense without any plugin:
widgets (the password card, a masked text field), data types (`fs::Place`, `fs::RemoteUri`),
the seams the Browser offers (`views::Remotes`).

## How they work

Plugins are **compiled into the program**, not loaded from `.so` files at run time:

- no C++ ABI to keep stable between Nordend and its plugins;
- no foreign code in the process: Nordend cages what it reads (see `src/isolate`), and a loaded
  library would undo that;
- a build (Nix, a distribution package) picks plugins with a CMake option.

The pieces:

| Where | What |
|-------|------|
| `src/plugin/include/nordend/plugin/plugin.hpp` | `plugin::Plugin` (the base class with the hooks), `plugin::Host` (what the app lends a plugin), `plugin::Builtin`, `plugin::Plugins` (the set running) |
| `src/plugin/src/plugin.cpp` | `plugin::Plugins`: makes the plugins, routes hooks to them |
| `src/plugin/builtin.cpp.in` | template of the generated list of plugins compiled in |
| `plugins/CMakeLists.txt` | finds every `plugins/<id>/`, gives it an option, generates the list |
| `plugins/<id>/` | one plugin: its `CMakeLists.txt`, `include/`, `src/` |
| `tests/plugin_test.cpp` | `plugin::Plugins` with fake plugins |
| `tests/plugin_<id>_test.cpp` | a plugin's own tests, built only with it |

**Life cycle.** At start, after the window and the sidebar exist, `src/app/main.cpp` makes one
`plugin::Plugins`. It goes through `plugin::builtin()` in id order, skips the ids in
`[plugins] disabled`, and calls each plugin's `nordend_plugin_make_<id>(Host&)`. The plugins live
until the window closes and are destroyed in reverse order. `config.toml` is read again live, but
`[plugins]` only at start.

**Threads.** Everything runs on the event loop's thread: construction, every hook, every callback
a plugin calls. A plugin must never block it. Waiting for a service is done asynchronously
(watch a file descriptor with `core::EventLoop`, or use `platform::BusLoop` for D-Bus) and the
answer comes back on the loop. Heavy work goes to a `core::WorkerPool`, with only its result
posted back with `loop.post()`.

**Host.** What a plugin gets:

```cpp
struct Host {
    core::EventLoop& loop;                // timers, fds, post()
    std::function<void()> places_changed; // ask for the sidebar to be built again
};
```

A plugin keeps a reference to `host.loop` if it needs it; `Host` itself is the app's.

## Switching plugins on and off

| When | How |
|------|-----|
| Building with CMake | `-DNORDEND_PLUGIN_<ID>=OFF`, e.g. `cmake -B build -DNORDEND_PLUGIN_REMOTES=OFF`; every plugin is ON by default |
| Building with Nix | `nordend.override { disabledPlugins = [ "remotes" ]; }` |
| Running | `~/.config/nordend/config.toml`, read at start: `[plugins]` `disabled = ["remotes"]` |
| Checking | `nordend --about` lists the plugins built in; the log says `plugin <id>: running` or `disabled` |

## Adding a plugin, step by step

The example adds `webdav`, a plugin that would mount `dav://` and `davs://` bookmarks. Pick an
**id** first: lower case letters, digits and `_`, the name of its directory, its CMake option
(`NORDEND_PLUGIN_WEBDAV`) and what people write in `[plugins] disabled`.

### 1. The directory

```
plugins/webdav/
├── CMakeLists.txt
├── include/nordend/plugins/webdav/   # headers its tests need; may stay empty
└── src/
    └── webdav.cpp
```

### 2. `plugins/webdav/CMakeLists.txt`

```cmake
# WebDAV bookmarks (dav://, davs://), mounted through <how>. Needs <library> to build.
pkg_check_modules(NEON REQUIRED IMPORTED_TARGET neon)   # its own dependencies, found here

nordend_add_plugin(webdav
    SUMMARY "dav:// and davs:// bookmarks"
    SOURCES src/webdav.cpp
    LINK PkgConfig::NEON)
```

`nordend_add_plugin(<id> SUMMARY … SOURCES … [LINK …])` makes the static library
`nordend_plugin_<id>`, links it to `nordend::plugin` (and so to core, fs and views), adds
`include/` as its public include directory, puts the warning flags on it and registers it in the
generated list. A dependency the core does not have is found in the plugin's own
`CMakeLists.txt`, never in the top-level one, so leaving the plugin out leaves the dependency out.

Nothing else needs editing to build it: `plugins/CMakeLists.txt` picks up every directory with a
`CMakeLists.txt`. Configure again and CMake prints `-- Plugins: remotes;webdav`.

### 3. The plugin class and its factory

```cpp
// plugins/webdav/src/webdav.cpp
#include <memory>

#include "nordend/plugin/plugin.hpp"

namespace nordend::plugins::webdav {
namespace {

class WebdavPlugin final : public plugin::Plugin {
public:
    explicit WebdavPlugin(plugin::Host& host) : loop_{host.loop} {}

    [[nodiscard]] std::vector<std::string> schemes() const override { return {"dav", "davs"}; }
    [[nodiscard]] bool can_mount() const override { return true; }

    void mount(const std::string& uri, views::Remotes::Prompts prompts,
               views::Remotes::Mounted done) override {
        // Start the mount without blocking; answer on the loop thread:
        //   done(local_folder, "")        it worked
        //   done({}, "cancelled")         the person gave up (no error is shown)
        //   done({}, "what went wrong")   shown, through i18n::tr
    }

    void unmount(const std::string& uri, views::Remotes::Done done) override {
        // done("") when unmounted, done("in use") or another error otherwise.
    }

private:
    core::EventLoop& loop_;
};

}  // namespace
}  // namespace nordend::plugins::webdav

// Called by the generated list (src/plugin/builtin.cpp.in). The name must be exactly
// nordend_plugin_make_<id>, outside any namespace.
std::unique_ptr<nordend::plugin::Plugin> nordend_plugin_make_webdav(nordend::plugin::Host& host) {
    return std::make_unique<nordend::plugins::webdav::WebdavPlugin>(host);
}
```

Rules for the class:

- Namespace `nordend::plugins::<id>`; the class itself in an anonymous namespace unless tests need
  it, then in a header under `include/nordend/plugins/<id>/`.
- Override only the hooks it needs; the others keep their defaults.
- The factory may return `nullptr` when the plugin cannot work on this system at all; it then
  does not count as running. When it can work later (a service that may start), return the
  plugin and say so through `can_mount()` instead.
- A constructor must be quick: no blocking calls, no waiting for a service.

### 4. Tests

Add `tests/plugin_webdav_test.cpp` and register it in `tests/CMakeLists.txt`, next to the
`remotes` block:

```cmake
if(TARGET nordend_plugin_webdav)
    target_sources(nordend-tests PRIVATE plugin_webdav_test.cpp)
    target_link_libraries(nordend-tests PRIVATE nordend_plugin_webdav)
endif()
```

What the tests must cover is in [Requirements → Tests](#tests).

### 5. Texts and translations

Every text a person sees goes through `i18n::tr("English")` or `i18n::trf("… {} …", value)`
(`nordend/core/i18n.hpp`), including error strings handed back through `done(…, error)`, which
the Browser translates. Add the German entry to `src/core/src/i18n_de.cpp`, near the plugin's
other texts. Error strings are short, lower case and say what is wrong ("there is no session
bus"), since they follow "“Name” could not be mounted: ".

### 6. Documentation

- the plugin's row in the table in [plugins/README.md](../plugins/README.md);
- what it does for the person in [features.md](features.md);
- a line in the README's feature list if it is visible;
- an entry in [devlog.md](devlog.md): done, decisions, tests, open points;
- if it adds keys to `config.toml`, the commented defaults in
  [config.example.toml](config.example.toml).

### 7. Build, test, try

```sh
nix develop
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build && ./build/tests/nordend-tests
cmake -B build-without -G Ninja -DNORDEND_PLUGIN_WEBDAV=OFF      # without the plugin
cmake --build build-without && ./build-without/tests/nordend-tests
nix build .#nordend                                              # what Nix ships
```

Then [try it by hand](#trying-it-by-hand) and go through the [checklist](#checklist).

## The hooks

All are virtual members of `plugin::Plugin` with defaults that do nothing, called on the loop
thread. `plugin::Plugins` combines the plugins' answers and hands them to the app.

### Network locations

Bookmarks with a URI (`fs::Place::uri`, any `scheme://host/…` in the GTK bookmarks file) and
Ctrl+L addresses.

| Hook | Called | Contract | Default |
|------|--------|----------|---------|
| `schemes()` | when places are built, for every mount | the URI schemes it mounts, lower case (`{"smb", "sftp"}`). Two plugins must not claim the same scheme; the first in id order would win | none |
| `can_mount()` | when the app sets up the Browser, before every mount | whether mounting works now. When no running plugin can mount, the Browser has no `Remotes` and says network folders are not available | `false` |
| `mount(uri, prompts, done)` | a remote bookmark is chosen, or a server typed in Ctrl+L | mount `uri`; ask the person through `prompts` (`login` for name and password, `question` for a choice, `cancel` when the server stops waiting); finish with `done(folder, "")`, `done({}, "cancelled")` or `done({}, error)` | `done({}, "not supported")` |
| `unmount(uri, done)` | the eject icon or "Unmount" | unmount; `done("")`, `done("in use")` or `done(error)` | `done("not supported")` |

`plugin::Plugins` hides bookmarks whose scheme no running plugin claims; it keeps those of a
plugin that cannot mount now, so a click says why. Ctrl+L refuses schemes no plugin claims
("No plugin connects to ftp:// servers") and adds no bookmark for them.

### Sidebar

| Hook | Called | Contract | Default |
|------|--------|----------|---------|
| `places(places)` | every time the sidebar is built (start, a bookmark or mount changes, `places_changed()`) | return the places as they should show: fill in its own (a mounted share: `path`, `disk.mounted = true`, `disk.ejectable = true` for the eject icon), add or drop some. Leave other places alone, and keep it cheap: no I/O, it runs often | unchanged |

When something it shows changes (a mount appears), the plugin calls `host.places_changed()`.

## Adding a hook

When a plugin needs something the hooks do not offer (a context menu entry, a preview, a
column), add a hook instead of reaching into the app:

1. A virtual member in `plugin::Plugin` with a default that does nothing (or returns what the
   app would do without plugins). Existing plugins must build unchanged.
2. A member in `plugin::Plugins` that combines the plugins' answers, and the call to it where the
   app needs it (`src/app/main.cpp`, or a seam on `views::Browser` set from there). The core
   never includes a plugin's header.
3. A case in `tests/plugin_test.cpp` with fake plugins: the default, one plugin, two plugins.
4. A row in [The hooks](#the-hooks) above.

Keep hooks about what the core offers ("a sidebar place", "a URI scheme"), not about one
plugin's service, so the next plugin can use them too.

## Requirements

A plugin is merged when all of the following hold. They are the core's rules too.

### Code

- C++20, formatted with the repository's `.clang-format` (`clang-format -i` on its files); the
  advisory `.clang-tidy` set (`-DNORDEND_CLANG_TIDY=ON`) without new findings.
- Builds without warnings under the project's flags (`-Wall -Wextra -Wpedantic -Wshadow
  -Wconversion -Wsign-conversion -Wold-style-cast …`, see the top-level `CMakeLists.txt`).
- No GTK, Qt or GLib in the process. A service that only has a GLib API is reached over D-Bus
  (`sdbus-c++`, `platform::BusLoop`) or a separate process.
- Loop thread only, never blocking it (see [How they work](#how-they-work)). Objects that outlive
  a callback guard against it (the Browser's `alive_` pattern, or a `std::weak_ptr`).
- No secrets in logs: passwords and tokens are never passed to `log::`, never written to files
  by Nordend; a password the person types is forgotten once handed on.
- Comments say why, in the project's voice (see the existing headers): what a function gives,
  what it promises, what it leaves out.

### Dependencies

- Found and linked in the plugin's own `CMakeLists.txt` (`LINK`), never added to the core's
  targets.
- A build-time dependency the core does not have is added to `nix/package.nix`
  (`buildInputs`) and named in [plugins/README.md](../plugins/README.md).
- A run-time dependency (a daemon, a program) must be optional at run time: without it the
  plugin still loads, `can_mount()` (or its hook's equivalent) says no, and the person gets a
  message saying what is missing. Nordend must start and work normally without it.

### Tests

Tests use doctest, like the rest (`tests/*.cpp`), and run in `nordend-tests`.

1. **Pure logic first.** Parsing, mapping URIs to what the service wants, matching: free
   functions in the plugin's header, tested without any service.
2. **The service faked.** The flow through the plugin (mount, ask for a password, answer, give
   up, error, unmount) against a fake of the service, in process. `tests/remote_test.cpp` shows
   the Browser side with a fake `views::Remotes`; `tests/plugin_test.cpp` shows fake plugins.
3. **The real system, read-only, optional.** At most a test that asks the real service something
   harmless and **skips** (`MESSAGE("…: skipped"); return;`) when it is not there, like
   "gvfs: the real session bus answers without starting gvfsd". Tests never mount, write or
   change anything outside a `test::TempDir`, never need the network, never start daemons.
4. **Both builds pass.** The full suite passes with the plugin and with
   `-DNORDEND_PLUGIN_<ID>=OFF`. Known failures that exist without the change are named in the
   devlog entry, not hidden.
5. **Sanitizers.** For code with threads, sockets or callbacks, a run under
   `-DNORDEND_SANITIZER=address,undefined` (and `thread` where threads are involved).

### Trying it by hand

Before it is called done, the plugin runs in a real Nordend, isolated from the session it is
developed in:

```sh
T=$(mktemp -d); mkdir -p $T/config/nordend $T/state $T/cache
XDG_CONFIG_HOME=$T/config XDG_STATE_HOME=$T/state XDG_CACHE_HOME=$T/cache ./build/nordend /tmp
```

Check it with the plugin running, with `[plugins] disabled = ["<id>"]` in
`$T/config/nordend/config.toml`, and without its service (stop it, or use a separate session
bus with `dbus-run-session`). For anything that mounts or changes state, use a headless
compositor and its own runtime directory rather than the session you work in.

### Commits

One topic per commit, a subject that says what changed for the person or the code ("Add the
webdav plugin: dav:// bookmarks mounted through …"), a body with what and why. Documentation and
tests are in the same commit as the code they describe.

## Checklist

- [ ] `plugins/<id>/CMakeLists.txt` with `nordend_add_plugin`, its own dependencies only
- [ ] `nordend_plugin_make_<id>` defined, outside any namespace; class in `nordend::plugins::<id>`
- [ ] only the hooks it needs overridden; quick constructor; loop thread; nothing blocks
- [ ] works without its run-time service: loads, says what is missing
- [ ] texts through `i18n::tr`/`trf`, German entries in `i18n_de.cpp`
- [ ] `tests/plugin_<id>_test.cpp`, registered under `if(TARGET nordend_plugin_<id>)`: logic,
      faked service, optional read-only real check that skips
- [ ] full suite passes with and without `-DNORDEND_PLUGIN_<ID>=OFF`
- [ ] `nix build .#nordend` passes; new build inputs in `nix/package.nix`
- [ ] clang-format clean, no new warnings, clang-tidy looked at
- [ ] tried by hand in an isolated instance: running, disabled, service missing
- [ ] docs: `plugins/README.md` table, `features.md`, README if visible, `devlog.md`,
      `config.example.toml` if it adds keys
- [ ] a new hook, if any: default, `plugin::Plugins`, `tests/plugin_test.cpp`, this document
