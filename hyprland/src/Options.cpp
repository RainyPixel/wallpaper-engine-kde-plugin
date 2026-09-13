#include "Options.hpp"

#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>

namespace wehypr
{

namespace
{
struct CommandName {
    const char* name;
    Command     command;
};

constexpr CommandName k_commands[] = {
    { "run", Command::Run },
    { "check", Command::Check },
    { "outputs", Command::Outputs },
    { "status", Command::Status },
    { "pause", Command::Pause },
    { "resume", Command::Resume },
    { "quit", Command::Quit },
    { "set-properties", Command::SetProperties },
    { "help", Command::Help },
    { "--help", Command::Help },
    { "-h", Command::Help },
    { "version", Command::Version },
    { "--version", Command::Version },
};

bool parseInt(const QString& text, int min, int max, int* out) {
    bool      ok    = false;
    const int value = text.toInt(&ok);
    if (! ok || value < min || value > max) return false;
    *out = value;
    return true;
}

bool parseJsonObject(const QString& text, QJsonObject* out, QString* error) {
    QJsonParseError parseError;
    const auto      doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = QStringLiteral("properties are not valid JSON: %1").arg(parseError.errorString());
        return false;
    }
    if (! doc.isObject()) {
        *error = QStringLiteral("properties must be a JSON object");
        return false;
    }
    *out = doc.object();
    return true;
}

bool validInstance(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,32}$"));
    return pattern.match(name).hasMatch();
}
} // namespace

ParseResult parseCommandLine(const QStringList& arguments) {
    ParseResult result;
    if (arguments.size() < 2) {
        result.error = QStringLiteral("missing command");
        return result;
    }

    const QString name  = arguments.at(1);
    bool          known = false;
    for (const auto& entry : k_commands) {
        if (name == QLatin1String(entry.name)) {
            result.options.command = entry.command;
            known                  = true;
            break;
        }
    }
    if (! known) {
        result.error = QStringLiteral("unknown command '%1'").arg(name);
        return result;
    }

    Options&      options = result.options;
    const Command command = options.command;
    if (command == Command::Help || command == Command::Version) return result;

    QStringList rest = arguments.mid(2);
    if (rest.contains(QStringLiteral("--help")) || rest.contains(QStringLiteral("-h"))) {
        options.command = Command::Help;
        return result;
    }
    rest.prepend(arguments.at(0));

    QCommandLineParser parser;
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);

    const QCommandLineOption instanceOpt(QStringLiteral("instance"), {}, QStringLiteral("name"));
    const QCommandLineOption outputOpt(QStringLiteral("output"), {}, QStringLiteral("name"));
    const QCommandLineOption allOutputsOpt(QStringLiteral("all-outputs"));
    const QCommandLineOption fpsOpt(QStringLiteral("fps"), {}, QStringLiteral("fps"));
    const QCommandLineOption propertiesOpt(QStringLiteral("properties"), {}, QStringLiteral("json"));
    const QCommandLineOption allowRemoteOpt(QStringLiteral("allow-remote"));
    const QCommandLineOption audioOpt(QStringLiteral("audio"));
    const QCommandLineOption windowOpt(QStringLiteral("window"));
    const QCommandLineOption diagnosticsOpt(QStringLiteral("diagnostics"));
    const QCommandLineOption gpuRasterOpt(QStringLiteral("gpu-rasterization"),
                                          {},
                                          QStringLiteral("mode"));
    const QCommandLineOption durationOpt(QStringLiteral("duration"), {}, QStringLiteral("seconds"));

    switch (command) {
    case Command::Run:
        parser.addOptions({ instanceOpt,
                            outputOpt,
                            allOutputsOpt,
                            fpsOpt,
                            propertiesOpt,
                            allowRemoteOpt,
                            audioOpt,
                            windowOpt,
                            diagnosticsOpt,
                            gpuRasterOpt,
                            durationOpt });
        break;
    case Command::Check: parser.addOption(propertiesOpt); break;
    case Command::Outputs: break;
    default: parser.addOption(instanceOpt); break;
    }

    if (! parser.parse(rest)) {
        result.error = parser.errorText();
        return result;
    }
    const QStringList positional = parser.positionalArguments();
    const QStringList found      = parser.optionNames();

    if (found.contains(instanceOpt.names().first())) {
        options.instance = parser.value(instanceOpt);
        if (! validInstance(options.instance)) {
            result.error = QStringLiteral("instance name must match [A-Za-z0-9_-]{1,32}");
            return result;
        }
    }
    if (found.contains(propertiesOpt.names().first())) {
        if (! parseJsonObject(parser.value(propertiesOpt), &options.properties, &result.error))
            return result;
    }

    switch (command) {
    case Command::Run:
    case Command::Check:
        if (positional.size() != 1 || positional.first().isEmpty()) {
            result.error = QStringLiteral("expected exactly one project path");
            return result;
        }
        options.project = positional.first();
        break;
    case Command::SetProperties:
        if (positional.size() != 1) {
            result.error = QStringLiteral("expected exactly one JSON object with property values");
            return result;
        }
        if (! parseJsonObject(positional.first(), &options.properties, &result.error))
            return result;
        break;
    default:
        if (! positional.isEmpty()) {
            result.error = QStringLiteral("unexpected argument '%1'").arg(positional.first());
            return result;
        }
        break;
    }

    if (command != Command::Run) return result;

    for (const QString& output : parser.values(outputOpt)) {
        if (output.isEmpty()) {
            result.error = QStringLiteral("output name must not be empty");
            return result;
        }
        if (! options.outputs.contains(output)) options.outputs.append(output);
    }
    options.allOutputs = parser.isSet(allOutputsOpt);
    if (options.allOutputs && ! options.outputs.isEmpty()) {
        result.error = QStringLiteral("--output and --all-outputs cannot be combined");
        return result;
    }
    if (parser.isSet(fpsOpt) && ! parseInt(parser.value(fpsOpt), 1, 240, &options.fps)) {
        result.error = QStringLiteral("--fps must be an integer between 1 and 240");
        return result;
    }
    if (parser.isSet(durationOpt) &&
        ! parseInt(parser.value(durationOpt), 0, 86400, &options.durationSeconds)) {
        result.error = QStringLiteral("--duration must be an integer between 0 and 86400");
        return result;
    }
    if (parser.isSet(gpuRasterOpt)) {
        const QString mode = parser.value(gpuRasterOpt);
        if (mode == QLatin1String("off"))
            options.disableGpuRasterization = true;
        else if (mode == QLatin1String("auto"))
            options.disableGpuRasterization = false;
        else {
            result.error = QStringLiteral("--gpu-rasterization must be 'off' or 'auto'");
            return result;
        }
    }
    options.allowRemote = parser.isSet(allowRemoteOpt);
    options.audio       = parser.isSet(audioOpt);
    options.window      = parser.isSet(windowOpt);
    options.diagnostics = parser.isSet(diagnosticsOpt);
    return result;
}

