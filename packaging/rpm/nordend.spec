Name:           nordend
Version:        0.5.0
Release:        1%{?dist}
Summary:        Native Wayland file manager
License:        MIT AND (MIT OR Apache-2.0)
URL:            https://github.com/barrulus/bordend-fm
Source0:        nordend-source.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
%if 0%{?suse_version}
# openSUSE packages the GLES 3 headers separately from pkgconfig(glesv2).
BuildRequires:  Mesa-libGLESv3-devel
BuildRequires:  ninja
%else
BuildRequires:  ninja-build
%endif
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-egl)
BuildRequires:  pkgconfig(wayland-cursor)
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(wayland-protocols)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(harfbuzz)
BuildRequires:  pkgconfig(freetype2)
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(tomlplusplus)
BuildRequires:  pkgconfig(libwebp)
BuildRequires:  pkgconfig(libwebpdemux)
BuildRequires:  pkgconfig(libjxl)
BuildRequires:  pkgconfig(zlib)
BuildRequires:  pkgconfig(sdbus-c++) >= 2
Requires:       shared-mime-info
Requires:       hicolor-icon-theme

%description
A file manager with column and grid views, isolated previews, configurable
custom actions and an optional desktop file chooser portal.

%prep
%autosetup -n nordend-checkout

%build
%if 0%{?suse_version}
%define __builder ninja
%endif
%cmake -G Ninja -DBUILD_SHARED_LIBS=OFF -DNORDEND_BUILD_TESTS=OFF -DNORDEND_WERROR=OFF
%if 0%{?suse_version}
%cmake_build nordend nordend-preview
%else
%cmake_build --target nordend nordend-preview
%endif

%install
%cmake_install
# Preserve bundled license notices under distinct names.
install -Dm644 third_party/tabler-icons/LICENSE %{buildroot}%{_datadir}/licenses/%{name}/tabler-icons-LICENSE
install -Dm644 third_party/fzy/LICENSE %{buildroot}%{_datadir}/licenses/%{name}/fzy-LICENSE
install -Dm644 third_party/wuffs/LICENSE-MIT %{buildroot}%{_datadir}/licenses/%{name}/wuffs-LICENSE-MIT
install -Dm644 third_party/wuffs/LICENSE-APACHE %{buildroot}%{_datadir}/licenses/%{name}/wuffs-LICENSE-APACHE

%files
%license LICENSE
%license %{_datadir}/licenses/%{name}/*-LICENSE*
%doc THIRD_PARTY.md
%{_bindir}/nordend
%{_libexecdir}/nordend/
%{_datadir}/nordend/
%{_datadir}/applications/nordend.desktop
%{_datadir}/icons/hicolor/scalable/apps/nordend.svg
%{_datadir}/xdg-desktop-portal/portals/nordend.portal
%{_datadir}/dbus-1/services/org.freedesktop.impl.portal.desktop.nordend.service
