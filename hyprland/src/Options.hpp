#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace wehypr
{

enum ExitCode
{
    ExitOk             = 0,
    ExitRuntimeFailure = 1,
    ExitUsage          = 2,
    ExitInvalidProject = 3,
    ExitEnvironment    = 4,
    ExitAlreadyRunning = 5,
    ExitNotRunning     = 6,
    ExitRejected       = 7,
};

enum class Command
{
    Help,
    Version,
    Run,
    Check,
    Outputs,
    Status,
    Pause,
    Resume,
    Quit,
    SetProperties,
};

struct Options {
    Command     command { Command::Help };
    QString     project;
    QStringList outputs;
    bool        allOutputs { false };
    QString     instance { QStringLiteral("default") };
    int         fps { 30 };
    QJsonObject properties;
    bool        allowRemote { false };
    bool        audio { false };
    bool        window { false };
    bool        diagnostics { false };
    bool        disableGpuRasterization { true };
    int         durationSeconds { 0 };
};

struct ParseResult {
    Options options;
    QString error;
    bool    ok() const { return error.isEmpty(); }
};

ParseResult parseCommandLine(const QStringList& arguments);
QString     usage();
bool        isControlCommand(Command command);
QString     controlCommandName(Command command);

struct OutputSelection {
    QStringList selected;
    QStringList missing;
    QString     error;
};

// available must not contain empty names (placeholder screens).
OutputSelection selectOutputs(const QStringList& available, const QStringList& requested, bool all);

} // namespace wehypr
