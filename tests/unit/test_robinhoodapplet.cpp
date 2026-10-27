#include "robinhoodapplet.h"
#include "rhsessionmanager.h"

#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QStandardPaths>
#include <QtTest/QtTest>

class RobinHoodAppletTest : public QObject
{
    Q_OBJECT
private slots:
    void packagedConfigurationAndLifecycle()
    {
        QStandardPaths::setTestModeEnabled(true);
        QQmlApplicationEngine engine;
        RobinHoodApplet applet;
        QCOMPARE(applet.id(), QStringLiteral("robin_hood"));
        QVERIFY(QFile::exists(QStringLiteral(":/robinhood/config/robinhood-api.json")));
        QFile notice(QStringLiteral(":/robinhood/third-party/robin-stocks-LICENSE.txt"));
        QVERIFY(notice.open(QIODevice::ReadOnly));
        QVERIFY(notice.readAll().contains("Copyright (c) 2018 Joshua M. Fernandes"));
        applet.initialize(engine);
        auto *sessions = qobject_cast<RHSessionManager *>(
            engine.rootContext()->contextProperty(QStringLiteral("RobinSessions")).value<QObject *>());
        QVERIFY(sessions);
        // Production authentication must not expose or instantiate a browser route.
        QVERIFY(!engine.rootContext()->contextProperty(QStringLiteral("RobinBrowser")).value<QObject *>());
        QVERIFY(sessions->metaObject()->indexOfProperty("nativeLoginPending") >= 0);
        QVERIFY(sessions->metaObject()->indexOfProperty("nativeLoginStatus") >= 0);
        QVERIFY(sessions->metaObject()->indexOfProperty("password") < 0);
        QVERIFY(sessions->metaObject()->indexOfProperty("token") < 0);
        auto *trades = engine.rootContext()->contextProperty(QStringLiteral("RobinTrades")).value<QObject *>();
        QVERIFY(trades);
        QVERIFY(trades->metaObject()->indexOfProperty("tradeResults") >= 0);
        QVERIFY(trades->metaObject()->indexOfMethod("submit(QString,QString)") >= 0);
        QVERIFY(trades->metaObject()->indexOfProperty("token") < 0);
        QVERIFY(trades->metaObject()->indexOfProperty("password") < 0);
        QVERIFY(!trades->property("available").toBool());
        QVERIFY(!trades->property("unavailableReason").toString().isEmpty());
        QCOMPARE(trades->property("liveOrdersEnabled").toBool(), bool(PUBLICS_RH_EXPECT_LIVE));
        QVERIFY(!sessions->browserLoginPending());
        applet.start();
        QVERIFY(applet.canStop());
        QVERIFY(!sessions->sessionActive());
        bool opened = false;
        QVERIFY(QMetaObject::invokeMethod(sessions, "beginNativeLogin", Qt::DirectConnection,
            Q_RETURN_ARG(bool, opened), Q_ARG(QString, QString())));
        QVERIFY(opened);
        QVERIFY(sessions->property("nativeLoginPending").toBool());
        QVERIFY(applet.canStop());
        const auto source = QStringLiteral(PUBLICS_RH_NATIVE_FORM_SOURCE);
        QVERIFY(!source.isEmpty());
        QQmlComponent component(&engine, QUrl::fromLocalFile(source));
        QScopedPointer<QObject> form(component.create());
        QVERIFY2(form, qPrintable(component.errorString()));
        auto *email = form->findChild<QObject *>(QStringLiteral("robinNativeEmail"));
        auto *password = form->findChild<QObject *>(QStringLiteral("robinNativePassword"));
        QVERIFY(email && password);
        QVERIFY(sessions->busy()); // Admission stays reserved; form entry must still work.
        QVERIFY(email->property("enabled").toBool());
        QVERIFY(password->property("enabled").toBool());
        sessions->cancelSignIn();
        QVERIFY(!sessions->property("nativeLoginPending").toBool());
        QVERIFY(!sessions->busy());
        applet.stop();
    }
};

QTEST_MAIN(RobinHoodAppletTest)
#include "test_robinhoodapplet.moc"
