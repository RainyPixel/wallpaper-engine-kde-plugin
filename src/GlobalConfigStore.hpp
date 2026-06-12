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
    void        setValue(const QString& key, const QVariant& val);
    void        setValues(const QVariantMap& patch);
    QVariantMap all() const;
    bool        enabled() const;
    void        reloadFromDisk();

signals:
    void changed(const QString& key, const QVariant& value);
    void enabledChanged();

private:
    QString filePath() const;
    void    load();
    void    persist() const;
    // applies patch in-memory, returns keys whose value actually changed
    QStringList apply(const QVariantMap& patch);

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
    Q_INVOKABLE void        set(const QString& key, const QVariant& val);
    Q_INVOKABLE void        setBatch(const QVariantMap& patch);
    Q_INVOKABLE QVariantMap all() const;
    Q_INVOKABLE void        reload();

signals:
    void changed(const QString& key, const QVariant& value);
    void enabledChanged();
};

} // namespace wekde
