# Hyprland host: design decision

## Decision

The Hyprland support lives in this repository as a second host next to the KDE Plasma
plugin, not as a separate product. The KDE build (`cmake -S .`) is unchanged. The Hyprland
host is a standalone CMake project in `hyprland/` (`cmake -S hyprland`), following the same
pattern as the existing standalone builds in `tests/` and `src/backend_scene/standalone_view/`.

## Reasons

- The KDE plugin is a Plasma wallpaper package. Its QML (`plugin/contents/ui/main.qml` and the
  backends) is bound to Plasma objects such as `WallpaperItem`, `wallpaper.configuration` and
  Plasma window/power models, and the root `CMakeLists.txt` requires ECM, KF6 Package and
  libplasma. None of that exists on Hyprland, so the Plasma QML cannot be hosted directly.
- The renderers are the reusable part. `src/backend_mpv` is a plain `QQuickRhiItem` and
  `src/backend_scene/qml_helper` is a plain `QQuickItem`; neither depends on Plasma. A native
  Qt Quick host can load them later without forking them.
- Web wallpapers only need Qt WebEngine, which is a system library. The Wallpaper Engine web
  API surface used by the KDE backend (`applyUserProperties`, `applyGeneralProperties`,
  `setPaused`) is small enough to implement in the host without touching the KDE QML.
- Keeping one repository keeps upstream history, licenses and renderer fixes mergeable. The
  Hyprland host adds files under `hyprland/` and a short README section, so upstream merges
  touch different files.

A standalone product would only be justified if the renderers had to diverge from upstream.
They do not at this stage.

## Host shape

- One process, `wallpaper-engine-hyprland`, built with Qt 6 and LayerShellQt.
- One `QQuickView` per selected output, each a `wlr-layer-shell` surface on the `bottom` layer,
  anchored to all edges, exclusive zone `-1`, no keyboard interactivity and transparent for
  input. Omarchy 4 draws its static background on the `background` layer, so it stays visible
  below the host and is shown again when the host exits.
- Qt Quick runs on OpenGL. Qt WebEngine requires `QtWebEngineQuick::initialize()` before the
  `QGuiApplication` exists, and the scene renderer's Vulkan/OpenGL interop needs an OpenGL
  scene graph as well.
- Project validation, command line parsing, output selection and the local IPC protocol are in
  a small Qt Core/Network library (`wehypr-core`) that has unit tests without a display.
- Control uses a per-user, per-instance Unix socket in `$XDG_RUNTIME_DIR` guarded by a lock
  file. Requests are single JSON lines with size, client count and idle time limits.

## Extension points

- Video: register `mpv::MpvObject` from `src/backend_mpv` in the host and add a video surface
  component selected by `project.json` type. Needs libmpv and `LC_NUMERIC=C` after the
  `QGuiApplication` is created.
- Scene: build `src/backend_scene` with `BUILD_QML=ON` (requires the git submodules, Vulkan
  and lz4), register `scenebackend::SceneObject`, and pass the Wallpaper Engine `assets`
  directory from the Steam installation.
- Automatic pause (fullscreen windows, battery, session lock) would use Hyprland IPC, UPower
  and logind instead of the Plasma models.

These are not implemented. The host rejects `scene` and `video` projects with a clear error
instead of showing an empty surface.
