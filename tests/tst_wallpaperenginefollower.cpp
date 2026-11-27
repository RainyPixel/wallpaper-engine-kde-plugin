#include "WallpaperEngineFollower.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

namespace
{
bool writeFile(const QString& path, const QByteArray& contents) {
    if (! QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() &&
           file.commit();
}

QJsonObject monitor(const QString& path) { return { { "file", path } }; }

QJsonObject profile(const QJsonValue& selected, const QString& lastMonitor = {}) {
    return { { "general",
               QJsonObject {
                   { "wallpaperconfig", QJsonObject { { "selectedwallpapers", selected } } },
                   { "browser", QJsonObject { { "lastselectedmonitor", lastMonitor } } } } } };
}

struct Fixture {
    QTemporaryDir directory;
    const QString install  = directory.filePath("SteamApps/common/wallpaper_engine");
    const QString config   = install + "/config.json";
    const QString workshop = directory.filePath("library/SteamApps/Workshop/Content/431960");
    const QString defaults = install + "/projects/defaultprojects";

    QVariantMap steamPaths() const {
        return { { "assets", install + "/assets" },
                 { "projectDirs", QStringList { workshop, defaults } } };
    }

    bool addProject(const QString& project, const QByteArray& metadata) {
        return writeFile(project + "/project.json", metadata) &&
               writeFile(project + "/clip #2.mp4", "fixture") &&
               writeFile(project + "/web/index.html", "fixture");
    }

    bool setup() {
        return directory.isValid() && QDir().mkpath(install + "/assets") &&
               addProject(workshop + "/42", R"({"type":"video","file":"clip #2.mp4"})") &&
               addProject(workshop + "/84", R"({"type":"web","file":"web/index.html"})") &&
               addProject(defaults + "/sample", R"({"type":"video","file":"clip #2.mp4"})");
    }
};

class WallpaperEngineFollowerTest : public QObject {
    Q_OBJECT
private slots:
    void explicitProfileAndMonitor() {
        Fixture fixture;
        QVERIFY(fixture.setup());
        const auto        native = fixture.workshop + "/42/project.json";
        const QJsonObject selections { { "Monitor0", monitor(native) },
                                       { "Other",
                                         monitor("D:\\Steam\\steamapps\\workshop\\content"
                                                 "\\431960\\84\\web\\INDEX.HTML") } };
        const QJsonObject doc {
            { "steamuser", profile(selections, "Other") },
            { "steamuser1", profile(QJsonObject { { "Only", monitor(native) } }) },
            { "unrelated", profile(selections, "Other") },
        };
        QVERIFY(writeFile(fixture.config, QJsonDocument(doc).toJson()));
        wekde::WallpaperEngineFollower follower;
        follower.configure(fixture.steamPaths(), "steamuser", true);
        QCOMPARE(follower.sections(), QStringList({ "steamuser", "steamuser1" }));
        QCOMPARE(follower.selection().value("workshopId").toString(), "84");
        QVERIFY(follower.selection().value("source").toString().endsWith("/web/index.html+web"));
        follower.configure(fixture.steamPaths(), "steamuser1", true);
        QCOMPARE(follower.selection().value("workshopId").toString(), "42");
        QVERIFY(follower.selection().value("source").toString().endsWith("/clip%20%232.mp4+video"));
    }

    void rejectsPathsOutsideProjects() {
        Fixture fixture;
        QVERIFY(fixture.setup());
        const auto external = fixture.directory.filePath("external/project.json");
        QVERIFY(fixture.addProject(QFileInfo(external).absolutePath(),
                                   R"({"type":"video","file":"clip #2.mp4"})"));
        QVERIFY(QFile::link(external, fixture.workshop + "/42/escape"));
        const QStringList paths {
            external,
            fixture.workshop + "/42/escape",
            "C:/Steam/steamapps/workshop/content/431960/42/../84/project.json",
            fixture.workshop + "other/42/project.json"
        };
        for (const auto& path : paths) {
            const QJsonObject doc { { "steamuser",
                                      profile(QJsonObject { { "Only", monitor(path) } }) } };
            QVERIFY(writeFile(fixture.config, QJsonDocument(doc).toJson()));
            wekde::WallpaperEngineFollower follower;
            follower.configure(fixture.steamPaths(), "steamuser", false);
            QVERIFY2(follower.selection().isEmpty(), qPrintable(path));
        }

        QVERIFY(writeFile(fixture.workshop + "/42/project.json",
                          R"({"type":"video","file":"../../../../external/clip #2.mp4"})"));
        const QJsonObject doc {
            { "steamuser",
              profile(QJsonObject { { "Only", monitor(fixture.workshop + "/42/project.json") } }) }
        };
        QVERIFY(writeFile(fixture.config, QJsonDocument(doc).toJson()));
        wekde::WallpaperEngineFollower follower;
        follower.configure(fixture.steamPaths(), "steamuser", false);
        QVERIFY(follower.selection().isEmpty());
    }

    void prefersTheLibraryNamedByProtonsLocalPath() {
        Fixture fixture;
        QVERIFY(fixture.setup());
        const auto second = fixture.directory.filePath("other/SteamApps/Workshop/Content/431960");
        QVERIFY(fixture.addProject(second + "/42", R"({"type":"web","file":"web/index.html"})"));
        auto discovery           = fixture.steamPaths();
        discovery["projectDirs"] = QStringList { fixture.workshop, second };
        const auto expected =
            QUrl::fromLocalFile(second + "/42/web/index.html").toString(QUrl::FullyEncoded) +
            "+web";
        const QJsonObject doc {
            { "steamuser",
              profile(QJsonObject { { "Only", monitor("Z:" + second + "/42/project.json") } }) }
        };
        QVERIFY(writeFile(fixture.config, QJsonDocument(doc).toJson()));
        wekde::WallpaperEngineFollower follower;
        follower.configure(discovery, "steamuser", false);
        QCOMPARE(follower.selection().value("source").toString(), expected);

        QVERIFY(QFile::remove(second + "/42/web/index.html"));
        follower.configure(discovery, "steamuser", true);
        QCOMPARE(follower.selection().value("source").toString(), expected);
        QVERIFY(! follower.status().isEmpty());
    }

    void followsAtomicReplacementKeepsLastAndTurnsOff() {
        Fixture fixture;
        QVERIFY(fixture.setup());
        wekde::WallpaperEngineFollower follower;
        follower.configure(fixture.steamPaths(), "steamuser", false);
        QVERIFY(follower.selection().isEmpty());
        QVERIFY(! follower.status().isEmpty());

        const auto save = [&](const QJsonValue& selected) {
            return writeFile(
                fixture.config,
                QJsonDocument(QJsonObject { { "steamuser", profile(selected) } }).toJson());
        };
        QVERIFY(save(QJsonObject { { "Only",
                                     monitor("..\\..\\workshop\\content\\431960"
                                             "\\42\\project.json") } }));
        QTRY_COMPARE(follower.selection().value("workshopId").toString(), "42");
        QVERIFY(follower.status().isEmpty());

        QVERIFY(save(QJsonValue::Null));
        QTRY_VERIFY(! follower.status().isEmpty());
        QCOMPARE(follower.selection().value("workshopId").toString(), "42");
        QVERIFY(QFile::remove(fixture.config));

        QVERIFY(save(QJsonObject { { "Only",
                                     monitor("projects\\defaultprojects\\sample"
                                             "\\project.json") } }));
        QTRY_COMPARE(follower.selection().value("workshopId").toString(), "sample");
        follower.configure(fixture.steamPaths(), {}, false);
        QVERIFY(follower.selection().isEmpty());
        QVERIFY(follower.status().isEmpty());
        QVERIFY(follower.sections().isEmpty());
        QSignalSpy changes(&follower, &wekde::WallpaperEngineFollower::selectionChanged);
        QVERIFY(save(QJsonObject { { "Only", monitor(fixture.workshop + "/84/project.json") } }));
        QTest::qWait(150);
        QCOMPARE(changes.count(), 0);
    }
};
} // namespace

QTEST_GUILESS_MAIN(WallpaperEngineFollowerTest)
#include "tst_wallpaperenginefollower.moc"
