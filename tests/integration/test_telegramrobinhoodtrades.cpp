#include "telegramrobinhoodallsessionsfixture.h"
#include "telegramrobinhoodtradeservice.h"

using TelegramRobinhoodAllSessionsFixture::Fixture;

class TelegramRobinhoodTradesTest : public QObject {
 Q_OBJECT
 static QString warning(QStringList emails) {
  return QStringLiteral("🔴🔴 <b>Robinhood Reauthentication Required for the following Accounts</b> 🔴🔴\n"
   "For each account send command /authenticate_robinhood_account accountEmail to remotely authenticate. (Note: Multi-Factor Authentication may be required)\n"
   "Session Emails Affected: %1\n"
   "🟡🟡 Warning: trades for these emails will not go through during this buy and sell order but can be part of the next set of orders if authenticated 🟡🟡").arg(emails.join(", ").toHtmlEscaped());
 }
 static void drain(Fixture &f, TelegramRobinhoodTradeService &service, int maximumMs = 15000) {
  QElapsedTimer clock; clock.start();
  while (service.busy() && clock.elapsed() < maximumMs) {
   QCoreApplication::processEvents();
   if (!f.timers.isEmpty()) { QCOMPARE(f.delays.last(), 2500); auto timer = f.timers.takeFirst(); timer(); }
   QTest::qWait(1);
  }
  QVERIFY(!service.busy());
 }
private slots:
 void expiryFixturePreservesNativeGrantAndOpaqueMetadata() {
  Fixture f; QVERIFY(f.ready); f.saveNative("owner@example.test", "owner");
  const auto metadata = [&] {
   QFile file(f.dir.filePath("sessions.json"));
   return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
  };
  const auto readGrant = [&] {
   QString ignored, secret; RHOAuthGrant grant;
   if (f.store.secrets.value(0)) f.store.secrets[0]->read(ignored, secret);
   if (!RHOAuthGrant::deserialize(secret, &grant)) return RHOAuthGrant{};
   return grant;
  };
  const auto original = readGrant(); QVERIFY(original.primary());
  const auto before = metadata(); QVERIFY(!before.isEmpty());
  const auto expiry = QDateTime::currentDateTimeUtc().addSecs(-60);
  QVERIFY(f.setExpiry(0, expiry));
  const auto adjusted = readGrant(); QVERIFY(adjusted.primary());
  QCOMPARE(adjusted.accessToken(), original.accessToken()); QCOMPARE(adjusted.refreshToken(), original.refreshToken());
  QCOMPARE(adjusted.accountId, original.accountId); QCOMPARE(adjusted.nativeEmail, original.nativeEmail);
  QCOMPARE(adjusted.nativePassword, original.nativePassword); QCOMPARE(adjusted.response, original.response);
  QCOMPARE(adjusted.clientId, original.clientId); QCOMPARE(adjusted.deviceToken, original.deviceToken);
  QCOMPARE(adjusted.expiresAt, expiry); QCOMPARE(adjusted.obtainedAt.addSecs(adjusted.response["expires_in"].toInteger()), expiry);
  auto oldRow = before["sessions"].toArray().first().toObject();
  const auto after = metadata(); auto newRow = after["sessions"].toArray().first().toObject();
  QCOMPARE(newRow["expires_at"].toString(), expiry.toString(Qt::ISODateWithMs));
  QString ignored, secret; QVERIFY(f.store.secrets[0]->read(ignored, secret));
  QCOMPARE(newRow["credential_digest"].toString(), QString::fromLatin1(QCryptographicHash::hash(secret.toUtf8(), QCryptographicHash::Sha256).toHex()));
  for (const auto key : {"created_at", "expires_at", "credential_digest"}) { oldRow.remove(key); newRow.remove(key); }
  QCOMPARE(newRow, oldRow);
  QCOMPARE(after["credential_revision_high_water"], before["credential_revision_high_water"]);
  bool finished = false;
  f.sessions->requestStoredSessionAccounts([&](bool ok, auto rows, QString) {
   QVERIFY(ok); QCOMPARE(rows.size(), 1); QCOMPARE(rows.first().email, "owner@example.test"); QVERIFY(rows.first().expired); finished = true;
  });
  QTRY_VERIFY(finished); QCOMPARE(f.renewals.size(), 1);
 }
 void metadataOnlyExpiryMismatchFailsClosed() {
  Fixture f; QVERIFY(f.ready); f.saveNative("owner@example.test", "owner");
  QVERIFY(f.editMetadata([](QJsonArray &rows) {
   auto row = rows[0].toObject(); row["expires_at"] = QDateTime::currentDateTimeUtc().addSecs(60).toString(Qt::ISODateWithMs); rows[0] = row;
  }));
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies());
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed), notices(&service, &TelegramRobinhoodTradeService::reportReady);
  const auto requests = f.requests.size(); QVERIFY(service.submit("mismatch", {"AAPL"}, "buy")); drain(f, service);
  QCOMPARE(completed.size(), 1); QCOMPARE(notices.size(), 0); QVERIFY(f.orders.isEmpty()); QCOMPARE(f.requests.size(), requests);
  QVERIFY(completed.first()[1].toStringList().join('\n').contains("could not be loaded completely")); QCOMPARE(f.renewals.size(), 1);
 }
 void warningObserverMayDestroyServiceBeforeAnyOrder() {
  Fixture f; QVERIFY(f.ready); f.saveNative("expired@example.test", "expired"); f.saveNative("active@example.test", "active"); QVERIFY(f.expire(0));
  auto service = std::make_unique<TelegramRobinhoodTradeService>(&f.network, f.sessions.get(), f.dependencies());
  connect(service.get(), &TelegramRobinhoodTradeService::reportReady, this, [&] { service.reset(); });
  QVERIFY(service->submit("destroy-warning", {"AAPL"}, "buy")); QTRY_VERIFY(!service);
  QTest::qWait(50); QVERIFY(f.orders.isEmpty()); QVERIFY(!f.sessions->busy());
 }
 void completedObserverMayReplaceBatchWithoutOldStateMutation() {
  Fixture f; QVERIFY(f.ready); f.accountCount = 1; f.saveNative("owner@example.test", "owner");
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies()); QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  bool replaced = false;
  connect(&service, &TelegramRobinhoodTradeService::busyChanged, &service, [&] {
   if (!service.busy() && !replaced) { replaced = true; QVERIFY(service.submit("replacement", {"MSFT"}, "buy")); }
  });
  QVERIFY(service.submit("original", {"AAPL"}, "buy")); drain(f, service); QCOMPARE(completed.size(), 2);
  QCOMPARE(completed[0][0].toString(), "original"); QCOMPARE(completed[1][0].toString(), "replacement"); QCOMPARE(f.orders.size(), 2);
 }
 void ownerDestructionReleasesActualLeaseExactlyOnce_data() {
  QTest::addColumn<QString>("phase"); for (const auto phase : {"acquiring", "cooldown", "reconciling", "busy-observer"}) QTest::newRow(phase) << QString(phase);
 }
 void ownerDestructionReleasesActualLeaseExactlyOnce() {
  QFETCH(QString, phase); Fixture f; QVERIFY(f.ready); f.saveNative("owner@example.test", "owner");
  auto deps = f.dependencies(); const auto release = deps.release; const auto acquire = deps.acquire; int releases = 0; std::function<void()> acquired;
  deps.release = [&] { ++releases; release(); };
  if (phase == "acquiring") deps.acquire = [&, acquire](auto done) { acquire([&, done = std::move(done)](bool ok, auto session, auto reason) mutable {
   acquired = [done = std::move(done), ok, session = std::move(session), reason]() mutable { done(ok, std::move(session), reason); };
  }); };
  if (phase == "reconciling") { f.partialOrderReceipts = true; f.holdOrderReads = true; }
  auto service = std::make_unique<TelegramRobinhoodTradeService>(&f.network, f.sessions.get(), deps);
  if (phase == "busy-observer") connect(service.get(), &TelegramRobinhoodTradeService::busyChanged, this, [&] { if (service && service->busy()) service.reset(); });
  QVERIFY(service->submit("destroy", {"AAPL", "MSFT"}, "buy"));
  if (phase == "acquiring") QTRY_VERIFY(bool(acquired));
  else if (phase == "reconciling") QTRY_VERIFY(f.heldOrderRead);
  else if (phase != "busy-observer") QTRY_COMPARE(f.timers.size(), 1);
  service.reset(); QVERIFY(!f.sessions->busy()); QCOMPARE(releases, phase == "busy-observer" ? 0 : 1);
  const auto orders = f.orders.size(); if (acquired) acquired(); f.releaseOrderRead();
  for (auto callback : std::exchange(f.timers, {})) callback();
  QTest::qWait(50); QCOMPARE(f.orders.size(), orders); QCOMPARE(releases, phase == "busy-observer" ? 0 : 1);
 }
 void dependencyDeletionAndNativeCancellationSettleOnce_data() {
  QTest::addColumn<bool>("destroy"); QTest::newRow("dependency-delete") << true; QTest::newRow("native-lease-released") << false;
 }
 void dependencyDeletionAndNativeCancellationSettleOnce() {
  QFETCH(bool, destroy); Fixture f; QVERIFY(f.ready); f.saveNative("owner@example.test", "owner");
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies()); QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QVERIFY(service.submit("cancel", {"AAPL", "MSFT"}, "buy")); QTRY_COMPARE(f.timers.size(), 1);
  if (destroy) f.sessions.reset(); else f.sessions->releaseTradingSession();
  if (!f.timers.isEmpty()) { auto cooldown = f.timers.takeFirst(); cooldown(); }
  QTRY_COMPARE(completed.size(), 1); QVERIFY(!service.busy()); QCOMPARE(f.orders.size(), 1);
  for (auto callback : std::exchange(f.timers, {})) callback(); QTest::qWait(50); QCOMPARE(completed.size(), 1); QCOMPARE(f.orders.size(), 1);
 }
 void newlyExpiredAtCompletionWarnsWithoutRefreshing() {
  Fixture f; QVERIFY(f.ready); f.accountCount = 1; f.saveNative("expiring@example.test", "expiring");
  QVERIFY(f.setExpiry(0, QDateTime::currentDateTimeUtc().addSecs(2)));
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies()); QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed); QSignalSpy notices(&service, &TelegramRobinhoodTradeService::reportReady);
  QVERIFY(service.submit("clock-expiry", {"AAPL", "MSFT"}, "buy")); QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(notices.size(), 0);
  QTest::qWait(2100); drain(f, service); QCOMPARE(completed.size(), 1); QCOMPARE(notices.size(), 1);
  QCOMPARE(notices.first()[1].toStringList(), QStringList{warning({"expiring@example.test"})}); QCOMPARE(f.orders.size(), 1); QCOMPARE(f.renewals.size(), 1);
 }
 void longCanonicalEmailWarningsRemainCompleteAndHtmlSafe() {
  Fixture f; QVERIFY(f.ready); QStringList emails;
  for (int i = 0; i < 6; ++i) { const auto email = QString(200, '&') + QString::number(i) + "@example.test"; emails.append(email); f.saveNative(email, QString("profile%1").arg(i)); }
  for (int i = 0; i < 6; ++i) QVERIFY(f.expire(i));
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies()); QSignalSpy notices(&service, &TelegramRobinhoodTradeService::reportReady);
  QVERIFY(service.submit("long", {"AAPL"}, "buy")); drain(f, service); QCOMPARE(notices.size(), 1);
  const auto messages = notices.first()[1].toStringList(); QVERIFY(messages.size() > 1); const auto report = messages.join('\n');
  qsizetype prior = -1; for (const auto &email : emails) { const auto escaped = email.toHtmlEscaped(); QCOMPARE(report.count(escaped), 1); const auto at = report.indexOf(escaped); QVERIFY(at > prior); prior = at; }
  for (const auto &message : messages) { QVERIFY(message.size() <= 3900); QCOMPARE(message.count("<b>"), 1); QCOMPARE(message.count("</b>"), 1); QVERIFY(message.endsWith("if authenticated 🟡🟡")); }
  QVERIFY(f.orders.isEmpty());
 }
 void completeInventoryFailureNeverTradesAValidSubset_data() {
  QTest::addColumn<QString>("mode");
  for (const auto mode : {"legacy", "missing-secret", "denied"}) QTest::newRow(mode) << QString(mode);
 }
 void completeInventoryFailureNeverTradesAValidSubset() {
  QFETCH(QString, mode); Fixture f; QVERIFY(f.ready); f.saveNative("valid@example.test", "valid");
  if (mode == "legacy") { QVERIFY(f.sessions->saveAuthenticatedSession("This is not an email", "legacy-fixture", 3600)); QTRY_VERIFY(!f.sessions->busy()); }
  else { f.saveNative("second@example.test", "second"); if (mode == "missing-secret") f.store.secrets.remove(1); else f.store.failRead = true; }
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies());
  QSignalSpy notices(&service, &TelegramRobinhoodTradeService::reportReady); QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  const auto count = f.requests.size(); QVERIFY(service.submit("inventory", {"AAPL"}, "buy")); drain(f, service);
  QCOMPARE(completed.size(), 1); QCOMPARE(notices.size(), 0); QCOMPARE(f.requests.size(), count); QVERIFY(f.orders.isEmpty());
  const auto text = completed.first()[1].toStringList().join('\n'); QVERIFY(text.contains("could not be loaded completely"));
  QVERIFY(!text.contains("This is not an email")); QVERIFY(!text.contains("fixture"));
 }
 void nativeSellAllAcrossProfilesIsFreshAndExact() {
  Fixture f; QVERIFY(f.ready); f.saveNative("one@example.test", "one"); f.saveNative("two@example.test", "two");
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies());
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QVERIFY(service.submit("sell", {"AAPL", "MSFT"}, "sell")); drain(f, service);
  QCOMPARE(completed.size(), 1); QCOMPARE(f.orders.size(), 4); QCOMPARE(f.delays.size(), 12);
  QCOMPARE(f.requests.count("GET /positions/"), 12);
  for (int i = 0; i < 4; ++i) { QCOMPARE(f.orders[i].value("quantity").toString(), i % 2 ? "2" : "5.5"); QCOMPARE(f.orders[i].value("side").toString(), "sell"); }
  const auto report = completed.first()[1].toStringList().join('\n'); QVERIFY(report.startsWith("<b>Robinhood Auto Sell Results</b>"));
  QVERIFY(report.contains("🟢 Sent Trades")); QVERIFY(report.contains("🔴 Failed/Not Sent Trades")); QVERIFY(report.contains("Skipped"));
 }
 void reportsPreserveNativeOutcomeAndNeverRetryUnknown_data() {
  QTest::addColumn<QString>("mode");
  for (const auto mode : {"unknown", "network", "rejected", "preview", "reconciled"}) QTest::newRow(mode) << QString(mode);
 }
 void reportsPreserveNativeOutcomeAndNeverRetryUnknown() {
  QFETCH(QString, mode); Fixture f; QVERIFY(f.ready); f.saveNative("one&owner@example.test", "one"); f.saveNative("two@example.test", "two");
  f.malformedPost = mode == "unknown"; f.dropPost = mode == "network"; f.postStatus = mode == "rejected" ? 400 : 200;
  f.partialOrderReceipts = mode == "reconciled";
  auto deps = f.dependencies(); if (mode == "preview") deps.loopbackLiveOrdersForTests = false;
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), deps);
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed); QSignalSpy notices(&service, &TelegramRobinhoodTradeService::reportReady);
  QVERIFY(service.submit(mode, {"AAPL", "MSFT"}, "buy")); drain(f, service); QCOMPARE(completed.size(), 1); QCOMPARE(notices.size(), 0);
  const auto report = completed.first()[1].toStringList().join('\n');
  QVERIFY(report.contains("one&amp;owner@example.test")); QVERIFY(report.contains("two@example.test"));
  QVERIFY(!report.contains("PROVIDER-SECRET")); QVERIFY(!report.contains("native-fixture"));
  if (mode == "unknown" || mode == "network") {
   QCOMPARE(f.orders.size(), 1); QVERIFY(report.contains("no automatic retry")); QVERIFY(report.contains("MSFT"));
   QVERIFY(report.contains("🔴 Failed/Not Sent Trades"));
   if (mode == "unknown") { QVERIFY(report.contains("🟢 Sent Trades")); QVERIFY(report.contains("Sent; confirmation unavailable")); }
   else { QVERIFY(!report.contains("🟢")); QVERIFY(report.contains("Submission outcome unknown")); }
  } else if (mode == "preview" && !f.network.stockLiveOrdersEnabled(false)) {
   QCOMPARE(f.orders.size(), 0); QVERIFY(report.contains("🟡 Preview Trades")); QVERIFY(!report.contains("🟢"));
  } else { QCOMPARE(f.orders.size(), 12); QVERIFY(report.contains(mode == "rejected" ? "🔴 Failed/Not Sent Trades" : "🟢 Sent Trades")); }
  QCOMPARE(f.renewals.size(), 2); QTest::qWait(50); QCOMPARE(completed.size(), 1);
 }
 void nativeChunksHaveOneLeasePerSessionAndBoundedCompleteReports() {
  Fixture f; QVERIFY(f.ready); f.saveNative("one@example.test", "one"); f.saveNative("two@example.test", "two");
  auto deps = f.dependencies(); const auto acquire = deps.acquire; const auto release = deps.release; int acquires = 0, releases = 0;
  deps.acquire = [&](auto done) { ++acquires; acquire(std::move(done)); }; deps.release = [&] { ++releases; release(); };
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), deps); QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QStringList symbols; for (int i = 0; i < 41; ++i) symbols.append(QString("T%1").arg(i));
  // 246 native orders plus preflights need headroom under concurrent ASan kits.
  QVERIFY(service.submit("chunks", symbols, "buy")); QVERIFY(!service.submit("overlap", {"MSFT"}, "buy")); drain(f, service, 60000);
  QCOMPARE(completed.size(), 1); QCOMPARE(f.orders.size(), 246); QCOMPARE(acquires, 2); QCOMPARE(releases, 2); QCOMPARE(f.delays.size(), 246);
  const auto messages = completed.first()[1].toStringList(); const auto report = messages.join('\n');
  for (const auto &symbol : symbols) QCOMPARE(report.count(symbol + " (1):"), 6);
  for (const auto &message : messages) { QVERIFY(message.size() <= 3900); QVERIFY(message.startsWith("<b>Robinhood Auto Buy Results</b>")); QCOMPARE(message.count("<b>"), 1); QCOMPARE(message.count("</b>"), 1); }
 }
 void allExpiredStillWarnsExactlyAndSubmitsNothing() {
  Fixture f; QVERIFY(f.ready); f.saveNative("owner&one@example.test", "one"); f.saveNative("owner2@example.test", "two");
  QVERIFY(f.expire(0)); QVERIFY(f.expire(1));
  QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
  const auto bytes = metadata.readAll(); QVERIFY(!bytes.contains("@example.test")); QVERIFY(!bytes.contains("fixture-native-password"));
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), f.dependencies());
  QSignalSpy notices(&service, &TelegramRobinhoodTradeService::reportReady); QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  const auto requests = f.requests.size(); QVERIFY(service.submit("expired", {"AAPL"}, "sell")); drain(f, service);
  QCOMPARE(completed.size(), 1); QCOMPARE(notices.size(), 1);
  QCOMPARE(notices.first()[1].toStringList(), QStringList{warning({"owner&one@example.test", "owner2@example.test"})});
  QCOMPARE(f.requests.size(), requests); QVERIFY(f.orders.isEmpty()); QVERIFY(f.timers.isEmpty());
 }
 void authenticationEvidenceIncludesAccountPreflightWithoutNativeRows_data() {
  QTest::addColumn<int>("http"); QTest::newRow("401") << 401; QTest::newRow("403") << 403;
 }
 void authenticationEvidenceIncludesAccountPreflightWithoutNativeRows() {
  QFETCH(int, http); Fixture f; QVERIFY(f.ready); f.saveNative("owner@example.test", "owner");
  auto deps = f.dependencies(); const auto acquire = deps.acquire;
  deps.acquire = [&, acquire](auto done) { acquire([&, done = std::move(done)](bool ok, auto session, auto reason) mutable {
   f.failedReadPath = "/accounts/"; f.readStatus = http; done(ok, std::move(session), reason);
  }); };
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), deps);
  QSignalSpy notices(&service, &TelegramRobinhoodTradeService::reportReady); QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  QVERIFY(service.submit("auth", {"AAPL", "MSFT"}, "buy")); drain(f, service);
  QCOMPARE(completed.size(), 1); QCOMPARE(notices.size(), 1);
  QCOMPARE(notices.first()[1].toStringList(), QStringList{warning({"owner@example.test"})});
  QVERIFY(f.orders.isEmpty()); QVERIFY(!completed.first()[1].toStringList().join('\n').contains("PROVIDER-SECRET"));
 }
 void allInitiallyUnexpiredSparseProfilesUseAccountTickerOrder() {
  Fixture f; QVERIFY(f.ready);
  f.saveNative("discard@example.test", "discard");
  f.saveNative("first&owner@example.test", "first");
  f.saveNative("expired@example.test", "expired");
  f.saveNative("second@example.test", "second");
  QCOMPARE(f.sessions->rowCount(), 4);
  QVERIFY(f.sessions->deleteSavedSession(0)); QTRY_VERIFY(!f.sessions->busy());
  QVERIFY(f.expire(2));
  auto deps = f.dependencies(); QList<int> acquiredSlots; int releases = 0;
  const auto acquire = deps.acquire; const auto release = deps.release;
  deps.acquire = [&](auto done) { acquire([&, done = std::move(done)](bool ok, auto session, auto reason) mutable {
   acquiredSlots.append(session.sessionIndex); done(ok, std::move(session), reason);
  }); };
  deps.release = [&] { ++releases; release(); };
  TelegramRobinhoodTradeService service(&f.network, f.sessions.get(), deps);
  QSignalSpy notices(&service, &TelegramRobinhoodTradeService::reportReady);
  QSignalSpy completed(&service, &TelegramRobinhoodTradeService::completed);
  connect(&service, &TelegramRobinhoodTradeService::reportReady, &service, [&](QString id, QStringList) {
   QCOMPARE(id, "all"); QVERIFY(service.busy()); QVERIFY(f.orders.isEmpty()); QCOMPARE(acquiredSlots.size(), 0);
  });
  QVERIFY(service.submit("all", {"AAPL", "MSFT"}, "buy"));
  for (int i = 0; i < 12; ++i) {
   QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(f.orders.size(), i + 1);
   QCOMPARE(completed.size(), 0); QVERIFY(service.busy()); QCOMPARE(f.delays.last(), 2500);
   const auto order = f.orders.last(); QCOMPARE(order.value("quantity").toString(), "1");
   QCOMPARE(order.value("account").toString(), f.brokerageOrigin + QString("/accounts/a%1/").arg((i % 6) / 2 + 1));
   QCOMPARE(order.value("instrument").toString(), f.brokerageOrigin + (i % 2 ? "/instruments/MSFT/" : "/instruments/AAPL/"));
   auto cooldown = f.timers.takeFirst(); cooldown();
  }
  QTRY_COMPARE(completed.size(), 1); QCOMPARE(acquiredSlots, QList<int>({1, 3})); QCOMPARE(releases, 2);
  QCOMPARE(notices.size(), 1); QCOMPARE(notices.first()[1].toStringList(), QStringList{warning({"expired@example.test"})});
  const auto report = completed.first()[1].toStringList().join('\n');
  QVERIFY(report.contains("first&amp;owner@example.test")); QVERIFY(report.contains("second@example.test"));
  QVERIFY(report.contains("<b>Robinhood Auto Buy Results</b>")); QVERIFY(report.contains("🟢 Sent Trades"));
  QVERIFY(!report.contains("Reauthentication Required")); QVERIFY(!report.contains("native-fixture"));
  QCOMPARE(f.renewals.size(), 4); // Only explicitly authenticated grants; no activation refresh.
 }
};
QTEST_GUILESS_MAIN(TelegramRobinhoodTradesTest)
#include "test_telegramrobinhoodtrades.moc"
