# Plugins

Features outside Nordend's core, one directory each, compiled in and switched off per build or
in `config.toml`. **How they work, how to add one and what a plugin must bring (tests, docs,
dependencies): [docs/plugins.md](../docs/plugins.md).**

| Plugin | What it does | Needs |
|--------|--------------|-------|
| `remotes` | `smb://` and `sftp://` bookmarks and Ctrl+L servers, mounted through gvfs over D-Bus, with a password card | sdbus-c++ to build; gvfs (gvfsd, gvfsd-fuse) at run time, in its own processes |

Switch one off:

- CMake: `-DNORDEND_PLUGIN_REMOTES=OFF`
- Nix: `nordend.override { disabledPlugins = [ "remotes" ]; }`
- At run time, in `~/.config/nordend/config.toml`: `[plugins]` `disabled = ["remotes"]`
