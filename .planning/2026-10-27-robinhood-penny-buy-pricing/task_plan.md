# Robinhood penny buy pricing and uppercase ticker entry
## Goal
Before any batch buy, fetch each distinct ticker quote once, freeze its order plan for all eligible accounts, use market buys for last_trade_price >= $1 and fixed ask + $0.01 limits below $1; uppercase ASCII letters as the user edits ticker input.
## Scope
Robinhood only. Preserve Publics, native authentication/credentials, Android launcher disablement, active-login account scope, buy-one/sell-all behavior, serial ticker/account order, every-result/final 2–3s delay, unknown-submission halt/no retry, TLS/origin/deadline/lease guards and Debug-preview/Release-live policy. No commits or real provider/keychain/trade operations during tests.
## Decisions
- Exactly $1 uses market orders; threshold is quote.last_trade_price, not ask.
- Finish ALL instrument/quote preflight before first POST; invalid preflight sends zero orders. One quote GET per unique ticker, reject unexpected quote pagination; reuse snapshot without refresh across accounts.
- Exact decimal-string ask + 0.01; no floating point or silent upward rounding beyond the requested maximum. Server remains authoritative for price precision/acceptance.
- Remove preset_percent_limit from both new buy paths. Fixed penny limit must not be reinterpreted as a percent collar.
- Snapshot timestamps must not be relabelled as freshly fetched at each account order.
- Uppercase ASCII a-z only; preserve separators, whitespace, digits and non-ASCII invalid characters so normalization cannot silently turn an invalid symbol into another stock.
## Ownership
Backend agent: rhtradecontroller/network .h/.cpp and controller/network/context tests only. Parent: StockTrade.qml, QML tests, docs/build/review/artifacts. Separate build trees.
## Current Phase
Complete
## Next Step
Deliver the verified macOS Release archive with live-order and fill caveats.
## Phases
1. Discovery and contract: complete.
2. Backend pricing and UI uppercase with regressions: complete.
3. Independent review/full desktop suites/Android build: complete.
4. Repackage verified app and deliver limits: complete.
