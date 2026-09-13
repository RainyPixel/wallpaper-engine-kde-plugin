#include "Ipc.hpp"
#include "Options.hpp"
#include "ProjectConfig.hpp"
#include "WallpaperHost.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickWindow>
#include <QScreen>
#include <QSocketNotifier>
#include <QTextStream>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

using namespace wehypr;

namespace
{
constexpr auto k_appName = "wallpaper-engine-hyprland";
constexpr int  k_requestTimeoutMs = 5000;

int g_signalFds[2] = { -1, -1 };

void printOut(const QString& text) { QTextStream(stdout) << text; }

void printError(const QString& text) {
    QTextStream(stderr) << k_appName << ": " << text << '\n';
}

QString indented(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

void handleSignal(int) {
    const char byte = 1;
    if (::write(g_signalFds[1], &byte, 1) < 0) return;
}

void installSignalHandlers(QCoreApplication* app) {
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, g_signalFds) != 0) {
        qWarning("cannot install signal handlers");
        return;
    }
    auto* notifier = new QSocketNotifier(g_signalFds[0], QSocketNotifier::Read, app);
    QObject::connect(notifier, &QSocketNotifier::activated, app, [notifier] {
        char byte = 0;
        if (::read(g_signalFds[0], &byte, 1) < 0) return;
        notifier->setEnabled(false);
        qInfo("termination signal received, stopping");
        QCoreApplication::quit();
    });

    struct sigaction action {};
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (int sig : { SIGINT, SIGTERM, SIGHUP }) ::sigaction(sig, &action, nullptr);
}

void appendChromiumFlag(const QByteArray& flag) {
    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (flags.split(' ').contains(flag)) return;
    if (! flags.isEmpty()) flags += ' ';
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags + flag);
}

std::optional<Project> loadWithProperties(const Options& options, QString* error) {
    auto project = loadProject(options.project, error);
    if (! project || options.properties.isEmpty()) return project;
    const auto values = validatePropertyValues(project->properties, options.properties, error);
    if (! values) return std::nullopt;
    applyPropertyValues(&project->properties, *values);
    return project;
}

int runCheck(const Options& options) {
    QString    error;
    const auto project = loadWithProperties(options, &error);
    if (! project) {
        printError(error);
        return ExitInvalidProject;
    }
    printOut(indented({ { QStringLiteral("ok"), true },
                        { QStringLiteral("type"), project->type },
                        { QStringLiteral("title"), project->title },
                        { QStringLiteral("project"), project->projectFile },
                        { QStringLiteral("entry"), project->entryFile },
                        { QStringLiteral("url"), project->entryUrl.toString() },
                        { QStringLiteral("properties"), project->properties } }));
    return ExitOk;
}

int runControl(const Options& options) {
    QString       error;
    const QString dir = runtimeDirectory(&error);
    if (dir.isEmpty()) {
        printError(error);
        return ExitEnvironment;
    }

    QJsonObject request { { QStringLiteral("protocol"), k_protocolVersion },
                          { QStringLiteral("command"), controlCommandName(options.command) } };
    if (options.command == Command::SetProperties)
        request.insert(QStringLiteral("properties"), options.properties);

    const QString path      = socketPath(dir, options.instance);
    bool          connected = false;
    const auto response = sendRequest(path, request, k_requestTimeoutMs, &error, &connected);
    if (! response) {
        if (! connected) {
            printError(QStringLiteral("no running host for instance '%1' (%2)")
                           .arg(options.instance, path));
            return ExitNotRunning;
        }
        printError(error);
        return ExitRuntimeFailure;
    }
    if (! response->value(QLatin1String("ok")).toBool()) {
        printError(response->value(QLatin1String("error")).toString());
        return ExitRejected;
    }
    printOut(indented(*response));
    return ExitOk;
}

int runOutputs(int& argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        printError(QStringLiteral("WAYLAND_DISPLAY is not set, start from a Wayland session"));
        return ExitEnvironment;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "wayland");
    QGuiApplication app(argc, argv);

    QJsonArray outputs;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen->name().isEmpty()) continue;
        const QRect geometry = screen->geometry();
        outputs.append(QJsonObject {
            { QStringLiteral("name"), screen->name() },
            { QStringLiteral("manufacturer"), screen->manufacturer() },
            { QStringLiteral("model"), screen->model() },
            { QStringLiteral("x"), geometry.x() },
            { QStringLiteral("y"), geometry.y() },
            { QStringLiteral("width"), geometry.width() },
            { QStringLiteral("height"), geometry.height() },
            { QStringLiteral("devicePixelRatio"), screen->devicePixelRatio() },
        });
    }
    printOut(QString::fromUtf8(QJsonDocument(outputs).toJson(QJsonDocument::Indented)));
    return ExitOk;
}

int runHost(const Options& options, int& argc, char** argv) {
    QString    error;
    const auto project = loadWithProperties(options, &error);
    if (! project) {
        printError(error);
        return ExitInvalidProject;
    }
    if (! options.window && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        printError(QStringLiteral("layer surfaces need a Wayland session, WAYLAND_DISPLAY is not set"));
        return ExitEnvironment;
    }

    const QString dir = runtimeDirectory(&error);
    if (dir.isEmpty()) {
        printError(error);
        return ExitEnvironment;
    }
    InstanceLock lock(lockPath(dir, options.instance));
    if (! lock.tryLock(&error)) {
        printError(QStringLiteral("instance '%1': %2").arg(options.instance, error));
        return lock.heldByOtherProcess() ? ExitAlreadyRunning : ExitEnvironment;
    }

    if (! options.window && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "wayland");
    // Chromium GPU rasterization lost its Skia context on tested Mesa laptops; compositing
    // stays on the GPU either way.
    if (options.disableGpuRasterization) appendChromiumFlag("--disable-gpu-rasterization");

    QCoreApplication::setApplicationName(QString::fromLatin1(k_appName));
    QCoreApplication::setApplicationVersion(QStringLiteral(WEHYPR_VERSION));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    if (! options.window && ! QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        printError(QStringLiteral("layer surfaces need the Qt Wayland platform, got '%1'")
                       .arg(QGuiApplication::platformName()));
        return ExitEnvironment;
    }
    installSignalHandlers(&app);

    WallpaperHost host(options, *project);
    const int     started = host.start(socketPath(dir, options.instance), &error);
    if (started != ExitOk) {
        printError(error);
        return started;
    }
    return app.exec();
}
} // namespace

int main(int argc, char** argv) {
    qSetMessagePattern(QStringLiteral("%{if-warning}warning: %{endif}%{if-critical}error: %{endif}"
                                      "%{if-fatal}fatal: %{endif}%{message}"));

    QStringList arguments;
    for (int i = 0; i < argc; ++i) arguments.append(QString::fromLocal8Bit(argv[i]));

    const ParseResult parsed = parseCommandLine(arguments);
    if (! parsed.ok()) {
        printError(parsed.error);
        printError(QStringLiteral("run '%1 help' for usage").arg(QString::fromLatin1(k_appName)));
        return ExitUsage;
    }

    const Options& options = parsed.options;
    switch (options.command) {
    case Command::Help: printOut(usage()); return ExitOk;
    case Command::Version:
        printOut(QStringLiteral("%1 %2\n")
                     .arg(QString::fromLatin1(k_appName), QLatin1String(WEHYPR_VERSION)));
        return ExitOk;
    case Command::Check: return runCheck(options);
    case Command::Outputs: return runOutputs(argc, argv);
    case Command::Run: return runHost(options, argc, argv);
    default: {
        QCoreApplication app(argc, argv);
        return runControl(options);
    }
    }
}
