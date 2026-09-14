# Wallpaper Engine for KDE (Plasma 6)

A wallpaper plugin integrating [Wallpaper Engine](https://store.steampowered.com/app/431960/Wallpaper_Engine) into KDE Plasma wallpaper settings.

> **This is a maintained fork** of the original [catsout/wallpaper-engine-kde-plugin](https://github.com/catsout/wallpaper-engine-kde-plugin) with improvements for Plasma 6.

## Changes in this fork

- **Removed Python dependency** — file operations now use native C++ (no more `python-websockets` issues)
- **Fixed KDE 6.5+ theme reactivity** — UI elements no longer become invisible when switching between light/dark themes
- **Plasma 6 / Qt6 support**

## Hyprland and Omarchy

The [`hyprland/`](hyprland/README.md) directory contains a separate host that shows web wallpapers
as a Wayland background on Hyprland without Plasma. It has its own build
(`cmake -B build/hyprland -S hyprland`) and currently supports web wallpapers only; scene and
video wallpapers remain KDE-only for now. The KDE plugin build below is unaffected.

## Install

### Arch Linux (AUR)
```sh
yay -S wallpaper-engine-kde-plugin-git
# or
paru -S wallpaper-engine-kde-plugin-git
```

### Fedora / rpm-ostree / Bazzite (RPM)

A prebuilt RPM from February 2026 is available from the
[CaptSilver fork](https://github.com/CaptSilver/wallpaper-engine-kde-plugin/releases). It predates
the current code, so for the current version build the RPM yourself as described under
[Build RPM package](#build-rpm-package-fedora).

```sh
curl -LO https://github.com/CaptSilver/wallpaper-engine-kde-plugin/releases/download/v1.0/wallpaper-engine-kde-plugin-qt6-0-1.fc43.x86_64.rpm
```

Install:
```sh
# Standard Fedora
sudo dnf install ./wallpaper-engine-kde-plugin-qt6-0-1.fc43.x86_64.rpm

# rpm-ostree / Bazzite
rpm-ostree install ./wallpaper-engine-kde-plugin-qt6-0-1.fc43.x86_64.rpm
```

### Build from source

#### Dependencies

Arch:
```sh
sudo pacman -S extra-cmake-modules plasma-framework gst-libav ninja \
base-devel mpv qt6-declarative qt6-webchannel vulkan-headers cmake lz4
```

Fedora:
```sh
# Add RPM Fusion repos (required for ffmpeg/mpv)
sudo dnf install -y \
    https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm \
    https://mirrors.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm

# Replace ffmpeg-free with full ffmpeg
sudo dnf swap -y ffmpeg-free ffmpeg --allowerasing
sudo dnf install -y ffmpeg-devel --allowerasing

sudo dnf install vulkan-headers plasma-workspace-devel kf6-plasma-devel \
    kf6-kcoreaddons-devel kf6-kpackage-devel gstreamer1-libav \
    lz4-devel mpv-libs-devel qt6-qtbase-private-devel libplasma-devel \
    qt6-qtwebchannel-devel cmake extra-cmake-modules
```

#### Build and Install
```sh
# Download source
git clone https://github.com/RainyPixel/wallpaper-engine-kde-plugin.git
cd wallpaper-engine-kde-plugin

# Download submodules
git submodule update --init --force --recursive

# Configure and build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Install (system-wide)
sudo cmake --install build

# Restart plasmashell
systemctl --user restart plasma-plasmashell.service
```

#### Build RPM package (Fedora)

Useful for rpm-ostree/Bazzite systems where layered packages survive updates.

On Bazzite and other rpm-ostree hosts `dnf` and `rpmbuild` do not run directly, so do the build
inside a Fedora toolbox or distrobox of the same release as the host (`rpm -E %fedora` prints it)
and skip the tmpfs mount there. Your home directory is shared with the container, so afterwards
layer the RPM from the host.

```sh
git clone https://github.com/RainyPixel/wallpaper-engine-kde-plugin.git
cd wallpaper-engine-kde-plugin

# Install build dependencies from spec
sudo dnf builddep ./rpm/wek.spec

# Initialise submodules
git submodule update --init --force --recursive

# Use tmpfs for the build directory to avoid slow disk writes
sudo mount -t tmpfs tmpfs ~/rpmbuild/BUILD

# Build the RPM
rpmbuild --define="commit $(git rev-parse HEAD)" \
    --define="reporoot $(pwd)" \
    --define="glslang_ver 11.8.0" \
    --undefine=_disable_source_fetch \
    -ba ./rpm/wek.spec

sudo umount ~/rpmbuild/BUILD
# Install (rpm-ostree example)
rpm-ostree install ~/rpmbuild/RPMS/x86_64/wallpaper-engine-kde-plugin-qt6-*.rpm
```

## Activate in Plasma

After installing via any method:

1. Right-click the desktop → **Configure Desktop and Wallpaper...**
2. Open the **Wallpaper Type** dropdown and select **Wallpaper Engine for KDE**
3. Your subscribed Workshop wallpapers appear in the list — select one and click **Apply**

The Steam library is detected automatically, including Flatpak and Snap installs and libraries on
other drives. Wallpapers from every library are listed together. Use the folder button on the
Wallpapers tab to override the detected library, for example to pick a second Steam installation.

> **Note:** After an rpm-ostree/Bazzite install you may need to reboot before the plugin starts working. For cmake installs, restarting plasmashell is enough: `systemctl --user restart plasma-plasmashell.service`

If the list stays empty and you copied the plugin into
`~/.local/share/plasma/wallpapers/com.github.catsout.wallpaperEngineKde` at some point, remove that
copy. Plasma prefers it over the installed package, and it stops matching the plugin's native part
as soon as either one is updated.

### Uninstall
1. Remove files listed in `build/install_manifest.txt`
2. `kpackagetool6 -t Plasma/Wallpaper -r com.github.catsout.wallpaperEngineKde`

## Usage
1. *Wallpaper Engine* installed on Steam
2. Subscribe to some wallpapers on the Workshop
3. Open the Wallpapers tab — the Steam library is found automatically
   - If nothing shows up, pick the *steamlibrary* folder with the folder button
   - The *steamlibrary* is the one containing the *steamapps* folder

## Requirements
- KDE Plasma 6
- Qt 6.7+ (requires `QQuickRhiItem`)
- Vulkan 1.1+
- C++20 (GCC 10+)
- [Vulkan driver](https://wiki.archlinux.org/title/Vulkan#Installation) installed (AMD users: use RADV)

## Known Issues
- Some scene wallpapers may **crash** KDE
  - Remove `WallpaperSource` line in `~/.config/plasma-org.kde.plasma.desktop-appletsrc` and restart KDE to fix
- Mouse long press (to enter panel edit mode) is broken on desktop
- Screen Locking is not supported

## Support Status

### Scene (2D)
Supported by Vulkan 1.1. Requires *Wallpaper Engine* installed for assets.

### Web
Basic web APIs supported. WebGL may not work properly.

### Video
- **QtMultimedia** (default) — uses GStreamer
- **MPV** — requires plugin lib compilation

## Acknowledgments
- RainyPixel fork: [RainyPixel/wallpaper-engine-kde-plugin](https://github.com/rainypixel/wallpaper-engine-kde-plugin)
- Original project: [catsout/wallpaper-engine-kde-plugin](https://github.com/catsout/wallpaper-engine-kde-plugin)
- [RePKG](https://github.com/notscuffed/repkg)
- All open-source libraries used in this project
