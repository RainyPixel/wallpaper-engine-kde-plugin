#include "TTYSwitchMonitor.hpp"
#include <QDebug>

using namespace wekde;

TTYSwitchMonitor::TTYSwitchMonitor(QQuickItem* parent): QQuickItem(parent), m_sleeping(false) {
    QDBusConnection systemBus = QDBusConnection::systemBus();
    if (! systemBus.isConnected()) {
        qWarning() << "TTYSwitchMonitor: Cannot connect to the D-Bus system bus."
                   << "Sleep/wake detection will be disabled.";
        return;
    }

    bool connected = systemBus.connect("org.freedesktop.login1",
                                       "/org/freedesktop/login1",
                                       "org.freedesktop.login1.Manager",
                                       "PrepareForSleep",
                                       this,
                                       SLOT(handlePrepareForSleep(bool)));

    if (! connected) {
        qWarning() << "TTYSwitchMonitor: Failed to connect to PrepareForSleep signal."
                   << "Sleep/wake detection will be disabled.";
    }
}

void TTYSwitchMonitor::handlePrepareForSleep(bool sleep) {
    if (m_sleeping != sleep) {
        m_sleeping = sleep;
        emit ttySwitch(sleep);
    }
}
