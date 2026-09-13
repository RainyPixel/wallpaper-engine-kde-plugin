#include "WallpaperHost.hpp"

#include "Ipc.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QQmlError>
#include <QQuickView>
#include <QScreen>
#include <QTimer>

#include <LayerShellQt/Window>

#include <algorithm>

namespace wehypr
{

namespace
{
constexpr auto k_layerNamespace = "wallpaper-engine-hyprland";

QJsonObject geometryJson(const QRect& rect) {
    return QJsonObject { { QStringLiteral("x"), rect.x() },
                         { QStringLiteral("y"), rect.y() },
                         { QStringLiteral("width"), rect.width() },
                         { QStringLiteral("height"), rect.height() } };
}
} // namespace

OutputSurface::OutputSurface(WallpaperHost* host, QScreen* screen)
    : m_host(host), m_screen(screen), m_output(screen->name()) {}

OutputSurface::~OutputSurface() {
    // The QML tree calls back into this object, so it has to go first.
    m_view.reset();
}

bool OutputSurface::create(QString* error) {
    const Options& options = m_host->options();

    m_view = std::make_unique<QQuickView>();
    m_view->setScreen(m_screen);
    m_view->setTitle(QStringLiteral("Wallpaper Engine (%1)").arg(m_output));
    m_view->setColor(Qt::black);
    m_view->setResizeMode(QQuickView::SizeRootObjectToView);

    if (options.window) {
        m_view->resize(1280, 720);
        m_view->installEventFilter(this);
    } else {
        m_view->setFlags(Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
                         Qt::WindowTransparentForInput);
        auto* layer = LayerShellQt::Window::get(m_view.get());
        layer->setScope(QString::fromLatin1(k_layerNamespace));
        layer->setLayer(LayerShellQt::Window::LayerBottom);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) |
                          LayerShellQt::Window::AnchorBottom | LayerShellQt::Window::AnchorLeft |
                          LayerShellQt::Window::AnchorRight);
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
#ifdef WEHYPR_HAVE_ACTIVATE_ON_SHOW
        layer->setActivateOnShow(false);
#endif
        m_view->resize(m_screen->size());
        connect(m_screen.data(), &QScreen::geometryChanged, this, [this] {
            if (m_view && m_screen) m_view->resize(m_screen->size());
        });
    }

    m_view->setInitialProperties(
        { { QStringLiteral("host"), QVariant::fromValue<QObject*>(m_host) },
          { QStringLiteral("surface"), QVariant::fromValue<QObject*>(this) } });
    m_view->loadFromModule("WallpaperEngineHyprland", "WallpaperSurface");
    if (m_view->status() != QQuickView::Ready) {
        QStringList messages;
        for (const QQmlError& qmlError : m_view->errors()) messages.append(qmlError.toString());
        *error = QStringLiteral("cannot load the wallpaper surface: %1")
                     .arg(messages.join(QStringLiteral("; ")));
        return false;
    }
    m_view->show();
    qInfo().noquote() << "output" << m_output << (options.window ? "window" : "layer surface")
                      << "created";
    return true;
}

bool OutputSurface::eventFilter(QObject* watched, QEvent* event) {
    // Only preview windows are filtered; closing one stops the host.
    if (watched == m_view.get() && event->type() == QEvent::Close) {
        qInfo().noquote() << "output" << m_output << "window closed, stopping";
        QCoreApplication::quit();
    }
    return QObject::eventFilter(watched, event);
}

QString OutputSurface::output() const { return m_output; }

QScreen* OutputSurface::screen() const { return m_screen; }

bool OutputSurface::loaded() const { return m_loaded; }

bool OutputSurface::frozen() const { return m_frozen; }

QJsonObject OutputSurface::status() const {
    QJsonObject result { { QStringLiteral("output"), m_output },
                         { QStringLiteral("loaded"), m_loaded },
                         { QStringLiteral("frozen"), m_frozen },
                         { QStringLiteral("recoveries"), m_recoveries },
                         { QStringLiteral("blockedNavigations"), m_blockedNavigations },
                         { QStringLiteral("lastBlockedNavigation"), m_lastBlockedNavigation },
                         { QStringLiteral("error"), m_lastError },
                         { QStringLiteral("diagnostics"), m_diagnostics } };
    if (m_screen) {
        result.insert(QStringLiteral("geometry"), geometryJson(m_screen->geometry()));
        result.insert(QStringLiteral("devicePixelRatio"), m_screen->devicePixelRatio());
    }
    return result;
}

void OutputSurface::reportLoading() {
    m_loaded      = false;
    m_diagnostics = QJsonObject();
}

void OutputSurface::reportLoaded() {
    m_loaded = true;
    m_lastError.clear();
    qInfo().noquote() << "output" << m_output << "page loaded";
}

void OutputSurface::reportLoadFailed(const QString& error) {
    m_loaded    = false;
    m_lastError = error;
    m_host->fail(QStringLiteral("output %1: cannot load %2").arg(m_output, error));
}

