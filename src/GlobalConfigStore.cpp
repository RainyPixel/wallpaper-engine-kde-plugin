#include "GlobalConfigStore.hpp"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGlobalStatic>
#include <QDebug>

using namespace wekde;

namespace
{
constexpr auto k_enabled_key = "enabled";

QStringList changedKeys(const QVariantMap& previous, const QVariantMap& updated) {
    QStringList keys;
    for (auto it = previous.constBegin(); it != previous.constEnd(); ++it)
        if (! updated.contains(it.key())) keys << it.key();
    for (auto it = updated.constBegin(); it != updated.constEnd(); ++it)
        if (previous.value(it.key()) != it.value()) keys << it.key();
    return keys;
}
} // namespace

Q_GLOBAL_STATIC(GlobalConfigBackend, g_backend)

GlobalConfigBackend* GlobalConfigBackend::instance() { return g_backend; }

GlobalConfigBackend::GlobalConfigBackend() { reloadFromDisk(); }

QString GlobalConfigBackend::filePath() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           "/wekde/global.json";
}

bool GlobalConfigBackend::readFromDisk(QVariantMap& data, QString& error) const {
    QFile file(filePath());
    if (! file.exists()) return true;
    if (! file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (file.error() != QFileDevice::NoError || ! doc.isObject()) {
        error = QStringLiteral("Invalid or unreadable global configuration");
        return false;
    }
    data = doc.object().toVariantMap();
    return true;
}

bool GlobalConfigBackend::persist(const QVariantMap& data, QString& error) const {
    const QString path = filePath();
    if (! QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = QStringLiteral("Cannot create global configuration directory");
        return false;
    }
    QSaveFile file(path);
    if (! file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(QJsonObject::fromVariantMap(data)).toJson();
    // Older Qt versions can miss buffered flush failures inside commit().
    if (file.write(bytes) != bytes.size() || ! file.flush() || ! file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}

QVariant GlobalConfigBackend::value(const QString& key, const QVariant& def) const {
    QReadLocker locker(&m_lock);
    return m_data.value(key, def);
}

bool GlobalConfigBackend::enabled() const { return value(k_enabled_key, false).toBool(); }

QVariantMap GlobalConfigBackend::all() const {
    QReadLocker locker(&m_lock);
    return m_data;
}

bool GlobalConfigBackend::setValue(const QString& key, const QVariant& val) {
    return setValues({ { key, val } });
}

bool GlobalConfigBackend::setValues(const QVariantMap& patch) {
    if (patch.isEmpty()) return true;
    QStringList changes;
    QVariantMap updated;
    QString     error;
    bool        saved;
    {
        QWriteLocker locker(&m_lock);
        QVariantMap  disk;
        saved = readFromDisk(disk, error);
        if (saved) {
            updated = disk;
            for (auto it = patch.constBegin(); it != patch.constEnd(); ++it)
                updated.insert(it.key(), it.value());
            saved = updated == disk || persist(updated, error);
        }
        if (saved) {
            changes = changedKeys(m_data, updated);
            m_data  = updated;
        }
    }
    if (! saved) {
        qWarning() << "GlobalConfigBackend: cannot save" << filePath() << error;
        emit persistenceFailed(error);
        return false;
    }
    for (const auto& key : changes) emit changed(key, updated.value(key));
    if (changes.contains(k_enabled_key)) emit enabledChanged();
    return true;
}

bool GlobalConfigBackend::reloadFromDisk() {
    QVariantMap fresh;
    QString     error;
    if (! readFromDisk(fresh, error)) {
        qWarning() << "GlobalConfigBackend: cannot reload" << filePath() << error;
        emit persistenceFailed(error);
        return false;
    }
    QStringList changes;
    {
        QWriteLocker locker(&m_lock);
        changes = changedKeys(m_data, fresh);
        if (changes.isEmpty()) return true;
        m_data = fresh;
    }
    for (const auto& key : changes) emit changed(key, fresh.value(key));
    if (changes.contains(k_enabled_key)) emit enabledChanged();
    return true;
}

// --- QML proxy ---

GlobalConfig::GlobalConfig(QObject* parent): QObject(parent) {
    auto* backend = GlobalConfigBackend::instance();
    connect(backend, &GlobalConfigBackend::changed, this, &GlobalConfig::changed);
    connect(backend, &GlobalConfigBackend::enabledChanged, this, &GlobalConfig::enabledChanged);
    connect(
        backend, &GlobalConfigBackend::persistenceFailed, this, &GlobalConfig::persistenceFailed);
}

bool GlobalConfig::enabled() const { return GlobalConfigBackend::instance()->enabled(); }

void GlobalConfig::setEnabled(bool on) { GlobalConfigBackend::instance()->setValue("enabled", on); }

QVariant GlobalConfig::get(const QString& key, const QVariant& def) const {
    return GlobalConfigBackend::instance()->value(key, def);
}

bool GlobalConfig::set(const QString& key, const QVariant& val) {
    return GlobalConfigBackend::instance()->setValue(key, val);
}

bool GlobalConfig::setBatch(const QVariantMap& patch) {
    return GlobalConfigBackend::instance()->setValues(patch);
}

QVariantMap GlobalConfig::all() const { return GlobalConfigBackend::instance()->all(); }

bool GlobalConfig::reload() { return GlobalConfigBackend::instance()->reloadFromDisk(); }
