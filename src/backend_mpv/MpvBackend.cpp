#include "MpvBackend.hpp"

#include <QtGlobal>
#include <QtCore/QObject>
#include <QtCore/QDir>
#include <QtQuick/QQuickWindow>
#include <rhi/qrhi.h>

#include <memory>
#include <vector>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>

Q_LOGGING_CATEGORY(wekdeMpv, "wekde.mpv")

#define _Q_DEBUG() qCDebug(wekdeMpv)

using namespace mpv;

// ── MpvObject property/command methods ──────────────────────────────────────

bool MpvObject::command(const QVariant& params) {
    auto* mpv       = m_mpv;
    int   errorCode = mpv::qt::get_error(mpv::qt::command(mpv, params));
    return (errorCode >= 0);
}

bool MpvObject::setProperty(const QString& name, const QVariant& value) {
    auto* mpv       = m_mpv;
    int   errorCode = mpv::qt::get_error(mpv::qt::set_property(mpv, name, value));
    _Q_DEBUG() << "Setting property" << name << "to" << value;
    return (errorCode >= 0);
}

QVariant MpvObject::getProperty(const QString& name, bool* ok) const {
    auto* mpv = m_mpv;
    if (ok) *ok = false;

    if (name.isEmpty()) {
        return QVariant();
    }
    QVariant  result    = mpv::qt::get_property(mpv, name);
    const int errorCode = mpv::qt::get_error(result);
    if (errorCode >= 0) {
        if (ok) {
            *ok = true;
        }
    } else {
        _Q_DEBUG() << "Failed to query property: " << name << "code" << errorCode << " result"
                   << result;
    }
    return result;
}

void MpvObject::initCallback() {
    QUrl temp(m_source.toString());
    m_source.clear();
    inited = true;
    setSource(temp);
    Q_EMIT initFinished();
}

void MpvObject::play() {
    if (status() != Paused) return;
    this->setProperty("pause", false);
}

void MpvObject::pause() {
    if (status() != Playing) return;
    this->setProperty("pause", true);
}

void MpvObject::stop() {
    if (status() == Stopped) return;
    bool result = this->command(QVariantList { "stop" });
    if (result) {
        m_source.clear();
        Q_EMIT sourceChanged();
    }
}

MpvObject::Status MpvObject::status() const {
    const bool stopped = getProperty("idle-active").toBool();
    const bool paused  = getProperty("pause").toBool();
    return stopped ? Stopped : (paused ? Paused : Playing);
}

QUrl MpvObject::source() const { return m_source; }

bool MpvObject::mute() const {
    QString aid = getProperty("aid").toString();
    return aid == "no";
}

QString MpvObject::logfile() const { return getProperty("log-file").toString(); }

int MpvObject::volume() const { return getProperty("volume").toInt(); }

void MpvObject::setMute(const bool& mute) { setProperty("aid", mute ? "no" : "auto"); }

void MpvObject::setVolume(const int& volume) { setProperty("volume", volume); }

QString MpvObject::hwdec() const { return m_hwdec; }

void MpvObject::setHwdec(const QString& hwdec) {
    if (m_hwdec == hwdec) return;
    m_hwdec = hwdec;
    // Use mpv_set_property_string for runtime changes (after mpv_initialize)
    mpv_set_property_string(m_mpv, "hwdec", hwdec.toUtf8().constData());
}

void MpvObject::setLogfile(const QString& logfile) { setProperty("log-file", logfile); }

void MpvObject::setSource(const QUrl& source) {
    if (source.isEmpty()) {
        stop();
        return;
    }
    if (! source.isValid() || (source == m_source)) {
        return;
    }
    if (! inited) {
        m_source = source;
        return;
    }
    bool result = this->command(QVariantList {
        "loadfile",
        source.isLocalFile() ? QDir::toNativeSeparators(source.toLocalFile()) : source.url() });
    if (result) {
        m_source = source;
        Q_EMIT sourceChanged();

        m_first_frame = false;
    }
}

// ── MpvRender (render thread) ───────────────────────────────────────────────

namespace mpv
{

class MpvRender : public QObject, public QQuickRhiItemRenderer {
    Q_OBJECT
public:
    MpvRender(std::shared_ptr<MpvHandle> mpv): m_shared_mpv(mpv), m_mpv(mpv->handle) {}

    virtual ~MpvRender() {
        _Q_DEBUG() << "destroyed";
        mpv::qt::command(m_mpv, QVariantList { "stop" });

        if (m_render_ctx) {
            mpv_render_context_set_update_callback(m_render_ctx, nullptr, nullptr);
            mpv_render_context_free(m_render_ctx);
        }
        m_render_ctx = nullptr;
    }

    bool Dirty() const { return m_dirty.load(); }
    bool setDirty(bool v) { return m_dirty.exchange(v); }

signals:
    void mpvRedraw();
    void inited();

protected:
    void initialize(QRhiCommandBuffer* cb) override { Q_UNUSED(cb) }

    void synchronize(QQuickRhiItem* item) override {
        MpvObject* mpv_obj = static_cast<MpvObject*>(item);

        if (! m_render_ctx) {
            mpv_render_param params[] = { { MPV_RENDER_PARAM_API_TYPE,
                                            (void*)MPV_RENDER_API_TYPE_SW },
                                          { MPV_RENDER_PARAM_INVALID, nullptr } };
            if (mpv_render_context_create(&m_render_ctx, m_mpv, params) >= 0) {
                mpv_render_context_set_update_callback(m_render_ctx, on_mpv_redraw, this);
                Q_EMIT this->inited();
            } else {
                _Q_DEBUG() << "failed to create SW render context";
            }
        }

        if (Dirty()) {
            mpv_obj->checkAndEmitFirstFrame();
        }
    }

