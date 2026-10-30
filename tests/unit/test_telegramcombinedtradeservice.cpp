#include "telegramcombinedtradeservice.h"
#include <QtTest>
#include <QSignalSpy>
#include <memory>

namespace {
using Service = TelegramCombinedTradeService;
struct Stage {
    QObject lifetime;
    struct Call { QString id; QStringList symbols; QString side; Service::Completion complete; };
    QList<Call> calls;
    bool accepted = true;
    bool running = false;
    Service::Stage dependency() {
        return {&lifetime, [this](QString id, QStringList symbols, QString side, Service::Completion complete) {
            calls.append({id, symbols, side, complete});
            return accepted;
        }, [this] { return running; }};
    }
    void finish(QStringList report) { const auto call = calls.last(); call.complete(call.id, report); }
};
}

class TelegramCombinedTradesTest : public QObject {
    Q_OBJECT
private slots:
    void firstTerminalReportWinsAcrossNestedSubmitEvents() {
        QObject lifetime;
        Service::Stage publics{&lifetime, [](QString id, QStringList, QString, Service::Completion complete) {
            complete(id, {"first trusted report"});
            QCoreApplication::processEvents();
            complete(id, {"duplicate must not replace first"});
            return true;
        }, {}};
        Service service({publics, {}});
        QSignalSpy reports(&service, &Service::reportReady);
        QVERIFY(service.submit("root", {"AAPL"}, "BUY"));
        QTRY_COMPARE(reports.size(), 1);
        QCOMPARE(reports.first().at(1).toStringList(), QStringList{"first trusted report"});
        QCoreApplication::processEvents(); QCOMPARE(reports.size(), 1);
    }

    void submissionMustReturnBeforeAnyTerminalReport() {
        QObject lifetime;
        bool insideSubmit = false;
        Service::Stage publics{&lifetime, [&](QString id, QStringList, QString, Service::Completion complete) {
            insideSubmit = true;
            complete(id, {"unaccepted inline result"});
            QCoreApplication::processEvents();
            insideSubmit = false;
            return false;
        }, {}};
        Service service({publics, {}});
        bool reportedDuringSubmit = false;
        connect(&service, &Service::reportReady, this, [&](QString, QStringList) {
            reportedDuringSubmit = insideSubmit;
        });
        QSignalSpy reports(&service, &Service::reportReady);
        QVERIFY(service.submit("root", {"AAPL"}, "BUY"));
        QTRY_COMPARE(reports.size(), 1);
        QVERIFY(!reportedDuringSubmit);
        QVERIFY(reports.first().at(1).toStringList().first().contains("Publics trade service unavailable"));
    }

    void refusedAdmissionCannotPublishAnInlineCompletion() {
        QObject lifetime;
        Stage robinhood;
        Service::Stage publics{&lifetime, [](QString id, QStringList, QString, Service::Completion complete) {
            complete(id, {"This callback does not establish accepted admission."});
            return false;
        }, {}};
        Service service({publics, robinhood.dependency()});
        QSignalSpy reports(&service, &Service::reportReady);
        QSignalSpy done(&service, &Service::completed);
        QVERIFY(service.submit("root", {"AAPL"}, "BUY"));
        QTRY_COMPARE(reports.size(), 1);
        QCOMPARE(reports.first().at(1).toStringList(), QStringList{
            "<b>Publics Auto Buy Results</b>\n\n🔴 Publics trade service unavailable; outcome could not be confirmed. Check Publics before submitting again."});
        QCOMPARE(robinhood.calls.size(), 0);
        service.continueAfterReport("root");
        QTRY_COMPARE(robinhood.calls.size(), 1);
        robinhood.finish({"r"}); QTRY_COMPARE(done.size(), 1);
    }

    void synchronousStageCompletionStillWaitsForDeliverySettlement() {
        QObject publicsLifetime, robinhoodLifetime;
        QStringList order;
        Service::Stage publics{&publicsLifetime, [&](QString id, QStringList, QString, Service::Completion done) {
            order.append("publics"); done(id, {"<b>trusted p</b>", "p tail"}); return true;
        }, {}};
        Service::Stage robinhood{&robinhoodLifetime, [&](QString id, QStringList, QString, Service::Completion done) {
            order.append("robinhood"); done(id, {"<b>trusted r</b>", "r tail"}); return true;
        }, {}};
        Service service({publics, robinhood});
        QSignalSpy completed(&service, &Service::completed);
        connect(&service, &Service::reportReady, this, [&](QString root, QStringList report) {
            QCOMPARE(report, (QStringList{"<b>trusted p</b>", "p tail"}));
            order.append("report");
            QCoreApplication::processEvents(); // Simulate bounded send attempts, including nested event delivery.
            QCOMPARE(order, (QStringList{"publics", "report"}));
            QVERIFY(service.busy());
            order.append("send attempts settled");
            service.continueAfterReport(root);
            service.continueAfterReport(root);
        });
        QVERIFY(service.submit("root", {"AAPL"}, "BUY"));
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(order, (QStringList{"publics", "report", "send attempts settled", "robinhood"}));
        QCOMPARE(completed.first().at(1).toStringList(), (QStringList{"<b>trusted r</b>", "r tail"}));
        QVERIFY(!service.busy());
    }

