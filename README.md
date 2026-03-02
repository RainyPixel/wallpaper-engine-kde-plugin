# Wallpaper Engine for KDE (Plasma 6)

A wallpaper plugin integrating [Wallpaper Engine](https://store.steampowered.com/app/431960/Wallpaper_Engine) into KDE Plasma wallpaper settings.

> **This is a maintained fork** of the original [catsout/wallpaper-engine-kde-plugin](https://github.com/catsout/wallpaper-engine-kde-plugin) with improvements for Plasma 6.

## Changes in this fork

- **Removed Python dependency** — file operations now use native C++ (no more `python-websockets` issues)
- **Fixed KDE 6.5+ theme reactivity** — UI elements no longer become invisible when switching between light/dark themes
- **Plasma 6 / Qt6 support**

## Install

### Arch Linux (AUR)
```sh
yay -S wallpaper-engine-kde-plugin-git
# or
paru -S wallpaper-engine-kde-plugin-git
```

### Fedora / rpm-ostree / Bazzite (RPM)

Download the latest RPM from [Releases](https://github.com/CaptSilver/wallpaper-engine-kde-plugin/releases):

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
base-devel mpv qt6-declarative qt6-websockets qt6-webchannel vulkan-headers cmake lz4
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
    qt6-qtwebchannel-devel qt6-qtwebsockets-devel cmake extra-cmake-modules
```

#### Build and Install
```sh
# Download source
git clone https://github.com/captsilver/wallpaper-engine-kde-plugin.git
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

```sh
git clone https://github.com/captsilver/wallpaper-engine-kde-plugin.git
cd wallpaper-engine-kde-plugin

# Install build dependencies from spec
sudo dnf builddep ./rpm/wek.spec

# Initialise submodules
git submodule update --init --force --recursive

# Copy QML plugin files (required at runtime)
mkdir -p ~/.local/share/plasma/wallpapers/com.github.catsout.wallpaperEngineKde/
cp -R ./plugin/* ~/.local/share/plasma/wallpapers/com.github.catsout.wallpaperEngineKde/

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
3. Under **Steam Library**, point to the folder containing your `steamapps` directory
   - Usually `~/.local/share/Steam`
   - *Wallpaper Engine* must be installed in this library
4. Your subscribed Workshop wallpapers will appear in the list — select one and click **Apply**

> **Note:** After an rpm-ostree/Bazzite install you may need to reboot before the plugin starts working. For cmake installs, restarting plasmashell is enough: `systemctl --user restart plasma-plasmashell.service`

### Uninstall
1. Remove files listed in `build/install_manifest.txt`
2. `kpackagetool6 -t Plasma/Wallpaper -r com.github.catsout.wallpaperEngineKde`

## Usage
1. *Wallpaper Engine* installed on Steam
2. Subscribe to some wallpapers on the Workshop
3. Select the *steamlibrary* folder on the Wallpapers tab of this plugin
   - The *steamlibrary* which contains the *steamapps* folder
   - This is usually `~/.local/share/Steam` by default
   - *Wallpaper Engine* needs to be installed in this *steamlibrary*

## Requirements
- KDE Plasma 6
- Qt 6
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
