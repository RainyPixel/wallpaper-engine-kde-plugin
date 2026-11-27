#include "WallpaperEngineFollower.hpp"
#include "SteamPaths.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

using namespace wekde;

namespace
{
QJsonObject readObject(const QString& path, qint64 limit) {
    if (path.isEmpty() || ! QFileInfo(path).isFile()) return {};
    QFile file(path);
    if (! file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > limit) return {};
    return QJsonDocument::fromJson(bytes).object();
}

QStringList safeParts(const QString& path) {
    const auto parts = path.split(u'/');
    for (const auto& part : parts) {
        if (part.isEmpty() || part == "." || part == ".." || part.contains(QChar::Null)) return {};
    }
    return parts;
}

// Resolve case-insensitive Steam names one component at a time, keeping
// symlinks in the approved tree before inspecting the next component.
QString resolveInside(const QString& base, const QStringList& parts) {
    if (parts.isEmpty()) return {};
    QString current = base;
    for (const auto& part : parts) {
        current = QFileInfo(steam::resolvePath(current, { part })).canonicalFilePath();
        if (! current.startsWith(base + u'/')) return {};
    }
    return current;
}

// Layout and last-selected-monitor semantics from Plaer1's contribution
// discussion in RainyPixel/wallpaper-engine-kde-plugin#41.
QString selectedPath(const QJsonObject& general) {
    const auto selected =
        general.value("wallpaperconfig").toObject().value("selectedwallpapers").toObject();
    auto monitor = general.value("browser").toObject().value("lastselectedmonitor").toString();
    if (! selected.contains(monitor)) {
        if (selected.size() == 1)
            monitor = selected.begin().key();
        else
            monitor = QStringLiteral("Monitor0");
    }
    return selected.value(monitor).toObject().value("file").toString();
}

QString rebasedTail(const QString& path, const QString& directory) {
    const auto dir = QFileInfo(directory).fileName();
    QString    match;
    if (dir == "431960")
        match = QStringLiteral("/workshop/content/431960/");
    else if (dir.compare("myprojects", Qt::CaseInsensitive) == 0 ||
             dir.compare("defaultprojects", Qt::CaseInsensitive) == 0)
        match = QStringLiteral("/projects/") + dir + u'/';
    if (match.isEmpty()) return {};

    const auto prefixed = u'/' + path;
    const auto index    = prefixed.indexOf(match, 0, Qt::CaseInsensitive);
    return index < 0 ? QString() : prefixed.mid(index + match.size());
}

QVariantMap readSelection(const QString& base, const QString& tail) {
    const auto parts = safeParts(tail);
    if (parts.size() < 2) return {};

    const auto project = resolveInside(base, { parts.first() });
    if (project.isEmpty() || ! QFileInfo(project).isDir() ||
        ! QFileInfo(resolveInside(project, parts.mid(1))).isFile())
        return {};
    const auto metadata =
        readObject(resolveInside(project, { QStringLiteral("project.json") }), 2 * 1024 * 1024);
    const auto type = metadata.value("type").toString().toLower();
    if (type != "scene" && type != "video" && type != "web") return {};

    auto file = metadata.value("file").toString();
    file.replace(u'\\', u'/');
    const auto media = resolveInside(project, safeParts(file));
    if (! QFileInfo(media).isFile()) return {};
    const auto source = QUrl::fromLocalFile(media).toString(QUrl::FullyEncoded) + u'+' + type;
    return { { QStringLiteral("source"), source },
             { QStringLiteral("projectDirectory"),
               QUrl::fromLocalFile(project).toString(QUrl::FullyEncoded) },
             { QStringLiteral("workshopId"), QFileInfo(project).fileName() } };
}

QVariantMap resolveSelection(QString path, const QStringList& projectDirs) {
    if (path.isEmpty()) return {};
    if (path.startsWith("file:", Qt::CaseInsensitive)) {
        const QUrl url(path, QUrl::StrictMode);
        if (! url.isLocalFile() || ! url.host().isEmpty()) return {};
        path = url.toLocalFile();
    }
    const bool drive           = path.size() > 2 && path.at(0).isLetter() && path.at(1) == u':';
    const bool canUseLocalPath = ! drive || path.at(0).toUpper() == u'Z';
    const bool windows         = drive || path.contains(u'\\');
    if (drive) path.remove(0, 2);
    path.replace(u'\\', u'/');

    if (canUseLocalPath && path.startsWith(u'/')) {
        // A Proton Z: or native path can identify a specific library even
        // when the same Workshop item also exists in another detected library.
        for (const auto& directory : projectDirs) {
            const auto base = QDir(directory).canonicalPath();
            if (base.isEmpty()) continue;
            for (const auto& prefix : { QDir::cleanPath(directory), base }) {
                if (path.startsWith(prefix + u'/'))
                    return readSelection(base, path.mid(prefix.size() + 1));
            }
        }
    }
    if (! windows && path.startsWith(u'/')) return {};
    for (const auto& directory : projectDirs) {
        const auto base = QDir(directory).canonicalPath();
        if (base.isEmpty()) continue;
        const auto selection = readSelection(base, rebasedTail(path, directory));
        if (! selection.isEmpty()) return selection;
    }
    return {};
}
} // namespace

