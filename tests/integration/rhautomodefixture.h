#pragma once
#include "telegramrobinhoodfixture.h"
#include <QNetworkRequest>
#include <QNetworkReply>

namespace RHAutoModeFixture {
// Reuses every existing synthetic route/store; the loopback-only response proxy
// lets Auto Mode test authoritative account metadata without changing the shared
// fixture used by Telegram siblings. No production URLs are ever contacted.
struct Fixture : TelegramRobinhoodFixture::Fixture {
    QTcpServer proxy;
    QNetworkAccessManager upstream;
    std::function<QJsonObject(const QString &, const QJsonObject &)> transform;
    Fixture() {
        if (!ready || !proxy.listen(QHostAddress::LocalHost)) { ready = false; return; }
        connect(&proxy, &QTcpServer::newConnection, this, [this] {
            while (proxy.hasPendingConnections()) {
                auto *peer = proxy.nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(peer, &QTcpSocket::disconnected, peer, &QObject::deleteLater);
                connect(peer, &QTcpSocket::readyRead, this, [this, peer, bytes] {
                    *bytes += peer->readAll();
                    const auto end = bytes->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    int length = 0;
                    for (const auto &line : bytes->first(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes->size() < end + 4 + length) return;
                    disconnect(peer, &QTcpSocket::readyRead, this, nullptr);
                    const auto first = bytes->first(bytes->indexOf('\n')).split(' ');
                    const auto path = QString::fromLatin1(first.value(1));
                    QNetworkRequest request{QUrl(origin + path)};
                    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
                    for (const auto &line : bytes->first(end).split('\n')) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0 && !line.toLower().startsWith("host:") && !line.toLower().startsWith("content-length:"))
                            request.setRawHeader(line.first(colon).trimmed(), line.mid(colon + 1).trimmed());
                    }
                    auto *reply = upstream.sendCustomRequest(request, first.value(0), bytes->mid(end + 4, length));
                    const QPointer<QTcpSocket> target(peer);
                    connect(reply, &QNetworkReply::finished, this, [this, target, reply, path] {
                        reply->deleteLater(); if (!target) return;
                        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                        if (!status) { target->disconnectFromHost(); return; }
                        auto body = reply->readAll();
                        const auto document = QJsonDocument::fromJson(body);
                        if (transform && document.isObject())
                            body = QJsonDocument(transform(QUrl(path).path(), document.object())).toJson(QJsonDocument::Compact);
                        target->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: "
                            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        target->disconnectFromHost();
                    });
                });
            }
        });
        QFile config(dir.filePath("api.json"));
        if (!config.open(QIODevice::ReadOnly)) { ready = false; return; }
        auto object = QJsonDocument::fromJson(config.readAll()).object(); config.close();
        auto endpoints = object.value("Endpoints").toObject();
        auto accounts = endpoints.value("Accounts").toObject();
        accounts.insert("URL", QString("http://127.0.0.1:%1/accounts/").arg(proxy.serverPort()));
        endpoints.insert("Accounts", accounts); object.insert("Endpoints", endpoints);
        if (!config.open(QIODevice::WriteOnly | QIODevice::Truncate)) { ready = false; return; }
        config.write(QJsonDocument(object).toJson()); config.close();
        ready = network.loadConfiguration(config.fileName());
    }
};
}
