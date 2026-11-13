#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv && $(id -u) == 0 ]]
xbps-install -Syu xbps
xbps-install -yu
xbps-install -y -R /packages rummage
xbps-pkgdb rummage
test ! -e /usr/libexec/rummage/rummage-media
# Check linked runtime dependencies before adding compositor/test dependencies.
for executable in /usr/bin/rummage /usr/libexec/rummage/rummage-preview; do
    dependencies=$(ldd "$executable")
    printf '%s\n' "$dependencies"
    if grep -F 'not found' <<< "$dependencies"; then exit 1; fi
done
xbps-install -y desktop-file-utils
bash /source/scripts/ci/check-installed.sh /usr "$1"
xbps-install -y weston mesa-dri dbus dejavu-fonts-ttf
bash /source/scripts/ci/check-wayland.sh /usr

# Install the optional sibling after checking the base package independently.
xbps-install -y -R /packages rummage-media
ldd /usr/libexec/rummage/rummage-media > /tmp/rummage-media-libraries
if grep -F 'not found' /tmp/rummage-media-libraries; then exit 1; fi
