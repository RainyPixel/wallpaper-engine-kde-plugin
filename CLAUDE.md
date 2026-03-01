# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Wallpaper Engine KDE Plugin — a plugin for KDE Plasma 6 that integrates Wallpaper Engine (Steam) wallpapers into the Linux desktop. Supports Scene (2D), Web, and Video wallpapers.

## Build

```bash
# Initialize submodules (required)
git submodule update --init --force --recursive

# Build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Install
sudo cmake --install build

# Restart plasmashell
systemctl --user restart plasma-plasmashell.service
```

### Standalone viewer for debugging

```bash
# In the src/backend_scene/standalone_view/ directory
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Run with Vulkan validation layers
./sceneviewer --valid-layer <steamapps>/common/wallpaper_engine/assets <steamapps>/workshop/content/431960/<id>/scene.pkg
```

## Architecture

### Main components

- **src/** — C++ code for the QML plugin
  - `plugin.cpp` — QML plugin entry point
  - `FileHelper.cpp` — native C++ helper for file operations (replaces Python)
  - `MouseGrabber` — mouse event capture
  - `TTYSwitchMonitor` — TTY switch monitoring via D-Bus

- **src/backend_mpv/** — MPV backend for video wallpapers
  - Uses libmpv for playback
  - Static library `mpvbackend`

- **src/backend_scene/** — Vulkan renderer for Scene wallpapers
  - `wescene-renderer` — main rendering library
  - `wescene-renderer-qml` — Qt/QML wrapper
  - Requires Vulkan 1.1+

### backend_scene subsystems

- **VulkanRender/** — Vulkan rendering and resource management
- **RenderGraph/** — automatic pass dependency resolution
- **Scene/** — scene parsing and representation
- **Particle/** — particle system
- **Audio/** — audio via miniaudio
- **Vulkan/** — base Vulkan abstraction
- **WP*Parser.cpp** — Wallpaper Engine format parsers (JSON, MDL, shaders, textures)

### QML UI

- **plugin/contents/ui/** — QML settings interface
  - `main.qml` — main plugin window
  - `config.qml` — configuration page
  - `WallpaperListModel.qml` — wallpaper list model

### Third-party libraries (in src/backend_scene/third_party/)

- Eigen — linear algebra
- glslang — GLSL to SPIR-V compilation
- SPIRV-Reflect — SPIR-V shader reflection
- nlohmann/json — JSON parsing
- miniaudio — cross-platform audio

## Dependencies

### Arch Linux
```bash
sudo pacman -S extra-cmake-modules libplasma gst-libav ninja \
base-devel mpv qt6-declarative qt6-websockets qt6-webchannel vulkan-headers cmake lz4
```

### Fedora
```bash
# Add RPM Fusion repos (required for ffmpeg/mpv)
sudo dnf install -y \
    https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm \
    https://mirrors.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm

sudo dnf swap -y ffmpeg-free ffmpeg --allowerasing
sudo dnf install -y ffmpeg-devel --allowerasing

sudo dnf install vulkan-headers plasma-workspace-devel kf6-plasma-devel \
    kf6-kcoreaddons-devel kf6-kpackage-devel gstreamer1-libav \
    lz4-devel mpv-libs-devel qt6-qtbase-private-devel libplasma-devel \
    qt6-qtwebchannel-devel qt6-qtwebsockets-devel cmake extra-cmake-modules
```

## Debugging

plasmashell logs:
```bash
journalctl /usr/bin/plasmashell -f
# or
plasmashell --replace
```

Install `vulkan-validation-layers` for Vulkan debugging.

## Code Style

- C++20
- Formatting: `.clang-format` (4 spaces, 100 character line width)
- Qt6 / KF6 / Plasma 6
