#pragma once
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>
#include <memory>

class ControllerBotServer final : public QTcpServer
{
public:
    QList<QPointer<QTcpSocket>> polls;
    bool holdIdentity = false;
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
                    const auto path = bytes->split(' ').value(1);
                    if (path.endsWith("/getMe") && !holdIdentity) {
                        // Independent provider-shaped identities for two synthetic test bots.
                        const int id = path.startsWith("/bot987654:") ? 987654 : 123456;
                        const QByteArray body = QJsonDocument(QJsonObject{{"ok", true}, {"result", QJsonObject{
                            {"id", id}, {"is_bot", true}, {"first_name", "Fixture"}}}}).toJson(QJsonDocument::Compact);
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
};
