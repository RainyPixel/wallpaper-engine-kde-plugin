#include "SceneBackend.hpp"

#include <QtGlobal>
#include <QtCore/QObject>
#include <QtCore/QDir>
#include <QtCore/QThread>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <QtGui/QGuiApplication>
#include <QtGui/QOpenGLContext>
#include <QtQuick/QQuickWindow>

#include <QtGui/QOffscreenSurface>
#include <QtQuick/QSGSimpleTextureNode>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#    include <QSGTexture>
#endif

#include <clocale>
#include <cstring>
#include <map>
#include <atomic>
#include <array>
#include <functional>

#include "glExtra.hpp"
#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"
#include "Type.hpp"
#include "Utils/Platform.hpp"
#include <cstdio>
#include <qobjectdefs.h>
#include <unistd.h>

using namespace scenebackend;

Q_LOGGING_CATEGORY(wekdeScene, "wekde.scene")

#define _Q_INFO(fmt, ...) qCInfo(wekdeScene, fmt, __VA_ARGS__)

namespace
{
void* get_proc_address(const char* name) {
    QOpenGLContext* glctx = QOpenGLContext::currentContext();
    if (! glctx) return nullptr;

    return reinterpret_cast<void*>(glctx->getProcAddress(QByteArray(name)));
}

QSGTexture* createTextureFromGl(uint32_t handle, QSize size, QQuickWindow* window) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    // Frames carry alpha: opaque scenes write 1.0, a video-underlay scene is
    // transparent where the mpv video beneath should show through.
    return QNativeInterface::QSGOpenGLTexture::fromNative(
        handle, window, size, QQuickWindow::TextureHasAlphaChannel);
#elif (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    return window->createTextureFromNativeObject(
        QQuickWindow::NativeObjectTexture, &handle, 0, size);
#else
    return window->createTextureFromId(handle, size);
#endif
}

wallpaper::FillMode ToWPFillMode(int fillMode) {
    switch ((SceneObject::FillMode)fillMode) {
    case SceneObject::FillMode::STRETCH: return wallpaper::FillMode::STRETCH;
    case SceneObject::FillMode::ASPECTFIT: return wallpaper::FillMode::ASPECTFIT;
    case SceneObject::FillMode::ASPECTCROP:
    default: return wallpaper::FillMode::ASPECTCROP;
    }
}

} // namespace

using sp_scene_t = std::shared_ptr<wallpaper::SceneWallpaper>;

namespace scenebackend
{

class TextureNode : public QObject, public QSGSimpleTextureNode {
    Q_OBJECT
public:
    typedef std::function<QSGTexture*(QQuickWindow*)> EatFrameOp;
    TextureNode(QQuickWindow* window, sp_scene_t scene, bool valid, bool share_gpu,
                bool mirror_scene, EatFrameOp eatFrameOp)
        : m_texture(nullptr),
          m_scene(scene),
          m_enable_valid(valid),
          m_share_gpu(share_gpu),
          m_mirror_scene(mirror_scene),
          m_eatFrameOp(eatFrameOp),
          m_window(window),
          m_first_frame(false) {
        // texture node must have a texture, so use the default 0 texture.
        m_texture      = createTextureFromGl(0, QSize(64, 64), window);
        m_init_texture = m_texture;
        setTexture(m_texture);
        setFiltering(QSGTexture::Linear);
        setOwnsTexture(false);
    }

    ~TextureNode() override {
        // At plasmashell exit the SceneObject is leaked (its destructor never runs),
        // so the render thread would keep running DRAW against torn-down GL/Vulkan
        // state. The texture node IS destroyed on teardown, so stop and join the
        // render loop here before this node and its resources go away.
        m_scene->stopRender();
        for (auto& item : texs_map) {
            auto& exh = item.second;
            // close(exh.fd);
            m_glex.deleteTexture(exh.gltex);
            delete exh.qsg;
        }
        delete m_init_texture;
        emit nodeDestroyed();
        _Q_INFO("Destroy texnode", "");
    }

    // only at qt render thread
    bool initGl() { return m_glex.init(get_proc_address); }

    // after gl, can run at any thread
    void initVulkan(uint16_t w, uint16_t h) {
        wallpaper::RenderInitInfo info;
        info.enable_valid_layer = m_enable_valid;
        info.offscreen          = true;
        info.share_gpu          = m_share_gpu;
        info.mirror_scene       = m_mirror_scene;
        info.offscreen_tiling   = m_glex.tiling();
        info.uuid               = m_glex.uuid();
        info.width              = w;
        info.height             = h;
        info.redraw_callback    = [this]() {
            Q_EMIT this->redraw();
        };

        auto cb = std::make_shared<wallpaper::FirstFrameCallback>([this]() {
            m_first_frame = true;
            Q_EMIT this->redraw();
        });
        m_scene->setPropertyObject(wallpaper::PROPERTY_FIRST_FRAME_CALLBACK, cb);
        // this send to looper, not in this thread
        m_scene->initVulkan(info);
    }

