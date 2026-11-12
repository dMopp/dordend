#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv && $EUID == 0 ]] || { echo 'Run native.sh, not this script on the host.' >&2; exit 1; }
format=${1:?}
version=${2:?}
case "$format" in
    deb)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends /packages/nordend_*.deb
        dpkg --verify nordend
        license=/usr/share/doc/nordend/copyright
        ;;
    rpm)
        dnf install -y /packages/nordend-[0-9]*.x86_64.rpm
        rpm --verify nordend
        license=/usr/share/licenses/nordend/LICENSE
        ;;
    tumbleweed)
        zypper --non-interactive refresh
        zypper --non-interactive dup --no-recommends
        zypper --non-interactive install --no-recommends --allow-unsigned-rpm /packages/nordend-[0-9]*.x86_64.rpm
        rpm --verify nordend
        license=/usr/share/licenses/nordend/LICENSE
        ;;
    arch)
        # The minimal image skips documentation; verify the complete package payload.
        sed -i 's@usr/share/doc/\*@@g' /etc/pacman.conf
        pacman -Syu --noconfirm
        pacman -U --noconfirm /packages/nordend-[0-9]*-x86_64.pkg.tar.zst
        pacman -Qkk nordend
        license=/usr/share/licenses/nordend/LICENSE
        ;;
    *) exit 1 ;;
esac
# Check shared libraries before adding tools which could mask a missing dependency.
for executable in /usr/bin/nordend /usr/libexec/nordend/nordend-preview; do
    libraries=$(ldd "$executable")
    printf '%s\n' "$libraries"
    if grep -F 'not found' <<< "$libraries"; then exit 1; fi
done
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
