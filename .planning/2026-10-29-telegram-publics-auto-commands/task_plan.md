# Telegram Publics Auto Buy/Sell

## Goal
Implement immediately executed, allowlisted private-chat publicsAutoBuy/publicsAutoSell commands using existing Publics AutoMode, encrypted per-bot Chat ID policy, and completed grouped HTML results with green/red circles.

## Current phase
Complete. Review findings fixed and final Qt6.12 desktop Release, QML lint, Android exclusion and focused ASan regressions verified. No live orders or production keychain verification performed.

## Phases
1. Contract and scope — complete. User explicitly chose immediate execution for allowlisted users/private Chat IDs and QtKeychain-encrypted persisted whitelist. No real orders are authorized for development verification.
2. Encrypted policy/UI/controller — complete (fresh atomic token/policy activation snapshot, consistent supported ID validation, encrypted reload/revocation regressions passed).
3. Authenticated command transport/durable duplicate prevention — complete (runtime152, webhook23, host43 worker checks; parent Release regression passed).
4. Reused Publics backend service/reporting — complete (committed nicknames refreshed and complete key-index discovery required before any credential/order work; failure regressions passed).
5. Actual applet composition and offline integration regression — complete (parent observed broker.orders=0 RED then real 2-account command→AutoMode→HTML reply GREEN).
6. Independent review, Release build and full tests — complete (findings fixed with RED->GREEN; parent rebuilt final Release and all27CTesttargets passed, four focused ASan targets passed, explicit Qt6.12 QML lint and Android app target passed).

## Boundaries
- Desktop-only; preserve unrelated applets/dirty changes and existing status/Stop lifecycle.
- No production keychain reads, public tunnels, Telegram messages, brokerage authentication or orders during testing.
- Empty/malformed policy denies trading. Private human sender ID must exactly match the allowlisted chat ID.
- Persist Chat IDs only inside the authenticated encrypted QtKeychain catalog, never a plaintext policy side index. Durable replay ledger contains no chat IDs or credentials.
- Mark durable admission before any trade dispatch; no replay of possibly committed orders.
- Results follow authoritative AutoMode completion, not signal submission; no false fill claims from submitted/pending/unknown outcomes.
- Each worker uses a separate build directory; final build/architecture-qt612-release belongs to parent only after writers complete.
