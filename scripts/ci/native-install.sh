#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv && $EUID == 0 ]] || { echo 'Run native.sh, not this script on the host.' >&2; exit 1; }
format=${1:?}
version=${2:?}
case "$format" in
    deb)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends /packages/rummage_*.deb
        dpkg --verify rummage
        license=/usr/share/doc/rummage/copyright
        ;;
    rpm)
        dnf install -y /packages/rummage-[0-9]*.x86_64.rpm
        rpm --verify rummage
        license=/usr/share/licenses/rummage/LICENSE
        ;;
    tumbleweed)
        zypper --non-interactive refresh
        zypper --non-interactive dup --no-recommends
        zypper --non-interactive install --no-recommends --allow-unsigned-rpm /packages/rummage-[0-9]*.x86_64.rpm
        rpm --verify rummage
        license=/usr/share/licenses/rummage/LICENSE
        ;;
    arch)
        # The minimal image skips documentation; verify the complete package payload.
        sed -i 's@usr/share/doc/\*@@g' /etc/pacman.conf
        pacman -Syu --noconfirm
        pacman -U --noconfirm /packages/rummage-[0-9]*-x86_64.pkg.tar.zst
        pacman -Qkk rummage
        license=/usr/share/licenses/rummage/LICENSE
        ;;
    *) exit 1 ;;
esac
# Check shared libraries before adding tools which could mask a missing dependency.
for executable in /usr/bin/rummage /usr/libexec/rummage/rummage-preview; do
    libraries=$(ldd "$executable")
    printf '%s\n' "$libraries"
    if grep -F 'not found' <<< "$libraries"; then exit 1; fi
done
# The base package must work before the optional media package is installed.
test ! -e /usr/libexec/rummage/rummage-media
case "$format" in
    deb) apt-get install -y --no-install-recommends /packages/rummage-media_*.deb ;;
    rpm) dnf install -y /packages/rummage-media-[0-9]*.x86_64.rpm ;;
    tumbleweed) zypper --non-interactive install --no-recommends --allow-unsigned-rpm /packages/rummage-media-[0-9]*.x86_64.rpm ;;
    arch) pacman -U --noconfirm /packages/rummage-media-[0-9]*-x86_64.pkg.tar.zst ;;
esac
libraries=$(ldd /usr/libexec/rummage/rummage-media)
printf '%s\n' "$libraries"
if grep -F 'not found' <<< "$libraries"; then exit 1; fi
case "$format" in
    deb) apt-get install -y --no-install-recommends desktop-file-utils ;;
    rpm) dnf install -y desktop-file-utils ;;
    tumbleweed) zypper --non-interactive install --no-recommends desktop-file-utils ;;
    arch) pacman -S --noconfirm --needed desktop-file-utils ;;
esac
bash /source/scripts/ci/check-installed.sh /usr "$version" "$license"
case "$format" in
    deb) apt-get install -y --no-install-recommends weston libgl1-mesa-dri dbus fonts-dejavu-core ;;
    rpm) dnf install -y weston mesa-dri-drivers dbus-daemon dejavu-sans-fonts ;;
    tumbleweed) zypper --non-interactive install --no-recommends weston Mesa-dri dbus-1-daemon dejavu-fonts ;;
    arch) pacman -S --noconfirm --needed weston mesa dbus ttf-dejavu ;;
esac
bash /source/scripts/ci/check-wayland.sh /usr
