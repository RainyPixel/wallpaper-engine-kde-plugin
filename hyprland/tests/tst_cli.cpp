// SPDX-License-Identifier: GPL-2.0-only
// Runs the built binary for commands and failure paths that never open a window

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>

class TestCli : public QObject {
    Q_OBJECT

private:
    struct Result {
        int        code { -1 };
        QByteArray out;
        QByteArray err;
    };

    QTemporaryDir       m_runtime;
    QProcessEnvironment m_env;

    Result run(const QStringList& args, const QProcessEnvironment& env) const {
        QProcess process;
        process.setProcessEnvironment(env);
        process.start(QStringLiteral(WEHYPR_BINARY), args);
        Result result;
        if (! process.waitForStarted(10000)) return result;
        if (! process.waitForFinished(30000)) {
            process.kill();
            process.waitForFinished();
            return result;
        }
        result.code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        result.out  = process.readAllStandardOutput();
        result.err  = process.readAllStandardError();
        return result;
    }

    Result run(const QStringList& args) const { return run(args, m_env); }

    static bool writeFile(const QString& path, const QByteArray& data) {
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
    }

private slots:
    void initTestCase() {
        QVERIFY(m_runtime.isValid());
        QVERIFY(QFileInfo::exists(QStringLiteral(WEHYPR_BINARY)));
        m_env = QProcessEnvironment::systemEnvironment();
        m_env.insert("XDG_RUNTIME_DIR", m_runtime.path());
        m_env.insert("WAYLAND_DISPLAY", "wehypr-test-no-such-display");
        m_env.remove("QTWEBENGINE_CHROMIUM_FLAGS");
    }

    void help_and_version() {
        const Result help = run({ "help" });
        QCOMPARE(help.code, 0);
        QVERIFY(help.out.contains("Usage:"));
        const Result version = run({ "--version" });
        QCOMPARE(version.code, 0);
        QVERIFY(version.out.startsWith("wallpaper-engine-hyprland "));
    }

    void usageErrors_data() {
        QTest::addColumn<QStringList>("args");
        QTest::newRow("no command") << QStringList {};
        QTest::newRow("unknown command") << QStringList { "start" };
        QTest::newRow("unknown option") << QStringList { "run", WEHYPR_FIXTURE, "--preview" };
        QTest::newRow("bad fps") << QStringList { "run", WEHYPR_FIXTURE, "--fps", "0" };
        QTest::newRow("bad instance") << QStringList { "status", "--instance", "a/b" };
        QTest::newRow("bad json") << QStringList { "set-properties", "{" };
    }

    void usageErrors() {
        QFETCH(QStringList, args);
        const Result result = run(args);
        QCOMPARE(result.code, 2);
        QVERIFY(result.err.contains("wallpaper-engine-hyprland: "));
    }

    void checkFixture() {
        const Result result = run({ "check", WEHYPR_FIXTURE });
        QCOMPARE(result.code, 0);
        const QJsonObject summary = QJsonDocument::fromJson(result.out).object();
        QCOMPARE(summary.value("ok").toBool(), true);
        QCOMPARE(summary.value("type").toString(), QStringLiteral("web"));
        QVERIFY(summary.value("url").toString().startsWith("file:///"));
        QCOMPARE(summary.value("properties")
                     .toObject()
                     .value("speed")
                     .toObject()
                     .value("value")
                     .toDouble(),
                 1.0);
    }

    void checkWithProperties() {
        const Result ok =
            run({ "check", WEHYPR_FIXTURE, "--properties", R"({"speed":2,"shape":"square"})" });
        QCOMPARE(ok.code, 0);
        const QJsonObject properties =
            QJsonDocument::fromJson(ok.out).object().value("properties").toObject();
        QCOMPARE(properties.value("speed").toObject().value("value").toDouble(), 2.0);
        QCOMPARE(properties.value("shape").toObject().value("value").toString(),
                 QStringLiteral("square"));

        const Result rejected = run({ "check", WEHYPR_FIXTURE, "--properties", R"({"speed":9})" });
        QCOMPARE(rejected.code, 3);
        QVERIFY(rejected.err.contains("speed"));
    }

    void invalidProjects() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir.filePath("project.json"), R"({"type":"scene","file":"scene.pkg"})"));

        const Result check = run({ "check", dir.path() });
        QCOMPARE(check.code, 3);
        QVERIFY(check.err.contains("not supported"));

        // Validation happens before any display connection.
        const Result runScene = run({ "run", dir.path(), "--output", "DP-1" });
        QCOMPARE(runScene.code, 3);

        const Result missing = run({ "check", dir.filePath("missing") });
        QCOMPARE(missing.code, 3);
    }

    void runWithoutWayland() {
        QProcessEnvironment env = m_env;
        env.remove("WAYLAND_DISPLAY");
        const Result result = run({ "run", WEHYPR_FIXTURE, "--instance", "no-wayland" }, env);
        QCOMPARE(result.code, 4);
        QVERIFY(result.err.contains("WAYLAND_DISPLAY"));

        const Result outputs = run({ "outputs" }, env);
        QCOMPARE(outputs.code, 4);
    }

    void runWhileInstanceLocked() {
        const QString dir = m_runtime.path() + "/wallpaper-engine-hyprland";
        QVERIFY(QDir().mkpath(dir));
        QVERIFY(QFile::setPermissions(
            dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QLockFile lock(dir + "/locked.lock");
        lock.setStaleLockTime(0);
        QVERIFY(lock.tryLock(0));

        const Result result = run({ "run", WEHYPR_FIXTURE, "--instance", "locked" });
        QCOMPARE(result.code, 5);
        QVERIFY(result.err.contains("already running"));
    }

    void controlWithoutHost() {
        for (const char* command : { "status", "pause", "resume", "quit" }) {
            const Result result = run({ command, "--instance", "tst-cli-nobody" });
            QCOMPARE(result.code, 6);
            QVERIFY(result.err.contains("no running host"));
        }
        const Result properties =
            run({ "set-properties", R"({"speed":1})", "--instance", "tst-cli-nobody" });
        QCOMPARE(properties.code, 6);
    }
};

QTEST_GUILESS_MAIN(TestCli)
#include "tst_cli.moc"