    void render(QRhiCommandBuffer* cb) override {
        QRhiTexture* tex = colorTexture();
        if (! tex) return;

        QSize texSize = tex->pixelSize();
        int   w       = texSize.width();
        int   h       = texSize.height();
        if (w <= 0 || h <= 0) return;

        // Align stride to 64 bytes for optimal SIMD performance
        size_t stride  = ((size_t)w * 4 + 63) & ~(size_t)63;
        size_t bufSize = stride * h;

        bool sizeChanged = (m_bufWidth != w || m_bufHeight != h);
        if (sizeChanged) {
            // bufSize must be a multiple of 64 for aligned_alloc
            size_t alignedBufSize = (bufSize + 63) & ~(size_t)63;
            m_buffer.resize(alignedBufSize);
            m_bufWidth  = w;
            m_bufHeight = h;
        }

        if (m_render_ctx && (setDirty(false) || sizeChanged)) {
            int size[] = { w, h };

            mpv_render_param params[] = { { MPV_RENDER_PARAM_SW_SIZE, size },
                                          { MPV_RENDER_PARAM_SW_FORMAT, (void*)"rgb0" },
                                          { MPV_RENDER_PARAM_SW_STRIDE, &stride },
                                          { MPV_RENDER_PARAM_SW_POINTER, m_buffer.data },
                                          { MPV_RENDER_PARAM_INVALID, nullptr } };

            if (mpv_render_context_render(m_render_ctx, params) < 0) {
                _Q_DEBUG() << "SW render failed";
                return;
            }

            // rgb0 format has alpha=0; set to 255 so the texture is opaque.
            // 0xFF000000 targets byte +3 (alpha) only on little-endian.
            static_assert(std::endian::native == std::endian::little,
                          "Alpha fix assumes little-endian byte order");
            for (int y = 0; y < h; y++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(m_buffer.data + y * stride);
                for (int x = 0; x < w; x++) {
                    row[x] |= 0xFF000000u;
                }
            }

            // Upload frame to colorTexture
            QByteArray data = QByteArray::fromRawData(reinterpret_cast<const char*>(m_buffer.data),
                                                      static_cast<qsizetype>(bufSize));

            QRhiTextureSubresourceUploadDescription desc(data);
            desc.setDataStride((quint32)stride);

            QRhiResourceUpdateBatch* batch = rhi()->nextResourceUpdateBatch();
            batch->uploadTexture(
                tex, QRhiTextureUploadDescription({ QRhiTextureUploadEntry(0, 0, desc) }));
            cb->resourceUpdate(batch);
        }
    }

private:
    static void on_mpv_redraw(void* ctx) {
        auto* render = static_cast<MpvRender*>(ctx);
        render->setDirty(true);
        Q_EMIT render->mpvRedraw();
    }

    mpv_handle*                m_mpv { nullptr };
    mpv_render_context*        m_render_ctx { nullptr };
    std::shared_ptr<MpvHandle> m_shared_mpv;

    struct AlignedBuffer {
        uint8_t* data { nullptr };
        size_t   size { 0 };
        void     resize(size_t newSize) {
            if (size == newSize) return;
            std::free(data);
            data = static_cast<uint8_t*>(std::aligned_alloc(64, newSize));
            size = data ? newSize : 0;
            if (data) std::memset(data, 0, size);
        }
        ~AlignedBuffer() { std::free(data); }
        AlignedBuffer()                                = default;
        AlignedBuffer(const AlignedBuffer&)            = delete;
        AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    };

    AlignedBuffer     m_buffer;
    int               m_bufWidth { 0 };
    int               m_bufHeight { 0 };
    std::atomic<bool> m_dirty { false };
};

} // namespace mpv

// ── MpvObject construction and renderer creation ────────────────────────────

MpvObject::MpvObject(QQuickItem* parent)
    : QQuickRhiItem(parent), m_shared_mpv(std::make_shared<MpvHandle>(mpv_create())) {
    m_mpv = m_shared_mpv->handle;

    if (! m_mpv) {
        _Q_DEBUG() << "could not create mpv context";
        return;
    }

    // All options MUST be set before mpv_initialize
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "msg-level", "all=info");
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", m_hwdec.toUtf8().constData());
    mpv_set_option_string(m_mpv, "vf", "vflip");
    mpv_set_option_string(m_mpv, "loop", "inf");

    if (mpv_initialize(m_mpv) < 0) {
        _Q_DEBUG() << "could not initialize mpv context";
        m_mpv = nullptr;
        return;
    }
}

MpvObject::~MpvObject() {}

void MpvObject::checkAndEmitFirstFrame() {
    if (! m_first_frame) {
        m_first_frame = true;
        Q_EMIT firstFrame();
    }
}

QQuickRhiItemRenderer* MpvObject::createRenderer() {
    window()->setPersistentSceneGraph(true);

    auto* render = new MpvRender(m_shared_mpv);

    connect(render, &MpvRender::mpvRedraw, this, &MpvObject::update, Qt::QueuedConnection);
    connect(render, &MpvRender::inited, this, &MpvObject::initCallback, Qt::QueuedConnection);
    return render;
}

#include "MpvBackend.moc"
