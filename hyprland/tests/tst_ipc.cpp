// SPDX-License-Identifier: GPL-2.0-only
// Unit tests for the instance lock and the local control socket

#include <QtTest>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>

#include "Ipc.hpp"

using namespace wehypr;

class TestIpc : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_runtime;
    QString       m_dir;
    int           m_handled { 0 };

    IpcServer::Handler echoHandler() {
        return [this](const QJsonObject& request) {
            ++m_handled;
            return QJsonObject { { "ok", true }, { "echo", request.value("command") } };
        };
    }

    // Sends raw bytes without the blocking client and collects everything the server
    // writes until it closes the connection.
    static QByteArray rawExchange(const QString& path, const QByteArray& payload,
                                  int timeoutMs = 5000) {
        QLocalSocket socket;
        socket.connectToServer(path);
        if (! socket.waitForConnected(1000)) return "not connected";
        if (! payload.isEmpty()) socket.write(payload);
        QByteArray    data;
        QElapsedTimer timer;
        timer.start();
        while (socket.state() != QLocalSocket::UnconnectedState && timer.elapsed() < timeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            data += socket.readAll();
        }
        return data + socket.readAll();
    }

    static QJsonObject parseLine(const QByteArray& data) {
        return QJsonDocument::fromJson(data.left(data.indexOf('\n'))).object();
    }