    void emitSceneFirstFrame() { Q_EMIT sceneFirstFrame(); }
signals:
    void textureInUse();
    void nodeDestroyed();
    void redraw();
    void sceneFirstFrame();

public slots:
    void newTexture() {
        if (! m_scene->inited()) return;
        // Hold a shared_ptr to the frame source for the whole eat: the render thread
        // may re-elect and swap the mirror source while we read it.
        auto swapchain = m_scene->currentSwapchain();
        if (! swapchain) return;

        wallpaper::ExHandle* exh = swapchain->eatFrame(m_last_frame_id);
        if (exh != nullptr) {
            int id = exh->id();
            if (texs_map.count(id) == 0) {
                _Q_INFO("receive external texture(%dx%d) from fd: %d",
                        exh->width,
                        exh->height,
                        exh->fd);
                ExTex ex_tex;
                uint  gltex = m_glex.genExTexture(*exh);

                ex_tex.gltex = gltex;
                ex_tex.qsg   = createTextureFromGl(gltex, QSize(exh->width, exh->height), m_window);
                texs_map[id] = ex_tex;
            }
            auto& newtex = texs_map.at(id);
            if (newtex.qsg != nullptr)
                m_texture = newtex.qsg;
            else
                m_texture = m_init_texture;

            setTexture(m_texture);
            markDirty(DirtyMaterial);
            Q_EMIT textureInUse();

            bool expected = true;
            if (m_first_frame.compare_exchange_strong(expected, false)) {
                Q_EMIT sceneFirstFrame();
            }
        }
    }

private:
    sp_scene_t m_scene;
    bool       m_enable_valid;
    bool       m_share_gpu;
    bool       m_mirror_scene;

    QSGTexture*       m_init_texture;
    QSGTexture*       m_texture;
    EatFrameOp        m_eatFrameOp;
    QQuickWindow*     m_window;
    std::atomic<bool> m_first_frame;
    std::uint64_t     m_last_frame_id { 0 };

    GlExtra m_glex;

    struct ExTex {
        // int fd;
        uint        gltex;
        QSGTexture* qsg;
    };
    std::unordered_map<int, ExTex> texs_map;
};

} // namespace scenebackend

SceneObject::SceneObject(QQuickItem* parent)
    : QQuickItem(parent), m_scene(std::make_shared<wallpaper::SceneWallpaper>()) {
    setFlag(ItemHasContents, true);
    m_scene->init();
    m_scene->setPropertyString(wallpaper::PROPERTY_CACHE_PATH, GetDefaultCachePath());
}

SceneObject::~SceneObject() { _Q_INFO("Destroy sceneobject", ""); }

void SceneObject::resizeFb() {
    QSize size;
    size.setWidth(this->width());
    size.setHeight(this->height());
}

QSGNode* SceneObject::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    TextureNode* node = static_cast<TextureNode*>(oldNode);
    if (! node) {
        node = new TextureNode(
            window(), m_scene, m_enable_valid, m_share_gpu, m_mirror_scene, [this](QQuickWindow*) {
                return (QSGTexture*)nullptr;
            });
        if (node->initGl()) {
            node->initVulkan(width() * window()->devicePixelRatio(),
                             height() * window()->devicePixelRatio());

            connect(
                node, &TextureNode::redraw, window(), &QQuickWindow::update, Qt::QueuedConnection);
            connect(window(),
                    &QQuickWindow::beforeRendering,
                    node,
                    &TextureNode::newTexture,
                    Qt::DirectConnection);
            connect(node, &TextureNode::sceneFirstFrame, this, &SceneObject::firstFrame);
        }
    }

    node->setRect(boundingRect());
    return node;
}

#define SET_PROPERTY(type, name, value) m_scene->setProperty##type(name, value);

void SceneObject::setScenePropertyQurl(std::string_view name, QUrl value) {
    auto str_value = QDir::toNativeSeparators(value.toLocalFile()).toStdString();
    SET_PROPERTY(String, name, str_value);
}
// qobject

QUrl SceneObject::source() const { return m_source; }
QUrl SceneObject::assets() const { return m_assets; }

int   SceneObject::fps() const { return m_fps; }
int   SceneObject::fillMode() const { return m_fillMode; }
float SceneObject::speed() const { return m_speed; }
float SceneObject::volume() const { return m_volume; }
bool  SceneObject::muted() const { return m_muted; }

