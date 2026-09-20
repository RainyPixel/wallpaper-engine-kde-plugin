// SPDX-License-Identifier: GPL-2.0-only
// Unit tests for wekde::FileHelper
//
// getDirSize behaviour note (depth > 0):
//   calcSize() starts at currentDepth=1 and recurses while currentDepth < depth,
//   so depth=N counts files up to N directory levels from the root.

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QSet>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QVariantList>
#include <QVariantMap>

#include "FileHelper.hpp"

using namespace wekde;

class TestFileHelper : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;

    // A steam library laid out the way Steam does, enough of it for
    // steam::detectLibrariesIn() to accept it.
    static bool makeLibrary(const QString& root, bool withInstall, bool withWorkshop) {
        const QString steamapps = root + "/steamapps";
        if (! QDir().mkpath(steamapps)) return false;

        if (withInstall) {
            const QString install = steamapps + "/common/wallpaper_engine";
            if (! QDir().mkpath(install + "/assets")) return false;
            if (! QDir().mkpath(install + "/projects/defaultprojects")) return false;
            if (! QDir().mkpath(install + "/projects/myprojects")) return false;
            if (! writeText(install + "/config.json", "{}")) return false;
            if (! writeText(steamapps + "/appmanifest_431960.acf",
                            "\"AppState\"\n{\n\t\"appid\"\t\t\"431960\"\n"
                            "\t\"installdir\"\t\t\"wallpaper_engine\"\n}\n"))
                return false;
        }
        if (withWorkshop && ! QDir().mkpath(steamapps + "/workshop/content/431960/1234567890"))
            return false;
        return true;
    }

    static bool writeText(const QString& filePath, const QByteArray& content) {
        if (! QDir().mkpath(QFileInfo(filePath).absolutePath())) return false;
        QFile f(filePath);
        if (! f.open(QIODevice::WriteOnly)) return false;
        return f.write(content) == content.size();
    }

    // Every returned path must be native, absolute and without a trailing slash.
    static void checkPathShape(const QString& path) {
        if (path.isEmpty()) return;
        QVERIFY(! path.startsWith("file:"));
        QVERIFY(path.startsWith('/'));
        QVERIFY(! path.endsWith('/'));
    }

    // Write `size` bytes to `filePath`; returns true on success.
    static bool writeBytes(const QString& filePath, int size, char fill = 'x') {
        QFile f(filePath);
        if (! f.open(QIODevice::WriteOnly)) return false;
        f.write(QByteArray(size, fill));
        return true;
    }

