#include "State.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace wehypr
{

namespace
{
constexpr qint64 k_maxStateFileSize = 64 * 1024;

QString stateDirectory() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty()) return {};
    return base + QStringLiteral("/wallpaper-engine-hyprland");
}
} // namespace

QString statePath(const QString& instance) {
    // An empty path means "no state" everywhere below, which is the right
    // answer for a name that would write outside the state directory.
    if (instance.isEmpty() || instance.size() > 64) return {};
    for (const QChar character : instance) {
        const bool allowed = character.isLetterOrNumber() || character == QLatin1Char('-') ||
                             character == QLatin1Char('_');
        if (! allowed) return {};
    }
    const QString dir = stateDirectory();
    if (dir.isEmpty()) return {};
    return dir + QLatin1Char('/') + instance + QStringLiteral(".json");
}

State loadState(const QString& instance) {
    State         state;
    const QString path = statePath(instance);
    if (path.isEmpty()) return state;

    QFile file(path);
    if (! file.open(QIODevice::ReadOnly)) return state;
    const auto doc = QJsonDocument::fromJson(file.read(k_maxStateFileSize));
    if (! doc.isObject()) return state;

    state.project = doc.object().value(QLatin1String("project")).toString();
    return state;
}

bool saveState(const QString& instance, const State& state) {
    const QString path = statePath(instance);
    if (path.isEmpty() || ! QDir().mkpath(stateDirectory())) return false;

    // QSaveFile writes to a temporary file and renames it on commit.
    QSaveFile file(path);
    if (! file.open(QIODevice::WriteOnly)) return false;
    const QJsonObject root { { QStringLiteral("project"), state.project } };
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0) return false;
    return file.commit();
}

} // namespace wehypr
