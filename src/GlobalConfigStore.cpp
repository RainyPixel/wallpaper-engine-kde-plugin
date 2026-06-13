#include "GlobalConfigStore.hpp"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGlobalStatic>
#include <QDebug>

using namespace wekde;

namespace
{
constexpr auto k_enabled_key = "enabled";
}

Q_GLOBAL_STATIC(GlobalConfigBackend, g_backend)

GlobalConfigBackend* GlobalConfigBackend::instance() { return g_backend; }

GlobalConfigBackend::GlobalConfigBackend() { load(); }

QString GlobalConfigBackend::filePath() const {
    QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/wekde";
    QDir().mkpath(dir);
    return dir + "/global.json";
}

void GlobalConfigBackend::load() {
    QFile f(filePath());
    if (! f.open(QIODevice::ReadOnly)) return;
    auto         doc = QJsonDocument::fromJson(f.readAll());
    QWriteLocker locker(&m_lock);
    if (doc.isObject()) m_data = doc.object().toVariantMap();
}

void GlobalConfigBackend::persist() const {
    QSaveFile f(filePath());
    if (! f.open(QIODevice::WriteOnly)) {
        qWarning() << "GlobalConfigBackend: cannot write" << filePath();
        return;
    }
    QJsonDocument doc(QJsonObject::fromVariantMap(m_data));
    f.write(doc.toJson(QJsonDocument::Indented));
    f.commit();
}

QStringList GlobalConfigBackend::apply(const QVariantMap& patch) {
    QStringList changedKeys;
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
        if (m_data.value(it.key()) != it.value()) {
            m_data.insert(it.key(), it.value());
            changedKeys << it.key();
        }
    }
    return changedKeys;
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

void GlobalConfigBackend::setValue(const QString& key, const QVariant& val) {
    setValues({ { key, val } });
}

void GlobalConfigBackend::setValues(const QVariantMap& patch) {
    QStringList changedKeys;
    {
        QWriteLocker locker(&m_lock);
        changedKeys = apply(patch);
        if (changedKeys.isEmpty()) return;
        persist();
    }
    for (const auto& key : changedKeys) emit changed(key, value(key));
    if (changedKeys.contains(k_enabled_key)) emit enabledChanged();
}

void GlobalConfigBackend::reloadFromDisk() {
    QVariantMap fresh;
    {
        QFile f(filePath());
        if (f.open(QIODevice::ReadOnly)) {
            auto doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) fresh = doc.object().toVariantMap();
        }
    }
    QStringList changedKeys;
    {
        QWriteLocker locker(&m_lock);
        // keys removed on disk
        for (auto it = m_data.constBegin(); it != m_data.constEnd(); ++it)
            if (! fresh.contains(it.key())) changedKeys << it.key();
        // added/modified keys
        for (auto it = fresh.constBegin(); it != fresh.constEnd(); ++it)
            if (m_data.value(it.key()) != it.value()) changedKeys << it.key();
        if (changedKeys.isEmpty()) return;
        m_data = fresh;
    }
    for (const auto& key : changedKeys) emit changed(key, value(key));
    if (changedKeys.contains(k_enabled_key)) emit enabledChanged();
}

// --- QML proxy ---

GlobalConfig::GlobalConfig(QObject* parent): QObject(parent) {
    auto* backend = GlobalConfigBackend::instance();
    connect(backend, &GlobalConfigBackend::changed, this, &GlobalConfig::changed);
    connect(backend, &GlobalConfigBackend::enabledChanged, this, &GlobalConfig::enabledChanged);
}

bool GlobalConfig::enabled() const { return GlobalConfigBackend::instance()->enabled(); }

void GlobalConfig::setEnabled(bool on) { GlobalConfigBackend::instance()->setValue("enabled", on); }

QVariant GlobalConfig::get(const QString& key, const QVariant& def) const {
    return GlobalConfigBackend::instance()->value(key, def);
}

void GlobalConfig::set(const QString& key, const QVariant& val) {
    GlobalConfigBackend::instance()->setValue(key, val);
}

void GlobalConfig::setBatch(const QVariantMap& patch) {
    GlobalConfigBackend::instance()->setValues(patch);
}

QVariantMap GlobalConfig::all() const { return GlobalConfigBackend::instance()->all(); }

void GlobalConfig::reload() { GlobalConfigBackend::instance()->reloadFromDisk(); }
