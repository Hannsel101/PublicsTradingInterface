# Telegram bot password security and deletion

## Goal
Add safe bot deletion, mandatory private Chat ID before Start, per-bot password-derived protection of private credentials/Chat IDs/generated persistent webhook session secrets, and a production-integrated TelegramWebhookManager.

## Contract
- One password per bot. Password and derived encryption keys are never persisted. Setup requires matching two hidden fields; each Start requires password entry and a current authenticated policy snapshot.
- Password-only encryption: the password is the sole secret used to derive the protected-payload key. QtKeychain stores ciphertext plus public salt/nonce/KDF metadata, never a stored master key, password, derived key or separate password verifier.
- Telegram security is entirely independent of Robinhood: separate Telegram-only QtKeychain namespace and lock, no Robinhood key/token/profile/lock reads, writes or reuse, and no changes to the Robinhood applet. Preserve old Telegram storage rather than silently overwrite it; do not automatically import legacy data if that would access a Robinhood shared key. Existing entries may need explicit manual recreation in the new vault.
- Protect BotFather token as well as private IDs and session secret using the password envelope; ciphertext can be fetched and decrypted without any Robinhood material.
- CSPRNG session secret generated only if absent, stored encrypted, used exactly as Telegram secret_token and receiver header verifier. No passwordless regeneration/reset or fallback after wrong password.
- New bots can have no Chat IDs. Start attempts without one show an edit-settings/private-Chat-ID popup and never activate transport.
- Delete only stopped/quiescent bots after confirmation and successful storage commit; preserve unrelated keys, other bots, pending-order gates and durable replay records.
- Correct provider origin is https://api.telegram.org/bot<TOKEN>/setWebhook. Shared receiver remains loopback, bounded and asynchronous; missing/wrong/duplicate secret headers rejected with403, and all commands require eligible allowlisted private human origin.
- Preserve existing financial AutoMode, at-most-once admission, generation/lifetime guards, registration/readback and Stop cleanup uncertainty handling.

## Phases
1. Source inspection and primary security references — complete (official OWASP scrypt baseline and Telegram webhook header/URL/token constraints verified).
2. Password envelope, legacy preservation, deletion storage — complete (worker101checks; parent Release/ASan pass; new vault has no Robinhood runtime dependency).
3. Controller/QML setup/unlock/delete/missing-ID UI — complete (worker42controller/186QML checks; parent full Release/QML lint pass).
4. TelegramWebhookManager + actual runtime/host integration — complete (worker native suites; parent actual password-protected bridge and full Release pass).
5. Actual composition, CMake integration and full verification — complete (parent29CTesttargets, four ASan targets, Qt6.12 QML lint and Android exclusion pass).
6. Independent security review and resolved regression verification — complete (vault/isolation audit had no actionable findings; manager abort-callback lifetime finding fixed with timeout/oversize RED->GREEN and direct-deletion ASan cases; final Release29/29 and focused ASan4/4 passed).

7. Telegram-only status response follow-up — complete (RED->GREEN; successful reply 🟢 Active; Inactive/UI labels unchanged; canonical publicsAutoBuy/publicsAutoSell verified, no underscored aliases added; final Release build and all29CTesttargets passed).

8. Debug-only Telegram command diagnostics — complete (Debug/Release app builds, Debug4/4 focused and Release30/30 targets passed; privacy/bounds/replay checks passed. Delayed review finding already resolved by polling owner/generation guard; added explicit message-handler stop/restart regression and verified dedicated Debug/Release and ASan suites, leak detection disabled).

9. Compact management actions — complete (Delete Bot immediately left of Edit Chat IDs in one row; responsive compact title span prevents overflow; composed desktop/360px regression RED->GREEN; Debug/Release app builds and188QML checks per configuration passed, plus Release launcher/applet2/2 native targets).

10. Underscored trading command rename — complete (publics_auto_buy/publics_auto_sell replace former names; comma-separated tickers, optional slash, unchanged AutoMode/results/security; Debug diagnostics synchronized; grammar RED->GREEN; Debug/Release app builds, Release30/30 and focused Debug4/4 targets passed).

11. Slow Public portfolio HTTP400 investigation — triaged; cause unresolved pending sanitized provider rejection details/read-only same-account comparison. Confirmed Debug preflight versus live GET distinction and failed-holdings not_submitted guard; two existing fixture regressions passed. No speculative production fix or live trade replay.

12. Confirmed orphan-ngrok inspector recovery — implemented and verified on macOS (exact Confirm/Cancel prompt, captured-bot routing and no Cancel mutation; positive private app-owned process receipts prevent PID1/launchd confusion; old untracked instances refused, macOS non-atomic signal risk disclosed before confirmation). Final Qt6.12 Debug/Release apps built; Release30/30, focused Debug3/3 and ASan2/2 targets passed. Windows native source/linking included but native compilation/execution remains unverified.

13. Robinhood Telegram commands — complete on macOS. Added robinhood_auto_buy/robinhood_auto_sell with existing private-human authorization/replay protection, first saved login/all its brokerage accounts (no fallback), native one-share BUY/sell-all, typed initial-POST and GET-auth evidence, exact reauth warning, safe complete-list chunking and captured origin/global busy. Final actual Debug/Release applications built; both full32/32 CTest suites and focused6/6 ASan targets passed after GET-auth fix. Windows native compilation/live providers remain unverified; Telegram vault remains independent.

14. Combined auto_buy / auto_sell — complete on macOS. One authenticated durable admission executes Publics, settles its report-send attempts, then Robinhood and its separate report; same complete ticker snapshot/side, existing broker scopes, global busy/origin retained throughout. Actual bridge verifies buy/sell ordering, first-login scope, switched selected bot, no duplicate work and lost-Publics-report acknowledgment before first RH order. Existing Debug preview/live guards preserved. Final Debug32/32, Release32/32 and focused ASan6/6 targets passed; no live-provider/orders claim.

15. Remove Telegram vault footer copy — complete. Deleted the legacy-vault explanatory label and its layout slot from CoreView, preserving runtime errors/actions and vault behavior. Desktop/compact absence-and-bottom-spacing regression observed RED then GREEN. Rebuilt actual Debug/Release apps; QML and Telegram applet targets passed2/2 in each configuration.

16. Empty trading commands and uppercase tickers — complete. All six exact commands (including optional slash/whitespace-only arguments) now return from shared admission before acknowledgement queue/trade claim; no Telegram reply or broker dispatch. Existing ASCII-validated uppercase normalization confirmed for buy/sell on all three routes; mixed/lowercase actual-composition rows retained. RED observed for unwanted no-argument acknowledgement, then GREEN. Actual Debug/Release app builds and5/5 focused CTest targets each passed; focused ASan polling/webhook/real bridge passed (leaks disabled).

## Verification scope
Only injected keychain backends, synthetic passwords/tokens/IDs, loopback Telegram/brokerage servers and fake managed tunnel processes. No production keychain access, public tunnels, live Telegram mutation or orders. Preserve prior work; no commits unless requested. Each worker has a distinct build directory; parent owns final architecture-qt612-release and CMake.
