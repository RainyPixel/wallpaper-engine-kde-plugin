#include "Ipc.hpp"
#include "Library.hpp"
#include "Options.hpp"
#include "ProjectConfig.hpp"
#include "State.hpp"
#include "WallpaperHost.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QQmlError>
#include <QQuickView>
#include <QQuickWindow>
#include <QScreen>
#include <QSize>
#include <QSocketNotifier>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include <functional>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

using namespace wehypr;

namespace
{
constexpr auto k_appName          = "wallpaper-engine-hyprland";
constexpr int  k_requestTimeoutMs = 5000;
// A host answers quit before it leaves its event loop, so browse waits for the
// socket and the instance lock to go away before it starts the next one.
constexpr int k_quitTimeoutMs  = 10000;
constexpr int k_startTimeoutMs = 10000;

int g_signalFds[2] = { -1, -1 };

void printOut(const QString& text) { QTextStream(stdout) << text; }

void printError(const QString& text) { QTextStream(stderr) << k_appName << ": " << text << '\n'; }

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
    auto project = loadProject(resolveProject(options.project), error);
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

QJsonArray wallpapersJson() {
    QJsonArray items;
    for (const Wallpaper& wallpaper : scanWallpapers()) {
        // QML needs a URL; a path with a space or a '#' does not survive concatenation.
        // A preview named in project.json is not always on disk; half-downloaded
        // items have the name and not the file.
        const QString previewFile = wallpaper.preview.isEmpty()
                                        ? QString()
                                        : wallpaper.path + QLatin1Char('/') + wallpaper.preview;
        const QString preview     = (previewFile.isEmpty() || ! QFileInfo::exists(previewFile))
                                        ? QString()
                                        : QUrl::fromLocalFile(previewFile).toString();
        items.append(QJsonObject { { QStringLiteral("id"), wallpaper.id },
                                   { QStringLiteral("title"), wallpaper.title },
                                   { QStringLiteral("type"), wallpaper.type },
                                   { QStringLiteral("path"), wallpaper.path },
                                   { QStringLiteral("preview"), wallpaper.preview },
                                   { QStringLiteral("previewUrl"), preview },
                                   { QStringLiteral("file"), wallpaper.file },
                                   { QStringLiteral("mtime"), wallpaper.mtime },
                                   { QStringLiteral("supported"), wallpaper.supported } });
    }
    return items;
}

int runList() {
    printOut(QString::fromUtf8(QJsonDocument(wallpapersJson()).toJson(QJsonDocument::Indented)));
    return ExitOk;
}

QStringList runArguments(const Options& options, const QString& project) {
    QStringList arguments { QStringLiteral("run"),        project,
                            QStringLiteral("--instance"), options.instance,
                            QStringLiteral("--fps"),      QString::number(options.fps) };
    for (const QString& output : options.outputs) arguments << QStringLiteral("--output") << output;
    // run refuses to guess when several outputs are connected, so a browse
    // started without an output selection covers all of them.
    if (options.allOutputs || options.outputs.isEmpty())
        arguments << QStringLiteral("--all-outputs");
    if (options.audio) arguments << QStringLiteral("--audio");
    if (options.allowRemote) arguments << QStringLiteral("--allow-remote");
    if (options.diagnostics) arguments << QStringLiteral("--diagnostics");
    if (! options.disableGpuRasterization)
        arguments << QStringLiteral("--gpu-rasterization") << QStringLiteral("auto");
    return arguments;
}

// Whether no host holds the instance. QLockFile is used rather than a plain
// existence check so a lock left behind by a killed host does not count.
bool instanceIsFree(const QString& path) {
    InstanceLock probe(path);
    QString      ignored;
    return probe.tryLock(&ignored);
}

// Keeps the window repainting while waiting. User input is excluded so a second
// Apply cannot re-enter this.
bool waitFor(const std::function<bool()>& ready, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (! ready()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 25);
        QThread::msleep(10);
    }
    return true;
}

// What the browser calls when the user applies a wallpaper.
class BrowserBridge : public QObject {
    Q_OBJECT

public:
    explicit BrowserBridge(const Options& options): m_options(options) {}

