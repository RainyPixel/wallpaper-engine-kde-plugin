#include "GamemodeMonitor.hpp"
#include <QDebug>

using namespace wekde;

GamemodeMonitor::GamemodeMonitor(QQuickItem* parent): QQuickItem(parent), m_reqPause(false) {
    QDBusConnection sessionBus = QDBusConnection::sessionBus();
    if (! sessionBus.isConnected()) {
        qWarning("gamemode: Cannot connect to the D-Bus session bus");
        return;
    }

    bool connected =
        sessionBus.connect("com.feralinteractive.GameMode",
                           "/com/feralinteractive/GameMode",
                           "org.freedesktop.DBus.Properties",
                           "PropertiesChanged",
                           this,
                           SLOT(handlePropsChanged(QString, QVariantMap, QStringList)));

    if (! connected) {
        qDebug("gamemode: Failed to connect to PropertiesChanged signal");
        return;
    }

    QDBusMessage msg = QDBusMessage::createMethodCall("com.feralinteractive.GameMode",
                                                      "/com/feralinteractive/GameMode",
                                                      "org.freedesktop.DBus.Properties",
                                                      "Get");
    msg << "com.feralinteractive.GameMode" << "ClientCount";

    QDBusPendingCall         currentClientCall = sessionBus.asyncCall(msg);
    QDBusPendingCallWatcher* watcher = new QDBusPendingCallWatcher(currentClientCall, this);

    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        QDBusPendingReply<QVariant> reply = *watcher;

        if (reply.isValid()) {
            int  clientCount = reply.value().toInt();
            bool newPlaying  = clientCount > 0;

            if (newPlaying != m_reqPause) {
                m_reqPause = newPlaying;
                emit reqPauseChanged();
            }
        }

        watcher->deleteLater();
    });
}

void GamemodeMonitor::handlePropsChanged(const QString&, const QVariantMap& map,
                                         const QStringList&) {
    bool newPause = map.value("ClientCount").toInt() > 0;

    if (newPause != m_reqPause) {
        m_reqPause = newPause;
        emit reqPauseChanged();
    }
}
