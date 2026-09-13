#include "Library.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include <optional>

namespace wehypr
{

namespace
{
constexpr qint64 k_maxProjectFileSize = 4 * 1024 * 1024;

// steamapps/workshop/downloads/431960 and steamapps/workshop/temp hold items
// Steam is still writing, with a partial or missing project.json. They sit next
// to content/, so a scan of workshopDir never walks into them; this only guards
// a hand-built Library pointed at one of them. Only the segments below the
// library's steamapps/ are inspected, or a library stored under a path such as
// /mnt/workshop/temp/SteamLibrary would drop out of the browser entirely.
bool isInFlightDir(const QString& path) {
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    qsizetype         start = 0;
    for (qsizetype i = parts.size() - 1; i >= 0; --i) {
        if (parts.at(i).compare(QLatin1String("steamapps"), Qt::CaseInsensitive) == 0) {
            start = i + 1;
            break;
        }
    }
    for (qsizetype i = start + 1; i < parts.size(); ++i) {
        if (parts.at(i - 1).compare(QLatin1String("workshop"), Qt::CaseInsensitive) != 0) continue;
        const QString& next = parts.at(i);
        if (next.compare(QLatin1String("downloads"), Qt::CaseInsensitive) == 0 ||
            next.compare(QLatin1String("temp"), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

// Same reading and BOM handling as loadProject(), but without its web-only
// rules: a browser has to list the items it cannot play.
std::optional<QJsonObject> readProjectJson(const QString& itemDir) {
    const QFileInfo info(QDir(itemDir).filePath(QStringLiteral("project.json")));
    if (! info.isFile() || info.size() > k_maxProjectFileSize) return std::nullopt;

    QFile file(info.absoluteFilePath());
    if (! file.open(QIODevice::ReadOnly)) return std::nullopt;
    QByteArray data = file.read(k_maxProjectFileSize + 1);
    if (data.startsWith("\xEF\xBB\xBF")) data.remove(0, 3);

    QJsonParseError parseError;
    const auto      doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || ! doc.isObject()) return std::nullopt;
    return doc.object();
}

void scanDir(const QString& dir, QList<Wallpaper>& out, QSet<QString>& seen) {
    if (dir.isEmpty() || isInFlightDir(dir)) return;

    const QFileInfoList entries =
        QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& entry : entries) {
        const QString itemDir = entry.canonicalFilePath();
        if (itemDir.isEmpty() || seen.contains(itemDir)) continue;
        // One item without a readable project.json must not end the scan.
        const auto root = readProjectJson(itemDir);
        if (! root) continue;
        seen.insert(itemDir);

        Wallpaper wallpaper;
        wallpaper.id      = entry.fileName();
        wallpaper.path    = itemDir;
        wallpaper.title   = root->value(QLatin1String("title")).toString();
        wallpaper.type    = root->value(QLatin1String("type")).toString().toLower();
        wallpaper.preview = root->value(QLatin1String("preview")).toString();
        wallpaper.file    = root->value(QLatin1String("file")).toString();
        if (wallpaper.title.isEmpty()) wallpaper.title = entry.fileName();
        wallpaper.mtime     = entry.lastModified().toMSecsSinceEpoch();
        wallpaper.supported = wallpaper.type == QLatin1String("web");
        out.append(wallpaper);
    }
}
} // namespace

QList<Wallpaper> scanWallpapersIn(const QList<steam::Library>& libraries) {
    QList<Wallpaper> wallpapers;
    QSet<QString>    seen;
    for (const steam::Library& library : libraries) {
        scanDir(library.workshopDir, wallpapers, seen);
        scanDir(steam::defaultProjectsDir(library.installDir), wallpapers, seen);
        scanDir(steam::myProjectsDir(library.installDir), wallpapers, seen);
    }
    return wallpapers;
}

QList<Wallpaper> scanWallpapers() { return scanWallpapersIn(steam::detectLibraries()); }

QString resolveProjectIn(const QString& projectOrId, const QList<steam::Library>& libraries) {
    if (projectOrId.isEmpty()) return projectOrId;
    for (const QChar character : projectOrId) {
        if (character < QLatin1Char('0') || character > QLatin1Char('9')) return projectOrId;
    }
    // A directory literally named after an id wins over the workshop lookup.
    if (QFileInfo::exists(projectOrId)) return projectOrId;

    for (const steam::Library& library : libraries) {
        const QString candidate = steam::resolvePath(library.workshopDir, { projectOrId });
        // An unsubscribe or an aborted download leaves the item directory
        // behind with nothing in it; that must not shadow a real copy of the
        // same id in another library.
        if (candidate.isEmpty()) continue;
        if (QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("project.json"))))
            return candidate;
    }
    return projectOrId;
}

QString resolveProject(const QString& projectOrId) {
    return resolveProjectIn(projectOrId, steam::detectLibraries());
}

} // namespace wehypr
