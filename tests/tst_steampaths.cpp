// SPDX-License-Identifier: GPL-2.0-only
// Unit tests for steam:: Steam library detection
//
// Every test builds a fake Steam tree in a temporary directory, so nothing here
// depends on Steam being installed on the machine running the suite.

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "SteamPaths.hpp"

class TestSteamPaths : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;

    QString path(const QString& relative) const { return m_tmp.filePath(relative); }

    static bool makeDir(const QString& dirPath) { return QDir().mkpath(dirPath); }

    static bool writeFile(const QString& filePath, const QByteArray& content) {
        if (! QDir().mkpath(QFileInfo(filePath).absolutePath())) return false;
        QFile file(filePath);
        if (! file.open(QIODevice::WriteOnly)) return false;
        return file.write(content) == content.size();
    }

    // A library laid out the way Steam does: steamapps/ with the manifest, the
    // install under common/ and the workshop tree. `steamappsName` and
    // `workshopSegments` let a test reproduce a Windows-cased NTFS library.
    bool makeLibrary(const QString& root, bool withInstall, bool withWorkshop,
                     const QString&     steamappsName    = QStringLiteral("steamapps"),
                     const QStringList& workshopSegments = { QStringLiteral("workshop"),
                                                             QStringLiteral("content") },
                     const QString&     installDirName   = QStringLiteral("wallpaper_engine")) {
        const QString steamapps = root + u'/' + steamappsName;
        if (! makeDir(steamapps)) return false;

        if (withInstall) {
            const QString install = steamapps + QStringLiteral("/common/") + installDirName;
            if (! makeDir(install + QStringLiteral("/assets"))) return false;
            if (! makeDir(install + QStringLiteral("/projects/defaultprojects"))) return false;
            if (! makeDir(install + QStringLiteral("/projects/myprojects"))) return false;
            if (! writeFile(install + QStringLiteral("/config.json"), "{}")) return false;
            if (! writeFile(steamapps + QStringLiteral("/appmanifest_431960.acf"),
                            "\"AppState\"\n{\n\t\"appid\"\t\t\"431960\"\n"
                            "\t\"StateFlags\"\t\t\"4\"\n\t\"installdir\"\t\t\"" +
                                installDirName.toUtf8() + "\"\n}\n"))
                return false;
        }
        if (withWorkshop) {
            QString workshop = steamapps;
            for (const QString& segment : workshopSegments) workshop += u'/' + segment;
            workshop += QStringLiteral("/431960");
            if (! makeDir(workshop + QStringLiteral("/1234567890"))) return false;
        }
        return true;
    }

