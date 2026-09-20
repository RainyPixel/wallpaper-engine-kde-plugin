#pragma once

#include <QList>
#include <QString>

#include "SteamPaths.hpp"

namespace wehypr
{

// One entry of the wallpaper browser. Types the host cannot play are listed
// with supported == false instead of being dropped.
struct Wallpaper {
    QString id;      // item directory name: workshop id, or a slug for local projects
    QString path;    // the item directory, absolute and canonical
    QString title;   // project.json title, the directory name when it has none
    QString type;    // project.json type, lowercased
    QString preview; // relative thumbnail name, empty when project.json has none
    QString file;    // relative entry from project.json
    qint64  mtime { 0 };
    bool    supported { false };
};

// Every wallpaper installed in the Steam libraries on this machine.
QList<Wallpaper> scanWallpapers();

// scanWallpapers() restricted to the given libraries. Lets tests run against a
// temporary directory instead of a real Steam installation.
QList<Wallpaper> scanWallpapersIn(const QList<steam::Library>& libraries);

// Turns a bare workshop id into the item directory holding it. An existing
// path, or anything that is not all digits, is returned unchanged.
QString resolveProject(const QString& projectOrId);

// resolveProject() against an explicit library list, for tests.
QString resolveProjectIn(const QString& projectOrId, const QList<steam::Library>& libraries);

} // namespace wehypr
