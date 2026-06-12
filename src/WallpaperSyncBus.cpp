#include "WallpaperSyncBus.hpp"

#include <QQuickWindow>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QUuid>
#include <QDebug>

using namespace wekde;

namespace
{
constexpr auto k_path          = "/WallpaperSync";
constexpr auto k_interface     = "com.github.catsout.wallpaperEngineKde.Sync";
constexpr auto k_signal_global = "GlobalConfigChanged";
} // namespace

WallpaperSyncBus::WallpaperSyncBus(QQuickItem* parent)
    : QQuickItem(parent), m_id(QUuid::createUuid().toString()) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (! bus.isConnected()) {
        qWarning() << "WallpaperSyncBus: session bus unavailable, cross-process sync disabled";
        return;
    }

    bool connected = bus.connect(
        QString(), k_path, k_interface, k_signal_global, this, SLOT(handleGlobalChange(QString)));
    if (! connected) {
        qWarning() << "WallpaperSyncBus: failed to subscribe to sync signal";
    }

    connect(this, &QQuickItem::windowChanged, this, [this] {
        connectScreenSignals();
        emit isPrimaryChanged();
    });
    connect(
        qApp, &QGuiApplication::primaryScreenChanged, this, &WallpaperSyncBus::isPrimaryChanged);
    connectScreenSignals();
}

void WallpaperSyncBus::connectScreenSignals() {
    if (auto* w = window()) {
        connect(w,
                &QWindow::screenChanged,
                this,
                &WallpaperSyncBus::isPrimaryChanged,
                Qt::UniqueConnection);
    }
}

bool WallpaperSyncBus::isPrimary() const {
    auto* w = window();
    if (! w) return false;
    return w->screen() == QGuiApplication::primaryScreen();
}

void WallpaperSyncBus::broadcastGlobalChanged() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (! bus.isConnected()) return;

    QDBusMessage msg = QDBusMessage::createSignal(k_path, k_interface, k_signal_global);
    msg << m_id;
    bus.send(msg);
}

void WallpaperSyncBus::handleGlobalChange(const QString& senderId) {
    if (senderId == m_id) return;
    emit globalConfigChanged();
}