    void completedObserverCanStartReplacementWithoutOldStateCleanup() {
        Stage publics, robinhood;
        Service service({publics.dependency(), robinhood.dependency()});
        QSignalSpy done(&service, &Service::completed);
        connect(&service, &Service::reportReady, this, [&](QString id, QStringList) { service.continueAfterReport(id); });
        connect(&service, &Service::completed, this, [&](QString id, QStringList) {
            if (id == "old") QVERIFY(service.submit("new", {"MSFT"}, "SELL"));
        });
        QVERIFY(service.submit("old", {"AAPL"}, "BUY"));
        QTRY_COMPARE(publics.calls.size(), 1); publics.finish({"p"});
        QTRY_COMPARE(robinhood.calls.size(), 1); robinhood.finish({"r"});
        QTRY_COMPARE(publics.calls.size(), 2);
        QVERIFY(service.busy()); QCOMPARE(done.size(), 1);
        QCOMPARE(publics.calls.last().symbols, QStringList{"MSFT"});
        publics.finish({"p2"}); QTRY_COMPARE(robinhood.calls.size(), 2); robinhood.finish({"r2"});
        QTRY_COMPARE(done.size(), 2); QCOMPARE(done.last().at(0).toString(), "new");
    }

    void completeListIsPreservedBeyondStandaloneBounds() {
        Stage publics, robinhood;
        Service service({publics.dependency(), robinhood.dependency()});
        QStringList symbols;
        for (int i = 0; i < 100; ++i)
            symbols.append(QString(20, QLatin1Char('A')) + QChar('A' + i / 26) + QChar('A' + i % 26));
        symbols.append("msft"); symbols.append("msft");
        QVERIFY(service.submit("long", symbols, "BUY"));
        symbols.clear(); // The admitted snapshot must own its data.
        QTRY_COMPARE(publics.calls.size(), 1);
        QCOMPARE(publics.calls.first().symbols.size(), 102);
        QVERIFY(publics.calls.first().symbols.join(',').size() > 1024);
        publics.finish({"Publics"});
        QSignalSpy reports(&service, &Service::reportReady);
        QTRY_COMPARE(reports.size(), 1);
        service.continueAfterReport("long");
        QTRY_COMPARE(robinhood.calls.size(), 1);
        QCOMPARE(robinhood.calls.first().symbols, publics.calls.first().symbols);
        QCOMPARE(robinhood.calls.first().side, "BUY");
    }

    void staleAndDuplicateCallbacksCannotAdvanceOrRetireJobs() {
        Stage publics, robinhood;
        Service service({publics.dependency(), robinhood.dependency()});
        QSignalSpy reports(&service, &Service::reportReady);
        QSignalSpy done(&service, &Service::completed);
        QVERIFY(service.submit("first", {"AAPL"}, "BUY"));
        QTRY_COMPARE(publics.calls.size(), 1);
        auto first = publics.calls.first();
        service.continueAfterReport("first"); // Too early, not a delivery acknowledgement.
        first.complete("first", {"wrong root"});
        first.complete("unrelated", {"standalone"});
        QCoreApplication::processEvents();
        QCOMPARE(reports.size(), 0); QCOMPARE(robinhood.calls.size(), 0);
        first.complete(first.id, {"trusted"});
        first.complete(first.id, {"duplicate"});
        QTRY_COMPARE(reports.size(), 1);
        service.continueAfterReport("first"); service.continueAfterReport("first");
        QTRY_COMPARE(robinhood.calls.size(), 1);
        auto last = robinhood.calls.first();
        last.complete(first.id, {"wrong stage"});
        last.complete(last.id, {"final"}); last.complete(last.id, {"duplicate"});
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(service.submit("second", {"MSFT"}, "SELL"));
        QTRY_COMPARE(publics.calls.size(), 2);
        first.complete(first.id, {"stale Publics"});
        last.complete(last.id, {"stale Robinhood"});
        service.continueAfterReport("first");
        QCoreApplication::processEvents();
        QCOMPARE(reports.size(), 1); QCOMPARE(done.size(), 1);
        QVERIFY(service.busy()); QCOMPARE(robinhood.calls.size(), 1);
        QVERIFY(first.id != publics.calls.last().id);
    }

