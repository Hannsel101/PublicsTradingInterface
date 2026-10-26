#include <QtTest/QtTest>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "publicsapplet.h"

class PublicsAppletTest : public QObject
{
    Q_OBJECT
private slots:
    void stopClearsOnlyTheAppletSession()
    {
        QQmlApplicationEngine engine;
        PublicsApplet applet;
        QCOMPARE(applet.id(), QStringLiteral("publics"));
        applet.initialize(engine);

        auto *auth = engine.rootContext()->contextProperty(QStringLiteral("AuthClient")).value<QObject *>();
        QVERIFY(auth != nullptr);
        QVERIFY(engine.rootContext()->contextProperty(QStringLiteral("ApiWorker")).value<QObject *>());
        QVERIFY(engine.rootContext()->contextProperty(QStringLiteral("AutoTrader")).value<QObject *>());
        QVERIFY(applet.canStop());

        QVERIFY(auth->setProperty("sessionActive", true));
        QVERIFY(auth->property("sessionActive").toBool());
        applet.stop();
        QVERIFY(!auth->property("sessionActive").toBool());
        QVERIFY(auth->property("secretKey").toString().isEmpty());
    }
};

QTEST_MAIN(PublicsAppletTest)
#include "test_publicsapplet.moc"