    // Returns an empty string on success, the message to show otherwise.
    Q_INVOKABLE QString apply(const QString& project) {
        if (project.isEmpty()) return QStringLiteral("no wallpaper selected");
        if (! saveState(m_options.instance, State { project }))
            return QStringLiteral("cannot save the choice for instance '%1'")
                .arg(m_options.instance);

        QString       error;
        const QString dir = runtimeDirectory(&error);
        if (dir.isEmpty()) return error;
        const QString socket = socketPath(dir, m_options.instance);
        const QString lock   = lockPath(dir, m_options.instance);

        bool connected = false;
        sendRequest(socket,
                    QJsonObject { { QStringLiteral("protocol"), k_protocolVersion },
                                  { QStringLiteral("command"), QStringLiteral("quit") } },
                    k_requestTimeoutMs,
                    &error,
                    &connected);

        // Waiting only after a delivered quit is not enough: runHost takes the
        // lock well before it starts listening, so an apply landing in that
        // window finds no socket, skips the wait and loses to the lock.
        if (! waitFor(
                [&lock] {
                    return instanceIsFree(lock);
                },
                k_quitTimeoutMs))
            return connected ? QStringLiteral("the host of instance '%1' did not stop")
                                   .arg(m_options.instance)
                             : QStringLiteral("a host for instance '%1' is still starting")
                                   .arg(m_options.instance);

        if (! QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                      runArguments(m_options, project)))
            return QStringLiteral("cannot start the wallpaper host");

        // startDetached only reports that the fork worked. The host can still
        // exit straight away, and browse usually has no terminal to show why.
        if (! waitFor(
                [&socket] {
                    return QFile::exists(socket);
                },
                k_startTimeoutMs))
            return QStringLiteral("the wallpaper host did not start, run '%1 run %2' to see why")
                .arg(QCoreApplication::applicationFilePath(), project);
        return {};
    }

private:
    Options m_options;
};

int runBrowse(const Options& options, int& argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
            printError(QStringLiteral("WAYLAND_DISPLAY is not set, start from a Wayland session"));
            return ExitEnvironment;
        }
        qputenv("QT_QPA_PLATFORM", "wayland");
    }
    QCoreApplication::setApplicationName(QString::fromLatin1(k_appName));
    QCoreApplication::setApplicationVersion(QStringLiteral(WEHYPR_VERSION));
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(true);

    BrowserBridge bridge(options);
    QQuickView    view;
    view.setTitle(QStringLiteral("Wallpaper Engine"));
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    // The detail pane has a fixed width, so without a floor the grid can be
    // squeezed until the cards are cut off.
    view.setMinimumSize(QSize(760, 480));
    view.setInitialProperties(
        { { QStringLiteral("bridge"), QVariant::fromValue<QObject*>(&bridge) },
          { QStringLiteral("wallpapers"), wallpapersJson().toVariantList() } });
    view.loadFromModule("WallpaperEngineHyprland", "Browser");
    if (view.status() != QQuickView::Ready) {
        QStringList messages;
        for (const QQmlError& qmlError : view.errors()) messages.append(qmlError.toString());
        printError(
            QStringLiteral("cannot load the browser: %1").arg(messages.join(QStringLiteral("; "))));
        return ExitRuntimeFailure;
    }
    view.resize(1100, 700);
    view.show();
    return app.exec();
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
    const auto    response  = sendRequest(path, request, k_requestTimeoutMs, &error, &connected);
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

int runHost(Options options, int& argc, char** argv) {
    if (options.project.isEmpty()) {
        options.project = loadState(options.instance).project;
        if (options.project.isEmpty()) {
            printError(QStringLiteral("no project given and instance '%1' has no saved choice, "
                                      "pass a project or pick one with 'browse'")
                           .arg(options.instance));
            return ExitUsage;
        }
    }

    QString    error;
    const auto project = loadWithProperties(options, &error);
    if (! project) {
        printError(error);
        return ExitInvalidProject;
    }
    if (! options.window && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        printError(
            QStringLiteral("layer surfaces need a Wayland session, WAYLAND_DISPLAY is not set"));
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

    if (! options.window &&
        ! QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
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
        printOut(QStringLiteral("%1 %2\n").arg(QString::fromLatin1(k_appName),
                                               QLatin1String(WEHYPR_VERSION)));
        return ExitOk;
    case Command::Check: return runCheck(options);
    case Command::List: return runList();
    case Command::Browse: return runBrowse(options, argc, argv);
    case Command::Outputs: return runOutputs(argc, argv);
    case Command::Run: return runHost(options, argc, argv);
    default: {
        QCoreApplication app(argc, argv);
        return runControl(options);
    }
    }
}

#include "main.moc"
