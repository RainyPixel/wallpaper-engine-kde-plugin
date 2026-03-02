Name:    wallpaper-engine-kde-plugin-qt6
Version: 0
Release: 1%{?dist}
Summary: A KDE wallpaper plugin integrating Wallpaper Engine (Plasma 6)

License: GPL-2.0-only
URL:     https://github.com/captsilver/wallpaper-engine-kde-plugin

# Built from a live git checkout.

BuildRequires: cmake extra-cmake-modules
BuildRequires: vulkan-headers
BuildRequires: plasma-workspace-devel libplasma-devel
BuildRequires: kf6-plasma-devel
BuildRequires: kf6-kcoreaddons-devel
BuildRequires: kf6-kpackage-devel
BuildRequires: lz4-devel
BuildRequires: mpv-libs-devel
BuildRequires: qt6-qtbase-private-devel
BuildRequires: qt6-qtwebchannel-devel qt6-qtwebsockets-devel

Requires: plasma-workspace
Requires: gstreamer1-libav
Requires: mpv-libs
Requires: lz4
Requires: qt6-qtwebchannel
Requires: qt6-qtwebsockets

%global _enable_debug_package 0
%global debug_package %{nil}

%{?commit:%global shortcommit %(c=%{commit}; echo ${c:0:7})}
%{!?commit:%global shortcommit unknown}

%description
A wallpaper plugin integrating Wallpaper Engine into KDE Plasma 6 wallpaper
settings. This is the RainyPixel fork with native C++ file operations
(no Python dependency), fixed KDE 6.5+ theme reactivity, and Plasma 6 / Qt6
support.

%prep
# No-op: building directly from the git checkout at %{reporoot}
# Ensure submodules are initialised before calling wallpaper.sh:
#   git submodule update --init --force --recursive

%build
cmake -B %{_builddir}/wek-build \
      -S %{reporoot} \
      -DCMAKE_BUILD_TYPE=Release
cmake --build %{_builddir}/wek-build -- %{?_smp_mflags}

%install
DESTDIR=%{buildroot} cmake --install %{_builddir}/wek-build \
      --prefix %{_prefix}

%files
%{_libdir}/*
%{_datadir}/*

%changelog
* Sat Feb 28 2026 packager - 0-1
- Add kf6-kcoreaddons-devel and kf6-kpackage-devel to BuildRequires
- Port to RainyPixel fork: drop python3-websockets and Qt5 dep,
  update URL, modernise cmake, remove tarball/setup dependency
