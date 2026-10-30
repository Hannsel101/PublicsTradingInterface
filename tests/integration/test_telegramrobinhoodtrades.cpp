#include "telegramrobinhoodfixture.h"
using TelegramRobinhoodFixture::Fixture;

#include "telegramrobinhoodtradeservice.h"
class TelegramRobinhoodTradesTest : public QObject {
 Q_OBJECT
private slots:
 void readAuthenticationFailures_data() {
  QTest::addColumn<QString>("path"); QTest::addColumn<int>("http");
  for (const auto &path : {"/accounts/", "/instruments/", "/quotes/", "/positions/", "/orders/integration-order-1/"})
   for (int http : {401, 403}) QTest::newRow(qPrintable(QString(path) + QString::number(http))) << QString(path) << http;
 }
 void readAuthenticationFailures() {
  QFETCH(QString, path); QFETCH(int, http); Fixture f; QVERIFY(f.ready);
  for (const auto &name : {"first", "second"}) {
   QVERIFY(f.sessions->saveAuthenticatedSession(name, QString("fixture-") + name, 3600)); QTRY_VERIFY(!f.sessions->busy());
  }
  auto deps = f.dependencies(); int releases = 0; const auto release = deps.release;
  deps.release = [&] { ++releases; release(); };
  const auto acquire = deps.acquire;
  // Arm only after startSession and native lease acquisition succeeded: the
  // account failure is the controller's fresh GET, not activation verification.
  deps.acquire = [&, acquire](auto done) { acquire([&, done = std::move(done)](bool ok, auto s, auto reason) mutable {
   QCOMPARE(s.sessionIndex, 0);
   f.failedReadPath = path; f.readStatus = http; done(ok, std::move(s), reason);
  }); };
  const bool confirmation = path.startsWith("/orders/"); f.partialOrderReceipts = confirmation;
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), deps);
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QStringList symbols{"AAPL", "MSFT"};
  for (int i = 0; i < 23; ++i) symbols.append(QString("T%1").arg(i));
  const auto priorReads = f.store.readIndices.size();
  QVERIFY(service.submit("read-auth", symbols, path == "/positions/" ? "sell" : "buy"));
  QTRY_COMPARE(f.timers.size(), 1); auto cooldown = f.timers.takeFirst(); cooldown();
  QTRY_COMPARE(completed.size(), 1);
  const auto messages = completed.first().at(1).toStringList(); const auto report = messages.join('\n');
  const QString warning = "Robinhood Re-authentication required before trades can be completed.";
  if (!confirmation) QCOMPARE(messages, QStringList{warning});
  else {
   QCOMPARE(report.count(warning), 1); QVERIFY(report.contains("🟢 Sent Trades"));
   QVERIFY(report.contains("Sent; confirmation unavailable")); QVERIFY(report.contains("MSFT"));
   QVERIFY(report.contains("🔴 Failed/Not Sent Trades")); QVERIFY(report.contains("T22"));
  }
  QCOMPARE(f.orders.size(), confirmation ? 1 : 0); QCOMPARE(releases, 1);
  QVERIFY(!report.contains("PROVIDER-SECRET")); QVERIFY(!report.contains("fixture-first")); QVERIFY(!report.contains("second"));
  QVERIFY(!service.busy()); QVERIFY(!f.sessions->busy());
  QTest::qWait(50); QCOMPARE(completed.size(), 1); QCOMPARE(releases, 1); QVERIFY(f.timers.isEmpty());
  QCOMPARE(f.orders.size(), confirmation ? 1 : 0);
  QVERIFY(!f.store.readIndices.mid(priorReads).contains(1));
 }
 void excessiveAccountScopeIsNotAuthenticationFailure() {
  Fixture f; QVERIFY(f.ready); f.accountCount = 21;
  QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
  auto deps = f.dependencies(); int releases = 0; const auto release = deps.release;
  deps.release = [&] { ++releases; release(); };
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), deps);
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QVERIFY(service.submit("capacity", {"AAPL"}, "buy")); QTRY_COMPARE(completed.size(), 1);
  QCOMPARE(completed.first().at(1).toStringList(), QStringList{"Robinhood account scope exceeds the supported capacity; no orders were sent."});
  QVERIFY(f.orders.isEmpty()); QCOMPARE(releases, 1); QVERIFY(!service.busy()); QVERIFY(!f.sessions->busy());
 }
 void destroyedSessionManagerSettlesWithoutStaleCallbacks() {
  Fixture f; QVERIFY(f.ready);
  QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
  auto deps = f.dependencies(); std::function<void()> acquired;
  const auto acquire = deps.acquire;
  deps.acquire = [&, acquire](auto done) { acquire([&, done = std::move(done)](bool ok, auto s, auto reason) mutable {
   acquired = [done = std::move(done), ok, s = std::move(s), reason]() mutable { done(ok, std::move(s), reason); };
  }); };
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), deps);
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QVERIFY(service.submit("manager-destroyed", {"AAPL"}, "buy")); QTRY_VERIFY(bool(acquired));
  f.sessions.reset();
  QTRY_COMPARE(completed.size(), 1); QVERIFY(!service.busy()); QVERIFY(f.orders.isEmpty());
  acquired(); QTest::qWait(50); QCOMPARE(completed.size(), 1);
 }
 void nativeSellAllKeepsExactQuantitiesAndSkipsEmptyAccount() {
  Fixture f; QVERIFY(f.ready);
  QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies());
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QVERIFY(service.submit("sell", {"AAPL"}, "sell"));
  for (int i = 0; i < 3; ++i) { QTRY_COMPARE(f.timers.size(), 1); auto cooldown = f.timers.takeFirst(); cooldown(); }
  QTRY_COMPARE(completed.size(), 1); QCOMPARE(f.orders.size(), 2);
  QCOMPARE(f.orders[0].value("quantity").toString(), "5.5"); QCOMPARE(f.orders[1].value("quantity").toString(), "2");
  for (const auto &order : f.orders) QCOMPARE(order.value("side").toString(), "sell");
  const auto report = completed.first().at(1).toStringList().join('\n');
  QVERIFY(report.startsWith("<b>Robinhood Auto Sell Results</b>"));
  QVERIFY(report.contains("🟢 Sent Trades")); QVERIFY(report.contains("🔴 Failed/Not Sent Trades")); QVERIFY(report.contains("Skipped"));
 }
 void midBatchOwnershipChangePreservesSentAndRemaining() {
  Fixture f; QVERIFY(f.ready);
  QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies());
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QStringList symbols; for (int i = 0; i < 21; ++i) symbols.append(QString("T%1").arg(i));
  QVERIFY(service.submit("revoked", symbols, "buy")); QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(f.orders.size(), 1);
  QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
  auto doc = QJsonDocument::fromJson(metadata.readAll()).object(); metadata.close();
  auto rows = doc.value("sessions").toArray(); auto row = rows[0].toObject(); row.insert("credential_revision", "999999"); rows[0] = row; doc.insert("sessions", rows);
  QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate)); metadata.write(QJsonDocument(doc).toJson()); metadata.close();
  auto cooldown = f.timers.takeFirst(); cooldown(); QTRY_COMPARE(completed.size(), 1);
  QCOMPARE(f.orders.size(), 1); QVERIFY(!f.sessions->busy());
  const auto report = completed.first().at(1).toStringList().join('\n');
  QVERIFY(report.contains("🟢 Sent Trades")); QVERIFY(report.contains("🔴 Failed/Not Sent Trades"));
  QVERIFY(report.contains("Robinhood Re-authentication required before trades can be completed."));
  for (const auto &symbol : symbols) QCOMPARE(report.count(symbol + (symbol == "T0" ? " (1):" : ":")) + report.count(symbol + (symbol == "T0" ? ":" : " (1):")), 3);
 }
 void ownerDestructionReleasesActualLeaseOnce_data() {
  QTest::addColumn<QString>("phase");
  for (const auto &phase : {"acquiring", "cooldown", "reconciling", "busy-observer"}) QTest::newRow(phase) << QString(phase);
 }
 void ownerDestructionReleasesActualLeaseOnce() {
  QFETCH(QString, phase); Fixture f; QVERIFY(f.ready);
  QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
  auto deps = f.dependencies(); int releases = 0; auto release = deps.release;
  deps.release = [&] { ++releases; release(); };
  std::function<void()> acquired;
  if (phase == "acquiring") {
   auto acquire = deps.acquire;
   deps.acquire = [&, acquire](auto done) { acquire([&, done = std::move(done)](bool ok, auto s, auto reason) mutable {
    acquired = [done = std::move(done), ok, s = std::move(s), reason]() mutable { done(ok, std::move(s), reason); };
   }); };
  }
  if (phase == "reconciling") { f.partialOrderReceipts = true; f.holdOrderReads = true; }
  auto service = std::make_unique<TelegramRobinhoodTradeService>(&f.network, f.sessions.get(), deps);
  if (phase == "busy-observer") connect(service.get(), &TelegramRobinhoodTradeService::busyChanged, this, [&] { if (service && service->busy()) service.reset(); });
  QVERIFY(service->submit("destroy", {"AAPL", "MSFT"}, "buy"));
  if (phase == "acquiring") QTRY_VERIFY(bool(acquired));
  else if (phase == "reconciling") QTRY_VERIFY(f.heldOrderRead);
  else if (phase != "busy-observer") QTRY_COMPARE(f.timers.size(), 1);
  service.reset(); QVERIFY(!f.sessions->busy()); QCOMPARE(releases, phase == "busy-observer" ? 0 : 1);
  const auto orders = f.orders.size();
  if (acquired) acquired(); f.releaseOrderRead();
  for (auto callback : std::exchange(f.timers, {})) callback();
  QTest::qWait(100); QCOMPARE(f.orders.size(), orders); QCOMPARE(releases, phase == "busy-observer" ? 0 : 1);
 }
 void completedObserverCanImmediatelyReplaceBatch() {
  Fixture f; QVERIFY(f.ready);
  QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies());
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  bool replaced = false;
  connect(&service, &TelegramRobinhoodTradeService::busyChanged, this, [&] {
   if (!service.busy() && !replaced) { replaced = true; QVERIFY(service.submit("replacement", {"MSFT"}, "buy")); }
  });
  QVERIFY(service.submit("original", {"AAPL"}, "buy"));
  for (int i = 0; i < 6; ++i) { QTRY_COMPARE(f.timers.size(), 1); auto cooldown = f.timers.takeFirst(); cooldown(); }
  QTRY_COMPARE(completed.size(), 2);
  QCOMPARE(completed[0][0].toString(), "original"); QCOMPARE(completed[1][0].toString(), "replacement"); QCOMPARE(f.orders.size(), 6);
 }
 void multipleNativeChunksRetainOneLeaseAndWholeReport() {
  Fixture f; QVERIFY(f.ready);
  QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
  auto deps = f.dependencies(); int acquires = 0, releases = 0;
  const auto acquire = deps.acquire; const auto release = deps.release;
  deps.acquire = [&](auto done) { ++acquires; acquire(std::move(done)); };
  deps.release = [&] { ++releases; release(); };
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), deps);
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QStringList symbols; for (int i = 0; i < 41; ++i) symbols.append(QString("T%1").arg(i));
  QVERIFY(service.submit("chunks", symbols, "buy")); QVERIFY(!service.submit("overlap", {"AAPL"}, "sell"));
  for (int i = 0; i < 123; ++i) {
   QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(f.orders.size(), i + 1);
   QVERIFY(service.busy()); QVERIFY(f.sessions->busy()); QCOMPARE(releases, 0); QCOMPARE(completed.size(), 0);
   QVERIFY(f.delays.last() >= 2000 && f.delays.last() <= 3000);
   auto cooldown = f.timers.takeFirst(); cooldown();
  }
  QTRY_COMPARE(completed.size(), 1); QCOMPARE(acquires, 1); QCOMPARE(releases, 1);
  QCOMPARE(f.orders.size(), 123); QVERIFY(!f.sessions->busy());
  const auto messages = completed.first().at(1).toStringList();
  const auto report = messages.join('\n');
  for (const auto &symbol : symbols) QCOMPARE(report.count(symbol + " (1):"), 3);
  for (const auto &message : messages) { QVERIFY(message.size() <= 4096); QVERIFY(message.startsWith("<b>Robinhood Auto Buy Results</b>")); }
  QVERIFY(!report.contains("🔴")); QVERIFY(!report.contains("fixture-first"));
 }
 void transportAndPreviewReports_data() {
  QTest::addColumn<QString>("mode");
  for (const auto &mode : {"invalid-2xx", "dropped-post", "http-400", "preview", "reconciled"}) QTest::newRow(mode) << QString(mode);
 }
 void transportAndPreviewReports() {
  QFETCH(QString, mode); Fixture f; QVERIFY(f.ready);
  QVERIFY(f.sessions->saveAuthenticatedSession("first & <owner>", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
  f.malformedPost = mode == "invalid-2xx"; f.dropPost = mode == "dropped-post";
  f.postStatus = mode == "http-400" ? 400 : 200;
  f.partialOrderReceipts = mode == "reconciled";
  auto deps = f.dependencies(); if (mode == "preview") deps.loopbackLiveOrdersForTests = false;
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), deps);
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QVERIFY(service.submit(mode, {"AAPL"}, "buy"));
  const int rows = mode == "invalid-2xx" || mode == "dropped-post" ? 1 : 3;
  for (int i = 0; i < rows; ++i) { QTRY_COMPARE(f.timers.size(), 1); auto cooldown = f.timers.takeFirst(); cooldown(); }
  QTRY_COMPARE(completed.size(), 1);
  const auto report = completed.first().at(1).toStringList().join('\n');
  QVERIFY(report.contains("first &amp; &lt;owner&gt;")); QVERIFY(!report.contains("PROVIDER-SECRET"));
  if (mode == "preview") {
   if (f.network.stockLiveOrdersEnabled(false)) { QVERIFY(report.contains("🟢")); QCOMPARE(f.orders.size(), 3); }
   else { QVERIFY(report.contains("🟡")); QVERIFY(report.contains("Debug preview; not sent")); QVERIFY(!report.contains("🟢")); QCOMPARE(f.orders.size(), 0); }
  }
  else if (mode == "http-400") { QVERIFY(report.contains("🔴")); QVERIFY(!report.contains("🟢")); QCOMPARE(f.orders.size(), 3); }
  else if (mode == "reconciled") { QVERIFY(report.contains("🟢")); QVERIFY(report.contains("not fill-confirmed")); QCOMPARE(f.orders.size(), 3); }
  else {
   QCOMPARE(f.orders.size(), 1); QVERIFY(report.contains("check broker; no automatic retry"));
   QVERIFY(report.contains(mode == "invalid-2xx" ? "🟢 Sent Trades" : "Submission outcome unknown"));
   if (mode == "dropped-post") QVERIFY(!report.contains("🟢"));
  }
 }
 void initialAuthenticationFailuresNeverFallBack_data() {
  QTest::addColumn<QString>("mode");
  for (const auto &mode : {"expired", "read-only", "bad-store", "missing-token", "reauthenticating"}) QTest::newRow(mode) << QString(mode);
 }
 void initialAuthenticationFailuresNeverFallBack() {
  QFETCH(QString, mode); Fixture f; QVERIFY(f.ready);
  QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
  QVERIFY(f.sessions->saveAuthenticatedSession("second", "fixture-second", 3600)); QTRY_VERIFY(!f.sessions->busy());
  QVERIFY(f.sessions->startSession(1)); QTRY_VERIFY(!f.sessions->busy());
  std::unique_ptr<RHBrowserLogin> browser;
  if (mode == "bad-store") f.store.failRead = true;
  if (mode == "missing-token") f.store.tokens.remove(0);
  if (mode == "expired" || mode == "read-only") {
   QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
   auto doc = QJsonDocument::fromJson(metadata.readAll()).object(); metadata.close();
   auto rows = doc.value("sessions").toArray(); auto row = rows[0].toObject();
   if (mode == "expired") {
    row.insert("created_at", QDateTime::currentDateTimeUtc().addSecs(-3600).toString(Qt::ISODateWithMs));
    row.insert("expires_at", QDateTime::currentDateTimeUtc().addSecs(-60).toString(Qt::ISODateWithMs));
   } else { row.insert("token_type", "WebSecondaryBearer"); row.insert("account_id", "integration-owner"); }
   rows[0] = row; doc.insert("sessions", rows);
   QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate)); metadata.write(QJsonDocument(doc).toJson()); metadata.close(); QVERIFY2(f.sessions->load(), qPrintable(f.sessions->error()));
  }
  if (mode == "reauthenticating") {
   browser = std::make_unique<RHBrowserLogin>(RHBrowserLogin::TokenHandler{}, nullptr, RHBrowserLogin::BackendMode::Test);
   f.sessions->setBrowserLogin(browser.get()); QVERIFY(f.sessions->beginBrowserLogin("first"));
  }
  const auto methods = f.methods.size();
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies());
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QVERIFY(service.submit(mode, {"AAPL"}, "buy")); QTRY_COMPARE(completed.size(), 1);
  QCOMPARE(completed.first().at(1).toStringList(), QStringList{"Robinhood Re-authentication required before trades can be completed."});
  QVERIFY(f.orders.isEmpty()); QVERIFY(!f.methods.mid(methods).contains("POST")); QVERIFY(!service.busy());
 }
 void unauthorizedPostRequiresExactWarningAndRemainingSymbols() {
  Fixture f; QVERIFY(f.ready); f.postStatus = 401;
  QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies());
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QStringList symbols; for (int i = 0; i < 25; ++i) symbols.append(QString("T%1").arg(i));
  QVERIFY(service.submit("401", symbols, "buy"));
  QTRY_COMPARE(f.timers.size(), 1); auto cooldown = f.timers.takeFirst(); cooldown();
  QTRY_COMPARE(completed.size(), 1); QCOMPARE(f.orders.size(), 1);
  const auto report = completed.first().at(1).toStringList().join('\n');
  QVERIFY(report.contains("Robinhood Re-authentication required before trades can be completed."));
  QVERIFY(report.contains("T24")); QVERIFY(!report.contains("🟢")); QVERIFY(!report.contains("PROVIDER-SECRET"));
  QVERIFY(!service.busy()); QVERIFY(!f.sessions->busy());
 }
 void firstSavedSparseLoginUsesAllAccountsAndFinalCooldown() {
  Fixture f; QVERIFY(f.ready);
  for (const auto &name : {"discard", "first <login>", "second"}) {
   QVERIFY(f.sessions->saveAuthenticatedSession(name, QString(name).replace(' ', '-').replace('<','x').replace('>','x'), 3600));
   QTRY_VERIFY(!f.sessions->busy());
  }
  QVERIFY(f.sessions->deleteSavedSession(0)); QTRY_VERIFY(!f.sessions->busy());
  QCOMPARE(f.sessions->data(f.sessions->index(0), RHSessionManager::SessionIndexRole).toInt(), 1);
  QVERIFY(f.sessions->startSession(2)); QTRY_VERIFY(!f.sessions->busy());
  auto deps = f.dependencies(); int releases = 0; auto release = deps.release;
  deps.release = [&] { ++releases; release(); };
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), deps);
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QVERIFY(service.submit("first", {"AAPL", "MSFT"}, "buy"));
  for (int i = 0; i < 6; ++i) {
   QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(f.orders.size(), i + 1);
   QVERIFY(service.busy()); QVERIFY(f.sessions->busy()); QCOMPARE(completed.size(), 0); QCOMPARE(releases, 0);
   QVERIFY(f.orderHeaders.last().contains("Authorization: Bearer first-xloginx"));
   QCOMPARE(f.orders.last().value("quantity").toString(), "1");
   auto cooldown = f.timers.takeFirst(); cooldown();
  }
  QTRY_COMPARE(completed.size(), 1); QVERIFY(!service.busy()); QVERIFY(!f.sessions->busy()); QCOMPARE(releases, 1);
  const auto report = completed.first().at(1).toStringList().join('\n');
  QVERIFY(report.contains("<b>Robinhood Auto Buy Results</b>")); QVERIFY(report.contains("🟢 Sent Trades"));
  QVERIFY(report.contains("first &lt;login&gt;")); QVERIFY(!report.contains("second"));
  for (int i = 1; i <= 3; ++i) QVERIFY(report.contains(QString("Account %1").arg(i)));
  QVERIFY(report.contains("not fill-confirmed"));
 }
 void missingFirstLoginRequiresReauthentication() {
  Fixture f; QVERIFY(f.ready);
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies());
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QVERIFY(service.submit("missing", {"AAPL"}, "buy"));
  QTRY_COMPARE(completed.size(), 1);
  QCOMPARE(completed.first().at(1).toStringList(), QStringList{"Robinhood Re-authentication required before trades can be completed."});
  QVERIFY(f.orders.isEmpty()); QVERIFY(!service.busy()); QVERIFY(!f.sessions->busy());
 }
};
QTEST_GUILESS_MAIN(TelegramRobinhoodTradesTest)
#include "test_telegramrobinhoodtrades.moc"
