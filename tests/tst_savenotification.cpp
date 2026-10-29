// SPDX-License-Identifier: GPL-2.0-only
#include <QtTest>
#include <QDBusConnection>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "FileHelper.hpp"

class Notifications : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

public:
    QList<QVariantList> calls;

public slots:
    uint Notify(const QString& app, uint replaces, const QString& icon, const QString& title,
                const QString& body, const QStringList& actions, const QVariantMap& hints,
                int timeout) {
        calls.append({ app, replaces, icon, title, body, actions, hints, timeout });
        return 1;
    }
};

class TestSaveNotification : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QByteArray    m_previousConfigHome = qgetenv("XDG_CONFIG_HOME");

private slots:
    void initTestCase() {
        if (qgetenv("WEKDE_NOTIFICATION_TEST_BUS") != "1")
            QSKIP("Run through CTest to use a private dbus-run-session bus");
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_CONFIG_HOME", m_tmp.path().toUtf8());
        QCOMPARE(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
                 m_tmp.path());
    }

    void cleanupTestCase() {
        if (m_previousConfigHome.isNull())
            qunsetenv("XDG_CONFIG_HOME");
        else
            qputenv("XDG_CONFIG_HOME", m_previousConfigHome);
    }

    void requestOutlivesHelper() {
        Notifications receiver;
        auto          bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                 QStringLiteral("notification-test-server"));
        QVERIFY(bus.isConnected());
        QVERIFY(bus.baseService() != QDBusConnection::sessionBus().baseService());
        QVERIFY(bus.registerObject(
            "/org/freedesktop/Notifications", &receiver, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerService("org.freedesktop.Notifications"));
        const QString body = QStringLiteral("Wallpaper changes were kept for retry.");
        {
            wekde::FileHelper helper;
            QVERIFY(! helper.writeWallpaperConfig("missing/child", {}));
            QVERIFY(receiver.calls.isEmpty());
            helper.notifySaveError(body);
        }
        QTRY_COMPARE(receiver.calls.size(), 1);
        const auto arguments = receiver.calls.first();
        QCOMPARE(arguments.at(0).toString(), QStringLiteral("Wallpaper Engine KDE"));
        QCOMPARE(arguments.at(1).toUInt(), 0u);
        QCOMPARE(arguments.at(2).toString(), QStringLiteral("dialog-error"));
        QCOMPARE(arguments.at(3).toString(),
                 QStringLiteral("Wallpaper settings could not be saved"));
        QCOMPARE(arguments.at(4).toString(), body);
        QVERIFY(arguments.at(5).toStringList().isEmpty());
        QVERIFY(arguments.at(6).toMap().isEmpty());
        QCOMPARE(arguments.at(7).toInt(), -1);
        QVERIFY(bus.unregisterService("org.freedesktop.Notifications"));
    }
};

QTEST_GUILESS_MAIN(TestSaveNotification)
#include "tst_savenotification.moc"
