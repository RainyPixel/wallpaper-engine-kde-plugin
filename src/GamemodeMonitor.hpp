#pragma once
#include <QQuickItem>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace wekde
{

class GamemodeMonitor : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool reqPause READ reqPause NOTIFY reqPauseChanged)

public:
    GamemodeMonitor(QQuickItem* parent = nullptr);

    bool reqPause() const { return m_reqPause; }

signals:
    void reqPauseChanged();

public slots:
    void handlePropsChanged(const QString& name, const QVariantMap& map, const QStringList& list);

private:
    bool m_reqPause;
};

} // namespace wekde
