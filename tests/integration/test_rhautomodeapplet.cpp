#include "rhautotradecontroller.h"
#include "telegramrobinhoodfixture.h"
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>
#include <memory>

Q_IMPORT_QML_PLUGIN(RobinHoodInterfacePlugin)

class RHAutoModeAppletTest final : public QObject
{
    Q_OBJECT
private slots:
    void qmlActionsUseNativeAccountMajorOrders_data()
    {
        QTest::addColumn<QString>("side");
        QTest::newRow("buy") << QStringLiteral("buy");
        QTest::newRow("sell") << QStringLiteral("sell");
    }
    void qmlActionsUseNativeAccountMajorOrders()
    {
        QFETCH(QString, side);
        TelegramRobinhoodFixture::Fixture fixture;
        QVERIFY(fixture.ready);
        QVERIFY(fixture.sessions->saveAuthenticatedSession("Fixture profile", "fixture-primary", 3600));
        QTRY_VERIFY(!fixture.sessions->busy());
        RHAutoTradeController autoTrades(fixture.network, *fixture.sessions, fixture.dependencies());
        auto manualDependencies = fixture.dependencies();
        const auto availability = manualDependencies.availabilityReason;
        manualDependencies.availabilityReason = [&] {
            return autoTrades.busy() ? QStringLiteral("Auto Mode running") : availability();
        };
        RHTradeController manual(fixture.network, std::move(manualDependencies));
        connect(&autoTrades, &RHAutoTradeController::stateChanged, &manual, &RHTradeController::refreshAvailability);
        connect(&manual, &RHTradeController::tradeResultsBusyChanged, &autoTrades,
            [&] { autoTrades.setExternalBusy(manual.tradeResultsBusy()); });
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("RobinSessions", fixture.sessions.get());
        engine.rootContext()->setContextProperty("RobinTrades", &manual);
        engine.rootContext()->setContextProperty("RobinAutoTrades", &autoTrades);
        QQuickWindow window;
        window.resize(1000, 800);
        QQmlComponent component(&engine);
        component.loadFromModule("RobinHoodInterface", "CoreView");
        std::unique_ptr<QObject> view(component.create());
        QVERIFY2(view, qPrintable(component.errorString()));
        auto *item = qobject_cast<QQuickItem *>(view.get());
        QVERIFY(item);
        item->setParentItem(window.contentItem());
        item->setSize(QSizeF(1000, 800));
        window.show();
        auto *open = view->findChild<QObject *>("robinAutoMode");
        QVERIFY(open);
        QVERIFY(open->property("enabled").toBool());
        const auto beforeOpen = fixture.requests.size();
        QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
        QTRY_VERIFY(view->findChild<QObject *>("robinAutoModeView"));
        QCOMPARE(fixture.requests.size(), beforeOpen); // Opening is presentation-only.
        auto *input = view->findChild<QObject *>("robinAutoTickers");
        auto *buy = view->findChild<QObject *>("robinAutoBuy");
        auto *sell = view->findChild<QObject *>("robinAutoSell");
        QVERIFY(input && buy && sell);
        QVERIFY(input->setProperty("text", "aapl, mSfT"));
        auto *action = side == "buy" ? buy : sell;
        QTRY_VERIFY(action->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(action, "clicked"));
        for (int i = 0; i < 6; ++i) {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.timers.size(), 1, 5000);
            QCOMPARE(fixture.delays.at(i), 2500);
            QVERIFY(autoTrades.busy());
            QVERIFY(!buy->property("enabled").toBool());
            QVERIFY(!sell->property("enabled").toBool());
            QVERIFY(!manual.available());
            QVERIFY(view->findChild<QObject *>("robinAutoModeView"));
            auto elapsed = fixture.timers.takeFirst();
            elapsed();
        }
        QTRY_VERIFY(!autoTrades.busy());
        QVERIFY(autoTrades.tradeResultsComplete());
        const auto rows = autoTrades.tradeResults();
        QCOMPARE(rows.size(), 6);
        const QStringList symbols{"AAPL", "MSFT"};
        for (int i = 0; i < rows.size(); ++i) {
            const auto row = rows.at(i).toMap();
            QCOMPARE(row.value("symbol").toString(), symbols.at(i % 2));
            QCOMPARE(row.value("accountNumber").toString(), QString::number(i / 2 + 1));
            QVERIFY(!row.value("sessionName").toString().isEmpty());
        }
        if (side == "buy") {
            QCOMPARE(fixture.orders.size(), 6);
            for (int i = 0; i < fixture.orders.size(); ++i) {
                const auto order = fixture.orders.at(i);
                QCOMPARE(order.value("account").toString(), fixture.brokerageOrigin
                    + QStringLiteral("/accounts/a%1/").arg(i / 2 + 1));
                QCOMPARE(order.value("instrument").toString(), fixture.brokerageOrigin
                    + "/instruments/" + symbols.at(i % 2) + "/");
                QCOMPARE(order.value("quantity").toString(), QStringLiteral("1"));
                QVERIFY(rows.at(i).toMap().value("success").toBool());
            }
        } else {
            QCOMPARE(fixture.orders.size(), 2);
            QCOMPARE(fixture.orders.at(0).value("quantity").toString(), QStringLiteral("5.5"));
            QCOMPARE(fixture.orders.at(1).value("quantity").toString(), QStringLiteral("2"));
            for (const auto &order : fixture.orders)
                QCOMPARE(order.value("side").toString(), QStringLiteral("sell"));
        }
    }
};

QTEST_MAIN(RHAutoModeAppletTest)
#include "test_rhautomodeapplet.moc"
