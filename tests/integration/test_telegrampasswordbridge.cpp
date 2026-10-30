#include <QtTest/QtTest>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

#include "telegrambotscontroller.h"
#include "telegrambotstore.h"
#include "../unit/telegramcontrollertestserver.h"

namespace {
class PasswordBridgeMemory {
public:
    QHash<QString, QByteArray> bytes;
    TelegramBotStore::BinaryBackend backend()
    {
        return [this](bool write, QString service, QString key, QByteArray value,
                      TelegramBotStore::BinaryCompletion done) {
            const QString identity = service + QLatin1Char('/') + key;
            if (write) { bytes.insert(identity, std::move(value)); done(TelegramBotStore::Status::Success, {}); }
            else if (bytes.contains(identity)) done(TelegramBotStore::Status::Success, bytes.value(identity));
            else done(TelegramBotStore::Status::Missing, {});
        };
    }
};
QJsonObject statusUpdate(qint64 id, qint64 chatId)
{
    return {{"update_id", id}, {"message", QJsonObject{{"message_id", id}, {"date", 1700000000},
        {"text", "/status"}, {"from", QJsonObject{{"id", chatId}, {"is_bot", false}, {"first_name", "Fixture"}}},
        {"chat", QJsonObject{{"id", chatId}, {"type", "private"}}}}}};
}
int postUpdate(const QUrl &url, const QByteArray &secret, const QJsonObject &update)
{
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("X-Telegram-Bot-Api-Secret-Token", secret);
    auto *reply = network.post(request, QJsonDocument(update).toJson(QJsonDocument::Compact));
    if (!QTest::qWaitFor([&] { return reply->isFinished(); }, 5000)) return -1;
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}
}

class TelegramPasswordBridgeTest final : public QObject {
    Q_OBJECT
private slots:
    void passwordSnapshotControlsRealRegistrationAndBothWebhookChecks()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PasswordBridgeMemory memory;
        TelegramBotStore store(directory.filePath(QStringLiteral("profiles.dat")), memory.backend());
        const QString password = QStringLiteral("synthetic local passphrase");
        QString id;
        bool saved = false;
        store.saveProtectedBot({}, QStringLiteral("Password fixture"), QStringLiteral("123456:synthetic"),
            {QStringLiteral("42")}, password, password, [&](bool ok, QString resultId, QString error) {
                QVERIFY2(ok, qPrintable(error)); id = std::move(resultId); saved = true;
            });
        QTRY_VERIFY_WITH_TIMEOUT(saved, 20000);
        QVERIFY(!id.isEmpty());
        QString expectedSecret;
        bool unlocked = false;
        store.readActivationSnapshot(id, password,
            [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
                QVERIFY2(ok, qPrintable(error));
                QVERIFY(!snapshot.sessionSecret.isEmpty());
                expectedSecret = std::move(snapshot.sessionSecret);
                unlocked = true;
            });
        QTRY_VERIFY_WITH_TIMEOUT(unlocked, 20000);

        ControllerBotServer provider;
        ControllerBotServer health;
        QTcpServer receiverPort, inspectorPort;
        QVERIFY(receiverPort.listen(QHostAddress::LocalHost));
        QVERIFY(inspectorPort.listen(QHostAddress::LocalHost));
        TelegramAgent::WebhookOptions options;
        options.tunnel.ownershipDirectory = directory.filePath(QStringLiteral("tunnel-ownership"));
        options.tunnel.port = receiverPort.serverPort();
        options.tunnel.inspectorUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/api/tunnels").arg(inspectorPort.serverPort()));
        options.tunnel.executable = QStringLiteral(PUBLICS_WEBHOOK_TEST_EXECUTABLE);
        options.tunnel.argumentPrefix = {QStringLiteral("--fake-ngrok"), QString::number(inspectorPort.serverPort()),
                                        QString::number(receiverPort.serverPort())};
        options.tunnel.discoveryDelayMs = 10;
        options.tunnel.startupTimeoutMs = 5000;
        receiverPort.close(); inspectorPort.close();
        int factories = 0;
        QPointer<TelegramAgent> runtime;
        TelegramBotsController controller(&store, [&](QObject *owner) {
            ++factories;
            runtime = new TelegramAgent(provider.root(), health.healthUrl(), options, owner);
            runtime->setCommandLedgerPathForTests(directory.filePath(QStringLiteral("admissions")));
            return runtime.data();
        });
        controller.load();
        QTRY_COMPARE(controller.bots().size(), 1);
        QVERIFY(controller.bots().first().toMap().value(QStringLiteral("allowedChatIds")).toStringList().isEmpty());
        controller.selectBot(id);
        QVERIFY(controller.startBotWithPassword(id, QStringLiteral("a wrong synthetic password")));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 20000);
        QCOMPARE(factories, 0);
        QVERIFY(!controller.error().isEmpty());

        QVERIFY(controller.startBotWithPassword(id, password));
        QTRY_VERIFY_WITH_TIMEOUT(runtime && runtime->active(), 20000);
        QCOMPARE(factories, 1);
        QVERIFY(provider.webhookSecret == expectedSecret); // Never print verification secrets.
        QVERIFY(!provider.registeredWebhook.path().isEmpty());
        QUrl receiver(QStringLiteral("http://127.0.0.1:%1").arg(options.tunnel.port));
        receiver.setPath(provider.registeredWebhook.path());
        QCOMPARE(postUpdate(receiver, QByteArray("wrong-synthetic-secret"), statusUpdate(100, 42)), 403);
        QVERIFY(provider.messages.isEmpty());
        const int denied = postUpdate(receiver, expectedSecret.toUtf8(), statusUpdate(101, 43));
        QVERIFY(denied == 403 || denied == 200); // Explicit denial or deliberate ignore are both safe.
        QVERIFY(provider.messages.isEmpty());
        QCOMPARE(postUpdate(receiver, expectedSecret.toUtf8(), statusUpdate(102, 42)), 200);
        QTRY_COMPARE(provider.messages.size(), 1);
        QCOMPARE(provider.messages.first().value(QStringLiteral("text")).toString(), QStringLiteral("🟢 Active"));

        controller.stopAll();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasRunningBots(), 20000);
        controller.startBot(); // Another Start must request a password, never reuse a saved unlock key.
        QVERIFY(runtime && !runtime->active() && !runtime->starting());
        QCOMPARE(factories, 1);
        bool removed = false;
        store.remove(id, [&](bool ok, QString, QString error) {
            QVERIFY2(ok, qPrintable(error)); removed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(removed, 20000);
        controller.load();
        QTRY_VERIFY(controller.bots().isEmpty());
        QVERIFY(!memory.bytes.isEmpty()); // Shared keychain identity/material was not deleted with one bot.
    }
};
QTEST_GUILESS_MAIN(TelegramPasswordBridgeTest)
#include "test_telegrampasswordbridge.moc"
