#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv && $EUID == 0 ]] || { echo 'Run native.sh, not this script on the host.' >&2; exit 1; }
format=${1:?}
case "$format" in
    deb)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends build-essential debhelper devscripts equivs python3 ca-certificates
        ;;
    rpm) dnf install -y rpm-build rpmdevtools python3 tar gzip ;;
    tumbleweed)
        zypper --non-interactive refresh
        zypper --non-interactive dup --no-recommends
        zypper --non-interactive install --no-recommends rpm-build python3 tar gzip shadow
        ;;
    arch)
        pacman -Syu --noconfirm --needed base-devel python
        ;;
    *) exit 1 ;;
esac
mkdir -p /build /output/packages
# Validate the archive before unpacking, and derive package versions from its identity.
python3 - <<'PY'
import hashlib, json, re
from pathlib import Path
m = json.loads(Path('/input/source.json').read_text())
assert re.fullmatch(r'[0-9a-f]{40}', m['commit'])
assert re.fullmatch(r'[0-9]{14}', m['timestamp'])
assert re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', m['version'])
assert hashlib.sha256(Path('/input/source.tar.gz').read_bytes()).hexdigest() == m['sha256']
Path('/build/version').write_text(m['version'])
Path('/build/snapshot').write_text(m['timestamp']+'.git'+m['commit'][:12])
PY
version=$(cat /build/version)
snapshot=$(cat /build/snapshot)
case "$format" in
    deb)
        tar -xzf /input/source.tar.gz -C /build
        cd /build/nordend-checkout
        cp -a packaging/debian debian
        DEBFULLNAME='Nordend fork maintainers' DEBEMAIL='barrulus@users.noreply.github.com' \
            dch --newversion "$version+git$snapshot-1" --distribution unstable 'Build exact CI source snapshot.'
        mk-build-deps --install --remove --tool 'apt-get -y --no-install-recommends' debian/control
        cp -a debian /output/recipe
        DEB_BUILD_OPTIONS="parallel=${CI_JOBS:-2}" dpkg-buildpackage -b -us -uc
        cp /build/*.deb /build/*.changes /build/*.buildinfo /output/packages/
        ;;
    rpm|tumbleweed)
        mkdir -p /build/rpm/{SOURCES,SPECS}
        cp /input/source.tar.gz /build/rpm/SOURCES/nordend-source.tar.gz
        tar -xzf /input/source.tar.gz -C /build
        cp /build/nordend-checkout/packaging/rpm/nordend.spec /build/rpm/SPECS/
        spec=/build/rpm/SPECS/nordend.spec
        sed -i -e "s/^Version:.*/Version:        $version/" \
            -e "s/^Release:.*/Release:        1.$snapshot%{?dist}/" "$spec"
        # Use the same recipe for dependency resolution and building.
        rpmspec -q --buildrequires "$spec" > /build/dependencies
        mapfile -t dependencies < /build/dependencies
        if [[ "$format" == rpm ]]; then
            dnf install -y "${dependencies[@]}"
        else
            zypper --non-interactive install --no-recommends "${dependencies[@]}"
        fi
        rpmbuild -ba --define '_topdir /build/rpm' \
            --define "_smp_mflags -j${CI_JOBS:-2}" "$spec"
        find /build/rpm/RPMS /build/rpm/SRPMS -name '*.rpm' -exec cp '{}' /output/packages/ \;
        cp "$spec" /output/nordend.spec
        ;;
    arch)
        mkdir -p /build/arch
        tar -xzf /input/source.tar.gz -C /build
        cp /build/nordend-checkout/packaging/arch/PKGBUILD /build/arch/
        cp /input/source.tar.gz /build/arch/nordend-source.tar.gz
        checksum=$(python3 -c 'import json; print(json.load(open("/input/source.json"))["sha256"])')
        sed -i -e "s/^pkgver=.*/pkgver=$version.r$snapshot/" \
            -e "s/CHECKOUT_SHA256/$checksum/" /build/arch/PKGBUILD
        # makepkg must run as an ordinary user; only dependency installation uses root.
        useradd --create-home builder
        chown -R builder:builder /build/arch
        cd /build/arch
        runuser -u builder -- makepkg --printsrcinfo > /build/arch.SRCINFO
        mapfile -t dependencies < <(awk '$1 == "depends" || $1 == "makedepends" {print $3}' /build/arch.SRCINFO)
        pacman -S --noconfirm --needed "${dependencies[@]}"
        runuser -u builder -- env CI_JOBS="${CI_JOBS:-2}" makepkg --noconfirm
        cp /build/arch/*.pkg.tar.zst /output/packages/
        mkdir -p /output/recipe
        cp /build/arch/PKGBUILD /output/recipe/
        cp /build/arch.SRCINFO /output/recipe/SRCINFO
        ;;
esac
cp /input/source.json /output/source.json
