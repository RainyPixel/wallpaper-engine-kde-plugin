// SPDX-License-Identifier: GPL-2.0-only
// Unit tests for command line parsing and output selection

#include <QtTest>

#include "Options.hpp"

using namespace wehypr;

class TestOptions : public QObject {
    Q_OBJECT

private:
    static ParseResult parse(QStringList args) {
        args.prepend(QStringLiteral("wallpaper-engine-hyprland"));
        return parseCommandLine(args);
    }

private slots:
    // ── commands ──────────────────────────────────────────────────────────────
    void missingCommand_isError() {
        QVERIFY(! parseCommandLine({ QStringLiteral("wallpaper-engine-hyprland") }).ok());
    }

    void unknownCommand_isError() { QVERIFY(! parse({ "start" }).ok()); }

    void help_and_version() {
        QCOMPARE(parse({ "help" }).options.command, Command::Help);
        QCOMPARE(parse({ "--help" }).options.command, Command::Help);
        QCOMPARE(parse({ "run", "--help" }).options.command, Command::Help);
        QCOMPARE(parse({ "--version" }).options.command, Command::Version);
    }

    void run_defaults() {
        const ParseResult result = parse({ "run", "/tmp/project" });
        QVERIFY2(result.ok(), qPrintable(result.error));
        const Options& o = result.options;
        QCOMPARE(o.command, Command::Run);
        QCOMPARE(o.project, QStringLiteral("/tmp/project"));
        QCOMPARE(o.instance, QStringLiteral("default"));
        QCOMPARE(o.fps, 30);
        QVERIFY(o.outputs.isEmpty());
        QVERIFY(! o.allOutputs);
        QVERIFY(o.disableGpuRasterization);
        QVERIFY(! o.audio);
        QVERIFY(! o.allowRemote);
        QVERIFY(! o.window);
        QCOMPARE(o.durationSeconds, 0);
    }

    void run_allOptions() {
        const ParseResult result = parse({ "run",
                                           "p",
                                           "--output",
                                           "DP-1",
                                           "--output=HDMI-A-1",
                                           "--output",
                                           "DP-1",
                                           "--instance",
                                           "desk_2",
                                           "--fps",
                                           "60",
                                           "--properties",
                                           R"({"speed":0.5})",
                                           "--audio",
                                           "--allow-remote",
                                           "--window",
                                           "--diagnostics",
                                           "--gpu-rasterization",
                                           "auto",
                                           "--duration",
                                           "20" });
        QVERIFY2(result.ok(), qPrintable(result.error));
        const Options& o = result.options;
        QCOMPARE(o.outputs, QStringList({ "DP-1", "HDMI-A-1" }));
        QCOMPARE(o.instance, QStringLiteral("desk_2"));
        QCOMPARE(o.fps, 60);
        QCOMPARE(o.properties.value(QStringLiteral("speed")).toDouble(), 0.5);
        QVERIFY(o.audio && o.allowRemote && o.window && o.diagnostics);
        QVERIFY(! o.disableGpuRasterization);
        QCOMPARE(o.durationSeconds, 20);
    }

    void run_invalidValues_data() {
        QTest::addColumn<QStringList>("args");
        QTest::newRow("no project") << QStringList { "run" };
        QTest::newRow("two projects") << QStringList { "run", "a", "b" };
        QTest::newRow("fps zero") << QStringList { "run", "p", "--fps", "0" };
        QTest::newRow("fps text") << QStringList { "run", "p", "--fps", "fast" };
        QTest::newRow("fps too high") << QStringList { "run", "p", "--fps", "241" };
        QTest::newRow("duration negative") << QStringList { "run", "p", "--duration", "-1" };
        QTest::newRow("gpu mode") << QStringList { "run", "p", "--gpu-rasterization", "on" };
        QTest::newRow("instance slash") << QStringList { "run", "p", "--instance", "../x" };
        QTest::newRow("instance empty") << QStringList { "run", "p", "--instance", "" };
        QTest::newRow("instance long")
            << QStringList { "run", "p", "--instance", QString(33, QLatin1Char('a')) };
        QTest::newRow("empty output") << QStringList { "run", "p", "--output", "" };
        QTest::newRow("output and all")
            << QStringList { "run", "p", "--output", "DP-1", "--all-outputs" };
        QTest::newRow("properties array") << QStringList { "run", "p", "--properties", "[1]" };
        QTest::newRow("properties broken") << QStringList { "run", "p", "--properties", "{" };
        QTest::newRow("unknown option") << QStringList { "run", "p", "--preview" };
        QTest::newRow("missing value") << QStringList { "run", "p", "--fps" };
    }

