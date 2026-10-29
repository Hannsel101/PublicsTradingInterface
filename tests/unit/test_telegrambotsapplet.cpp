#include <QtTest/QtTest>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <memory>

#include "launchercontroller.h"
#include "telegrambotsapplet.h"
#include "telegrambotscontroller.h"
#include "telegramcontrollertestserver.h"

Q_IMPORT_QML_PLUGIN(TelegramBotsInterfacePlugin)

class AppletMemoryBotStore final : public TelegramBotStore
{
public:
    void load(LoadCompletion done) override { done(true, {{QStringLiteral("one"), QStringLiteral("Fixture")}}, {}); }
    void add(QString, QString, AddCompletion done) override { done(false, {}, QStringLiteral("Fixture read-only")); }
    void readToken(QString, TokenCompletion done) override { done(true, QStringLiteral("123456:synthetic"), {}); }
};

class TelegramBotsAppletTest final : public QObject
{
    Q_OBJECT
private slots:
    void consoleLoadsWithoutAnyBrokerageModule()
    {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine);
        component.loadFromModule("TelegramBotsInterface", "CoreView");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        auto *view = qobject_cast<QQuickItem *>(object.get());
        QVERIFY2(view, qPrintable(component.errorString()));
        view->setSize(QSizeF(360, 568));
        auto *heading = view->findChild<QObject *>(QStringLiteral("telegramConsoleTitle"));
        QVERIFY(heading);
        QCOMPARE(heading->property("text").toString(), QStringLiteral("Telegram Bot Console"));
        QCOMPARE(warnings.size(), 0);
    }

    void hasIndependentIdentityAndNoBrokerageContext()
    {
        QQmlApplicationEngine engine;
        AppletMemoryBotStore store;
        TelegramBotsController controller(&store);
        TelegramBotsApplet telegram(&controller);
        LauncherController launcher(engine, {&telegram});
        QCOMPARE(telegram.id(), QStringLiteral("telegram_bots"));
        QVERIFY(launcher.activate(telegram.id()));
        QCOMPARE(launcher.activeAppletId(), telegram.id());
        QCOMPARE(engine.rootContext()->contextProperty(QStringLiteral("TelegramBots")).value<QObject *>(), &controller);
        QVERIFY(launcher.canReturnHome());
        for (const auto *name : {"AuthClient", "ApiWorker", "AutoTrader", "RobinSessions", "RobinTrades"})
            QVERIFY(!engine.rootContext()->contextProperty(QString::fromLatin1(name)).isValid());
        QVERIFY(launcher.returnHome());
        QCOMPARE(launcher.activeAppletId(), QString());
        QVERIFY(launcher.activate(telegram.id()));
        QVERIFY(launcher.returnHome());
    }

    void consoleActionsBindToProductionController()
    {
        QQmlApplicationEngine engine;
        ControllerBotServer server;
        AppletMemoryBotStore store;
        TelegramBotsController controller(&store, [&server](QObject *owner) {
            return new TelegramBotRuntime(server.root(), owner);
        });
        TelegramBotsApplet telegram(&controller);
        telegram.initialize(engine);
        telegram.start();
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine);
        component.loadFromModule("TelegramBotsInterface", "CoreView");
        std::unique_ptr<QObject> view(component.create());
        QVERIFY2(view, qPrintable(component.errorString()));
        auto *start = view->findChild<QObject *>(QStringLiteral("startBot"));
        auto *stop = view->findChild<QObject *>(QStringLiteral("stopBot"));
        QVERIFY(start && stop);
        QVERIFY(!start->property("enabled").toBool());
        QVERIFY(!stop->property("enabled").toBool());
        controller.selectBot(QStringLiteral("one"));
        QTRY_VERIFY(start->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(start, "clicked"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.bots().first().toMap().value(QStringLiteral("active")).toBool(), 1500);
        QTRY_VERIFY(stop->property("enabled").toBool());
        QVERIFY(!start->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(stop, "clicked"));
        QVERIFY(!controller.hasRunningBots());
        QTRY_VERIFY(start->property("enabled").toBool());
        QCOMPARE(warnings.size(), 0);
    }

    void navigationCannotLeaveRunningBots()
    {
        QQmlApplicationEngine engine;
        ControllerBotServer server;
        AppletMemoryBotStore store;
        TelegramBotsController controller(&store, [&server](QObject *owner) {
            return new TelegramBotRuntime(server.root(), owner);
        });
        TelegramBotsApplet telegram(&controller);
        LauncherController launcher(engine, {&telegram});
        QVERIFY(launcher.activate(telegram.id()));
        controller.selectBot(QStringLiteral("one"));
        QVERIFY(controller.startBot());
        QTRY_VERIFY_WITH_TIMEOUT(controller.bots().first().toMap().value(QStringLiteral("active")).toBool(), 1500);
        QVERIFY(!launcher.canReturnHome());
        QVERIFY(!launcher.returnHome());
        QCOMPARE(launcher.activeAppletId(), telegram.id());
        QVERIFY(controller.stopAll());
        QVERIFY(launcher.returnHome());
        QVERIFY(launcher.activeAppletId().isEmpty());
    }
};

QTEST_MAIN(TelegramBotsAppletTest)
#include "test_telegrambotsapplet.moc"
