#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>

namespace wekde
{

class FileHelper : public QObject {
    Q_OBJECT

public:
    explicit FileHelper(QObject* parent = nullptr);
    virtual ~FileHelper();

    // File operations
    Q_INVOKABLE QByteArray  readFile(const QString& path);
    Q_INVOKABLE qint64      getDirSize(const QString& path, int depth = 3);
    Q_INVOKABLE QVariantMap getFolderList(const QString& path, const QVariantMap& opt = {});

    // Steam auto-detection. configuredLibrary is the library the user picked
    // by hand, empty when there is none; it takes precedence over detection so
    // the folder button stays a real override. The map holds:
    //   library      the library root to seed SteamLibraryPath with, "" if none
    //   projectDirs  every wallpaper directory found, across all libraries
    //   assets       <install>/assets, "" if the app is not installed
    //   globalConfig <install>/config.json, "" if the app is not installed
    // All values are native paths without a trailing slash, deduplicated.
    Q_INVOKABLE QVariantMap detectSteam(const QString& configuredLibrary = {});
    // detectSteam() restricted to the given Steam roots, for tests.
    QVariantMap detectSteamIn(const QStringList& steamRoots, const QString& configuredLibrary = {});

    // Wallpaper config operations
    Q_INVOKABLE QVariantMap readWallpaperConfig(const QString& id);
    Q_INVOKABLE void        writeWallpaperConfig(const QString& id, const QVariantMap& changed);
    Q_INVOKABLE void        resetWallpaperConfig(const QString& id);

private:
    QString configDir() const;
    QString wallpaperConfigDir() const;
    QString wallpaperConfigFile(const QString& id) const;
};

} // namespace wekde
