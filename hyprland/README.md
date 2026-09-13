# Wallpaper Engine web wallpapers on Hyprland

`wallpaper-engine-hyprland` shows [Wallpaper Engine](https://store.steampowered.com/app/431960/Wallpaper_Engine)
web wallpapers as a native Wayland background on Hyprland, including Omarchy. It does not need
KDE Plasma.

This directory is an addition to
[RainyPixel/wallpaper-engine-kde-plugin](https://github.com/RainyPixel/wallpaper-engine-kde-plugin),
which is based on [catsout/wallpaper-engine-kde-plugin](https://github.com/catsout/wallpaper-engine-kde-plugin).
The KDE Plasma plugin in the repository root is unchanged and keeps its own build. Why the host
lives next to the plugin instead of in a separate project is described in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Status

Only **web** wallpapers are supported. Scene and video wallpapers are rejected with an error;
they need the renderers in `src/backend_scene` and `src/backend_mpv` to be wired into the host,
which has not been done yet.

| Feature | Hyprland host | KDE plugin |
| --- | --- | --- |
| Web wallpapers | yes | yes |
| Video wallpapers | no | yes |
| Scene wallpapers | no | yes |
| Wallpaper browser and settings UI | no, command line only | yes |
| User properties | via command line, validated, not saved | yes, saved |
| Pause and resume | manual, the page is frozen | automatic rules |
| Pause on fullscreen, battery or lock | no | yes |
| Mouse input and interactive wallpapers | no, surfaces ignore input | yes |
| Audio | muted unless `--audio` | yes |
| Several monitors | one surface per selected output, reconnects | per screen |

The host was developed for Hyprland on Omarchy 4 with Qt 6.11 and layer-shell-qt 6.7. It uses
only the `wlr-layer-shell` protocol, but other compositors have not been tried. Debian and
Fedora package names below are provided for convenience and are untested.

## Build

The host needs Qt 6.7 or newer with Qt Quick, Qt WebEngine and the Wayland platform plugin,
and LayerShellQt for Qt 6. The git submodules of the scene renderer are not needed.

Arch Linux and Omarchy:

```sh
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-declarative qt6-webengine \
    qt6-wayland layer-shell-qt python
```

Fedora:

```sh
sudo dnf install cmake ninja-build gcc-c++ qt6-qtbase-devel qt6-qtdeclarative-devel \
    qt6-qtwebengine-devel qt6-qtwayland layer-shell-qt-devel python3
```

Debian and Ubuntu (a release with Qt 6.7 or newer):

```sh
sudo apt install cmake ninja-build g++ qt6-base-dev qt6-declarative-dev qt6-webengine-dev \
    qt6-wayland qml6-module-qtquick qml6-module-qtwebengine liblayershellqtinterface-dev python3
```

From the repository root:

```sh
cmake -B build/hyprland -S hyprland -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/hyprland
ctest --test-dir build/hyprland -L unit --output-on-failure

# optional
cmake --install build/hyprland --prefix ~/.local
```

Without Ninja, drop `-G Ninja`. Pass `-DWEHYPR_BUILD_TESTS=OFF` to skip the tests.

## Usage

A project is a Wallpaper Engine project directory (or its `project.json`) with `"type": "web"`.
Workshop items are usually in `~/.local/share/Steam/steamapps/workshop/content/431960/<id>`.

```sh
wallpaper-engine-hyprland check ~/wallpapers/my-web-wallpaper
wallpaper-engine-hyprland outputs
wallpaper-engine-hyprland run ~/wallpapers/my-web-wallpaper --output eDP-1
```

`run` stays in the foreground until it gets `quit`, Ctrl+C or SIGTERM. When only one output is
connected it is used automatically. With several outputs, pass `--output NAME` (repeatable) or
`--all-outputs`. A requested output that is not connected yet, or is disconnected later, gets
its surface as soon as it appears; at least one requested output must be connected at start.

From another terminal:

```sh
wallpaper-engine-hyprland status
wallpaper-engine-hyprland pause
wallpaper-engine-hyprland resume
wallpaper-engine-hyprland set-properties '{"speed": 0.7, "brightness": {"value": 0.6}}'
wallpaper-engine-hyprland quit
```

`status` reports every surface separately (`surfaces`, `missingOutputs`); `allLoaded` and
`allFrozen` are only true when they hold for all surfaces.

Log messages go to stderr when the host runs in a terminal. Without a controlling terminal, Qt may
send them to the systemd journal instead; set `QT_FORCE_STDERR_LOGGING=1` to keep them on stderr.

Property names and values are checked against `general.properties` in `project.json`: sliders
must be numbers within `min`/`max`, combos one of their options, and so on. Changes are passed to
`wallpaperPropertyListener.applyUserProperties` and are lost when the host stops; use
`run --properties '{...}'` for values that should apply at start.

Several hosts can run side by side with different `--instance` names, for example a different
wallpaper per monitor.

### Options for `run`

- `--fps N` is passed to the page through `applyGeneralProperties`. The page has to honour it;
  the host does not limit Chromium's frame rate.
- `--audio` unmutes the page. Audio is muted by default.
- `--allow-remote` lets the local page load remote URLs (fonts, APIs). Off by default.
- `--gpu-rasterization off|auto`: `off` (default) starts Chromium with
  `--disable-gpu-rasterization` for this process only, because GPU rasterization lost its
  Skia context on the laptop the host was developed on. Page compositing still runs on the GPU.
  Use `auto` to let Chromium decide.
- `--window` opens a normal window instead of a background surface, for previews. Closing the
  window stops the host.
- `--diagnostics` logs page console output and polls `window.wallpaperDiagnostics()` if the page
  defines it; the result appears in `status`.
- `--duration N` stops after N seconds.

### Autostart

Nothing is installed into the Hyprland or Omarchy configuration. To start the wallpaper with the
session, add a line like this to `~/.config/hypr/autostart.conf` on Omarchy, or to
`hyprland.conf` elsewhere:

```
exec-once = uwsm app -- wallpaper-engine-hyprland run /path/to/project --output eDP-1
```

Without uwsm, use `exec-once = wallpaper-engine-hyprland run ...`.

## Behaviour on Hyprland and Omarchy

Each surface is a layer-shell surface with namespace `wallpaper-engine-hyprland` on the `bottom`
layer, anchored to all edges, with exclusive zone `-1`, no keyboard focus and no input. Windows,
bars and notifications stay above it and clicks go to the desktop below.

Omarchy 4 draws its static background with Quickshell on the `background` layer. That surface is
left alone and stays visible below the host, so it reappears whenever the host stops or crashes.
Changing the Omarchy theme changes that static background, not the web wallpaper.

Pausing first calls `wallpaperPropertyListener.setPaused(true)`, then shows a still image of the
last frame and freezes the page in Chromium, so its scripts and timers stop. Property changes made
while paused are delivered on resume.

Whenever the page starts loading a new document, its surface reports `loaded: false` until the
load finishes, and user properties and the pause state are applied again to the new document.
Navigations the host refuses leave the current page running; each surface counts them in `status`
as `blockedNavigations` together with `lastBlockedNavigation`. If the page never loaded, a failed
load stops the host with exit code 1. A failed load after that, or an exited web renderer, loads
the project entry again after one second; the fourth such recovery on a surface stops the host
with exit code 1. `status` shows the count as `recoveries`.

## Security

Web wallpapers are full web pages. They run in Chromium's sandbox, but only trusted projects
should be used. The host additionally limits top-level navigation to files inside the project
directory, blocks remote content unless `--allow-remote` is given, rejects JavaScript, file and
authentication dialogs, and never passes input to the page.

The control socket is `$XDG_RUNTIME_DIR/wallpaper-engine-hyprland/<instance>.sock`, accessible
only to the current user. Requests are one JSON object per line, for example
`{"protocol":1,"command":"status"}`, limited to 64 KiB, eight concurrent clients and two seconds
per client.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | success |
| 1 | runtime failure, e.g. the page never loaded or kept failing after recoveries |
| 2 | invalid command line |
| 3 | invalid project or property values |
| 4 | no Wayland session, runtime directory problem, or no matching output |
| 5 | the instance is already running |
| 6 | no running host for the instance |
| 7 | the host rejected the request |

## Tests

Unit tests (`ctest -L unit`) cover project validation, property checks, command line parsing,
output selection, the lock and the control socket, and run the binary for commands that do not
open windows. They need no display.

The smoke test opens a real background surface for about a minute on the running session. It
uses the fixture in `tests/fixtures/web-basic`, picks the focused Hyprland monitor or the first
output unless `--output` is given, and checks page load, animation, layer placement (when
`hyprctl` is available), property updates, a page reload, a refused navigation, recovery from a
failed load, the singleton lock, freeze and resume, recovery from a killed web renderer while
paused, `quit` and SIGTERM. The fixture reloads or navigates when its `trigger` property changes.
Closing a `--window` preview is not covered because it needs the compositor to close the window.

```sh
python3 hyprland/tests/integration/smoke_test.py \
    --binary build/hyprland/wallpaper-engine-hyprland \
    [--output NAME] [--expect-background omarchy-background]
```

It can also be registered with CTest via `-DWEHYPR_INTEGRATION_TESTS=ON` and run with
`ctest --test-dir build/hyprland -L integration --output-on-failure`. It exits with 77 (skipped)
outside a Wayland session.

## Known limitations

- Every output runs its own web page, so CPU and GPU use grow with the number of outputs.
- Session lock, idle, fullscreen windows and battery state are not detected.
- `file`, `directory` and `text` properties cannot be changed at runtime.
- The Wallpaper Engine audio visualizer and media APIs are not provided.

## License and credits

GPL-2.0, see [LICENSE](../LICENSE). Based on the work of the
[catsout](https://github.com/catsout/wallpaper-engine-kde-plugin) and
[RainyPixel](https://github.com/RainyPixel/wallpaper-engine-kde-plugin) projects and their
contributors.
