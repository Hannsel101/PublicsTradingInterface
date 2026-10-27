#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhsessionmanager.h"
#include "securecredentiallocker.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLockFile>
#include <QProcess>
#include <QScopeGuard>
#include <QTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class DeviceCredentialStore final : public RHCredentialStore
{
public:
    QHash<int, QString> values;
    int calls = 0;
    void write(int index, const QString &token, Completion done) override
    { ++calls; values[index] = token; done(true, {}); }
    void read(int index, ReadCompletion done) override
    { ++calls; done(true, values.value(index), {}); }
    void remove(int index, Completion done) override
    { ++calls; values.remove(index); done(true, {}); }
};

static bool writeMetadata(const QString &path, const QJsonObject &object)
{
    QFile file(path);
    const QByteArray bytes = QJsonDocument(object).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

struct DeviceFixture {
    QTemporaryDir dir;
    QTcpServer server;
    RHNetworkEngine network;
    DeviceCredentialStore store;
    int keys = 0;
    SecureCredentialLocker locker;
    QList<QString> tokens;
    bool configured = false;
    DeviceFixture()
        : locker(dir.filePath("profiles.dat"),
                 [this](bool, SecureCredentialLocker::KeyCompletion done) {
                     ++keys; done(true, QByteArray(32, 'k'), {});
                 })
    {
        if (!server.listen(QHostAddress::LocalHost))
            return;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (server.hasPendingConnections()) {
                QTcpSocket *peer = server.nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(peer, &QTcpSocket::readyRead, peer, [this, peer, buffer] {
                    buffer->append(peer->readAll());
                    const auto end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const auto length = QRegularExpression("content-length: (\\d+)")
                        .match(QString::fromLatin1(buffer->first(end).toLower()));
                    if (buffer->size() < end + 4 + length.captured(1).toInt())
                        return;
                    const QUrlQuery form(QString::fromUtf8(buffer->mid(end + 4)));
                    tokens.append(form.queryItemValue("device_token", QUrl::FullyDecoded));
                    const QByteArray body = R"({"error":"invalid_grant"})";
                    peer->write("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: "
                                + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    peer->disconnectFromHost();
                });
                QObject::connect(peer, &QTcpSocket::disconnected, peer, &QObject::deleteLater);
            }
        });
        const QJsonObject login{{"URL", QStringLiteral("http://127.0.0.1:%1/login/").arg(server.serverPort())},
                                {"Method", "POST"}, {"BodyEncoding", "form"},
                                {"HeaderSets", QJsonArray{"Form"}}};
        const QString config = dir.filePath("api.json");
        configured = writeMetadata(config, {{"Headers", QJsonObject{{"Form", QJsonObject{
            {"Content-Type", "application/x-www-form-urlencoded"}}}}},
            {"Endpoints", QJsonObject{{"Login", login}}}}) && network.loadConfiguration(config);
    }
    QString path() const { return dir.filePath("sessions.json"); }
    QJsonObject metadata() const {
        QFile file(path());
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }
};

