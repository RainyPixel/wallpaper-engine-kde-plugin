#pragma once
#include <QObject>
#include <QVariant>
#include <QVariantMap>
#include <QReadWriteLock>

namespace wekde
{

// Process-wide source of truth shared by every wallpaper instance in plasmashell.
// Backed by ~/.config/wekde/global.json. Not a QML type; reached through GlobalConfig.
class GlobalConfigBackend : public QObject {
    Q_OBJECT

public:
    GlobalConfigBackend();
    static GlobalConfigBackend* instance();

    QVariant    value(const QString& key, const QVariant& def = {}) const;
    bool        setValue(const QString& key, const QVariant& val);
    bool        setValues(const QVariantMap& patch);
    QVariantMap all() const;
    bool        enabled() const;
    bool        reloadFromDisk();

signals:
    void changed(const QString& key, const QVariant& value);
    void enabledChanged();
    void persistenceFailed(const QString& error);

private:
    QString filePath() const;
    bool    readFromDisk(QVariantMap& data, QString& error) const;
    bool    persist(const QVariantMap& data, QString& error) const;

    QVariantMap            m_data;
    mutable QReadWriteLock m_lock;
};

// Lightweight per-instance QML proxy that forwards to the shared backend.
class GlobalConfig : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

public:
    explicit GlobalConfig(QObject* parent = nullptr);

    bool enabled() const;
    void setEnabled(bool on);

    Q_INVOKABLE QVariant    get(const QString& key, const QVariant& def = {}) const;
    Q_INVOKABLE bool        set(const QString& key, const QVariant& val);
    Q_INVOKABLE bool        setBatch(const QVariantMap& patch);
    Q_INVOKABLE QVariantMap all() const;
    Q_INVOKABLE bool        reload();

signals:
    void changed(const QString& key, const QVariant& value);
    void enabledChanged();
    void persistenceFailed(const QString& error);
};

} // namespace wekde