private slots:
    void initTestCase() {
        QVERIFY(m_runtime.isValid());
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(m_runtime.path()));
        QString error;
        m_dir = runtimeDirectory(&error);
        QVERIFY2(! m_dir.isEmpty(), qPrintable(error));
    }

    void init() { m_handled = 0; }

    // ── paths and lock ────────────────────────────────────────────────────────
    void runtimeDirectory_isPrivate() {
        QVERIFY(m_dir.startsWith(m_runtime.path()));
        const QFileInfo info(m_dir);
        QVERIFY(info.isDir());
        QCOMPARE(info.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther |
                                       QFileDevice::WriteGroup | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
        QCOMPARE(socketPath(m_dir, "a"), m_dir + "/a.sock");
        QCOMPARE(lockPath(m_dir, "a"), m_dir + "/a.lock");
    }

    void instanceLock_isExclusive() {
        const QString path = lockPath(m_dir, "lock-test");
        QString       error;
        {
            InstanceLock first(path);
            QVERIFY2(first.tryLock(&error), qPrintable(error));
            InstanceLock second(path);
            QVERIFY(! second.tryLock(&error));
            QVERIFY(second.heldByOtherProcess());
            QVERIFY(error.contains("already running"));
        }
        InstanceLock again(path);
        QVERIFY2(again.tryLock(&error), qPrintable(error));
    }

    // ── server ────────────────────────────────────────────────────────────────
    void roundTrip() {
        const QString path = socketPath(m_dir, "roundtrip");
        IpcServer     server(echoHandler());
        QString       error;
        QVERIFY2(server.listen(path, &error), qPrintable(error));

        std::optional<QJsonObject> response;
        bool                       connected = false;
        QString                    clientError;
        std::unique_ptr<QThread>   client(QThread::create([&] {
            response = sendRequest(path, { { "command", "status" } }, 5000, &clientError, &connected);
        }));
        client->start();
        QTRY_VERIFY_WITH_TIMEOUT(client->isFinished(), 10000);

        QVERIFY(connected);
        QVERIFY2(response, qPrintable(clientError));
        QCOMPARE(response->value("ok").toBool(), true);
        QCOMPARE(response->value("echo").toString(), QStringLiteral("status"));
        QCOMPARE(m_handled, 1);
    }

    void invalidRequests_data() {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<QString>("message");
        QTest::newRow("broken json") << QByteArray("{broken\n") << QStringLiteral("JSON object");
        QTest::newRow("array") << QByteArray("[1]\n") << QStringLiteral("JSON object");
        QTest::newRow("no command") << QByteArray("{\"x\":1}\n") << QStringLiteral("no command");
        QTest::newRow("too large") << QByteArray(IpcServer::k_maxRequestBytes + 10, 'a')
                                   << QStringLiteral("too large");
    }

    void invalidRequests() {
        QFETCH(QByteArray, payload);
        QFETCH(QString, message);
        const QString path = socketPath(m_dir, "invalid");
        IpcServer     server(echoHandler());
        QString       error;
        QVERIFY2(server.listen(path, &error), qPrintable(error));

        const QJsonObject response = parseLine(rawExchange(path, payload));
        QCOMPARE(response.value("ok").toBool(true), false);
        QVERIFY2(response.value("error").toString().contains(message),
                 qPrintable(response.value("error").toString()));
        QCOMPARE(m_handled, 0);
    }

    void onlyFirstLineHandled() {
        const QString path = socketPath(m_dir, "lines");
        IpcServer     server(echoHandler());
        QString       error;
        QVERIFY2(server.listen(path, &error), qPrintable(error));

        const QByteArray data =
            rawExchange(path, "{\"command\":\"a\"}\n{\"command\":\"b\"}\n");
        QCOMPARE(parseLine(data).value("echo").toString(), QStringLiteral("a"));
        QCOMPARE(data.count('\n'), qsizetype(1));
        QCOMPARE(m_handled, 1);
    }

    void idleClientDisconnected() {
        const QString path = socketPath(m_dir, "idle");
        IpcServer     server(echoHandler());
        QString       error;
        QVERIFY2(server.listen(path, &error), qPrintable(error));

        QElapsedTimer timer;
        timer.start();
        const QByteArray data = rawExchange(path, {}, IpcServer::k_clientTimeoutMs * 3);
        QVERIFY(data.isEmpty());
        QVERIFY(timer.elapsed() < IpcServer::k_clientTimeoutMs * 3);
        QCOMPARE(m_handled, 0);
    }

    void clientLimit() {
        const QString path = socketPath(m_dir, "limit");
        IpcServer     server(echoHandler());
        QString       error;
        QVERIFY2(server.listen(path, &error), qPrintable(error));

        std::vector<std::unique_ptr<QLocalSocket>> sockets;
        for (int i = 0; i < IpcServer::k_maxClients + 2; ++i) {
            auto socket = std::make_unique<QLocalSocket>();
            socket->connectToServer(path);
            sockets.push_back(std::move(socket));
            // lets the server accept each connection before the next one arrives
            QTest::qWait(50);
        }
        QTest::qWait(700);
        int open = 0;
        for (const auto& socket : sockets) {
            if (socket->state() == QLocalSocket::ConnectedState) ++open;
        }
        QCOMPARE(open, IpcServer::k_maxClients);

        // Capacity is released again once idle clients are gone.
        sockets.clear();
        QTest::qWait(200);
        QCOMPARE(parseLine(rawExchange(path, "{\"command\":\"after\"}\n")).value("echo").toString(),
                 QStringLiteral("after"));
    }

    void closeRemovesSocket_andStaleSocketReplaced() {
        const QString path = socketPath(m_dir, "stale");
        {
            QFile stale(path);
            QVERIFY(stale.open(QIODevice::WriteOnly));
        }
        IpcServer server(echoHandler());
        QString   error;
        QVERIFY2(server.listen(path, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(path));
        server.close();
        QVERIFY(! QFileInfo::exists(path));
    }

    void tooLongPathRejected() {
        IpcServer server(echoHandler());
        QString   error;
        QVERIFY(! server.listen(m_dir + "/" + QString(120, QLatin1Char('x')) + ".sock", &error));
        QVERIFY(error.contains("too long"));
    }

    void sendRequest_withoutServer() {
        bool       connected = true;
        QString    error;
        const auto response = sendRequest(socketPath(m_dir, "nobody"),
                                          { { "command", "status" } },
                                          1000,
                                          &error,
                                          &connected);
        QVERIFY(! response);
        QVERIFY(! connected);
    }
};

QTEST_GUILESS_MAIN(TestIpc)
#include "tst_ipc.moc"
