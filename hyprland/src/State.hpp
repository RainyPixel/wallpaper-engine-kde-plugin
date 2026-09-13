#pragma once

#include <QString>

namespace wehypr
{

// What the host remembers between runs, so a wallpaper picked in the GUI
// survives a logout. One key today.
struct State {
    QString project;
};

// <config>/wallpaper-engine-hyprland/<instance>.json, one file per instance.
QString statePath(const QString& instance);

// A missing, unreadable or corrupt state file reads as a default State.
State loadState(const QString& instance);

// Writes through a temporary file, so a crash cannot leave an unreadable state.
bool saveState(const QString& instance, const State& state);

} // namespace wehypr
