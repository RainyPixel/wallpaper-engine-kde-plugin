// SPDX-License-Identifier: GPL-2.0-only
// Unit tests for project.json validation and user property updates

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "ProjectConfig.hpp"

using namespace wehypr;

class TestProject : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_tmp;
    QString                        m_root;

    static bool writeFile(const QString& path, const QByteArray& data) {
        QFile f(path);
        if (! f.open(QIODevice::WriteOnly)) return false;
        return f.write(data) == data.size();
    }

    bool writeProject(const QJsonObject& project) {
        return writeFile(m_root + "/project.json", QJsonDocument(project).toJson());
    }

    static QJsonObject web(const QString& file) { return { { "type", "web" }, { "file", file } }; }

    static QJsonObject definitions() {
        const auto doc = QJsonDocument::fromJson(R"({
            "speed":  {"type": "slider", "min": 0.1, "max": 3, "value": 1},
            "grid":   {"type": "bool", "value": true},
            "accent": {"type": "color", "value": "0.2 0.7 0.9"},
            "shape":  {"type": "combo", "value": "circle",
                       "options": [{"label": "Circle", "value": "circle"},
                                   {"label": "Level two", "value": 2}]},
            "label":  {"type": "textinput", "value": "x"},
            "header": {"type": "text"},
            "image":  {"type": "file", "value": ""},
            "custom": {"value": 5},
            "legacy": {"text": "no type"}
        })");
        return doc.object();
    }

    std::optional<Project> load(QString* error) { return loadProject(m_root, error); }

