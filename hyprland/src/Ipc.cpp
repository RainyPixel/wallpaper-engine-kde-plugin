#include "Ipc.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>

#include <unistd.h>

namespace wehypr
{

namespace
{
constexpr qsizetype k_maxResponseBytes = 4 * 1024 * 1024;
// sockaddr_un::sun_path including the terminating zero
constexpr qsizetype k_maxSocketPath   = 108;
const char*         k_repliedProperty = "wehyprReplied";
} // namespace

QString runtimeDirectory(QString* error) {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty()) {
        *error = QStringLiteral("no usable runtime directory, XDG_RUNTIME_DIR must be set");
        return {};
    }
    const QString dir = base + QStringLiteral("/wallpaper-engine-hyprland");
    if (! QDir().mkpath(dir)) {
        *error = QStringLiteral("cannot create %1").arg(dir);
        return {};
    }
    const QFileInfo info(dir);
    if (! info.isDir() || info.ownerId() != ::getuid() ||
        ! QFile::setPermissions(
            dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        *error = QStringLiteral("%1 must be a directory owned by the current user").arg(dir);
        return {};
    }
    return dir;
}

QString socketPath(const QString& runtimeDir, const QString& instance) {
    return runtimeDir + QLatin1Char('/') + instance + QStringLiteral(".sock");
}

QString lockPath(const QString& runtimeDir, const QString& instance) {
    return runtimeDir + QLatin1Char('/') + instance + QStringLiteral(".lock");
}

InstanceLock::InstanceLock(const QString& path): m_lock(path) {
    // A running host keeps the lock for days; only a dead owner makes it stale.
    m_lock.setStaleLockTime(0);
}

bool InstanceLock::tryLock(QString* error) {
    if (m_lock.tryLock(0)) return true;
    switch (m_lock.error()) {
    case QLockFile::LockFailedError: {
        qint64  pid = 0;
        QString host;
        QString app;
        if (m_lock.getLockInfo(&pid, &host, &app))
            *error = QStringLiteral("instance is already running (pid %1)").arg(pid);
        else
            *error = QStringLiteral("instance is already running");
        break;
    }
    case QLockFile::PermissionError:
        *error = QStringLiteral("no permission to create the instance lock file");
        break;
    default: *error = QStringLiteral("cannot create the instance lock file"); break;
    }
    return false;
}

bool InstanceLock::heldByOtherProcess() const {
    return m_lock.error() == QLockFile::LockFailedError;
}

IpcServer::IpcServer(Handler handler, QObject* parent)
    : QObject(parent), m_handler(std::move(handler)) {}

IpcServer::~IpcServer() { close(); }

bool IpcServer::listen(const QString& path, QString* error) {
    close();
    if (path.toLocal8Bit().size() >= k_maxSocketPath) {
        *error = QStringLiteral("socket path is too long: %1").arg(path);
        return false;
    }
    QLocalServer::removeServer(path);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    m_server->setMaxPendingConnections(k_maxClients);
    if (! m_server->listen(path)) {
        *error = QStringLiteral("cannot listen on %1: %2").arg(path, m_server->errorString());
        delete m_server;
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, &IpcServer::acceptClients);
    return true;
}

void IpcServer::close() {
    if (! m_server) return;
    const QString path = m_server->fullServerName();
    m_server->close();
    delete m_server;
    m_server  = nullptr;
    m_clients = 0;
    QLocalServer::removeServer(path);
}

QString IpcServer::path() const { return m_server ? m_server->fullServerName() : QString(); }

void IpcServer::acceptClients() {
    while (QLocalSocket* client = m_server->nextPendingConnection()) {
        if (m_clients >= k_maxClients) {
            client->abort();
            client->deleteLater();
            continue;
        }
        ++m_clients;
        handleClient(client);
    }
}

void IpcServer::handleClient(QLocalSocket* client) {
    connect(client, &QObject::destroyed, this, [this] {
        if (m_clients > 0) --m_clients;
    });
    connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);

    auto* timeout = new QTimer(client);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, client, [client] {
        client->abort();
        client->deleteLater();
    });
    timeout->start(k_clientTimeoutMs);

    auto buffer = std::make_shared<QByteArray>();
    connect(client, &QLocalSocket::readyRead, this, [this, client, buffer] {
        if (client->property(k_repliedProperty).toBool()) {
            client->readAll();
            return;
        }
        buffer->append(client->read(k_maxRequestBytes + 1 - buffer->size()));
        const qsizetype newline = buffer->indexOf('\n');
        if (newline < 0) {
            if (buffer->size() > k_maxRequestBytes)
                reply(client, errorResponse(QStringLiteral("request is too large")));
            return;
        }
        QJsonParseError parseError;
        const auto      doc = QJsonDocument::fromJson(buffer->left(newline), &parseError);
        if (parseError.error != QJsonParseError::NoError || ! doc.isObject()) {
            reply(client,
                  errorResponse(QStringLiteral("request must be one JSON object per line")));
            return;
        }
        const QJsonObject request = doc.object();
        if (! request.value(QLatin1String("command")).isString()) {
            reply(client, errorResponse(QStringLiteral("request has no command")));
            return;
        }
        reply(client, m_handler(request));
    });
}

void IpcServer::reply(QLocalSocket* client, const QJsonObject& response) {
    client->setProperty(k_repliedProperty, true);
    client->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
    client->flush();
    client->disconnectFromServer();
}

QJsonObject errorResponse(const QString& message) {
    return QJsonObject { { QStringLiteral("ok"), false }, { QStringLiteral("error"), message } };
}

std::optional<QJsonObject> sendRequest(const QString& path, const QJsonObject& request,
                                       int timeoutMs, QString* error, bool* connected) {
    *connected = false;
    QLocalSocket socket;
    socket.connectToServer(path);
    if (! socket.waitForConnected(timeoutMs)) {
        *error = socket.errorString();
        return std::nullopt;
    }
    *connected = true;

    QElapsedTimer timer;
    timer.start();
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    socket.flush();

    QByteArray response;
    while (! response.contains('\n') && response.size() <= k_maxResponseBytes) {
        response += socket.readAll();
        if (response.contains('\n')) break;
        const qint64 remaining = timeoutMs - timer.elapsed();
        if (remaining <= 0) break;
        if (! socket.waitForReadyRead(static_cast<int>(remaining))) {
            response += socket.readAll();
            break;
        }
    }

    const qsizetype newline = response.indexOf('\n');
    if (newline < 0) {
        *error = QStringLiteral("no complete response from the host");
        return std::nullopt;
    }
    QJsonParseError parseError;
    const auto      doc = QJsonDocument::fromJson(response.left(newline), &parseError);
    if (parseError.error != QJsonParseError::NoError || ! doc.isObject()) {
        *error = QStringLiteral("invalid response from the host");
        return std::nullopt;
    }
    return doc.object();
}

} // namespace wehypr
