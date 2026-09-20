// SPDX-License-Identifier: GPL-2.0-only
// Unit tests for the wallpaper library scan and the persistent host state

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QTemporaryDir>

#include "Library.hpp"
#include "State.hpp"

using namespace wehypr;

class TestLibrary : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_tmp;
    QString                        m_workshop;
    QString                        m_install;

    static bool writeFile(const QString& path, const QByteArray& data) {
        QFile f(path);
        if (! f.open(QIODevice::WriteOnly)) return false;
        return f.write(data) == data.size();
    }

    // An item directory with a raw project.json, so malformed input can be tested.
    static bool writeItem(const QString& parent, const QString& id, const QByteArray& json) {
        const QString dir = parent + QLatin1Char('/') + id;
        if (! QDir().mkpath(dir)) return false;
        if (json.isNull()) return true;
        return writeFile(dir + "/project.json", json);
    }

    QList<Wallpaper> scan() {
        return scanWallpapersIn(steam::detectLibrariesIn({ m_tmp->path() }));
    }

    static const Wallpaper* find(const QList<Wallpaper>& wallpapers, const QString& id) {
        for (const Wallpaper& wallpaper : wallpapers) {
            if (wallpaper.id == id) return &wallpaper;
        }
        return nullptr;
    }

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // A Steam library with an installed Wallpaper Engine and workshop content.
    void init() {
        m_tmp = std::make_unique<QTemporaryDir>();
        QVERIFY(m_tmp->isValid());
        const QString steamapps = m_tmp->path() + "/steamapps";
        m_workshop              = steamapps + "/workshop/content/431960";
        m_install               = steamapps + "/common/wallpaper_engine";
        QVERIFY(QDir().mkpath(m_workshop));
        QVERIFY(QDir().mkpath(m_install + "/assets"));
        QVERIFY(writeFile(steamapps + "/appmanifest_431960.acf",
                          "\"AppState\"\n{\n\t\"appid\"\t\"431960\"\n"
                          "\t\"installdir\"\t\"wallpaper_engine\"\n}\n"));
    }

    void cleanup() { m_tmp.reset(); }

    // ── scanWallpapersIn ──────────────────────────────────────────────────────
    void listsPlayableAndUnplayable() {
        QVERIFY(writeItem(m_workshop,
                          "111",
                          R"({"type": "web", "title": "Clock", "file": "index.html",
                              "preview": "preview.gif"})"));
        QVERIFY(writeItem(
            m_workshop, "222", R"({"type": "scene", "title": "Forest", "file": "scene.pkg"})"));
        QVERIFY(writeItem(
            m_workshop, "333", R"({"type": "video", "title": "Loop", "file": "clip.mp4"})"));

        const auto wallpapers = scan();
        QCOMPARE(wallpapers.size(), qsizetype(3));

        const Wallpaper* web = find(wallpapers, "111");
        QVERIFY(web != nullptr);
        QCOMPARE(web->title, QStringLiteral("Clock"));
        QCOMPARE(web->type, QStringLiteral("web"));
        QCOMPARE(web->file, QStringLiteral("index.html"));
        QCOMPARE(web->preview, QStringLiteral("preview.gif"));
        QCOMPARE(web->path, QFileInfo(m_workshop + "/111").canonicalFilePath());
        QVERIFY(web->mtime > 0);
        QVERIFY(web->supported);

        QVERIFY(find(wallpapers, "222") != nullptr);
        QVERIFY(! find(wallpapers, "222")->supported);
        QVERIFY(! find(wallpapers, "333")->supported);
    }

    void brokenItemsSkipped() {
        QVERIFY(writeItem(m_workshop, "111", R"({"type": "web", "file": "index.html"})"));
        QVERIFY(writeItem(m_workshop, "222", QByteArray())); // no project.json at all
        QVERIFY(writeItem(m_workshop, "333", "{ not json"));
        QVERIFY(writeItem(m_workshop, "444", "[]"));              // valid JSON, wrong shape
        QVERIFY(QDir().mkpath(m_workshop + "/555/project.json")); // a directory, not a file

        const auto wallpapers = scan();
        QCOMPARE(wallpapers.size(), qsizetype(1));
        QCOMPARE(wallpapers.first().id, QStringLiteral("111"));
    }

    void inFlightDownloadsExcluded() {
        const QString steamapps = m_tmp->path() + "/steamapps";
        QVERIFY(writeItem(m_workshop, "111", R"({"type": "web", "file": "index.html"})"));
        QVERIFY(writeItem(steamapps + "/workshop/downloads/431960",
                          "222",
                          R"({"type": "web", "file": "index.html"})"));
        QVERIFY(writeItem(
            steamapps + "/workshop/temp", "333", R"({"type": "web", "file": "index.html"})"));

        const auto wallpapers = scan();
        QCOMPARE(wallpapers.size(), qsizetype(1));
        QCOMPARE(wallpapers.first().id, QStringLiteral("111"));

        // Also refused when a library is pointed straight at them.
        steam::Library downloads;
        downloads.workshopDir = steamapps + "/workshop/downloads/431960";
        steam::Library temp;
        temp.workshopDir = steamapps + "/workshop/temp";
        QVERIFY(scanWallpapersIn({ downloads, temp }).isEmpty());
    }

    void typeLowercased_titleFallsBackToDirectory_previewOptional() {
        QVERIFY(writeItem(m_workshop, "111", R"({"type": "Web", "file": "index.html"})"));
        QVERIFY(writeItem(m_workshop, "222", R"({"type": "SCENE", "title": ""})"));

        const auto  wallpapers = scan();
        const auto* web        = find(wallpapers, "111");
        QVERIFY(web != nullptr);
        QCOMPARE(web->type, QStringLiteral("web"));
        QVERIFY(web->supported);
        QCOMPARE(web->title, QStringLiteral("111")); // no title in project.json
        QVERIFY(web->preview.isEmpty());             // no preview in project.json

        const auto* scene = find(wallpapers, "222");
        QVERIFY(scene != nullptr);
        QCOMPARE(scene->type, QStringLiteral("scene"));
        QCOMPARE(scene->title, QStringLiteral("222")); // empty title in project.json
        QVERIFY(scene->file.isEmpty());
    }

    void localProjectsScanned() {
        QVERIFY(writeItem(m_install + "/projects/defaultprojects",
                          "flow",
                          R"({"type": "web", "title": "Flow", "file": "index.html"})"));
        QVERIFY(writeItem(m_install + "/projects/myprojects",
                          "mine",
                          R"({"type": "web", "file": "index.html"})"));
        QVERIFY(writeItem(m_workshop, "111", R"({"type": "web", "file": "index.html"})"));

        const auto wallpapers = scan();
        QCOMPARE(wallpapers.size(), qsizetype(3));
        QVERIFY(find(wallpapers, "flow") != nullptr);
        QCOMPARE(find(wallpapers, "flow")->title, QStringLiteral("Flow"));
        QVERIFY(find(wallpapers, "mine") != nullptr);
    }

    void emptyLibrariesScanToNothing() { QVERIFY(scanWallpapersIn({}).isEmpty()); }

    // ── resolveProject ────────────────────────────────────────────────────────
    void resolveProjectKeepsNonIds() {
        QCOMPARE(resolveProject(QString()), QString());
        QCOMPARE(resolveProject(m_workshop), m_workshop);
        QCOMPARE(resolveProject(QStringLiteral("/no/such/path")), QStringLiteral("/no/such/path"));
        // Not all digits, so never looked up as a workshop id.
        QCOMPARE(resolveProject(QStringLiteral("12a34")), QStringLiteral("12a34"));
    }

    void resolveProjectPrefersExistingPath() {
        QVERIFY(writeItem(m_workshop, "111", R"({"type": "web", "file": "index.html"})"));
        // A failing QCOMPARE returns from the slot, so the working directory is
        // restored on every path out or the next test runs from a deleted dir.
        const QString     previous = QDir::currentPath();
        const QScopeGuard restore([&previous] {
            QDir::setCurrent(previous);
        });
        QVERIFY(QDir::setCurrent(m_workshop));
        QCOMPARE(resolveProject(QStringLiteral("111")), QStringLiteral("111"));
    }

    void resolveProjectFindsWorkshopId() {
        QVERIFY(writeItem(m_workshop, "111", R"({"type": "web", "file": "index.html"})"));
        const QList<steam::Library> libraries = steam::detectLibrariesIn({ m_tmp->path() });
        QCOMPARE(resolveProjectIn(QStringLiteral("111"), libraries), m_workshop + "/111");
        QCOMPARE(resolveProjectIn(QStringLiteral("999"), libraries), QStringLiteral("999"));
    }

    void resolveProjectSkipsLeftoverItemDirectory() {
        // An unsubscribe leaves the directory behind with no project.json; it
        // must not shadow the real copy in the next library.
        QVERIFY(writeItem(m_workshop, "111", QByteArray()));

        QTemporaryDir other;
        QVERIFY(other.isValid());
        const QString otherWorkshop = other.path() + "/steamapps/workshop/content/431960";
        QVERIFY(QDir().mkpath(otherWorkshop));
        QVERIFY(writeItem(otherWorkshop, "111", R"({"type": "web", "file": "index.html"})"));

        const QList<steam::Library> libraries =
            steam::detectLibrariesIn({ m_tmp->path(), other.path() });
        QCOMPARE(resolveProjectIn(QStringLiteral("111"), libraries), otherWorkshop + "/111");
    }

    void statePathRejectsUnsafeInstanceNames() {
        QVERIFY(! statePath(QStringLiteral("default")).isEmpty());
        QVERIFY(! statePath(QStringLiteral("left-monitor_2")).isEmpty());
        // An empty path is how every caller here says "no state".
        QVERIFY(statePath(QString()).isEmpty());
        QVERIFY(statePath(QStringLiteral("../escape")).isEmpty());
        QVERIFY(statePath(QStringLiteral("a/b")).isEmpty());
        QVERIFY(statePath(QString(65, QLatin1Char('x'))).isEmpty());
        QVERIFY(! saveState(QStringLiteral("../escape"), State {}));
        QCOMPARE(loadState(QStringLiteral("../escape")).project, QString());
    }

    // ── State ─────────────────────────────────────────────────────────────────
    void stateRoundTrip() {
        const QString path = statePath(QStringLiteral("default"));
        QVERIFY(! path.isEmpty());
        QFile::remove(path);
        QCOMPARE(loadState(QStringLiteral("default")).project, QString()); // missing file

        State state;
        state.project = m_workshop + "/111";
        QVERIFY(saveState(QStringLiteral("default"), state));
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(loadState(QStringLiteral("default")).project, state.project);

        state.project.clear();
        QVERIFY(saveState(QStringLiteral("default"), state));
        QCOMPARE(loadState(QStringLiteral("default")).project, QString());
        QVERIFY(QFile::remove(path));
    }

    void stateIsPerInstance() {
        State first;
        first.project = QStringLiteral("/one");
        State second;
        second.project = QStringLiteral("/two");
        QVERIFY(saveState(QStringLiteral("a"), first));
        QVERIFY(saveState(QStringLiteral("b"), second));
        QVERIFY(statePath(QStringLiteral("a")) != statePath(QStringLiteral("b")));
        QCOMPARE(loadState(QStringLiteral("a")).project, first.project);
        QCOMPARE(loadState(QStringLiteral("b")).project, second.project);
        QVERIFY(QFile::remove(statePath(QStringLiteral("a"))));
        QVERIFY(QFile::remove(statePath(QStringLiteral("b"))));
    }

    void corruptStateReadsAsDefault_data() {
        QTest::addColumn<QByteArray>("content");
        QTest::newRow("truncated") << QByteArray("{\"project\": \"/a");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("wrong value type") << QByteArray(R"({"project": 5})");
    }

    void corruptStateReadsAsDefault() {
        QFETCH(QByteArray, content);
        const QString path = statePath(QStringLiteral("broken"));
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QVERIFY(writeFile(path, content));
        QCOMPARE(loadState(QStringLiteral("broken")).project, QString());
        // A corrupt file is never an error, and saving over it recovers.
        State state;
        state.project = QStringLiteral("/ok");
        QVERIFY(saveState(QStringLiteral("broken"), state));
        QCOMPARE(loadState(QStringLiteral("broken")).project, state.project);
        QVERIFY(QFile::remove(path));
    }
};

QTEST_GUILESS_MAIN(TestLibrary)
#include "tst_library.moc"