void OutputSurface::reportFrozen(bool frozen) {
    if (m_frozen == frozen) return;
    m_frozen = frozen;
    qInfo().noquote() << "output" << m_output << (frozen ? "page frozen" : "page active");
}

void OutputSurface::reportDiagnostics(const QString& json) {
    if (json.size() > k_maxDiagnosticsBytes) {
        m_diagnostics = { { QStringLiteral("error"), QStringLiteral("diagnostics too large") } };
        return;
    }
    QJsonParseError parseError;
    const auto      doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || ! doc.isObject()) {
        m_diagnostics = { { QStringLiteral("error"), QStringLiteral("invalid diagnostics") } };
        return;
    }
    m_diagnostics = doc.object();
}

void OutputSurface::reportConsoleMessage(const QString& message, int line, const QString& source) {
    qInfo().noquote() << "output" << m_output << "console:" << message.left(2000)
                      << QStringLiteral("(%1:%2)").arg(source, QString::number(line));
}

void OutputSurface::reportBlockedNavigation(const QUrl& url) {
    ++m_blockedNavigations;
    m_lastBlockedNavigation = url.toString().left(k_maxReportedUrl);
    qWarning().noquote() << "output" << m_output << "blocked navigation to"
                         << m_lastBlockedNavigation;
}

bool OutputSurface::recover(const QString& reason) {
    m_loaded      = false;
    m_frozen      = false;
    m_diagnostics = QJsonObject();
    m_lastError   = reason;
    ++m_recoveries;
    if (m_recoveries > k_maxRecoveries) {
        m_host->fail(QStringLiteral("output %1: %2, giving up after %3 reloads")
                         .arg(m_output, reason, QString::number(k_maxRecoveries)));
        return false;
    }
    qWarning().noquote() << "output" << m_output << reason << "- loading the project again";
    return true;
}

WallpaperHost::WallpaperHost(Options options, Project project, QObject* parent)
    : QObject(parent), m_options(std::move(options)), m_project(std::move(project)) {}

WallpaperHost::~WallpaperHost() {
    m_stopping = true;
    m_surfaces.clear();
    m_server.reset();
}

int WallpaperHost::start(const QString& socketPath, QString* error) {
    QStringList available;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (! screen->name().isEmpty()) available.append(screen->name());
    }
    const OutputSelection selection =
        selectOutputs(available, m_options.outputs, m_options.allOutputs);
    if (! selection.error.isEmpty()) {
        *error = selection.error;
        return ExitEnvironment;
    }
    if (! m_options.allOutputs)
        m_wantedOutputs = m_options.outputs.isEmpty() ? selection.selected : m_options.outputs;
    for (const QString& name : selection.missing)
        qWarning().noquote() << "output" << name << "is not connected, waiting for it";

    m_server = std::make_unique<IpcServer>([this](const QJsonObject& request) {
        return handleRequest(request);
    });
    if (! m_server->listen(socketPath, error)) return ExitRuntimeFailure;

    connect(qApp, &QGuiApplication::screenAdded, this, &WallpaperHost::addScreen);
    connect(qApp, &QGuiApplication::screenRemoved, this, &WallpaperHost::removeScreen);

    for (QScreen* screen : QGuiApplication::screens()) {
        if (! wantsOutput(screen->name())) continue;
        if (m_options.window && ! m_surfaces.empty()) break;
        auto surface = std::make_unique<OutputSurface>(this, screen);
        if (! surface->create(error)) return ExitRuntimeFailure;
        m_surfaces.push_back(std::move(surface));
    }

    if (m_options.durationSeconds > 0) {
        QTimer::singleShot(m_options.durationSeconds * 1000, this, [] {
            qInfo("duration elapsed, stopping");
            QCoreApplication::quit();
        });
    }
    return ExitOk;
}

const Options& WallpaperHost::options() const { return m_options; }

QUrl WallpaperHost::url() const { return m_project.entryUrl; }

int WallpaperHost::fps() const { return m_options.fps; }

bool WallpaperHost::audio() const { return m_options.audio; }

bool WallpaperHost::allowRemote() const { return m_options.allowRemote; }

bool WallpaperHost::diagnostics() const { return m_options.diagnostics; }

bool WallpaperHost::paused() const { return m_paused; }

QVariantMap WallpaperHost::currentProperties() const { return m_project.properties.toVariantMap(); }

bool WallpaperHost::navigationAllowed(const QUrl& url) const {
    return isInsideProject(m_project.rootDir, url);
}

void WallpaperHost::fail(const QString& message) {
    if (m_stopping) return;
    m_stopping = true;
    qCritical().noquote() << message;
    QCoreApplication::exit(ExitRuntimeFailure);
}

