#include "robinhoodapplet.h"
#include "rhsessionmanager.h"

#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
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
        applet.initialize(engine);
        auto *sessions = qobject_cast<RHSessionManager *>(
            engine.rootContext()->contextProperty(QStringLiteral("RobinSessions")).value<QObject *>());
        QVERIFY(sessions);
        applet.start();
        QVERIFY(applet.canStop());
        QVERIFY(!sessions->sessionActive());
        applet.stop();
    }
};

QTEST_MAIN(RobinHoodAppletTest)
#include "test_robinhoodapplet.moc"