class RHDeviceTokenTest : public QObject
{
    Q_OBJECT
private slots:
    void deletingAllProfilesPreservesDeviceAndUnknownMetadata()
    {
        DeviceFixture f;
        QVERIFY(f.configured);
        const QString token = "00112233-4455-4677-AB99-AABBCCDDEEFF";
        QVERIFY(writeMetadata(f.path(), {{"sessions", QJsonArray{}}, {"device_token", token},
                                       {"future_metadata", QJsonObject{{"keep", true}}}}));
        RHSessionManager manager(f.network, f.path(), &f.store, nullptr, 1000, 10, &f.locker);
        QVERIFY(manager.saveAuthenticatedSession("one@example.test", "one-secret", 3600));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(f.metadata().value("device_token").toString(), token);
        QVERIFY(manager.saveAuthenticatedSession("two@example.test", "two-secret", 3600));
        QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.deleteSavedSession(0));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(f.metadata().value("sessions").toArray().size(), 1);
        QCOMPARE(f.metadata().value("device_token").toString(), token);
        QVERIFY(manager.deleteSavedSession(1));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(f.metadata().value("sessions").toArray().size(), 0);
        QCOMPARE(f.metadata().value("device_token").toString(), token);
        QCOMPARE(f.metadata().value("future_metadata").toObject().value("keep").toBool(), true);
        QVERIFY(!manager.signIn("third@example.test", "do-not-save"));
        QTRY_VERIFY(!manager.busy());
        QVERIFY(f.tokens.isEmpty());
    }

    void changedCorruptMetadataIsNeverOverwritten_data()
    {
        QTest::addColumn<QJsonObject>("metadata");
        QTest::addColumn<bool>("login");
        for (bool login : {false, true}) {
            const QByteArray prefix = login ? "ensure-" : "persist-";
            QTest::newRow((prefix + "token").constData())
                << QJsonObject{{"sessions", QJsonArray{}}, {"device_token", "bad"}} << login;
            QTest::newRow((prefix + "record").constData())
                << QJsonObject{{"sessions", QJsonArray{QJsonObject{{"index", -1}}}}} << login;
        }
    }
    void changedCorruptMetadataIsNeverOverwritten()
    {
        QFETCH(QJsonObject, metadata);
        QFETCH(bool, login);
        DeviceFixture f;
        QVERIFY(f.configured);
        RHSessionManager manager(f.network, f.path(), &f.store, nullptr, 1000, 10, &f.locker);
        QVERIFY(writeMetadata(f.path(), metadata));
        if (login) {
            QVERIFY(!manager.signIn("owner@example.test", "do-not-save"));
            QCOMPARE(f.store.calls, 0);
            QCOMPARE(f.keys, 0);
        } else {
            // Ownership validation rejects corrupt metadata before any secret-store access.
            QVERIFY(!manager.saveAuthenticatedSession("owner@example.test", "token", 3600));
            QVERIFY(!manager.busy());
            QCOMPARE(manager.rowCount(), 0);
            QCOMPARE(f.store.calls, 0);
            QCOMPARE(f.keys, 0);
            QVERIFY(f.store.values.isEmpty());
        }
        QVERIFY(!manager.error().isEmpty());
        QCOMPARE(f.metadata(), metadata);
        QVERIFY(f.tokens.isEmpty());
    }

    void staleModelSavePreservesLatestDeviceAndOtherRecords()
    {
        DeviceFixture f;
        QVERIFY(f.configured);
        RHSessionManager manager(f.network, f.path(), &f.store, nullptr, 1000, 10, &f.locker);
        QVERIFY(manager.saveAuthenticatedSession("owner@example.test", "old", 3600));
        QTRY_VERIFY(!manager.busy());
        QJsonObject metadata = f.metadata();
        QJsonObject owner = metadata.value("sessions").toArray().first().toObject();
        owner.insert("extension", "keep");
        QJsonObject other = owner;
        other.insert("index", 1);
        other.insert("account_name", "external@example.test");
        const QString latest = "00112233-4455-4677-AB99-AABBCCDDEEFF";
        metadata.insert("sessions", QJsonArray{owner, other});
        metadata.insert("device_token", latest);
        metadata.insert("external", true);
        QVERIFY(writeMetadata(f.path(), metadata));
        QVERIFY(manager.saveAuthenticatedSession("owner@example.test", "renewed", 7200));
        QTRY_VERIFY(!manager.busy());
        const QJsonArray saved = f.metadata().value("sessions").toArray();
        QCOMPARE(saved.size(), 2);
        QCOMPARE(saved.at(1).toObject(), other);
        QCOMPARE(saved.at(0).toObject().value("extension").toString(), QStringLiteral("keep"));
        QCOMPARE(f.metadata().value("device_token").toString(), latest);
        QCOMPARE(f.metadata().value("external").toBool(), true);
        QVERIFY(!manager.signIn("new@example.test", "password"));
        QTRY_VERIFY(!manager.busy());
        QVERIFY(f.tokens.isEmpty());
    }

    void malformedSavedTokenFailsClosed_data()
    {
        QTest::addColumn<QJsonValue>("token");
        QTest::newRow("empty") << QJsonValue("");
        QTest::newRow("null") << QJsonValue(QJsonValue::Null);
        QTest::newRow("number") << QJsonValue(42);
        QTest::newRow("braces") << QJsonValue("{00112233-4455-4677-8899-aabbccddeeff}");
        QTest::newRow("version-1") << QJsonValue("00112233-4455-1677-8899-aabbccddeeff");
        QTest::newRow("variant") << QJsonValue("00112233-4455-4677-7899-aabbccddeeff");
        QTest::newRow("whitespace") << QJsonValue("00112233-4455-4677-8899-aabbccddeeff ");
        QTest::newRow("newline") << QJsonValue("00112233-4455-4677-8899-aabbccddeeff\n");
    }
    void malformedSavedTokenFailsClosed()
    {
        QFETCH(QJsonValue, token);
        QTemporaryDir dir;
        const QString path = dir.filePath("sessions.json");
        const QJsonObject metadata{{"sessions", QJsonArray{}}, {"device_token", token}};
        QVERIFY(writeMetadata(path, metadata));
        RHNetworkEngine network;
        DeviceCredentialStore store;
        int keys = 0;
        SecureCredentialLocker locker(dir.filePath("profiles.dat"),
            [&](bool, SecureCredentialLocker::KeyCompletion done) {
                ++keys; done(false, {}, "Unexpected key access");
            });
        RHSessionManager manager(network, path, &store, nullptr, 1000, 10, &locker);
        QVERIFY(!manager.load());
        QVERIFY(!manager.saveAuthenticatedSession("owner@example.test", "secret", 3600));
        QCOMPARE(store.calls, 0);
        QCOMPARE(keys, 0);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object(), metadata);
    }
};

QTEST_MAIN(RHDeviceTokenTest)
#include "test_rhdevicetoken.moc"
