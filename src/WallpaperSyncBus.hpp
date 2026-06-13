#pragma once
#include <QQuickItem>
#include <QString>

namespace wekde
{

class WallpaperSyncBus : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool isPrimary READ isPrimary NOTIFY isPrimaryChanged)

public:
    WallpaperSyncBus(QQuickItem* parent = nullptr);

    bool isPrimary() const;

    // Wakes wallpaper instances living in other processes (e.g. the config
    // dialog) so they reload the global config from disk.
    Q_INVOKABLE void broadcastGlobalChanged();

signals:
    void globalConfigChanged();
    void isPrimaryChanged();

private slots:
    void handleGlobalChange(const QString& senderId);

private:
    void connectScreenSignals();

    QString m_id;
};

} // namespace wekde
