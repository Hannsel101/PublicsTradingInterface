# Restore Publics ENTITY support

## Goal
Latest user reverses prior exclusion: allow ENTITY (Business) alongside BROKERAGE/ROTH_IRA/TRADITIONAL_IRA in Publics manual/Auto trading and preflights. Preserve unrelated ticker/auth/Robinhood changes and safety/pacing. No live provider/keychain/orders.

## Current Phase
Complete: ENTITY restored, verified and reviewed

## Phases
### Phase1: reproduce and restore supported account type
- **Status:** complete
- Read AGENTS/current source, refreshed CodeGraph. Compiled restored mixed-account, ENTITY-only and holdings-result tests RED (3 expected failures), then added only ENTITY to the native discovery allowlist. Updated README and superseded stale native-policy skill instruction. Added unknown-type rejection regression.
### Phase2: verify actual builds and affected composition
- **Status:** complete
- Focused native GREEN; actual whole macOS Debug/Release builds and six affected suites in Debug/Release/ASan, matching unchanged-source receipts; bounded independent review.
- Actual whole Debug/Release application builds succeeded;6/6 affected suites passed in each Debug/Release/ASan (detect_leaks=0). Parsed all exact JSON/JUnit, matching inventories and unchanged common source fingerprint. Independent source-only review deleg_4f7be7ac found no introduced P1/P2. git diff --check clean.
### Phase3: deliver
- **Status:** complete
- Record results, complete plan, explain restored support. No Windows/live-provider claim.