private slots:
    void init() {
        m_tmp = std::make_unique<QTemporaryDir>();
        QVERIFY(m_tmp->isValid());
        m_root = m_tmp->path() + "/project";
        QVERIFY(QDir().mkpath(m_root));
        QVERIFY(writeFile(m_root + "/index.html", "<html></html>"));
    }

    void cleanup() { m_tmp.reset(); }

    // ── loadProject ───────────────────────────────────────────────────────────
    void minimalProject() {
        QVERIFY(writeProject(web("index.html")));
        QString    error;
        const auto project = load(&error);
        QVERIFY2(project, qPrintable(error));
        QCOMPARE(project->type, QStringLiteral("web"));
        QCOMPARE(project->rootDir, QDir(m_root).canonicalPath());
        QCOMPARE(project->entryFile, QFileInfo(m_root + "/index.html").canonicalFilePath());
        QCOMPARE(project->entryUrl, QUrl::fromLocalFile(project->entryFile));
        QVERIFY(project->properties.isEmpty());
        QVERIFY(project->title.isEmpty());
    }

    void projectFileAsPath_andTitle() {
        QJsonObject data = web("index.html");
        data.insert("title", "Demo");
        QVERIFY(writeProject(data));
        QString    error;
        const auto project = loadProject(m_root + "/project.json", &error);
        QVERIFY2(project, qPrintable(error));
        QCOMPARE(project->title, QStringLiteral("Demo"));
    }

    void typeIsCaseInsensitive_andBomAccepted() {
        const QByteArray json = "\xEF\xBB\xBF"
                                R"({"type": "Web", "file": "index.html"})";
        QVERIFY(writeFile(m_root + "/project.json", json));
        QString error;
        QVERIFY2(load(&error), qPrintable(error));
    }

    void encodedEntryPath() {
        const QString name = QString::fromUtf8("page # ü.html");
        QVERIFY(writeFile(m_root + "/" + name, "<html></html>"));
        QVERIFY(writeProject(web(name)));
        QString    error;
        const auto project = load(&error);
        QVERIFY2(project, qPrintable(error));
        const QByteArray encoded = project->entryUrl.toEncoded();
        QVERIFY(encoded.contains("%23"));
        QVERIFY(encoded.contains("%C3%BC"));
        QCOMPARE(project->entryUrl.toLocalFile(), project->entryFile);
    }

    void nestedEntry_andSymlinkInsideProject() {
        QVERIFY(QDir().mkpath(m_root + "/site"));
        QVERIFY(writeFile(m_root + "/site/main.htm", "<html></html>"));
        QVERIFY(QFile::link(m_root + "/site/main.htm", m_root + "/link.html"));
        QString error;
        QVERIFY(writeProject(web("site/main.htm")));
        QVERIFY2(load(&error), qPrintable(error));
        QVERIFY(writeProject(web("link.html")));
        QVERIFY2(load(&error), qPrintable(error));
    }

    void entryOutsideProjectRejected_data() {
        QTest::addColumn<QString>("entry");
        QTest::newRow("parent") << QStringLiteral("../outside.html");
        QTest::newRow("symlink") << QStringLiteral("escape.html");
        QTest::newRow("absolute") << QStringLiteral("ABSOLUTE");
        QTest::newRow("sibling prefix") << QStringLiteral("../project2/index.html");
    }

    void entryOutsideProjectRejected() {
        QFETCH(QString, entry);
        const QString outside = m_tmp->path() + "/outside.html";
        QVERIFY(writeFile(outside, "<html></html>"));
        QVERIFY(QDir().mkpath(m_tmp->path() + "/project2"));
        QVERIFY(writeFile(m_tmp->path() + "/project2/index.html", "<html></html>"));
        QVERIFY(QFile::link(outside, m_root + "/escape.html"));
        if (entry == QLatin1String("ABSOLUTE")) entry = outside;

        QVERIFY(writeProject(web(entry)));
        QString error;
        QVERIFY(! load(&error));
        QVERIFY(! error.isEmpty());
    }

    void invalidProjects_data() {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("not json") << QByteArray("{");
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("no type") << QByteArray(R"({"file": "index.html"})");
        QTest::newRow("scene") << QByteArray(R"({"type": "scene", "file": "scene.pkg"})");
        QTest::newRow("video") << QByteArray(R"({"type": "video", "file": "clip.mp4"})");
        QTest::newRow("unknown type") << QByteArray(R"({"type": "application", "file": "a.exe"})");
        QTest::newRow("no file") << QByteArray(R"({"type": "web"})");
        QTest::newRow("file number") << QByteArray(R"({"type": "web", "file": 3})");
        QTest::newRow("missing entry") << QByteArray(R"({"type": "web", "file": "missing.html"})");
        QTest::newRow("not html") << QByteArray(R"({"type": "web", "file": "style.css"})");
        QTest::newRow("directory") << QByteArray(R"({"type": "web", "file": "folder.html"})");
        QTest::newRow("title number")
            << QByteArray(R"({"type": "web", "file": "index.html", "title": 1})");
        QTest::newRow("general array")
            << QByteArray(R"({"type": "web", "file": "index.html", "general": []})");
        QTest::newRow("properties array") << QByteArray(
            R"({"type": "web", "file": "index.html", "general": {"properties": []}})");
        QTest::newRow("property scalar") << QByteArray(
            R"({"type": "web", "file": "index.html", "general": {"properties": {"a": 1}}})");
    }

    void invalidProjects() {
        QFETCH(QByteArray, json);
        QVERIFY(writeFile(m_root + "/style.css", "body {}"));
        QVERIFY(QDir().mkpath(m_root + "/folder.html"));
        QVERIFY(writeFile(m_root + "/project.json", json));
        QString error;
        QVERIFY(! load(&error));
        QVERIFY(! error.isEmpty());
    }

    void missingPaths() {
        QString error;
        QVERIFY(! loadProject(m_tmp->path() + "/does-not-exist", &error));
        QVERIFY(! error.isEmpty());
        error.clear();
        QVERIFY(! loadProject(m_root, &error)); // directory without project.json
        QVERIFY(error.contains("project.json"));
    }

    void propertiesLoaded() {
        QJsonObject data = web("index.html");
        data.insert("general", QJsonObject { { "properties", definitions() } });
        QVERIFY(writeProject(data));
        QString    error;
        const auto project = load(&error);
        QVERIFY2(project, qPrintable(error));
        QCOMPARE(project->properties, definitions());
    }

    // ── validatePropertyValues ────────────────────────────────────────────────
    void validValues_data() {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("slider") << QByteArray(R"({"speed": 2.5})");
        QTest::newRow("slider bounds") << QByteArray(R"({"speed": 3})");
        QTest::newRow("wrapped") << QByteArray(R"({"speed": {"value": 0.1}})");
        QTest::newRow("bool") << QByteArray(R"({"grid": false})");
        QTest::newRow("color") << QByteArray(R"({"accent": "1  0 0.5"})");
        QTest::newRow("color alpha") << QByteArray(R"({"accent": "1 0 0.5 1"})");
        QTest::newRow("combo string") << QByteArray(R"({"shape": "circle"})");
        QTest::newRow("combo number") << QByteArray(R"({"shape": 2})");
        QTest::newRow("combo number as string") << QByteArray(R"({"shape": "2"})");
        QTest::newRow("textinput") << QByteArray(R"({"label": "hello"})");
        QTest::newRow("untyped same type") << QByteArray(R"({"custom": 7})");
        QTest::newRow("several") << QByteArray(R"({"speed": 1, "grid": true})");
        QTest::newRow("empty") << QByteArray("{}");
    }

    void validValues() {
        QFETCH(QByteArray, json);
        const QJsonObject values = QJsonDocument::fromJson(json).object();
        QString           error;
        const auto        result = validatePropertyValues(definitions(), values, &error);
        QVERIFY2(result, qPrintable(error));
        QCOMPARE(result->size(), values.size());
        for (auto it = result->constBegin(); it != result->constEnd(); ++it)
            QVERIFY(! it.value().isObject());
    }

    void invalidValues_data() {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("unknown") << QByteArray(R"({"brightness": 1})");
        QTest::newRow("slider string") << QByteArray(R"({"speed": "fast"})");
        QTest::newRow("below min") << QByteArray(R"({"speed": 0})");
        QTest::newRow("above max") << QByteArray(R"({"speed": 3.5})");
        QTest::newRow("bool number") << QByteArray(R"({"grid": 1})");
        QTest::newRow("color two parts") << QByteArray(R"({"accent": "1 0"})");
        QTest::newRow("color text") << QByteArray(R"({"accent": "red green blue"})");
        QTest::newRow("color number") << QByteArray(R"({"accent": 1})");
        QTest::newRow("combo unknown") << QByteArray(R"({"shape": "triangle"})");
        QTest::newRow("combo bool") << QByteArray(R"({"shape": true})");
        QTest::newRow("textinput number") << QByteArray(R"({"label": 5})");
        QTest::newRow("text type") << QByteArray(R"({"header": "x"})");
        QTest::newRow("file type") << QByteArray(R"({"image": "/etc/passwd"})");
        QTest::newRow("untyped other type") << QByteArray(R"({"custom": "5"})");
        QTest::newRow("untyped without value") << QByteArray(R"({"legacy": "x"})");
        QTest::newRow("wrapped extra key") << QByteArray(R"({"speed": {"value": 1, "min": 0}})");
        QTest::newRow("array") << QByteArray(R"({"custom": [5]})");
    }

    void invalidValues() {
        QFETCH(QByteArray, json);
        QString error;
        QVERIFY(! validatePropertyValues(
            definitions(), QJsonDocument::fromJson(json).object(), &error));
        QVERIFY(! error.isEmpty());
    }

    void textinputLengthLimited() {
        QString     error;
        QJsonObject values { { "label", QString(5000, QLatin1Char('a')) } };
        QVERIFY(! validatePropertyValues(definitions(), values, &error));
    }

    // ── applyPropertyValues ───────────────────────────────────────────────────
    void applyKeepsDefinition() {
        QJsonObject defs    = definitions();
        const auto  changed = applyPropertyValues(&defs, { { "speed", 2 }, { "grid", false } });
        QCOMPARE(changed.size(), qsizetype(2));
        QCOMPARE(changed.value("speed").toObject().value("value").toDouble(), 2.0);
        QCOMPARE(changed.value("speed").toObject().value("max").toDouble(), 3.0);
        QCOMPARE(defs.value("grid").toObject().value("value").toBool(), false);
        QCOMPARE(defs.value("label"), definitions().value("label"));
    }

    // ── isInsideProject ───────────────────────────────────────────────────────
    void navigationScope() {
        const QString root = QDir(m_root).canonicalPath();
        QVERIFY(isInsideProject(root, QUrl::fromLocalFile(root + "/index.html")));
        QVERIFY(isInsideProject(root, QUrl::fromLocalFile(root + "/sub/not-yet.html")));
        QVERIFY(! isInsideProject(root, QUrl::fromLocalFile(root + "/../outside.html")));
        QVERIFY(! isInsideProject(root, QUrl::fromLocalFile(root + "2/index.html")));
        QVERIFY(! isInsideProject(root, QUrl::fromLocalFile(root)));
        QVERIFY(! isInsideProject(root, QUrl("https://example.org/")));
        QVERIFY(! isInsideProject(root, QUrl("about:blank")));
    }
};

QTEST_GUILESS_MAIN(TestProject)
#include "tst_project.moc"