void SceneObject::setSource(const QUrl& source) {
    if (source == m_source) return;
    m_source = source;
    setScenePropertyQurl(wallpaper::PROPERTY_SOURCE, m_source);
    Q_EMIT sourceChanged();
}

namespace
{
// Minimal read-only view of a project: files come from "<stem>.pkg" when it
// exists (the format WPPkgFs reads), else from the project directory.
class ProjectFiles {
public:
    explicit ProjectFiles(const QString& scene_path) {
        QFileInfo fi(scene_path);
        m_dir = fi.absolutePath();
        QFile pkg(m_dir + "/" + fi.completeBaseName() + ".pkg");
        if (! pkg.open(QIODevice::ReadOnly)) return;
        auto readU32 = [&pkg](quint32& v) {
            return pkg.read(reinterpret_cast<char*>(&v), 4) == 4;
        };
        auto readStr = [&](QByteArray& out) {
            quint32 n;
            if (! readU32(n) || n > 4096) return false;
            out = pkg.read(n);
            return out.size() == (qsizetype)n;
        };
        QByteArray version;
        quint32    count;
        if (! readStr(version) || ! version.startsWith("PKGV") || ! readU32(count)) return;
        for (quint32 i = 0; i < count; i++) {
            QByteArray name;
            quint32    off, size;
            if (! readStr(name) || ! readU32(off) || ! readU32(size)) return;
            m_entries[QString::fromUtf8(name)] = { off, size };
        }
        m_base    = pkg.pos();
        m_pkgPath = pkg.fileName();
    }

    // absolute file path + byte range of an entry
    bool locate(const QString& name, QString& file, qint64& off, qint64& size) const {
        if (! m_pkgPath.isEmpty()) {
            auto it = m_entries.find(name);
            if (it == m_entries.end()) return false;
            file = m_pkgPath;
            off  = m_base + it->second.first;
            size = it->second.second;
            return true;
        }
        file = m_dir + "/" + name;
        off  = 0;
        size = QFileInfo(file).size();
        return QFileInfo::exists(file);
    }

    QByteArray read(const QString& name, qint64 max = -1) const {
        QString file;
        qint64  off, size;
        if (! locate(name, file, off, size)) return {};
        QFile f(file);
        if (! f.open(QIODevice::ReadOnly) || ! f.seek(off)) return {};
        return f.read(max < 0 ? size : std::min(max, size));
    }

    QJsonObject json(const QString& name) const {
        return QJsonDocument::fromJson(read(name)).object();
    }

private:
    QString                                        m_dir;
    QString                                        m_pkgPath;
    qint64                                         m_base { 0 };
    std::map<QString, std::pair<quint32, quint32>> m_entries;
};
} // namespace

QString SceneObject::videoUnderlayUrl(const QUrl& source) const {
    const QString     scene_path = source.isLocalFile() ? source.toLocalFile() : source.path();
    ProjectFiles      files(scene_path);
    const QJsonObject scene = files.json(QFileInfo(scene_path).fileName());

    // Mirror WPSceneParser: the underlay must be the bottom *drawable* layer
    // (image or particle, in file order); sounds and lights are not drawn.
    QJsonObject bottom;
    for (const auto& v : scene.value("objects").toArray()) {
        auto obj      = v.toObject();
        bool is_layer = false;
        for (auto key : { "image", "particle" })
            if (obj.contains(key) && ! obj.value(key).isNull()) is_layer = true;
        if (! is_layer) continue;
        bottom = obj;
        break;
    }
    if (! bottom.contains("image") || bottom.value("visible").toBool(true) == false) return {};

    const auto model = files.json(bottom.value("image").toString());
    const auto mat   = files.json(model.value("material").toString());
    const auto tex =
        mat.value("passes").toArray().at(0).toObject().value("textures").toArray().at(0).toString();
    if (tex.isEmpty()) return {};

    // .tex header, then the single mip: ... i32 size, then the video bytes:
    // an MP4 opens with a 4-byte box length and "ftyp", a WebM with the EBML
    // magic 1A 45 DF A3. The i32 right before the data is its byte size.
    const QString tex_name = "materials/" + tex + ".tex";
    QString       file;
    qint64        off, size;
    if (! files.locate(tex_name, file, off, size)) return {};
    const QByteArray head = files.read(tex_name, 256);
    if (! head.startsWith("TEXV")) return {};
    qint64 data = -1;
    if (auto p = head.indexOf("ftyp"); p >= 8)
        data = p - 4;
    else if (auto e = head.indexOf(QByteArray("\x1A\x45\xDF\xA3", 4)); e >= 4)
        data = e;
    if (data < 4) return {};
    qint32 mp4_size;
    std::memcpy(&mp4_size, head.constData() + data - 4, 4);
    const qint64 start = off + data;
    if (mp4_size <= 0 || data + (qint64)mp4_size > size) return {};

    const QString url = QString("slice://%1-%2@%3").arg(start).arg(start + mp4_size).arg(file);
    _Q_INFO("video underlay: %s", qPrintable(url));
    return url;
}