    void missingAndRefusedStagesHaveFixedSafeReports_data() {
        QTest::addColumn<bool>("missing");
        QTest::newRow("missing") << true;
        QTest::newRow("refused") << false;
    }
    void missingAndRefusedStagesHaveFixedSafeReports() {
        QFETCH(bool, missing);
        Stage publics, robinhood;
        publics.accepted = false; robinhood.accepted = false;
        Service service(missing ? Service::Dependencies{} : Service::Dependencies{publics.dependency(), robinhood.dependency()});
        QSignalSpy reports(&service, &Service::reportReady);
        QSignalSpy done(&service, &Service::completed);
        QVERIFY(service.submit("root", {"AAPL"}, "SELL"));
        QTRY_COMPARE(reports.size(), 1);
        QCOMPARE(reports.first().at(1).toStringList(), QStringList{
            "<b>Publics Auto Sell Results</b>\n\n🔴 Publics trade service unavailable; outcome could not be confirmed. Check Publics before submitting again."});
        QCOMPARE(done.size(), 0); QCOMPARE(robinhood.calls.size(), 0);
        QVERIFY(service.busy());
        service.continueAfterReport("root");
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.first().at(1).toStringList(), QStringList{
            "<b>Robinhood Auto Sell Results</b>\n\n🔴 Robinhood trade service unavailable; outcome could not be confirmed. Check Robinhood before submitting again."});
        QCOMPARE(publics.calls.size(), missing ? 0 : 1);
        QCOMPARE(robinhood.calls.size(), missing ? 0 : 1);
        QVERIFY(!service.busy());
    }

    void destroyedStageStillCrossesReportBoundary() {
        auto publics = std::make_unique<Stage>();
        auto robinhood = std::make_unique<Stage>();
        Service service({publics->dependency(), robinhood->dependency()});
        QSignalSpy reports(&service, &Service::reportReady);
        QSignalSpy done(&service, &Service::completed);
        QVERIFY(service.submit("root", {"AAPL"}, "BUY"));
        QTRY_COMPARE(publics->calls.size(), 1);
        const auto late = publics->calls.first();
        publics.reset();
        QTRY_COMPARE(reports.size(), 1);
        late.complete(late.id, {"late"});
        QCoreApplication::processEvents();
        QCOMPARE(reports.size(), 1); QCOMPARE(robinhood->calls.size(), 0);
        service.continueAfterReport("root");
        QTRY_COMPARE(robinhood->calls.size(), 1);
        robinhood.reset();
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done.first().at(1).toStringList().first().contains("Robinhood trade service unavailable"));
    }

    void rejectsMalformedInputWithoutBrokerAdmission() {
        Stage publics, robinhood;
        Service service({publics.dependency(), robinhood.dependency()});
        QVERIFY(!service.submit("", {"AAPL"}, "BUY"));
        QVERIFY(!service.submit("root", {}, "BUY"));
        QVERIFY(!service.submit("root", {"AAPL"}, "SHORT"));
        for (const auto &symbol : QStringList{"BRK.B", "A1", "A B", "ß", "", " AAPL", "AAPL\n"})
            QVERIFY(!service.submit("root", {symbol}, "BUY"));
        QCoreApplication::processEvents();
        QVERIFY(!service.busy()); QCOMPARE(publics.calls.size(), 0); QCOMPARE(robinhood.calls.size(), 0);
    }

    void directObserversCanDeleteAtEveryNotification_data() {
        QTest::addColumn<QString>("notification");
        for (const auto &name : {"admission", "report", "idle", "completed"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void directObserversCanDeleteAtEveryNotification() {
        QFETCH(QString, notification);
        Stage publics, robinhood;
        QPointer<Service> service = new Service({publics.dependency(), robinhood.dependency()});
        if (notification == "admission" || notification == "idle")
            connect(service, &Service::busyChanged, this, [&] {
                if (service && ((notification == "admission") == service->busy())) delete service.data();
            });
        if (notification == "report")
            connect(service, &Service::reportReady, this, [&] { delete service.data(); });
        if (notification == "completed")
            connect(service, &Service::completed, this, [&] { delete service.data(); });
        QVERIFY(service->submit("root", {"AAPL"}, "BUY"));
        if (notification == "admission") {
            QVERIFY(!service); QCoreApplication::processEvents(); QCOMPARE(publics.calls.size(), 0); return;
        }
        QTRY_COMPARE(publics.calls.size(), 1);
        publics.finish({"Publics"});
        if (notification == "report") {
            QTRY_VERIFY(!service); QCOMPARE(robinhood.calls.size(), 0); return;
        }
        QTRY_VERIFY(service && service->busy());
        QCoreApplication::processEvents();
        service->continueAfterReport("root");
        QTRY_COMPARE(robinhood.calls.size(), 1);
        robinhood.finish({"Robinhood"});
        QTRY_VERIFY(!service);
    }

    void idleObserverCanImmediatelySubmitReplacement() {
        Stage publics, robinhood;
        Service service({publics.dependency(), robinhood.dependency()});
        QSignalSpy done(&service, &Service::completed);
        bool replaced = false;
        connect(&service, &Service::busyChanged, this, [&] {
            if (!service.busy() && !replaced) {
                replaced = true;
                QVERIFY(service.submit("replacement", {"MSFT"}, "SELL"));
            }
        });
        connect(&service, &Service::reportReady, this, [&](QString id, QStringList) { service.continueAfterReport(id); });
        QVERIFY(service.submit("original", {"AAPL"}, "BUY"));
        QTRY_COMPARE(publics.calls.size(), 1); publics.finish({"p"});
        QTRY_COMPARE(robinhood.calls.size(), 1); robinhood.finish({"r"});
        QTRY_COMPARE(done.size(), 1); QTRY_COMPARE(publics.calls.size(), 2);
        QCOMPARE(done.first().at(0).toString(), "original");
        QVERIFY(service.busy()); QCOMPARE(publics.calls.last().symbols, QStringList{"MSFT"});
        publics.finish({"p2"}); QTRY_COMPARE(robinhood.calls.size(), 2); robinhood.finish({"r2"});
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(done.last().at(0).toString(), "replacement"); QVERIFY(!service.busy());
    }

    void deletingCoordinatorDoesNotCancelOrRestartBroker() {
        Stage publics, robinhood;
        auto service = std::make_unique<Service>(Service::Dependencies{publics.dependency(), robinhood.dependency()});
        QVERIFY(service->submit("root", {"AAPL"}, "BUY"));
        QTRY_COMPARE(publics.calls.size(), 1);
        publics.running = true;
        const auto late = publics.calls.first();
        service.reset();
        Service replacement({publics.dependency(), robinhood.dependency()});
        QVERIFY(replacement.busy());
        QVERIFY(!replacement.submit("replacement", {"MSFT"}, "BUY"));
        late.complete(late.id, {"late"}); QCoreApplication::processEvents();
        QCOMPARE(publics.calls.size(), 1); QCOMPARE(robinhood.calls.size(), 0);
        publics.running = false; QVERIFY(!replacement.busy());
    }

    void waitsForPublicsReportBeforeRobinhood() {
        Stage publics, robinhood;
        Service service({publics.dependency(), robinhood.dependency()});
        QSignalSpy reports(&service, &Service::reportReady);
        QSignalSpy done(&service, &Service::completed);
        QSignalSpy busy(&service, &Service::busyChanged);
        const QStringList symbols{QStringLiteral("AAPL"), QStringLiteral("MSFT")};
        QVERIFY(service.submit("root", symbols, "SELL"));
        QVERIFY(service.busy());
        QTRY_COMPARE(publics.calls.size(), 1);
        QCOMPARE(robinhood.calls.size(), 0);
        QVERIFY(publics.calls.first().id != "root");
        publics.finish({"<b>Publics failure</b>"});
        QTRY_COMPARE(reports.size(), 1);
        QCOMPARE(reports.first().at(0).toString(), "root");
        QCOMPARE(reports.first().at(1).toStringList(), QStringList{"<b>Publics failure</b>"});
        QVERIFY(service.busy());
        QVERIFY(!service.submit("other", symbols, "BUY"));
        QCOMPARE(robinhood.calls.size(), 0);
        QCOMPARE(done.size(), 0);
        service.continueAfterReport("wrong");
        QCoreApplication::processEvents();
        QCOMPARE(robinhood.calls.size(), 0);
        service.continueAfterReport("root");
        service.continueAfterReport("root");
        QTRY_COMPARE(robinhood.calls.size(), 1);
        const auto &p = publics.calls.first(); const auto &r = robinhood.calls.first();
        QVERIFY(p.id != r.id); QVERIFY(r.id != "root");
        QCOMPARE(p.symbols, symbols); QCOMPARE(r.symbols, symbols);
        QCOMPARE(p.side, "SELL"); QCOMPARE(r.side, p.side);
        const QString reauth = "Robinhood Re-authentication required before trades can be completed.";
        robinhood.finish({reauth});
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.first().at(1).toStringList(), QStringList{reauth});
        QVERIFY(!service.busy());
        QCOMPARE(busy.size(), 2);
    }
};
QTEST_GUILESS_MAIN(TelegramCombinedTradesTest)
#include "test_telegramcombinedtradeservice.moc"
