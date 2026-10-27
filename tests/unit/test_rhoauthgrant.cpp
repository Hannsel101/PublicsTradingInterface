#include "rhoauthgrant.h"
#include <QJsonDocument>
#include <QtTest/QtTest>

namespace {
QJsonObject response()
{
    return {{"access_token", "access+fixture/="}, {"refresh_token", "refresh+fixture/="},
            {"token_type", "Bearer"}, {"scope", "internal"}, {"expires_in", 1234},
            {"provider_extension", QJsonObject{{"kept", true}}}};
}
const auto now = QDateTime::fromString("2026-10-27T12:00:00.000Z", Qt::ISODateWithMs);
}
class RHOAuthGrantTest : public QObject
{
    Q_OBJECT
private slots:
    void nativeEnvelopeRejectsControlCharactersInEmail()
    {
        RHOAuthGrant grant; QVERIFY(RHOAuthGrant::parseResponse(response(), "client", "device", now, &grant));
        grant.accountId="principal"; grant.nativeEmail="owner\n@example.test"; grant.nativePassword="valid-password";
        QVERIFY(grant.serialize().isEmpty());
    }
    void nativeCredentialsAreStrictSeparateAndInherited()
    {
        RHOAuthGrant grant; QVERIFY(RHOAuthGrant::parseResponse(response(), "client", "device", now, &grant));
        grant.accountId = "principal";
        grant.nativeEmail = QString::fromUtf8("owner+é@example.test");
        grant.nativePassword = QString::fromUtf8("p+\"\\é\nword");
        const auto encoded = grant.serialize();
        const auto envelope = QJsonDocument::fromJson(encoded.toUtf8()).object();
        QCOMPARE(envelope.value("version"), QJsonValue(2));
        QVERIFY(!grant.response.contains("native_credentials"));
        RHOAuthGrant restored; QVERIFY(RHOAuthGrant::deserialize(encoded, &restored));
        QCOMPARE(restored.nativeEmail, grant.nativeEmail); QCOMPARE(restored.nativePassword, grant.nativePassword);
        auto refreshed = response(); refreshed.remove("refresh_token"); refreshed.remove("scope");
        RHOAuthGrant next; QVERIFY(RHOAuthGrant::parseResponse(refreshed, "client", "device", now.addSecs(10), &next, nullptr, &restored));
        QCOMPARE(next.nativeEmail, grant.nativeEmail); QCOMPARE(next.nativePassword, grant.nativePassword);
        QVERIFY(RHOAuthGrant::deserialize(next.serialize(), &restored));
        auto bad = envelope; auto credentials = bad["native_credentials"].toObject();
        credentials["extra"] = true; bad["native_credentials"] = credentials;
        QVERIFY(!RHOAuthGrant::deserialize(QString::fromUtf8(QJsonDocument(bad).toJson()), &restored));
        bad = envelope; bad["version"] = 1;
        QVERIFY(!RHOAuthGrant::deserialize(QString::fromUtf8(QJsonDocument(bad).toJson()), &restored));
        grant.nativeEmail.clear(); QVERIFY(grant.serialize().isEmpty());
    }

    void renewalRetainsOmittedScopeAndRotationAcrossRestart()
    {
        RHOAuthGrant previous; QVERIFY(RHOAuthGrant::parseResponse(response(), "client", "device", now, &previous));
        previous.accountId = "principal";
        auto refreshed = response(); refreshed.remove("scope"); refreshed.remove("refresh_token");
        refreshed["access_token"] = "fresh-access";
        RHOAuthGrant next; QString error;
        QVERIFY2(RHOAuthGrant::parseResponse(refreshed, "client", "device", now.addSecs(2000), &next, &error, &previous), qPrintable(error));
        QCOMPARE(next.response, refreshed); QCOMPARE(next.refreshToken(), previous.refreshToken());
        QCOMPARE(next.effectiveScope, QString("internal")); QCOMPARE(next.accountId, previous.accountId);
        RHOAuthGrant restored; QVERIFY(RHOAuthGrant::deserialize(next.serialize(), &restored));
        QCOMPARE(restored.response, refreshed); QCOMPARE(restored.refreshToken(), previous.refreshToken());
        QVERIFY(RHOAuthGrant::parseResponse(refreshed, "client", "device", now.addSecs(4000), &next, &error, &restored));
        refreshed["refresh_token"] = "rotated-refresh";
        QVERIFY(RHOAuthGrant::parseResponse(refreshed, "client", "device", now.addSecs(6000), &next, &error, &restored));
        QCOMPARE(next.refreshToken(), QByteArray("rotated-refresh"));
        QVERIFY(RHOAuthGrant::deserialize(next.serialize(), &restored));
        QCOMPARE(restored.refreshToken(), next.refreshToken());
    }

