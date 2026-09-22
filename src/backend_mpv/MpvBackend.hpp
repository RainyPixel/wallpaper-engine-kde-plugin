#ifndef MPVRENDERER_H_
#define MPVRENDERER_H_

#include <mpv/client.h>
#include <mpv/render.h>

#include <QtQuick/QQuickRhiItem>
#include <QtCore/QLoggingCategory>
#include <memory>

#include "qthelper.hpp"

Q_DECLARE_LOGGING_CATEGORY(wekdeMpv)

namespace mpv
{

struct MpvHandle {
    MpvHandle(mpv_handle* mpv): handle(mpv) {}
    ~MpvHandle() { mpv_terminate_destroy(handle); }
    mpv_handle* handle;
};

class MpvRender;

class MpvObject : public QQuickRhiItem {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool mute READ mute WRITE setMute)
    Q_PROPERTY(QString logfile READ logfile WRITE setLogfile)
    Q_PROPERTY(int volume READ volume WRITE setVolume)
    Q_PROPERTY(QString hwdec READ hwdec WRITE setHwdec)
    Q_PROPERTY(int maxFps READ maxFps WRITE setMaxFps NOTIFY maxFpsChanged)

    friend class MpvRender;

public:
    explicit MpvObject(QQuickItem* parent = nullptr);
    virtual ~MpvObject();
    QQuickRhiItemRenderer* createRenderer() override;

    enum Status
    {
        Stopped,
        Playing,
        Paused,
    };
    Q_ENUM(Status)
    Status  status() const;
    QUrl    source() const;
    bool    mute() const;
    QString logfile() const;
    int     volume() const;

    void    setSource(const QUrl& source);
    void    setMute(const bool& mute);
    void    setLogfile(const QString& logfile);
    void    setVolume(const int& volume);
    QString hwdec() const;
    void    setHwdec(const QString& hwdec);
    int     maxFps() const;
    void    setMaxFps(int fps);

public slots:
    void play();
    void pause();
    void stop();

    bool     command(const QVariant& params);
    bool     setProperty(const QString& name, const QVariant& value);
    QVariant getProperty(const QString& name, bool* ok = nullptr) const;
    void     initCallback();
    void     checkAndEmitFirstFrame();

signals:
    void initFinished();
    void statusChanged();
    void sourceChanged();
    void firstFrame();
    void maxFpsChanged();

private:
    void applyFpsLimit();

    bool    inited = false;
    QUrl    m_source;
    Status  m_status = Stopped;
    QString m_hwdec { "auto" };
    int     m_maxFps { 0 };

private:
    mpv_handle*                m_mpv { nullptr };
    std::shared_ptr<MpvHandle> m_shared_mpv { nullptr };
    bool                       m_first_frame { true };
};
} // namespace mpv

#endif
