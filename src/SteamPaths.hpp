#pragma once
#include <QList>
#include <QString>
#include <QStringList>

// Locating a Steam installation and the Wallpaper Engine content inside it.
// Qt Core only, no QObject: the KDE plugin reaches this through FileHelper,
// the Hyprland host compiles the same file into wehypr-core.
namespace steam
{

// A Steam library: any directory that contains a "steamapps" folder. The app
// and its workshop content may live in different libraries, so both members
// are filled independently and either one may be empty.
struct Library {
    QString root;        // the library dir itself, no trailing slash
    QString workshopDir; // <root>/steamapps/workshop/content/431960
    QString installDir;  // <root>/steamapps/common/<installdir> of app 431960
};

// Steam installation roots that exist on this machine, deduplicated by
// canonical path. Covers native, Debian bootstrap, Flatpak and Snap layouts.
QStringList steamRoots();

// Every library root reachable from one Steam installation root, starting with
// the root itself. Reads steamapps/libraryfolders.vdf and the newer
// config/libraryfolders.vdf, merging both when present.
QStringList libraryRoots(const QString& steamRoot);

// Libraries holding Wallpaper Engine content. Libraries with neither workshop
// content nor an install are dropped, so an empty result means "not found".
// The library that has an installDir comes first: it owns assets/ and
// config.json, which exist only once.
QList<Library> detectLibraries();

// detectLibraries() restricted to the given Steam roots. Lets tests run
// against a temporary directory instead of a real Steam installation.
QList<Library> detectLibrariesIn(const QStringList& steamRoots);

// The canonical form of a path, with symlinks resolved. Falls back to a
// cleaned path when it does not exist. Detected paths are always canonical, so
// comparing a user-supplied path against one has to go through this.
QString canonicalPath(const QString& path);

// Resolves base + "/" + segments against the real on-disk spelling, trying the
// exact name first and falling back to a case-insensitive match per segment.
// Returns an empty string when a segment does not exist. Steam libraries
// shared with a Windows install carry "SteamApps" and "Workshop/Content", and
// ntfs-3g matches case-sensitively.
QString resolvePath(const QString& base, const QStringList& segments);

// The Wallpaper Engine directories derived from a library, each an empty
// string when absent. installDir is the value from Library::installDir.
QString assetsDir(const QString& installDir);
QString globalConfigPath(const QString& installDir);
QString defaultProjectsDir(const QString& installDir);
QString myProjectsDir(const QString& installDir);

} // namespace steam
