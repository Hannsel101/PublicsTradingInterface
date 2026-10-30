# Progress

## Final review fixes and verification — complete
- P1 activation policy: readActivationSnapshot returns token and current Chat IDs from one authenticated locked catalog read; controller no longer grants from cached UI metadata. Shared encrypted-backend/two-controller revocation regression passed.
- Canonical positive Chat ID validation now agrees across controller/storage/QML/runtime. Unsupported values fail visibly before commit; legacy unsupported policies fail closed.
- PublicAuthClient exposes explicit NotStarted/Loading/Ready/Failed key-index discovery. Production trading waits for Ready; read failure/malformed index yields a sanitized whole-batch failure with every requested ticker and zero credential/auth/order requests. Confirmed missing-index legacy fallback remains distinct from failure.
- Publics nickname metadata refreshes from committed settings before each batch snapshot; cross-instance rename/removal regression passed. Prior callback-deletion ASan fix retained.
- Parent independently rebuilt the final Qt6.12 Release app and reran all27CTesttargets: zero failing targets. Four focused ASan CTesttargets (service, actual bridge, encrypted store, controller) passed without findings; leak detection disabled. Explicit Qt6.12 telegram_bots_ui_qmllint target exited0. Android6.11.2 app target rebuilt after shared auth changes and still excludes Telegram.
- Artifacts: scratch/telegram-auto-reviewed-release-ctest.xml, telegram-reviewed-asan-ctest.xml, telegram-auto-reviewed-qmllint.log and telegram-auto-reviewed-android-build.log. Updated Release executable exists in build/architecture-qt612-release/launcher/appPublicsTradingInterface.app.
- No production credentials/keychains, real Telegram messages, live tunnels or brokerage orders were exercised. Platform keychain live tests remain opt-in/skipped; Windows/iOS were not built. No commits made. Documented durable-admission availability limitations remain.

## Contract
- User approves immediate execution from locally allowlisted private Telegram users/chats, not per-batch desktop confirmation.
- Added requirement: 🟢 prefix successful lines; 🔴 prefix failed lines; both buy/sell reports omit empty categories.
- User explicitly requires encrypted Chat IDs via QtKeychain and restoration when loading the applet.

## Work ownership
- Policy worker: encrypted store migration, Chat ID editor UI, controller policy/routing/busy hook and respective tests.
- Runtime worker: strict authorization/parsing, durable token-free replay ledger, generation-bound command tickets and HTML notifications, existing runtime/webhook tests.
- Publics worker: existing AutoTradeController-backed service and formatting, own integration tests, sole CMake owner while active.
- Parent: TelegramBotsApplet actual composition and new real seam integration test, review and final Release verification.
- Parent authored tests/integration/test_telegramtradebridge.cpp: real injected controller/runtime/service/AutoMode with two loopback brokerage accounts, held first order, navigation/Stop busy gating, no partial report, selected-bot change and HTML delivery to origin. Target registration delegated to sole CMake owner; RED execution waits for sibling interfaces, then parent adds its missing applet composition overload. No stale binary results claimed.
- Runtime owner also addresses receiver-visible capacity admission so a saturated command queue is not silently acknowledged HTTP200; authenticated retryable rejections must not execute orders twice.

## Verification
- All initial implementation workers completed. Parent inspected source, raised runtime CTest timeout to240, and composed the production/injected TelegramBotsApplet with the reusable Publics service and stable-origin result routing.
- Parent full Qt6.12 macOS Release application build succeeded; all27CTesttargets passed with0failures/errors/skips. Parent composition regression observed broker.orders0 RED before wiring, then GREEN with actual two-account AutoMode and HTML response to original bot despite selection change.
- Android6.11.2 native app target rebuilt successfully, retaining Telegram desktop-only exclusion. No mobile SDK upgrade or live orders performed.
- Parent found and reproduced a service startController use-after-free when a direct empty-key AutoTradeController completion observer deletes the adapter during submit. New regression failed under ASan at telegrampublicstradeservice.cpp:115 (exit134, heap-use-after-free). Added a QPointer guard across submit; sanitizer and final Release reruns are in progress.
- ASan RED->GREEN confirmed the synchronous-submit deletion fix. Full service suite14 checks and actual applet bridge3 checks passed under ASan with no findings; final rebuilt Release all27CTesttargets passed again. ASan leak detection disabled for this platform run; no leak-free claim.
- Independent reviews returned actionable findings: P1 stale cached policy at activation, P2 editor/store/runtime Chat ID range disagreement, P2 stale Publics nickname metadata and P2 trading an incomplete key subset after authoritative index discovery failure. Parent dispatched isolated policy and Publics/auth owners for RED->GREEN fixes; completion remains blocked on fixes and rerun verification.
- Existing watermark availability limitations remain documented, not treated as guaranteed execution or permission to retry trades.
- Refreshed CodeGraph/read applicable guidance and inspected source. No live providers/credentials/orders used.
