#include "FileHelper.hpp"
#include "SteamPaths.hpp"
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDateTime>
#include <QHash>
#include <QSet>

namespace wekde
{

namespace
{

QVariantMap packLibraries(const QList<steam::Library>& libraries,
                          const QString&               configuredLibrary) {
    const QString configured = steam::canonicalPath(configuredLibrary);

    // A library the user picked by hand answers for assets/ and config.json
    // whenever it actually carries the app, so pointing the folder button at a
    // second Steam install is not silently overruled by detection order.
    const steam::Library* chosen    = nullptr;
    const steam::Library* installed = nullptr;
    for (const steam::Library& lib : libraries) {
        if (! configured.isEmpty() && lib.root == configured) chosen = &lib;
        if (installed == nullptr && ! lib.installDir.isEmpty()) installed = &lib;
    }
    const steam::Library* source =
        (chosen != nullptr && ! chosen->installDir.isEmpty()) ? chosen : installed;

    // Every root here is canonical, so plain string comparison deduplicates
    // correctly even when the same library is reached through a symlink.
    QStringList   projectDirs;
    QSet<QString> seen;
    const auto    add = [&](const QString& dir) {
        if (dir.isEmpty() || seen.contains(dir)) return;
        seen.insert(dir);
        projectDirs.append(dir);
    };
    for (const steam::Library& lib : libraries) add(lib.workshopDir);
    if (source != nullptr) {
        add(steam::defaultProjectsDir(source->installDir));
        add(steam::myProjectsDir(source->installDir));
    }

    QVariantMap result;
    result["library"]     = libraries.isEmpty() ? QString() : libraries.first().root;
    result["projectDirs"] = projectDirs;
    result["assets"]      = source != nullptr ? steam::assetsDir(source->installDir) : QString();
    result["globalConfig"] =
        source != nullptr ? steam::globalConfigPath(source->installDir) : QString();
    return result;
}

} // namespace

FileHelper::FileHelper(QObject* parent): QObject(parent) {
    // Ensure config directory exists
    QDir dir(wallpaperConfigDir());
    if (! dir.exists()) {
        dir.mkpath(".");
    }
}

FileHelper::~FileHelper() {}

QString FileHelper::configDir() const {
    QString xdgConfig = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return xdgConfig + "/wekde";
}

QString FileHelper::wallpaperConfigDir() const { return configDir() + "/wallpaper"; }

QString FileHelper::wallpaperConfigFile(const QString& id) const {
    return wallpaperConfigDir() + "/" + id + ".json";
}

QByteArray FileHelper::readFile(const QString& path) {
    QFile file(path);
    if (! file.open(QIODevice::ReadOnly)) {
        qWarning() << "FileHelper: Cannot open file:" << path;
        return QByteArray();
    }
    return file.readAll();
}

qint64 FileHelper::getDirSize(const QString& path, int depth) {
    qint64 totalSize = 0;
    QDir   dir(path);

    if (! dir.exists()) {
        return 0;
    }

    if (depth <= 0) {
        // Recursive with no depth limit
        QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            totalSize += it.fileInfo().size();
        }
    } else {
        std::function<qint64(const QString&, int)> calcSize = [&](const QString& dirPath,
                                                                  int currentDepth) -> qint64 {
            if (currentDepth > depth) return 0;

            qint64 size = 0;
            QDir   d(dirPath);

            for (const QFileInfo& info : d.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
                size += info.size();
            }

            if (currentDepth < depth) {
                for (const QFileInfo& info : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    size += calcSize(info.absoluteFilePath(), currentDepth + 1);
                }
            }

            return size;
        };

        totalSize = calcSize(path, 1);
    }

    return totalSize;
}

QVariantMap FileHelper::getFolderList(const QString& path, const QVariantMap& opt) {
    QVariantMap result;

    bool        onlyDir   = opt.value("only_dir", true).toBool();
    QStringList fallbacks = opt.value("fallbacks", QStringList()).toStringList();

    // Find first existing directory
    QString folder = path;
    QDir    dir(folder);

    if (! dir.exists()) {
        for (const QString& fb : fallbacks) {
            QDir fbDir(fb);
            if (fbDir.exists()) {
                folder = fb;
                dir    = fbDir;
                break;
            }
        }
    }

    if (! dir.exists()) {
        return QVariantMap(); // Return null/empty
    }

    result["folder"] = folder;

    QVariantList  items;
    QDir::Filters filters = onlyDir ? QDir::Dirs : (QDir::Dirs | QDir::Files);
    filters |= QDir::NoDotAndDotDot;

    for (const QFileInfo& info : dir.entryInfoList(filters)) {
        QVariantMap item;
        item["name"]  = info.fileName();
        item["mtime"] = static_cast<qint64>(info.lastModified().toSecsSinceEpoch());
        items.append(item);
    }

    result["items"] = items;
    return result;
}

QVariantMap FileHelper::detectSteam(const QString& configuredLibrary) {
    // plasmashell builds one wallpaper item per screen and each one asks for
    // this, but the walk gives the same answer every time, so it runs once per
    // configured library per process instead of once per screen.
    static QHash<QString, QVariantMap> cache;
    const auto                         cached = cache.constFind(configuredLibrary);
    if (cached != cache.constEnd()) return *cached;

    // A library the user picked by hand is not necessarily reachable from any
    // of the standard Steam roots, so it is probed as a root of its own.
    QStringList roots;
    if (! configuredLibrary.isEmpty()) roots.append(configuredLibrary);
    roots.append(steam::steamRoots());

    const QVariantMap result = detectSteamIn(roots, configuredLibrary);
    cache.insert(configuredLibrary, result);
    return result;
}

QVariantMap FileHelper::detectSteamIn(const QStringList& steamRoots,
                                      const QString&     configuredLibrary) {
    return packLibraries(steam::detectLibrariesIn(steamRoots), configuredLibrary);
}

QVariantMap FileHelper::readWallpaperConfig(const QString& id) {
    QString filePath = wallpaperConfigFile(id);
    QFile   file(filePath);

    if (! file.exists()) {
        return QVariantMap();
    }

    if (! file.open(QIODevice::ReadOnly)) {
        qWarning() << "FileHelper: Cannot read config:" << filePath;
        return QVariantMap();
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isNull() || ! doc.isObject()) {
        return QVariantMap();
    }

    return doc.object().toVariantMap();
}

void FileHelper::writeWallpaperConfig(const QString& id, const QVariantMap& changed) {
    // Read existing config
    QVariantMap config = readWallpaperConfig(id);

    // Merge changes
    for (auto it = changed.constBegin(); it != changed.constEnd(); ++it) {
        config[it.key()] = it.value();
    }

    // Write back
    QString filePath = wallpaperConfigFile(id);
    QFile   file(filePath);

    if (! file.open(QIODevice::WriteOnly)) {
        qWarning() << "FileHelper: Cannot write config:" << filePath;
        return;
    }

    QJsonDocument doc(QJsonObject::fromVariantMap(config));
    file.write(doc.toJson());
}

void FileHelper::resetWallpaperConfig(const QString& id) {
    QString filePath = wallpaperConfigFile(id);
    QFile::remove(filePath);
}

} // namespace wekde