private slots:
    // ── test-suite setup / teardown ───────────────────────────────────────────
    void initTestCase() {
        // Redirect QStandardPaths to a safe test location so tests never
        // touch the real user config directory.
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY2(m_tmp.isValid(), "Could not create temporary directory for tests");
    }

    void cleanupTestCase() {
        QString testCfg =
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/wekde";
        QDir(testCfg).removeRecursively();
        QStandardPaths::setTestModeEnabled(false);
    }

    // ── readFile ──────────────────────────────────────────────────────────────
    void readFile_existingFile() {
        QTemporaryFile f(m_tmp.filePath("read_XXXXXX"));
        f.setAutoRemove(false);
        QVERIFY(f.open());
        f.write("hello world");
        f.close();

        FileHelper helper;
        QCOMPARE(helper.readFile(f.fileName()), QByteArray("hello world"));
    }

    void readFile_nonExistentFile() {
        FileHelper helper;
        QByteArray result = helper.readFile("/tmp/wekde_test_nonexistent_file_xyz.txt");
        QVERIFY(result.isEmpty());
    }

    void readFile_emptyFile() {
        QTemporaryFile f(m_tmp.filePath("empty_XXXXXX"));
        QVERIFY(f.open());
        f.close(); // zero bytes

        FileHelper helper;
        QVERIFY(helper.readFile(f.fileName()).isEmpty());
    }

    void readFile_binaryContent() {
        QByteArray binary;
        for (int i = 0; i < 256; ++i) binary.append(static_cast<char>(i));

        QTemporaryFile f(m_tmp.filePath("bin_XXXXXX"));
        f.setAutoRemove(false);
        QVERIFY(f.open());
        f.write(binary);
        f.close();

        FileHelper helper;
        QCOMPARE(helper.readFile(f.fileName()), binary);
    }

    // ── getDirSize ────────────────────────────────────────────────────────────
    void getDirSize_nonExistentDir() {
        FileHelper helper;
        QCOMPARE(helper.getDirSize("/tmp/wekde_test_nonexistent_dir_xyz"), qint64(0));
    }

    void getDirSize_emptyDir() {
        QTemporaryDir d;
        FileHelper    helper;
        QCOMPARE(helper.getDirSize(d.path()), qint64(0));
    }

    void getDirSize_topLevelFiles_depth1() {
        QTemporaryDir d;
        QVERIFY(writeBytes(d.filePath("a.txt"), 100));
        QVERIFY(writeBytes(d.filePath("b.txt"), 150));

        FileHelper helper;
        QCOMPARE(helper.getDirSize(d.path(), 1), qint64(250));
    }

    void getDirSize_depth1_ignoresSubdirFiles() {
        QTemporaryDir d;
        QVERIFY(writeBytes(d.filePath("root.txt"), 100));
        QVERIFY(d.path().length() > 0);
        QDir(d.path()).mkdir("sub");
        QVERIFY(writeBytes(d.filePath("sub/sub.txt"), 200));

        FileHelper helper;
        // depth=1 → only root files counted
        QCOMPARE(helper.getDirSize(d.path(), 1), qint64(100));
    }

    void getDirSize_depth2_includesOneLevel() {
        QTemporaryDir d;
        QVERIFY(writeBytes(d.filePath("root.txt"), 100));
        QDir(d.path()).mkdir("sub");
        QVERIFY(writeBytes(d.filePath("sub/sub.txt"), 200));
        QDir(d.filePath("sub")).mkdir("subsub");
        QVERIFY(writeBytes(d.filePath("sub/subsub/deep.txt"), 300));

        FileHelper helper;
        // depth=2 → root + immediate subdir, NOT sub/subsub
        QCOMPARE(helper.getDirSize(d.path(), 2), qint64(300));
    }

    void getDirSize_depth3_includesTwoLevels() {
        QTemporaryDir d;
        QVERIFY(writeBytes(d.filePath("root.txt"), 100));
        QDir(d.path()).mkdir("sub");
        QVERIFY(writeBytes(d.filePath("sub/sub.txt"), 200));
        QDir(d.filePath("sub")).mkdir("subsub");
        QVERIFY(writeBytes(d.filePath("sub/subsub/deep.txt"), 300));

        FileHelper helper;
        // depth=3 (default) → root + sub + sub/subsub
        QCOMPARE(helper.getDirSize(d.path(), 3), qint64(600));
    }

    void getDirSize_unlimitedDepth() {
        QTemporaryDir d;
        // Create a 4-level-deep file (beyond default depth=3)
        QDir r(d.path());
        QVERIFY(r.mkpath("a/b/c/d"));
        QVERIFY(writeBytes(d.filePath("a/b/c/d/deep.txt"), 500));

        FileHelper helper;
        // depth=0 → unlimited; must find the file no matter how deep
        QCOMPARE(helper.getDirSize(d.path(), 0), qint64(500));
    }

    // ── getFolderList ─────────────────────────────────────────────────────────
    void getFolderList_nonExistentDirNoFallback() {
        FileHelper  helper;
        QVariantMap result = helper.getFolderList("/tmp/wekde_test_nodir_xyz");
        QVERIFY(result.isEmpty());
    }

    void getFolderList_existingDir_returnsItems() {
        QTemporaryDir d;
        QDir(d.path()).mkdir("wallA");
        QDir(d.path()).mkdir("wallB");

        FileHelper  helper;
        QVariantMap result = helper.getFolderList(d.path());
        QVERIFY(! result.isEmpty());
        QCOMPARE(result["folder"].toString(), d.path());

        QVariantList items = result["items"].toList();
        QCOMPARE(items.size(), 2);

        // Each item must have "name" and numeric "mtime"
        for (const QVariant& v : items) {
            QVariantMap item = v.toMap();
            QVERIFY(item.contains("name"));
            QVERIFY(item.contains("mtime"));
            QVERIFY(item["mtime"].toLongLong() > 0);
        }
    }

    void getFolderList_fallbackUsedWhenPrimaryMissing() {
        QTemporaryDir d;
        QDir(d.path()).mkdir("fallback");

        FileHelper  helper;
        QVariantMap opts;
        opts["fallbacks"] = QStringList { d.filePath("fallback") };

        QVariantMap result = helper.getFolderList("/tmp/wekde_test_nodir_xyz", opts);
        QVERIFY(! result.isEmpty());
        QCOMPARE(result["folder"].toString(), d.filePath("fallback"));
    }

    void getFolderList_firstValidFallbackChosen() {
        QTemporaryDir d;
        QDir(d.path()).mkdir("second");

        FileHelper  helper;
        QVariantMap opts;
        // first fallback does not exist; second does
        opts["fallbacks"] = QStringList { "/tmp/wekde_no_such_dir_1", d.filePath("second") };

        QVariantMap result = helper.getFolderList("/tmp/wekde_no_such_dir_2", opts);
        QVERIFY(! result.isEmpty());
        QCOMPARE(result["folder"].toString(), d.filePath("second"));
    }

    void getFolderList_onlyDir_true_excludesFiles() {
        QTemporaryDir d;
        QDir(d.path()).mkdir("sub");
        QVERIFY(writeBytes(d.filePath("file.txt"), 1));

        FileHelper helper;
        // Default: only_dir=true
        QVariantMap  result = helper.getFolderList(d.path());
        QVariantList items  = result["items"].toList();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].toMap()["name"].toString(), QString("sub"));
    }

    void getFolderList_onlyDir_false_includesFiles() {
        QTemporaryDir d;
        QDir(d.path()).mkdir("sub");
        QVERIFY(writeBytes(d.filePath("file.txt"), 1));

        FileHelper  helper;
        QVariantMap opts;
        opts["only_dir"] = false;

        QVariantMap  result = helper.getFolderList(d.path(), opts);
        QVariantList items  = result["items"].toList();
        QCOMPARE(items.size(), 2);
    }

    void getFolderList_emptyDir_returnsEmptyItems() {
        QTemporaryDir d;
        FileHelper    helper;
        QVariantMap   result = helper.getFolderList(d.path());
        QVERIFY(! result.isEmpty());
        QVERIFY(result["items"].toList().isEmpty());
    }

    // ── wallpaper config round-trip ───────────────────────────────────────────
    void config_readNonExistent_returnsEmpty() {
        FileHelper helper;
        QVERIFY(helper.readWallpaperConfig("__no_such_wallpaper__").isEmpty());
    }

    void config_readCorruptJson_returnsEmpty() {
        // Write a file with invalid JSON directly into the config dir so that
        // readWallpaperConfig finds it but QJsonDocument::fromJson returns null.
        FileHelper    helper;
        const QString id = "test_corrupt";
        const QString path =
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
            "/wekde/wallpaper/" + id + ".json";
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not valid json {{{");
        f.close();

        QVERIFY(helper.readWallpaperConfig(id).isEmpty());

        QFile::remove(path);
    }

    void config_writeAndRead_roundTrip() {
        FileHelper    helper;
        const QString id = "test_roundtrip";

        QVariantMap cfg;
        cfg["volume"] = 75;
        cfg["fps"]    = 30;
        cfg["mute"]   = false;
        helper.writeWallpaperConfig(id, cfg);

        QVariantMap got = helper.readWallpaperConfig(id);
        QCOMPARE(got["volume"].toInt(), 75);
        QCOMPARE(got["fps"].toInt(), 30);
        QCOMPARE(got["mute"].toBool(), false);

        helper.resetWallpaperConfig(id);
    }

    void config_write_mergesPreviousValues() {
        FileHelper    helper;
        const QString id = "test_merge";

        helper.writeWallpaperConfig(id, { { "volume", 50 }, { "fps", 60 } });

        // Partial update: only change volume
        helper.writeWallpaperConfig(id, { { "volume", 80 } });

        QVariantMap got = helper.readWallpaperConfig(id);
        QCOMPARE(got["volume"].toInt(), 80);
        QCOMPARE(got["fps"].toInt(), 60); // unchanged

        helper.resetWallpaperConfig(id);
    }

    void config_write_addsNewKey() {
        FileHelper    helper;
        const QString id = "test_newkey";

        helper.writeWallpaperConfig(id, { { "a", 1 } });
        helper.writeWallpaperConfig(id, { { "b", 2 } });

        QVariantMap got = helper.readWallpaperConfig(id);
        QCOMPARE(got["a"].toInt(), 1);
        QCOMPARE(got["b"].toInt(), 2);

        helper.resetWallpaperConfig(id);
    }

    void config_reset_removesConfig() {
        FileHelper    helper;
        const QString id = "test_reset";

        helper.writeWallpaperConfig(id, { { "key", "value" } });
        QVERIFY(! helper.readWallpaperConfig(id).isEmpty());

        helper.resetWallpaperConfig(id);
        QVERIFY(helper.readWallpaperConfig(id).isEmpty());
    }

    void config_reset_nonExistent_noError() {
        FileHelper helper;
        // Resetting a config that was never written must not crash or throw
        helper.resetWallpaperConfig("__never_existed__");
        QVERIFY(helper.readWallpaperConfig("__never_existed__").isEmpty());
    }

    void config_stringValues_preserved() {
        FileHelper    helper;
        const QString id = "test_strings";

        helper.writeWallpaperConfig(id, { { "name", "My Wallpaper" }, { "path", "/some/path" } });

        QVariantMap got = helper.readWallpaperConfig(id);
        QCOMPARE(got["name"].toString(), QString("My Wallpaper"));
        QCOMPARE(got["path"].toString(), QString("/some/path"));

        helper.resetWallpaperConfig(id);
    }

    // ── detectSteam ───────────────────────────────────────────────────────────
    void detectSteam_noSteamRoot_allEmpty() {
        FileHelper  helper;
        QVariantMap got = helper.detectSteamIn({ m_tmp.filePath("no_such_steam") });
        QVERIFY(got["library"].toString().isEmpty());
        QVERIFY(got["projectDirs"].toStringList().isEmpty());
        QVERIFY(got["assets"].toString().isEmpty());
        QVERIFY(got["globalConfig"].toString().isEmpty());
    }

    void detectSteam_singleLibrary() {
        const QString root = m_tmp.filePath("single/root");
        QVERIFY(makeLibrary(root, true, true));

        FileHelper  helper;
        QVariantMap got = helper.detectSteamIn({ root });
        QCOMPARE(got["library"].toString(), QFileInfo(root).canonicalFilePath());

        // workshop content plus the install's defaultprojects and myprojects
        QStringList projectDirs = got["projectDirs"].toStringList();
        QCOMPARE(projectDirs.size(), 3);
        QVERIFY(projectDirs.first().endsWith("/steamapps/workshop/content/431960"));
        QVERIFY(projectDirs.at(1).endsWith("/projects/defaultprojects"));
        QVERIFY(projectDirs.at(2).endsWith("/projects/myprojects"));
        QVERIFY(got["assets"].toString().endsWith("/wallpaper_engine/assets"));
        QVERIFY(got["globalConfig"].toString().endsWith("/wallpaper_engine/config.json"));
    }

    void detectSteam_multipleLibraries_installFirst() {
        const QString root  = m_tmp.filePath("multi/root");
        const QString extra = m_tmp.filePath("multi/extra");
        // The app and one workshop tree in the steam root, a second workshop
        // tree on another library registered in libraryfolders.vdf.
        QVERIFY(makeLibrary(root, true, true));
        QVERIFY(makeLibrary(extra, false, true));
        QVERIFY(writeText(root + "/steamapps/libraryfolders.vdf",
                          "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" +
                              root.toUtf8() + "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" +
                              extra.toUtf8() + "\"\n\t}\n}\n"));

        FileHelper  helper;
        QVariantMap got = helper.detectSteamIn({ root });

        // library is the one carrying the install: it owns assets/ and config.json
        QCOMPARE(got["library"].toString(), QFileInfo(root).canonicalFilePath());
        QVERIFY(got["assets"].toString().startsWith(QFileInfo(root).canonicalFilePath()));

        // both libraries' workshop trees, then the install's project dirs
        QStringList projectDirs = got["projectDirs"].toStringList();
        QCOMPARE(projectDirs.size(), 4);
        QVERIFY(projectDirs.first().startsWith(QFileInfo(root).canonicalFilePath()));
        QVERIFY(projectDirs.at(1).startsWith(QFileInfo(extra).canonicalFilePath()));
    }

    void detectSteam_workshopOnly_noAssets() {
        const QString root = m_tmp.filePath("wsonly/root");
        QVERIFY(makeLibrary(root, false, true));

        FileHelper  helper;
        QVariantMap got = helper.detectSteamIn({ root });
        QCOMPARE(got["library"].toString(), QFileInfo(root).canonicalFilePath());
        QCOMPARE(got["projectDirs"].toStringList().size(), 1);
        QVERIFY(got["assets"].toString().isEmpty());
        QVERIFY(got["globalConfig"].toString().isEmpty());
    }

    void detectSteam_pathsAreNativeWithoutTrailingSlash() {
        const QString root = m_tmp.filePath("shape/root");
        QVERIFY(makeLibrary(root, true, true));

        FileHelper  helper;
        QVariantMap got = helper.detectSteamIn({ root });
        checkPathShape(got["library"].toString());
        checkPathShape(got["assets"].toString());
        checkPathShape(got["globalConfig"].toString());
        for (const QString& dir : got["projectDirs"].toStringList()) checkPathShape(dir);
    }

    void detectSteam_configuredLibraryWinsForAssets() {
        const QString first  = m_tmp.filePath("override/first");
        const QString second = m_tmp.filePath("override/second");
        // Two Steam installs, both carrying the app: a native one and, say, a
        // flatpak one. The folder button has to stay a real override.
        QVERIFY(makeLibrary(first, true, true));
        QVERIFY(makeLibrary(second, true, true));

        FileHelper        helper;
        const QStringList roots { first, second };
        QVERIFY(helper.detectSteamIn(roots, QString())["assets"].toString().startsWith(
            QFileInfo(first).canonicalFilePath()));
        QVERIFY(helper.detectSteamIn(roots, second)["assets"].toString().startsWith(
            QFileInfo(second).canonicalFilePath()));
        QVERIFY(helper.detectSteamIn(roots, second)["globalConfig"].toString().startsWith(
            QFileInfo(second).canonicalFilePath()));
    }

    void detectSteam_symlinkedLibraryIsListedOnce() {
        const QString root = m_tmp.filePath("symlink/root");
        QVERIFY(makeLibrary(root, true, true));
        // ~/.local/share/Steam is a symlink to ~/.steam/debian-installation on
        // Debian, and both spellings are probed.
        const QString link = m_tmp.filePath("symlink/alias");
        QVERIFY(QFile::link(root, link));

        FileHelper        helper;
        const QStringList dirs =
            helper.detectSteamIn({ root, link }, link)["projectDirs"].toStringList();
        QCOMPARE(dirs.size(), QSet<QString>(dirs.cbegin(), dirs.cend()).size());
        QCOMPARE(dirs.size(), 3);
    }

    void detectSteam_realMachine_shapeInvariants() {
        // Runs against whatever is installed; only invariants that always hold.
        FileHelper  helper;
        QVariantMap got = helper.detectSteam();
        QVERIFY(got.contains("library"));
        QVERIFY(got.contains("projectDirs"));
        QVERIFY(got.contains("assets"));
        QVERIFY(got.contains("globalConfig"));

        // Content without a library root would break the QML wiring. The
        // reverse of the second one does not hold: assets/ ships with the app
        // but config.json is only written once it has been run.
        if (! got["projectDirs"].toStringList().isEmpty())
            QVERIFY(! got["library"].toString().isEmpty());
        if (! got["globalConfig"].toString().isEmpty())
            QVERIFY(! got["assets"].toString().isEmpty());

        checkPathShape(got["library"].toString());
        checkPathShape(got["assets"].toString());
        checkPathShape(got["globalConfig"].toString());
        for (const QString& dir : got["projectDirs"].toStringList()) checkPathShape(dir);
    }
};

QTEST_MAIN(TestFileHelper)
#include "tst_filehelper.moc"