QJsonObject WallpaperHost::handleRequest(const QJsonObject& request) {
    const QJsonValue protocol = request.value(QLatin1String("protocol"));
    if (! protocol.isUndefined() && protocol.toInt(-1) != k_protocolVersion)
        return errorResponse(QStringLiteral("unsupported protocol version"));

    const QString command = request.value(QLatin1String("command")).toString();
    if (command == QLatin1String("status")) return status();
    if (command == QLatin1String("pause")) {
        setPaused(true);
        return status();
    }
    if (command == QLatin1String("resume")) {
        setPaused(false);
        return status();
    }
    if (command == QLatin1String("set-properties")) {
        const QJsonValue properties = request.value(QLatin1String("properties"));
        if (! properties.isObject())
            return errorResponse(QStringLiteral("set-properties needs a properties object"));
        QString    error;
        const auto values =
            validatePropertyValues(m_project.properties, properties.toObject(), &error);
        if (! values) return errorResponse(error);
        const QJsonObject changed = applyPropertyValues(&m_project.properties, *values);
        if (! changed.isEmpty()) emit propertyValuesChanged(changed.toVariantMap());
        return QJsonObject { { QStringLiteral("ok"), true },
                             { QStringLiteral("changed"),
                               QJsonArray::fromStringList(changed.keys()) } };
    }
    if (command == QLatin1String("quit")) {
        qInfo("quit requested");
        QTimer::singleShot(0, qApp, [] {
            QCoreApplication::quit();
        });
        return QJsonObject { { QStringLiteral("ok"), true } };
    }
    return errorResponse(QStringLiteral("unknown command '%1'").arg(command));
}

QJsonObject WallpaperHost::status() const {
    QJsonArray  surfaces;
    QStringList active;
    bool        allLoaded = ! m_surfaces.empty();
    bool        allFrozen = ! m_surfaces.empty();
    for (const auto& surface : m_surfaces) {
        surfaces.append(surface->status());
        active.append(surface->output());
        allLoaded = allLoaded && surface->loaded();
        allFrozen = allFrozen && surface->frozen();
    }
    QStringList missing;
    for (const QString& name : m_wantedOutputs) {
        if (! active.contains(name)) missing.append(name);
    }

    return QJsonObject {
        { QStringLiteral("ok"), true },
        { QStringLiteral("protocol"), k_protocolVersion },
        { QStringLiteral("pid"), QCoreApplication::applicationPid() },
        { QStringLiteral("instance"), m_options.instance },
        { QStringLiteral("project"), m_project.projectFile },
        { QStringLiteral("title"), m_project.title },
        { QStringLiteral("type"), m_project.type },
        { QStringLiteral("url"), m_project.entryUrl.toString() },
        { QStringLiteral("mode"),
          m_options.window ? QStringLiteral("window") : QStringLiteral("layer") },
        { QStringLiteral("fps"), m_options.fps },
        { QStringLiteral("audio"), m_options.audio },
        { QStringLiteral("allowRemote"), m_options.allowRemote },
        { QStringLiteral("gpuRasterization"),
          m_options.disableGpuRasterization ? QStringLiteral("off") : QStringLiteral("auto") },
        { QStringLiteral("allOutputs"), m_options.allOutputs },
        { QStringLiteral("paused"), m_paused },
        { QStringLiteral("allLoaded"), allLoaded },
        { QStringLiteral("allFrozen"), allFrozen },
        { QStringLiteral("surfaces"), surfaces },
        { QStringLiteral("missingOutputs"), QJsonArray::fromStringList(missing) },
    };
}

void WallpaperHost::setPaused(bool paused) {
    if (m_paused == paused) return;
    m_paused = paused;
    qInfo(paused ? "paused" : "resumed");
    emit pausedChanged();
}

bool WallpaperHost::wantsOutput(const QString& name) const {
    if (name.isEmpty()) return false;
    return m_options.allOutputs || m_wantedOutputs.contains(name);
}

void WallpaperHost::addScreen(QScreen* screen) {
    if (m_stopping || ! wantsOutput(screen->name())) return;
    if (m_options.window && ! m_surfaces.empty()) return;
    const bool exists = std::any_of(m_surfaces.begin(), m_surfaces.end(), [screen](const auto& s) {
        return s->output() == screen->name();
    });
    if (exists) return;

    qInfo().noquote() << "output" << screen->name() << "connected";
    auto    surface = std::make_unique<OutputSurface>(this, screen);
    QString error;
    if (! surface->create(&error)) {
        fail(error);
        return;
    }
    m_surfaces.push_back(std::move(surface));
}

void WallpaperHost::removeScreen(QScreen* screen) {
    // Qt moves a preview window to another screen by itself.
    if (m_options.window) return;
    // Layer surfaces are bound to their output. The QScreen is still alive here; the view
    // must be gone before it is destroyed.
    for (auto it = m_surfaces.begin(); it != m_surfaces.end();) {
        if ((*it)->screen() != screen) {
            ++it;
            continue;
        }
        qInfo().noquote() << "output" << (*it)->output() << "removed, surface closed";
        it = m_surfaces.erase(it);
    }
}

} // namespace wehypr
