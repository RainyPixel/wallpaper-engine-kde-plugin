#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

namespace wekde
{
class WallpaperEngineFollower : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList sections READ sections NOTIFY sectionsChanged)
    Q_PROPERTY(QVariantMap selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    explicit WallpaperEngineFollower(QObject* parent = nullptr);

    QStringList sections() const { return m_sections; }
    QVariantMap selection() const { return m_selection; }
    QString     status() const { return m_status; }

    Q_INVOKABLE void configure(const QVariantMap& steamPaths, const QString& section, bool inspect);

signals:
    void sectionsChanged();
    void selectionChanged();
    void statusChanged();

private:
    void refresh(bool keepSelection);

    QFileSystemWatcher m_watcher;
    QTimer             m_update;
    QString            m_installDir;
    QStringList        m_projectDirs;
    QString            m_section;
    QStringList        m_sections;
    QVariantMap        m_selection;
    QString            m_status;
    bool               m_inspect { false };
};
} // namespace wekde
