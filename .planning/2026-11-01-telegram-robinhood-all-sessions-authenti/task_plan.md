# Telegram Robinhood All Sessions and Remote Authentication

## Goal
Use native Robinhood Auto Mode for standalone/combined Telegram trades across all initially unexpired saved logins; report every expired account email; add authorized stored-credential remote push authentication with an eight-minute deadline. Fix Single Mode premature token renewal.

## Contract
- Latest user scope supersedes earlier Telegram first-saved-login-only restriction. Same native session→account→ticker order, supported standard/IRA/Roth, BUY1/SELLfreshall, precise2500ms cooldowns and preview/live/uncertain guards.
- Exact red/bold reauthentication heading, instructional account command, affected email list and yellow skipped-trades warning. Bound/escape HTML without dropping affected emails. Combined remains Publics/report settlement→Robinhood/results.
- authenticate_robinhood_account accountEmail (optional slash), private-human allowlist/webhook-secret enforcement before lookup, durable replay claim shared with trades, captured originating bot/chat and global cross-broker/auth busy admission.
- Existing encrypted native email/password only; no secret/MFA input in Telegram. Match pinned stable saved identity, same-owner primary grant, successful durable native commit before success. Meaningful sanitized rejection/network/unsupported-challenge/timeout reasons; no raw provider/store/secret diagnostics.
- Eight-minute total authentication deadline, no approval assumptions or late second terminal; preserve reservations until secure cleanup. No automatic trade replay: authenticated accounts participate only in later commands.
- Latest exact terminal replies: Authentication Successful for accountEmail; Authentication Failed for accountEmail, reason: 8 minute timeout reached. An unexpired token with at least one hour remaining must not reauthenticate: Authentication not performed for accountEmail, reason: token expires at dateOfExpiration, using actual validated expiration date/time with timezone. Only expired or less-than-one-hour tokens are eligible; exactly one hour skips.
- Unexpired saved OAuth tokens and original actual provider expiry reused across Single Mode opens; no background pre-expiry refresh. Preserve expiry/revocation/ownership/lease safeguards; explicit fresh authentication can replace expired credentials.
- Preserve worktree; no production keychain/provider/push/tunnel/order operations and no commits.

## Current Phase
Complete: implementation, regression verification, independent review and delivery

## Phases
### Phase1: source contracts and security boundaries
- **Status:** complete
- Read native authentication/grants/manager, Telegram composition and current plans; pinned shared native callback/DTO and protocol interfaces, four isolated implementation owners.
### Phase2: native/protocol/all-session/auth adapter and token reuse
- **Status:** complete
- Protocol owner; native inventory/stored-auth owner; all-session trading/report owner; Single Mode reuse owner. Parent owns auth adapter, applet/composite integration, CMake and actual composition tests.
### Phase2a: Single Mode saved-token reuse
- **Status:** complete
- Unexpired, consistent saved OAuth grant now verifies the existing bearer without grant renewal or secret writes; original created_at/expires_at remain unchanged on repeated visits. Idle renewal waits for actual expiry rather than120sec before it. Existing expired-only renewal, ownership/digest/revision/rotation and revocation guards retained.
- Actual macOS Debug/Release apps rebuilt; five focused session/native/actual-trading-context/default-applet/QML targets passed5/5 each. Focused ASan session/native/context targets passed3/3, leak detection disabled. Independent focused read-only review completed with no concrete high-confidence findings; overall Telegram integration remains in progress.
### Phase3: real composition and regression verification
- **Status:** complete
- Build actual macOS Debug/Release apps; full configured CTest plus focused supported sanitizers. Test actual authorized command→native push/validation/persistence→same-origin response and all-session orders, not only fake adapters.
### Phase4: independent security/lifecycle review and delivery
- **Status:** complete
- Review replay, secret/identity disclosure, ownership, timeout/late callbacks, report boundaries, cancellation and no trade retries; resolve regressions and update reusable skill contracts.
- Native/adapter findings independently closed by deleg_7650ae15; transport known-status finding independently closed by deleg_ed9491b8, no introduced issues in either bounded source review. Actual final macOS whole Debug/Release builds and CTest37/37 each, focused ASan11/11 (leak detection disabled), all against the same unchanged source fingerprint. No live provider/keychain/order or Windows verification claimed.

## Known constraints
- No production authentication or orders in development. Windows/mobile and live provider behavior unverified.
- Provider decides verification method; reuse existing supported push flow. Report code/CAPTCHA requirements safely rather than bypassing MFA or collecting secrets through Telegram.