void SceneObject::setAssets(const QUrl& assets) {
    if (m_assets == assets) return;
    m_assets = assets;
    setScenePropertyQurl(wallpaper::PROPERTY_ASSETS, m_assets);
}

void SceneObject::setFps(int value) {
    if (m_fps == value) return;
    m_fps = value;
    SET_PROPERTY(Int32, wallpaper::PROPERTY_FPS, value);
    Q_EMIT fpsChanged();
}
void SceneObject::setFillMode(int value) {
    if (m_fillMode == value) return;
    m_fillMode = value;
    SET_PROPERTY(Int32, wallpaper::PROPERTY_FILLMODE, (int32_t)ToWPFillMode(value));
    Q_EMIT fillModeChanged();
}
void SceneObject::setSpeed(float value) {
    if (m_speed == value) return;
    m_speed = value;
    SET_PROPERTY(Float, wallpaper::PROPERTY_SPEED, value);
    Q_EMIT speedChanged();
}
void SceneObject::setVolume(float value) {
    if (m_volume == value) return;
    m_volume = value;
    SET_PROPERTY(Float, wallpaper::PROPERTY_VOLUME, value);
    Q_EMIT volumeChanged();
}
void SceneObject::setMuted(bool value) {
    if (m_muted == value) return;
    m_muted = value;
    SET_PROPERTY(Bool, wallpaper::PROPERTY_MUTED, value);
}

bool SceneObject::cachePasses() const { return m_cachePasses; }
void SceneObject::setCachePasses(bool value) {
    if (m_cachePasses == value) return;
    m_cachePasses = value;
    SET_PROPERTY(Bool, wallpaper::PROPERTY_CACHE_PASSES, value);
}

bool SceneObject::shareGpu() const { return m_share_gpu; }
void SceneObject::setShareGpu(bool value) {
    // Consumed when the render node is created; takes effect on next load.
    m_share_gpu = value;
}

bool SceneObject::mirrorScene() const { return m_mirror_scene; }
void SceneObject::setMirrorScene(bool value) {
    // Consumed when the render node is created; takes effect on next load.
    m_mirror_scene = value;
}

QString SceneObject::userProperties() const { return m_userProperties; }

void SceneObject::setUserProperties(const QString& value) {
    if (m_userProperties == value) return;
    m_userProperties = value;
    SET_PROPERTY(String, wallpaper::PROPERTY_USER_PROPS, value.toStdString());
    Q_EMIT userPropertiesChanged();
}

void SceneObject::play() { m_scene->play(); }
void SceneObject::pause() { m_scene->pause(); }

bool SceneObject::vulkanValid() const { return m_enable_valid; }
void SceneObject::enableVulkanValid() { m_enable_valid = true; }
void SceneObject::enableGenGraphviz() { SET_PROPERTY(Bool, wallpaper::PROPERTY_GRAPHIVZ, true); }

void SceneObject::setAcceptMouse(bool value) {
    if (value)
        setAcceptedMouseButtons(Qt::LeftButton);
    else
        setAcceptedMouseButtons(Qt::NoButton);
}

void SceneObject::setAcceptHover(bool value) { setAcceptHoverEvents(value); }

void SceneObject::centerPointer() {
    if (m_scene)
        m_scene->mouseInput(wallpaper::SCENE_POINTER_CENTER, wallpaper::SCENE_POINTER_CENTER);
}

void SceneObject::mousePressEvent(QMouseEvent* event) {}
void SceneObject::mouseMoveEvent(QMouseEvent* event) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    auto pos = event->position();
#else
    auto pos = event->localPos();
#endif
    m_scene->mouseInput(pos.x() / width(), pos.y() / height());
}

void SceneObject::hoverMoveEvent(QHoverEvent* event) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    auto pos = event->position();
#else
    auto pos = event->posF();
#endif
    m_scene->mouseInput(pos.x() / width(), pos.y() / height());
}

std::string SceneObject::GetDefaultCachePath() {
    return wallpaper::platform::GetCachePath(CACHE_DIR);
}

#include "SceneBackend.moc"