private slots:
    void initTestCase() { QVERIFY2(m_tmp.isValid(), "Could not create temporary directory"); }

    // ── libraryRoots ──────────────────────────────────────────────────────────
    void libraryRoots_modernFormat() {
        const QString root  = path("modern/root");
        const QString extra = path("modern/extra");
        QVERIFY(makeLibrary(root, true, true));
        QVERIFY(makeLibrary(extra, false, true));

        QVERIFY(writeFile(root + "/steamapps/libraryfolders.vdf",
                          "\"libraryfolders\"\n{\n"
                          "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" +
                              root.toUtf8() +
                              "\"\n"
                              "\t\t\"apps\"\n\t\t{\n\t\t\t\"431960\"\t\t\"123456\"\n\t\t}\n\t}\n"
                              "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" +
                              extra.toUtf8() + "\"\n\t\t\"apps\"\n\t\t{\n\t\t}\n\t}\n}\n"));

        const QStringList roots = steam::libraryRoots(root);
        QCOMPARE(roots.size(), 2);
        QVERIFY(roots.contains(QFileInfo(root).canonicalFilePath()));
        QVERIFY(roots.contains(QFileInfo(extra).canonicalFilePath()));
    }

    void libraryRoots_legacyFormat() {
        const QString root  = path("legacy/root");
        const QString extra = path("legacy/extra");
        QVERIFY(makeLibrary(root, true, true));
        QVERIFY(makeLibrary(extra, false, true));

        // Pre-2021 shape: numeric keys map straight to a path, interleaved with
        // bookkeeping keys that must be skipped, and the root is not listed.
        QVERIFY(writeFile(root + "/steamapps/libraryfolders.vdf",
                          "\"LibraryFolders\"\n{\n"
                          "\t\"TimeNextStatsReport\"\t\t\"1600000000\"\n"
                          "\t\"ContentStatsID\"\t\t\"-123456789\"\n"
                          "\t\"1\"\t\t\"" +
                              extra.toUtf8() + "\"\n}\n"));

        const QStringList roots = steam::libraryRoots(root);
        QCOMPARE(roots.size(), 2);
        QCOMPARE(roots.first(), QFileInfo(root).canonicalFilePath());
        QVERIFY(roots.contains(QFileInfo(extra).canonicalFilePath()));
    }

    void libraryRoots_mergesConfigCopy() {
        const QString root  = path("merge/root");
        const QString extra = path("merge/extra");
        QVERIFY(makeLibrary(root, true, false));
        QVERIFY(makeLibrary(extra, false, true));

        // Only the newer config/ copy knows about the second library.
        QVERIFY(writeFile(root + "/steamapps/libraryfolders.vdf",
                          "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" +
                              root.toUtf8() + "\"\n\t}\n}\n"));
        QVERIFY(writeFile(root + "/config/libraryfolders.vdf",
                          "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" +
                              root.toUtf8() + "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" +
                              extra.toUtf8() + "\"\n\t}\n}\n"));

        QCOMPARE(steam::libraryRoots(root).size(), 2);
    }

    void libraryRoots_toleratesCommentsBomAndEscapes() {
        const QString root = path("messy/root");
        QVERIFY(makeLibrary(root, true, true));
        QVERIFY(writeFile(root + "/steamapps/libraryfolders.vdf",
                          QByteArray("\xEF\xBB\xBF") +
                              "// written by Steam\n"
                              "\"libraryfolders\"\n{\n"
                              "\t\"0\"\n\t{\n"
                              "\t\t\"path\"\t\t\"" +
                              root.toUtf8() +
                              "\"\n"
                              "\t\t\"label\"\t\t\"say \\\"hi\\\"\"\n"
                              "\t\t\"mounted\"\t\t\"1\"\t\t[$WIN32]\n"
                              "\t}\n}\n"));

        // A malformed or unreadable file must not lose the Steam root itself.
        QCOMPARE(steam::libraryRoots(root).size(), 1);
    }

    void libraryRoots_missingVdfKeepsRoot() {
        const QString root = path("novdf/root");
        QVERIFY(makeLibrary(root, true, true));
        QCOMPARE(steam::libraryRoots(root), QStringList { QFileInfo(root).canonicalFilePath() });
    }

    // ── resolvePath ───────────────────────────────────────────────────────────
    void resolvePath_exactAndCaseInsensitive() {
        const QString base = path("resolve");
        QVERIFY(makeDir(base + "/SteamApps/Workshop/Content/431960"));

        QCOMPARE(steam::resolvePath(base, { "SteamApps", "Workshop", "Content" }),
                 base + "/SteamApps/Workshop/Content");
        // Windows-cased library reached through the lowercase Linux spelling.
        QCOMPARE(steam::resolvePath(base, { "steamapps", "workshop", "content" }),
                 base + "/SteamApps/Workshop/Content");
        QCOMPARE(steam::resolvePath(base, { "steamapps", "nope" }), QString());
        QCOMPARE(steam::resolvePath(path("resolve-missing"), { "steamapps" }), QString());
    }

    // ── detectLibrariesIn ─────────────────────────────────────────────────────
    void detect_findsInstallAndWorkshop() {
        const QString root = path("detect/root");
        QVERIFY(makeLibrary(root, true, true));

        const QList<steam::Library> libraries = steam::detectLibrariesIn({ root });
        QCOMPARE(libraries.size(), 1);
        QCOMPARE(libraries.first().root, QFileInfo(root).canonicalFilePath());
        QVERIFY(libraries.first().workshopDir.endsWith("/steamapps/workshop/content/431960"));
        QVERIFY(libraries.first().installDir.endsWith("/common/wallpaper_engine"));
    }

    void detect_installAndWorkshopInDifferentLibraries() {
        const QString root  = path("split/root");
        const QString extra = path("split/extra");
        // The app lives in the Steam root, the workshop content on another drive.
        QVERIFY(makeLibrary(root, true, false));
        QVERIFY(makeLibrary(extra, false, true));
        QVERIFY(writeFile(root + "/steamapps/libraryfolders.vdf",
                          "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" +
                              root.toUtf8() + "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" +
                              extra.toUtf8() + "\"\n\t}\n}\n"));

        const QList<steam::Library> libraries = steam::detectLibrariesIn({ root });
        QCOMPARE(libraries.size(), 2);
        // The library carrying the install sorts first: it owns assets/ and
        // config.json, which exist only once.
        QVERIFY(! libraries.first().installDir.isEmpty());
        QVERIFY(libraries.first().workshopDir.isEmpty());
        QVERIFY(libraries.at(1).installDir.isEmpty());
        QVERIFY(! libraries.at(1).workshopDir.isEmpty());
    }

    void detect_readsInstallDirNameFromManifest() {
        const QString root = path("renamed/root");
        // Steam keeps the directory name in the manifest; it is not always
        // spelled "wallpaper_engine".
        QVERIFY(makeLibrary(root,
                            true,
                            false,
                            QStringLiteral("steamapps"),
                            { QStringLiteral("workshop"), QStringLiteral("content") },
                            QStringLiteral("Wallpaper Engine")));

        const QList<steam::Library> libraries = steam::detectLibrariesIn({ root });
        QCOMPARE(libraries.size(), 1);
        QVERIFY(libraries.first().installDir.endsWith("/common/Wallpaper Engine"));
    }

    void detect_windowsCasedLibrary() {
        const QString root = path("ntfs/root");
        QVERIFY(makeLibrary(root,
                            true,
                            true,
                            QStringLiteral("SteamApps"),
                            { QStringLiteral("Workshop"), QStringLiteral("Content") }));

        const QList<steam::Library> libraries = steam::detectLibrariesIn({ root });
        QCOMPARE(libraries.size(), 1);
        QVERIFY(libraries.first().workshopDir.endsWith("/SteamApps/Workshop/Content/431960"));
        QVERIFY(libraries.first().installDir.endsWith("/SteamApps/common/wallpaper_engine"));
    }

    void detect_manifestWithoutFilesIsRejected() {
        const QString root      = path("stale/root");
        const QString steamapps = root + "/steamapps";
        QVERIFY(makeDir(steamapps));
        // A failed uninstall can leave the manifest behind with no directory.
        QVERIFY(writeFile(steamapps + "/appmanifest_431960.acf",
                          "\"AppState\"\n{\n\t\"installdir\"\t\t\"wallpaper_engine\"\n}\n"));

        QVERIFY(steam::detectLibrariesIn({ root }).isEmpty());
    }

    void detect_installWithoutManifestIsStillFound() {
        const QString root = path("nomanifest/root");
        QVERIFY(makeDir(root + "/steamapps/common/wallpaper_engine/assets"));

        const QList<steam::Library> libraries = steam::detectLibrariesIn({ root });
        QCOMPARE(libraries.size(), 1);
        QVERIFY(libraries.first().installDir.endsWith("/common/wallpaper_engine"));
    }

    void detect_deduplicatesRootsReachedTwice() {
        const QString root = path("dedupe/root");
        QVERIFY(makeLibrary(root, true, true));
        QVERIFY(writeFile(root + "/steamapps/libraryfolders.vdf",
                          "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" +
                              root.toUtf8() + "\"\n\t}\n}\n"));

        // Listed by the vdf, passed in directly, and reached again through a
        // second Steam root that is a symlink to the first.
        QCOMPARE(steam::detectLibrariesIn({ root, root }).size(), 1);
    }

    void detect_noSteamReturnsEmpty() {
        QVERIFY(steam::detectLibrariesIn({ path("does-not-exist") }).isEmpty());
        QVERIFY(steam::detectLibrariesIn({}).isEmpty());
    }

    void detect_emptyLibraryIsDropped() {
        const QString root = path("empty/root");
        QVERIFY(makeDir(root + "/steamapps/common/some_other_game"));
        QVERIFY(steam::detectLibrariesIn({ root }).isEmpty());
    }

    // ── derived paths ─────────────────────────────────────────────────────────
    void derivedPaths() {
        const QString root = path("derived/root");
        QVERIFY(makeLibrary(root, true, false));
        const QString install = steam::detectLibrariesIn({ root }).first().installDir;

        QCOMPARE(steam::assetsDir(install), install + "/assets");
        QCOMPARE(steam::globalConfigPath(install), install + "/config.json");
        QCOMPARE(steam::defaultProjectsDir(install), install + "/projects/defaultprojects");
        QCOMPARE(steam::myProjectsDir(install), install + "/projects/myprojects");
        QCOMPARE(steam::assetsDir(QString()), QString());
    }

    // ── steamRoots ────────────────────────────────────────────────────────────
    void steamRoots_returnsOnlyExistingDirectories() {
        // The machine running the tests may or may not have Steam; either way
        // every returned path must be an existing directory.
        for (const QString& root : steam::steamRoots()) QVERIFY(QFileInfo(root).isDir());
    }
};

QTEST_MAIN(TestSteamPaths)
#include "tst_steampaths.moc"