WallpaperEngineFollower::WallpaperEngineFollower(QObject* parent): QObject(parent) {
    m_update.setSingleShot(true);
    m_update.setInterval(75);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] {
        m_update.start();
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        m_update.start();
    });
    connect(&m_update, &QTimer::timeout, this, [this] {
        refresh(true);
    });
}

void WallpaperEngineFollower::configure(const QVariantMap& steamPaths, const QString& section,
                                        bool inspect) {
    const auto assets   = steamPaths.value("assets").toString();
    const auto install  = assets.isEmpty() ? QString() : QFileInfo(assets).dir().canonicalPath();
    const auto projects = steamPaths.value("projectDirs").toStringList();
    if (m_installDir == install && m_projectDirs == projects && m_section == section &&
        m_inspect == inspect)
        return;

    const bool keepSelection = ! m_section.isEmpty() && ! section.isEmpty();
    m_update.stop();
    m_installDir  = install;
    m_projectDirs = projects;
    m_section     = section;
    m_inspect     = inspect;
    refresh(keepSelection);
}

void WallpaperEngineFollower::refresh(bool keepSelection) {
    const bool observe = m_inspect || ! m_section.isEmpty();
    const auto config  = observe && ! m_installDir.isEmpty()
                             ? resolveInside(m_installDir, { QStringLiteral("config.json") })
                             : QString();
    for (const auto& path : m_watcher.files()) {
        if (path != config) m_watcher.removePath(path);
    }
    for (const auto& path : m_watcher.directories()) {
        if (! observe || path != m_installDir) m_watcher.removePath(path);
    }
    if (observe && ! m_installDir.isEmpty() && m_watcher.directories().isEmpty())
        m_watcher.addPath(m_installDir);
    if (! config.isEmpty() && QFileInfo(config).isFile() && m_watcher.files().isEmpty())
        m_watcher.addPath(config);

    const auto  document = readObject(config, 8 * 1024 * 1024);
    QStringList sections;
    for (auto it = document.begin(); it != document.end(); ++it) {
        if (it.key().startsWith("steamuser", Qt::CaseInsensitive) &&
            it.value().toObject().value("general").isObject())
            sections.append(it.key());
    }

    const auto account =
        sections.contains(m_section) ? document.value(m_section).toObject() : QJsonObject();
    auto next = resolveSelection(selectedPath(account.value("general").toObject()), m_projectDirs);
    if (! next.isEmpty())
        next["assets"] =
            QUrl::fromLocalFile(steam::assetsDir(m_installDir)).toString(QUrl::FullyEncoded);
    const auto selection = ! next.isEmpty() || ! keepSelection ? next : m_selection;
    QString    status;
    if (! m_section.isEmpty() && next.isEmpty())
        status = tr("Waiting for a saved wallpaper in the selected Wallpaper Engine profile.");
    else if (m_inspect && sections.isEmpty())
        status = tr("No local Wallpaper Engine profiles found.");

    if (m_sections != sections) {
        m_sections = sections;
        emit sectionsChanged();
    }
    if (m_selection != selection) {
        m_selection = selection;
        emit selectionChanged();
    }
    if (m_status != status) {
        m_status = status;
        emit statusChanged();
    }
}
