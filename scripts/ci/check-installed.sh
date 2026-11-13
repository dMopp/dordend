#!/usr/bin/env bash
set -euo pipefail
prefix=${1:?usage: check-installed.sh PREFIX VERSION}
version=${2:?usage: check-installed.sh PREFIX VERSION}
test -s "${3:-$prefix/share/licenses/rummage/LICENSE}"
app="$prefix/bin/rummage"
"$app" --version | grep -Fx "rummage $version"
"$app" --help | grep -F -- '--pick'
"$app" --config "$prefix/share/rummage/config.example.toml" --check-config
for file in \
    share/rummage/config.example.toml \
    share/rummage/engines/base.toml \
    share/applications/rummage.desktop \
    share/icons/hicolor/scalable/apps/rummage.svg \
    share/rummage/noctalia/rummage \
    share/xdg-desktop-portal/portals/rummage.portal \
    share/dbus-1/services/org.freedesktop.impl.portal.desktop.rummage.service; do
    test -s "$prefix/$file"
done
test -x "$prefix/libexec/rummage/rummage-preview"
desktop-file-validate "$prefix/share/applications/rummage.desktop"
grep -Fx 'Interfaces=org.freedesktop.impl.portal.FileChooser;' \
    "$prefix/share/xdg-desktop-portal/portals/rummage.portal"
grep -Fx "Exec=$app --portal" \
    "$prefix/share/dbus-1/services/org.freedesktop.impl.portal.desktop.rummage.service"
# The installed binary must understand the features documented in the installed reference.
config=$(mktemp)
trap 'rm -f "$config"' EXIT
cat > "$config" <<'TOML'
[[actions]]
id = "compress"
label = "Compress"
argv = ["7z", "a", "{destination}", "--", "{files}"]
selection = "many"
destination = "file"
destination_name = "Archive.zip"
notify_on_completion = true
[context_menu]
entry = [{ id = "action:compress" }, { remaining = true }]
TOML
"$app" --config "$config" --check-config