    void rejectsMalformedResponses_data()
    {
        QTest::addColumn<QString>("key"); QTest::addColumn<QJsonValue>("value");
        QTest::newRow("error") << QString("error") << QJsonValue("secret-server-error");
        QTest::newRow("mfa") << QString("mfa_required") << QJsonValue(true);
        QTest::newRow("challenge") << QString("challenge") << QJsonValue(QJsonObject{});
        QTest::newRow("missing-expiry") << QString("expires_in") << QJsonValue();
        QTest::newRow("string-expiry") << QString("expires_in") << QJsonValue("1234");
        QTest::newRow("fractional-expiry") << QString("expires_in") << QJsonValue(1.5);
        QTest::newRow("zero-expiry") << QString("expires_in") << QJsonValue(0);
        QTest::newRow("negative-expiry") << QString("expires_in") << QJsonValue(-1);
        QTest::newRow("huge-expiry") << QString("expires_in") << QJsonValue(1e30);
        QTest::newRow("wrong-kind") << QString("token_type") << QJsonValue("WebSecondaryBearer");
        QTest::newRow("readonly") << QString("scope") << QJsonValue("read");
        QTest::newRow("secondary") << QString("scope") << QJsonValue("web_secondary");
        QTest::newRow("scope-absent") << QString("scope") << QJsonValue();
        QTest::newRow("scope-array") << QString("scope") << QJsonValue(QJsonArray{"internal"});
        QTest::newRow("refresh-absent") << QString("refresh_token") << QJsonValue();
        QTest::newRow("refresh-unicode") << QString("refresh_token") << QJsonValue(QString::fromUtf8("réfresh"));
        QTest::newRow("header-injection") << QString("access_token") << QJsonValue("access\r\nsecret");
        QTest::newRow("oversize-token") << QString("access_token") << QJsonValue(QString(16385, 'x'));
        QTest::newRow("client-mismatch") << QString("client_id") << QJsonValue("other");
        QTest::newRow("device-mismatch") << QString("device_token") << QJsonValue("other");
    }
    void rejectsMalformedResponses()
    {
        QFETCH(QString, key); QFETCH(QJsonValue, value);
        auto bad = response(); if (value.isNull()) bad.remove(key); else bad[key] = value;
        RHOAuthGrant grant; QString error;
        QVERIFY(!RHOAuthGrant::parseResponse(bad, "client", "device", now, &grant, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(!error.contains("secret")); QVERIFY(grant.response.isEmpty());
    }
    void strictEnvelopeAndLegacyExtraction()
    {
        QByteArray access;
        QVERIFY(RHOAuthGrant::extractAccessToken("legacy+bearer/=", &access));
        QCOMPARE(access, QByteArray("legacy+bearer/="));
        for (const auto &bad : {QString("{broken"), QString::fromUtf8(QJsonDocument(response()).toJson()), QString("{}"), QString("[1]"), QString(" bearer ")})
            QVERIFY(!RHOAuthGrant::extractAccessToken(bad, &access));
        RHOAuthGrant grant; QVERIFY(RHOAuthGrant::parseResponse(response(), "client", "device", now, &grant));
        auto envelope = QJsonDocument::fromJson(grant.serialize().toUtf8()).object();
        envelope["unexpected"] = true;
        QVERIFY(!RHOAuthGrant::deserialize(QString::fromUtf8(QJsonDocument(envelope).toJson()), &grant));
    }

    void providerSuccessfulFlagsAreNotChallenges()
    {
        auto actual = response(); actual["mfa_required"] = false;
        actual["read_only_secondary_access_token"] = "secondary-only";
        RHOAuthGrant grant; QVERIFY(RHOAuthGrant::parseResponse(actual, "client", "device", now, &grant));
        QVERIFY(grant.primary()); QCOMPARE(grant.accessToken(), QByteArray("access+fixture/="));
        QCOMPARE(grant.response, actual);
    }

    void mutableInvalidValuesCannotBeSerialized()
    {
        RHOAuthGrant grant; QVERIFY(RHOAuthGrant::parseResponse(response(), "client", "device", now, &grant));
        auto mutated = grant; mutated.response["scope"] = "read";
        QVERIFY(!mutated.primary());
        QVERIFY(mutated.serialize().isEmpty());
        mutated = grant; mutated.expiresAt = now.addSecs(10);
        QVERIFY(mutated.serialize().isEmpty());
        mutated = grant; mutated.clientId.clear();
        QVERIFY(mutated.serialize().isEmpty());
    }

    void renewalRejectsReducedScopeAndRebinding()
    {
        RHOAuthGrant previous; QVERIFY(RHOAuthGrant::parseResponse(response(), "client", "device", now, &previous));
        RHOAuthGrant output = previous; QString error;
        auto reduced = response(); reduced["scope"] = "read";
        QVERIFY(!RHOAuthGrant::parseResponse(reduced, "client", "device", now, &output, &error, &previous));
        QCOMPARE(output.serialize(), previous.serialize());
        QVERIFY(!RHOAuthGrant::parseResponse(response(), "other", "device", now, &output, &error, &previous));
        QVERIFY(!RHOAuthGrant::parseResponse(response(), "client", "other", now, &output, &error, &previous));
        auto missing = response(); missing.remove("refresh_token");
        missing["scope"] = QJsonValue();
        QVERIFY(!RHOAuthGrant::parseResponse(missing, "client", "device", now, &output, &error, &previous));
    }

    void secretExpiryRequiresAbsoluteUtcTimes()
    {
        RHOAuthGrant grant; QVERIFY(RHOAuthGrant::parseResponse(response(), "client", "device", now, &grant));
        const auto original = QJsonDocument::fromJson(grant.serialize().toUtf8()).object();
        auto invalid = original;
        invalid["obtained_at"] = now.toString(Qt::ISODateWithMs).chopped(1);
        invalid["expires_at"] = now.addSecs(1234).toString(Qt::ISODateWithMs).chopped(1);
        QVERIFY(!RHOAuthGrant::deserialize(QString::fromUtf8(QJsonDocument(invalid).toJson()), &grant));
    }

    void completeResponseAndContextRoundTrip()
    {
        RHOAuthGrant grant; QString error;
        QVERIFY2(RHOAuthGrant::parseResponse(response(), "original-client", "original-device", now, &grant, &error), qPrintable(error));
        QVERIFY(grant.primary()); QCOMPARE(grant.response, response());
        QCOMPARE(grant.accessToken(), QByteArray("access+fixture/="));
        QCOMPARE(grant.refreshToken(), QByteArray("refresh+fixture/="));
        QCOMPARE(grant.expiresAt, now.addSecs(1234));
        grant.accountId = "principal-id";
        RHOAuthGrant restored;
        QVERIFY(RHOAuthGrant::deserialize(grant.serialize(), &restored, &error));
        QCOMPARE(restored.response, grant.response); QCOMPARE(restored.accountId, grant.accountId);
        QCOMPARE(restored.clientId, grant.clientId); QCOMPARE(restored.deviceToken, grant.deviceToken);
        QCOMPARE(restored.expiresAt, grant.expiresAt); QCOMPARE(restored.obtainedAt, now);
        QByteArray access;
        QVERIFY(RHOAuthGrant::extractAccessToken(grant.serialize(), &access));
        QCOMPARE(access, grant.accessToken());
    }
};
QTEST_GUILESS_MAIN(RHOAuthGrantTest)
#include "test_rhoauthgrant.moc"
