// SPDX-License-Identifier: GPL-2.0-only
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "ConfigTestUtils.hpp"
#include "GlobalConfigStore.hpp"

using namespace wekde;

class TestGlobalConfig : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QByteArray    m_previousConfigHome = qgetenv("XDG_CONFIG_HOME");

    QString    path() const { return m_tmp.filePath("wekde/global.json"); }
    QByteArray read() const {
        QFile file(path());
        if (! file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
    bool write(const QByteArray& bytes) const {
        if (! QDir().mkpath(m_tmp.filePath("wekde"))) return false;
        QFile file(path());
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }

private slots:
    void initTestCase() {
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
    void init() { QVERIFY(! QFileInfo::exists(path()) || QFile::remove(path())); }

    void batch_mergesAndNotifiesAfterPersistence() {
        GlobalConfigBackend backend;
        QSignalSpy          changed(&backend, &GlobalConfigBackend::changed);
        QSignalSpy          enabled(&backend, &GlobalConfigBackend::enabledChanged);
        QSignalSpy          failed(&backend, &GlobalConfigBackend::persistenceFailed);
        QVERIFY(backend.setValues({ { "enabled", true }, { "fps", 30 } }));
        QCOMPARE(changed.count(), 2);
        QCOMPARE(enabled.count(), 1);
        QVERIFY(backend.setValue("fps", 60));
        QCOMPARE(backend.value("fps").toInt(), 60);
        QVERIFY(backend.enabled());
        const auto persisted = QJsonDocument::fromJson(read()).toVariant().toMap();
        QCOMPARE(persisted, backend.all());
        GlobalConfigBackend reloaded;
        QCOMPARE(reloaded.all(), backend.all());
        QVERIFY(backend.setValue("fps", 60));
        QCOMPARE(changed.count(), 3);
        QCOMPARE(enabled.count(), 1);
        QCOMPARE(failed.count(), 0);
    }

    void failedWrite_preservesDiskMemoryAndNotifications() {
        GlobalConfigBackend backend;
        QVERIFY(backend.setValues({ { "enabled", true }, { "fps", 30 } }));
        const auto previous = backend.all();
        const auto bytes    = read();
        QSignalSpy changed(&backend, &GlobalConfigBackend::changed);
        QSignalSpy enabled(&backend, &GlobalConfigBackend::enabledChanged);
        QSignalSpy failed(&backend, &GlobalConfigBackend::persistenceFailed);
        {
            ScopedFileSizeLimit limit(64);
            QVERIFY(limit.active());
            QVERIFY(! backend.setValues({ { "enabled", false }, { "large", QString(4096, 'x') } }));
        }
        QCOMPARE(read(), bytes);
        QCOMPARE(backend.all(), previous);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(enabled.count(), 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY(! failed.first().first().toString().isEmpty());
        GlobalConfigBackend reloaded;
        QCOMPARE(reloaded.all(), previous);
        QVERIFY(backend.setValue("enabled", false));
        QVERIFY(! backend.enabled());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(enabled.count(), 1);
    }

    void invalidFile_preservesMemoryAndBytes() {
        GlobalConfigBackend backend;
        QVERIFY(backend.setValues({ { "enabled", true }, { "fps", 30 } }));
        const auto       previous = backend.all();
        const QByteArray invalid  = "{broken configuration";
        QVERIFY(write(invalid));
        QSignalSpy changed(&backend, &GlobalConfigBackend::changed);
        QSignalSpy enabled(&backend, &GlobalConfigBackend::enabledChanged);
        QSignalSpy failed(&backend, &GlobalConfigBackend::persistenceFailed);
        QVERIFY(! backend.reloadFromDisk());
        QCOMPARE(backend.all(), previous);
        QVERIFY(! backend.setValue("fps", 60));
        QCOMPARE(backend.all(), previous);
        QCOMPARE(read(), invalid);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(enabled.count(), 0);
        QCOMPARE(failed.count(), 2);
        QVERIFY(QFile::remove(path()));
        QVERIFY(backend.reloadFromDisk());
        QVERIFY(backend.all().isEmpty());
        QCOMPARE(changed.count(), 2);
        QCOMPARE(enabled.count(), 1);
    }

    void proxy_reportsFailureAndKeepsAllInstancesConsistent() {
        GlobalConfig first;
        GlobalConfig second;
        QVERIFY(first.reload());
        QVERIFY(first.setBatch({ { "enabled", true }, { "fps", 30 } }));
        const auto previous = second.all();
        const auto bytes    = read();
        QSignalSpy changed(&second, &GlobalConfig::changed);
        QSignalSpy enabled(&second, &GlobalConfig::enabledChanged);
        QSignalSpy failed(&second, &GlobalConfig::persistenceFailed);
        {
            ScopedFileSizeLimit limit(1);
            QVERIFY(limit.active());
            first.setEnabled(false);
        }
        QVERIFY(second.enabled());
        QCOMPARE(first.all(), previous);
        QCOMPARE(second.all(), previous);
        QCOMPARE(read(), bytes);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(enabled.count(), 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY(second.set("fps", 60));
        QCOMPARE(first.get("fps").toInt(), 60);
    }
};

QTEST_GUILESS_MAIN(TestGlobalConfig)
#include "tst_globalconfig.moc"
