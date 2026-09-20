#pragma once

#include "Options.hpp"
#include "ProjectConfig.hpp"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <vector>

class QQuickView;
class QScreen;

namespace wehypr
{

class IpcServer;
class WallpaperHost;

// One wallpaper view on one output.
class OutputSurface : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString output READ output CONSTANT)

public:
    static constexpr int       k_maxRecoveries       = 3;
    static constexpr qsizetype k_maxDiagnosticsBytes = 16 * 1024;
    static constexpr qsizetype k_maxReportedUrl      = 2048;

    OutputSurface(WallpaperHost* host, QScreen* screen);
    ~OutputSurface() override;

    bool        create(QString* error);
    QString     output() const;
    QScreen*    screen() const;
    bool        loaded() const;
    bool        frozen() const;
    QJsonObject status() const;

    Q_INVOKABLE void reportLoading();
    Q_INVOKABLE void reportLoaded();
    // Fatal; used when the page has never loaded.
    Q_INVOKABLE void reportLoadFailed(const QString& error);
    Q_INVOKABLE void reportFrozen(bool frozen);
    Q_INVOKABLE void reportDiagnostics(const QString& json);
    Q_INVOKABLE void reportConsoleMessage(const QString& message, int line, const QString& source);
    Q_INVOKABLE void reportBlockedNavigation(const QUrl& url);
    // Counts a failed load or renderer exit. Returns true if the project entry should be
    // loaded again, false once the limit is reached and the host is stopping.
    Q_INVOKABLE bool recover(const QString& reason);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    WallpaperHost*    m_host;
    QPointer<QScreen> m_screen;
    QString           m_output;
    bool              m_loaded { false };
    bool              m_frozen { false };
    int               m_recoveries { 0 };
    int               m_blockedNavigations { 0 };
    QString           m_lastBlockedNavigation;
    QString           m_lastError;
    QJsonObject       m_diagnostics;

    std::unique_ptr<QQuickView> m_view;
};

class WallpaperHost : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl url READ url CONSTANT)
    Q_PROPERTY(int fps READ fps CONSTANT)
    Q_PROPERTY(bool audio READ audio CONSTANT)
    Q_PROPERTY(bool allowRemote READ allowRemote CONSTANT)
    Q_PROPERTY(bool diagnostics READ diagnostics CONSTANT)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)

public:
    WallpaperHost(Options options, Project project, QObject* parent = nullptr);
    ~WallpaperHost() override;

    // Listens on the control socket and creates the surfaces. Returns an ExitCode.
    int start(const QString& socketPath, QString* error);

    const Options& options() const;
    QUrl           url() const;
    int            fps() const;
    bool           audio() const;
    bool           allowRemote() const;
    bool           diagnostics() const;
    bool           paused() const;

    Q_INVOKABLE QVariantMap currentProperties() const;
    Q_INVOKABLE bool        navigationAllowed(const QUrl& url) const;

    // Logs the message and leaves the event loop with ExitRuntimeFailure.
    void fail(const QString& message);

signals:
    void pausedChanged();
    void propertyValuesChanged(const QVariantMap& changed);

private:
    QJsonObject handleRequest(const QJsonObject& request);
    QJsonObject status() const;
    void        setPaused(bool paused);
    bool        wantsOutput(const QString& name) const;
    void        addScreen(QScreen* screen);
    void        removeScreen(QScreen* screen);

    Options     m_options;
    Project     m_project;
    QStringList m_wantedOutputs;
    bool        m_paused { false };
    bool        m_stopping { false };

    std::unique_ptr<IpcServer>                  m_server;
    std::vector<std::unique_ptr<OutputSurface>> m_surfaces;
};

} // namespace wehypr
