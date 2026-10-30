#include "telegramwebhookmanager.h"
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

class TestTelegramWebhookManager : public QObject
{
    Q_OBJECT
    static QJsonObject update(qint64 chat = 42, QString text = "status")
    {
        return {{"update_id", 1}, {"message", QJsonObject{{"message_id", 7}, {"date", 1700000000},
            {"text", text}, {"from", QJsonObject{{"id", chat}, {"is_bot", false}}},
            {"chat", QJsonObject{{"id", chat}, {"type", "private"}}}}}};
    }
    static int post(TelegramWebhookManager &manager, QByteArray body, QByteArray headers)
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, manager.listeningPort());
        if (!socket.waitForConnected(1000)) return -1;
        socket.write("POST /telegram HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: application/json\r\n" + headers
            + "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        QByteArray response;
        if (!QTest::qWaitFor([&] { response += socket.readAll(); return response.contains("\r\n"); }, 2000)) return -1;
        return response.split(' ').value(1).toInt();
    }
private slots:
    void registrationPostsCanonicalPathAndJsonWithBoundedReceipt()
    {
        QTcpServer provider;
        QVERIFY(provider.listen(QHostAddress::LocalHost));
        QByteArray received;
        connect(&provider, &QTcpServer::newConnection, &provider, [&] {
            auto *socket = provider.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                received += socket->readAll();
                const auto end = received.indexOf("\r\n\r\n");
                if (end < 0 || socket->property("answered").toBool()) return;
                int length = 0;
                for (const auto &line : received.left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                if (received.size() < end + 4 + length) return;
                socket->setProperty("answered", true);
                const auto body = QByteArray(80 * 1024, ' ') + "{\"ok\":true,\"result\":true}";
                socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: "
                    + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        TelegramWebhookManager::Options options;
        options.externallyHosted = true; options.allowLoopbackProviderForTests = true;
        options.apiRoot = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(provider.serverPort()));
        options.requestTimeoutMs = 500;
        TelegramWebhookManager manager("123456:synthetic", "persisted_secret", "https://example.test/telegram", 42, options);
        QSignalSpy finished(&manager, &TelegramWebhookManager::registrationFinished);
        auto *reply = manager.registerWebhook();
        QVERIFY(reply);
        connect(reply, &QIODevice::readyRead, reply, [reply] { reply->readAll(); });
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished[0][0].toBool()); QVERIFY(!finished[0][1].toBool());
        QVERIFY(received.startsWith("POST /bot123456:synthetic/setWebhook HTTP/1.1\r\n"));
        const auto payload = QJsonDocument::fromJson(received.mid(received.indexOf("\r\n\r\n") + 4)).object();
        QCOMPARE(payload.value("url").toString(), QString("https://example.test/telegram"));
        QCOMPARE(payload.value("secret_token").toString(), QString("persisted_secret"));
        QCOMPARE(payload.value("drop_pending_updates"), QJsonValue(false));
    }
    void abortCompletionMayDeleteManagerOnlyAfterReplyFinishes_data()
    {
        QTest::addColumn<bool>("oversized");
        QTest::newRow("timeout") << false;
        QTest::newRow("oversized") << true;
    }
    void abortCompletionMayDeleteManagerOnlyAfterReplyFinishes()
    {
        QFETCH(bool, oversized);
        QTcpServer provider;
        QVERIFY(provider.listen(QHostAddress::LocalHost));
        connect(&provider, &QTcpServer::newConnection, &provider, [&] {
            while (auto *socket = provider.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [socket, oversized] {
                    socket->readAll();
                    if (!oversized || socket->property("answered").toBool()) return;
                    socket->setProperty("answered", true);
                    const QByteArray body(1024 * 1024 + 128, 'x');
                    socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                });
            }
        });
        TelegramWebhookManager::Options options;
        options.externallyHosted = true;
        options.allowLoopbackProviderForTests = true;
        options.apiRoot = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(provider.serverPort()));
        options.requestTimeoutMs = oversized ? 5000 : 50;
        auto *manager = new TelegramWebhookManager("123456:synthetic", "persisted_secret",
            "https://example.test/telegram", 42, options, &provider);
        QPointer<TelegramWebhookManager> guard(manager);
        bool replyFinished = false, delivered = false, deliveredEarly = false;
        int notifications = 0;
        connect(manager, &TelegramWebhookManager::registrationFinished, &provider,
            [&](bool confirmed, bool uncertain) {
                ++notifications;
                deliveredEarly = !replyFinished;
                delivered = true;
                QVERIFY(!confirmed);
                QVERIFY(uncertain);
                // RED avoids destroying a reply still inside abort(); GREEN exercises
                // direct deletion from the public callback under ASan.
                if (!deliveredEarly) delete manager;
            });
        auto *reply = manager->registerWebhook();
        QVERIFY(reply);
        connect(reply, &QNetworkReply::finished, &provider, [&] { replyFinished = true; });
        QTRY_VERIFY_WITH_TIMEOUT(delivered, 10000);
        QVERIFY2(!deliveredEarly, "Public registration completion ran inside the reply finished/abort stack");
        QVERIFY(!guard);
        QCOMPARE(notifications, 1);
    }

    void authenticatesBeforeParsingAndOnlyEmitsForAllowedPrivateHuman()
    {
        TelegramWebhookManager::Options options; options.port = 0;
        TelegramWebhookManager manager("123456:synthetic", "persisted_secret", "https://example.test/telegram", 42, options);
        QVERIFY(manager.listeningPort());
        QSignalSpy commands(&manager, &TelegramWebhookManager::commandReceived);
        const auto body = QJsonDocument(update()).toJson(QJsonDocument::Compact);
        QCOMPARE(post(manager, "not json", {}), 403);
        QCOMPARE(post(manager, body, "X-Telegram-Bot-Api-Secret-Token: wrong\r\n"), 403);
        const QByteArray header = "X-Telegram-Bot-Api-Secret-Token: persisted_secret\r\n";
        QCOMPARE(post(manager, body, header + header), 403);
        QCOMPARE(commands.size(), 0);
        QCOMPARE(post(manager, QJsonDocument(update(84)).toJson(), header), 200);
        QCOMPARE(commands.size(), 0);
        QCOMPARE(post(manager, body, header), 200);
        QCOMPARE(commands.size(), 1); QCOMPARE(commands[0][0].toString(), QString("status"));
        manager.setAllowedChatIds({"84"});
        QCOMPARE(post(manager, body, header), 200); QCOMPARE(commands.size(), 1);
        QCOMPARE(post(manager, QJsonDocument(update(84, "unknown")).toJson(), header), 200);
        QCOMPARE(commands.size(), 2);
        // The same allowlist gate protects status and unknown messages; forwarded,
        // edited, business, topic and malformed identity envelopes never emit.
        for (const auto *field : {"forward_origin", "forward_from", "via_bot", "edit_date",
                 "business_connection_id", "message_thread_id", "is_topic_message"}) {
            auto invalid = update(84);
            auto message = invalid.value("message").toObject(); message[QLatin1String(field)] = true;
            invalid["message"] = message;
            QCOMPARE(post(manager, QJsonDocument(invalid).toJson(), header), 200);
            QCOMPARE(commands.size(), 2);
        }
        for (const QJsonValue id : {QJsonValue(84.5), QJsonValue("84"), QJsonValue(-84), QJsonValue(true)}) {
            auto invalid = update(84); auto message = invalid.value("message").toObject();
            auto from = message.value("from").toObject(); from["id"] = id; message["from"] = from;
            invalid["message"] = message;
            QCOMPARE(post(manager, QJsonDocument(invalid).toJson(), header), 200);
            QCOMPARE(commands.size(), 2);
        }
        auto edited = update(84); edited["edited_message"] = edited.take("message");
        QCOMPARE(post(manager, QJsonDocument(edited).toJson(), header), 200);
        QCOMPARE(commands.size(), 2);
    }
};
QTEST_GUILESS_MAIN(TestTelegramWebhookManager)
#include "test_telegramwebhookmanager.moc"
