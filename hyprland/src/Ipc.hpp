#pragma once

#include <QJsonObject>
#include <QLockFile>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

class QLocalServer;
class QLocalSocket;

namespace wehypr
{

constexpr int k_protocolVersion = 1;

// $XDG_RUNTIME_DIR/wallpaper-engine-hyprland, created with mode 0700.
QString runtimeDirectory(QString* error);
QString socketPath(const QString& runtimeDir, const QString& instance);
QString lockPath(const QString& runtimeDir, const QString& instance);

class InstanceLock {
public:
    explicit InstanceLock(const QString& path);
    bool tryLock(QString* error);
    // After a failed tryLock: true if another live process holds the lock.
    bool heldByOtherProcess() const;

private:
    QLockFile m_lock;
};

class IpcServer : public QObject {
    Q_OBJECT

public:
    using Handler = std::function<QJsonObject(const QJsonObject& request)>;

    static constexpr qsizetype k_maxRequestBytes = 64 * 1024;
    static constexpr int       k_maxClients      = 8;
    static constexpr int       k_clientTimeoutMs = 2000;

    explicit IpcServer(Handler handler, QObject* parent = nullptr);
    ~IpcServer() override;

    // The caller must hold the instance lock; a leftover socket file is replaced.
    bool    listen(const QString& path, QString* error);
    void    close();
    QString path() const;

private:
    void acceptClients();
    void handleClient(QLocalSocket* client);
    void reply(QLocalSocket* client, const QJsonObject& response);

    Handler       m_handler;
    QLocalServer* m_server { nullptr };
    int           m_clients { 0 };
};

QJsonObject errorResponse(const QString& message);

// Sends one request and waits for the response. Returns std::nullopt if no host is
// listening; *connected tells whether the failure happened before or after connecting.
std::optional<QJsonObject> sendRequest(const QString& path, const QJsonObject& request,
                                       int timeoutMs, QString* error, bool* connected);

} // namespace wehypr