QString usage() {
    return QStringLiteral(
        R"(Usage: wallpaper-engine-hyprland <command> [options]

Commands:
  run <project>              Show a web wallpaper on layer-shell surfaces
  check <project>            Validate a project and print a JSON summary
  outputs                    List connected outputs as JSON
  status                     Print the state of a running host as JSON
  pause | resume | quit      Control a running host
  set-properties <json>      Change user properties of the running wallpaper
  help | version

<project> is a Wallpaper Engine project directory or its project.json.

Options for run:
  --output <name>            Output to cover; repeat for several outputs
  --all-outputs              Cover every output, including outputs added later
  --fps <1-240>              Frame rate passed to the wallpaper (default 30)
  --properties <json>        Initial property values, e.g. '{"speed":0.5}'
  --audio                    Unmute page audio (muted by default)
  --allow-remote             Allow the local page to load remote URLs
  --gpu-rasterization <off|auto>
                             Chromium GPU rasterization (default off)
  --window                   Open a normal window instead of a layer surface
  --diagnostics              Poll window.wallpaperDiagnostics() and log console output
  --duration <seconds>       Quit after the given time (default 0, run until stopped)

Without --output or --all-outputs, run uses the only connected output and fails
when several outputs are connected.

Options for run and the control commands:
  --instance <name>          Instance name (default "default")

check also accepts --properties.

Exit codes: 0 ok, 1 runtime failure, 2 usage error, 3 invalid project or
properties, 4 environment or output problem, 5 instance already running,
6 no running host, 7 request rejected by the host.
)");
}

bool isControlCommand(Command command) {
    switch (command) {
    case Command::Status:
    case Command::Pause:
    case Command::Resume:
    case Command::Quit:
    case Command::SetProperties: return true;
    default: return false;
    }
}

QString controlCommandName(Command command) {
    switch (command) {
    case Command::Status: return QStringLiteral("status");
    case Command::Pause: return QStringLiteral("pause");
    case Command::Resume: return QStringLiteral("resume");
    case Command::Quit: return QStringLiteral("quit");
    case Command::SetProperties: return QStringLiteral("set-properties");
    default: return {};
    }
}

OutputSelection selectOutputs(const QStringList& available, const QStringList& requested,
                              bool all) {
    OutputSelection selection;
    const QString   list =
        available.isEmpty() ? QStringLiteral("none") : available.join(QStringLiteral(", "));

    if (all) {
        selection.selected = available;
        if (available.isEmpty()) selection.error = QStringLiteral("no outputs are connected");
        return selection;
    }

    if (requested.isEmpty()) {
        if (available.size() == 1)
            selection.selected = available;
        else if (available.isEmpty())
            selection.error = QStringLiteral("no outputs are connected");
        else
            selection.error = QStringLiteral("several outputs are connected (%1); "
                                             "pass --output <name> or --all-outputs")
                                  .arg(list);
        return selection;
    }

    for (const QString& name : requested) {
        if (selection.selected.contains(name) || selection.missing.contains(name)) continue;
        if (available.contains(name))
            selection.selected.append(name);
        else
            selection.missing.append(name);
    }
    if (selection.selected.isEmpty())
        selection.error =
            QStringLiteral("none of the requested outputs is connected (available: %1)").arg(list);
    return selection;
}

} // namespace wehypr
