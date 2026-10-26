#include <QtTest/QtTest>
#include <QQmlApplicationEngine>

#include "launchercontroller.h"

class FakeApplet final : public AppletInterface
{
    Q_OBJECT
public:
    explicit FakeApplet(const QString &name) : m_name(name) {}
    QString id() const override { return m_name; }
    void initialize(QQmlApplicationEngine &) override { ++initialized; }
    void start() override { ++started; }
    bool canStop() const override { return stoppable; }
    void stop() override { ++stopped; }

    void setStoppable(bool value)
    {
        stoppable = value;
        emit navigationAvailabilityChanged();
    }

    int initialized = 0;
    int started = 0;
    int stopped = 0;
    bool stoppable = true;
private:
    QString m_name;
};

class LauncherTest : public QObject
{
    Q_OBJECT
private slots:
    void createsOnlySelectedAppletAndStopsBeforeReturningHome()
    {
        QQmlApplicationEngine engine;
        FakeApplet trading(QStringLiteral("publics"));
        FakeApplet other(QStringLiteral("future"));
        LauncherController launcher(engine, {&trading, &other});
        QSignalSpy navigation(&launcher, &LauncherController::canReturnHomeChanged);

        QCOMPARE(launcher.activeAppletId(), QString());
        QCOMPARE(trading.initialized, 0);
        QCOMPARE(other.initialized, 0);
        QVERIFY(!launcher.activate(QStringLiteral("unknown")));
        QVERIFY(launcher.activate(QStringLiteral("publics")));
        QCOMPARE(launcher.activeAppletId(), QStringLiteral("publics"));
        QCOMPARE(trading.initialized, 1);
        QCOMPARE(trading.started, 1);
        QCOMPARE(other.initialized, 0);
        QVERIFY(!launcher.activate(QStringLiteral("future")));
        trading.setStoppable(false);
        QVERIFY(!launcher.canReturnHome());
        QVERIFY(!launcher.returnHome());
        QCOMPARE(trading.stopped, 0);
        trading.setStoppable(true);
        QVERIFY(launcher.canReturnHome());
        QVERIFY(!navigation.isEmpty());
        QVERIFY(launcher.returnHome());
        QCOMPARE(trading.stopped, 1);
        QCOMPARE(launcher.activeAppletId(), QString());
        QVERIFY(launcher.activate(QStringLiteral("publics")));
        QCOMPARE(trading.initialized, 1);
        QCOMPARE(trading.started, 2);
        QVERIFY(launcher.returnHome());
        QVERIFY(launcher.activate(QStringLiteral("future")));
        QCOMPARE(other.initialized, 1);
    }
};

QTEST_MAIN(LauncherTest)
#include "test_launcher.moc"