    void run_invalidValues() {
        QFETCH(QStringList, args);
        const ParseResult result = parse(args);
        QVERIFY(! result.ok());
        QVERIFY(! result.error.isEmpty());
    }

    void control_commands() {
        const ParseResult status = parse({ "status", "--instance", "test" });
        QVERIFY2(status.ok(), qPrintable(status.error));
        QCOMPARE(status.options.command, Command::Status);
        QCOMPARE(status.options.instance, QStringLiteral("test"));
        QVERIFY(isControlCommand(Command::Status));
        QCOMPARE(controlCommandName(Command::SetProperties), QStringLiteral("set-properties"));

        QVERIFY(! parse({ "pause", "extra" }).ok());
        QVERIFY(! parse({ "quit", "--fps", "10" }).ok());
        QVERIFY(! isControlCommand(Command::Run));
        QVERIFY(! isControlCommand(Command::Check));
    }

    void setProperties_needsObject() {
        const ParseResult ok = parse({ "set-properties", R"({"speed":{"value":2}})" });
        QVERIFY2(ok.ok(), qPrintable(ok.error));
        QVERIFY(ok.options.properties.value(QStringLiteral("speed")).isObject());

        QVERIFY(! parse({ "set-properties" }).ok());
        QVERIFY(! parse({ "set-properties", "2" }).ok());
        QVERIFY(! parse({ "set-properties", "{}", "{}" }).ok());
    }

    void check_acceptsOnlyProperties() {
        QVERIFY(parse({ "check", "p", "--properties", "{}" }).ok());
        QVERIFY(! parse({ "check", "p", "--output", "DP-1" }).ok());
    }

    // ── output selection ──────────────────────────────────────────────────────
    void select_singleOutputImplicit() {
        const auto s = selectOutputs({ "eDP-1" }, {}, false);
        QVERIFY(s.error.isEmpty());
        QCOMPARE(s.selected, QStringList { "eDP-1" });
    }

    void select_severalOutputsNeedChoice() {
        const auto s = selectOutputs({ "eDP-1", "DP-2" }, {}, false);
        QVERIFY(s.selected.isEmpty());
        QVERIFY(s.error.contains("eDP-1"));
        QVERIFY(s.error.contains("DP-2"));
    }

    void select_noOutputs() {
        QVERIFY(! selectOutputs({}, {}, false).error.isEmpty());
        QVERIFY(! selectOutputs({}, {}, true).error.isEmpty());
        QVERIFY(! selectOutputs({}, { "DP-1" }, false).error.isEmpty());
    }

    void select_all() {
        const auto s = selectOutputs({ "eDP-1", "DP-2" }, {}, true);
        QVERIFY(s.error.isEmpty());
        QCOMPARE(s.selected, QStringList({ "eDP-1", "DP-2" }));
    }

    void select_requestedWithMissing() {
        const auto s = selectOutputs({ "eDP-1", "DP-2" }, { "DP-2", "HDMI-A-1", "DP-2" }, false);
        QVERIFY(s.error.isEmpty());
        QCOMPARE(s.selected, QStringList { "DP-2" });
        QCOMPARE(s.missing, QStringList { "HDMI-A-1" });
    }

    void select_requestedNoneConnected() {
        const auto s = selectOutputs({ "eDP-1" }, { "DP-9" }, false);
        QVERIFY(s.selected.isEmpty());
        QVERIFY(s.error.contains("eDP-1"));
    }
};

QTEST_GUILESS_MAIN(TestOptions)
#include "tst_options.moc"
