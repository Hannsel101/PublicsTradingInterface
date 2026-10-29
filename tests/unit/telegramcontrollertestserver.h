#pragma once
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>
#include <QUrlQuery>
#include <memory>

class ControllerBotServer final : public QTcpServer
{
public:
    QList<QPointer<QTcpSocket>> polls;
    QList<QJsonObject> messages;
    int healthStatus = 200;
    bool holdHealth = false;
    bool holdIdentity = false;
    bool holdWebhookDeletion = false;
    QUrl registeredWebhook;
    QString webhookSecret;
    QList<QPointer<QTcpSocket>> deletions;
    explicit ControllerBotServer(QObject *parent = nullptr) : QTcpServer(parent)
    {
        if (!listen(QHostAddress::LocalHost)) qFatal("Cannot open loopback fixture");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                const auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    *bytes += socket->readAll();
                    const auto end = bytes->indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    int length = 0;
                    for (const auto &line : bytes->left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes->size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    const auto target = QUrl::fromEncoded(bytes->split(' ').value(1));
                    const auto path = target.path().toUtf8();
                    if (path.endsWith("/getMe") && !holdIdentity) {
                        // Independent provider-shaped identities for two synthetic test bots.
                        const int id = path.startsWith("/bot987654:") ? 987654 : 123456;
                        const QByteArray body = QJsonDocument(QJsonObject{{"ok", true}, {"result", QJsonObject{
                            {"id", id}, {"is_bot", true}, {"first_name", "Fixture"}}}}).toJson(QJsonDocument::Compact);
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                      + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    } else if (path.endsWith("/setWebhook")) {
                        const QUrlQuery query(target);
                        registeredWebhook = QUrl(query.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded));
                        webhookSecret = query.queryItemValue(QStringLiteral("secret_token"));
                        answer(socket, true);
                    } else if (path.endsWith("/getWebhookInfo")) {
                        answer(socket, QJsonObject{{"url", registeredWebhook.toString()},
                            {"has_custom_certificate", false}, {"pending_update_count", 0}});
                    } else if (path.endsWith("/deleteWebhook")) {
                        if (holdWebhookDeletion) deletions.append(socket);
                        else { registeredWebhook = QUrl(); answer(socket, true); }
                    } else if (path == "/health") {
                        if (!holdHealth) {
                            socket->write("HTTP/1.1 " + QByteArray::number(healthStatus)
                                + " Health\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                            socket->disconnectFromHost();
                        }
                    } else if (path.endsWith("/sendMessage")) {
                        const auto payload = QJsonDocument::fromJson(bytes->mid(end + 4, length)).object();
                        messages.append(payload);
                        const QByteArray body = QJsonDocument(QJsonObject{{"ok", true}, {"result", QJsonObject{
                            {"message_id", messages.size()}, {"date", 1700000001},
                            {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}}}}).toJson(QJsonDocument::Compact);
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    } else polls.append(socket);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QUrl root() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort())); }
    static void answer(QTcpSocket *socket, const QJsonValue &result)
    {
        const auto body = QJsonDocument(QJsonObject{{"ok", true}, {"result", result}}).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    void finishWebhookDeletion()
    {
        if (deletions.isEmpty()) qFatal("No pending webhook deletion");
        const auto socket = deletions.takeFirst();
        if (!socket) qFatal("Webhook deletion was already canceled");
        registeredWebhook = QUrl();
        answer(socket, true);
    }
    QUrl healthUrl() const { return root().resolved(QUrl(QStringLiteral("/health"))); }
    void sendStatus(qint64 updateId)
    {
        if (polls.isEmpty()) qFatal("No pending bot poll");
        const auto socket = polls.takeFirst();
        if (!socket) qFatal("Poll was already destroyed");
        const QByteArray body = QJsonDocument(QJsonObject{{"ok", true}, {"result", QJsonArray{
            QJsonObject{{"update_id", updateId}, {"message", QJsonObject{
                {"message_id", updateId}, {"date", 1700000000}, {"text", "/status"},
                {"from", QJsonObject{{"id", 42}, {"is_bot", false}, {"first_name", "Owner"}}},
                {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}}}}}}}).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};
