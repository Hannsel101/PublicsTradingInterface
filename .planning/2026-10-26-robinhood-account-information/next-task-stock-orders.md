# Queued next task: Robinhood manual stock orders

Status: queued after the current account-classification/holdings-information implementation and verification. Do not interrupt or broaden the in-flight read-only account-information work.

## User requirements

- Robinhood applet only; mirror the existing Publics ticker-entry field and transaction-results experience.
- Accept a single ticker or multiple tickers using the Publics field's parsing/normalization behavior.
- Apply each entered ticker to ALL brokerage accounts in the active Robinhood session, as explicitly clarified by the user; show the affected accounts before submission. Do not automatically authenticate or include other saved login profiles.
- Buy button: buy exactly one share of each entered ticker in each account.
- Sell button: sell all sellable shares of each entered ticker in each account, using that account's fresh position.
- Execute account/ticker transactions sequentially, never concurrently. For each ticker, process the accounts in displayed order before advancing to the next ticker.
- AFTER each individual account/ticker buy or sell request returns an accepted/rejected submission result (without waiting for an exchange fill), start a newly randomized 2–3-second asynchronous delay before dispatching the next transaction. The first request has no preceding delay; wait after the last transaction as well before marking the batch complete. Never block the GUI thread.
- Example: one ticker and three accounts means account1 transaction -> random delay -> account2 transaction -> new random delay -> account3 transaction -> new random delay -> batch complete. The delay is not just once per ticker or once per batch.
- Show per-transaction success/failure results, including an understandable reason on failure, similar to Publics.
- Construct requests using the verified robin_stocks-style headers/payload conventions rather than guessing or inventing order fields.

## Implementation prerequisites and safety boundaries

- Finish the current account-information task first; inspect the actual Publics ticker parser, account-selection semantics, order construction, sequencing and results components before designing the Robinhood equivalent.
- Apply the clarified all-active-accounts scope explicitly, including brokerage/IRA/Roth IRA accounts belonging to the active login. Mirror Publics order-type/time-in-force behavior where appropriate; show batch scope clearly before the user initiates it.
- Preserve Robinhood's Android launcher disablement.
- Current browser secondary-token transport is restricted to verified read-only calls; do not assume it grants trading permissions. Implement an explicit, independently verified authenticated order boundary before enabling live Buy/Sell controls. Never widen read-only dispatch into unrestricted credentialed requests.
- Resolve instruments/accounts through validated official-origin URLs. Validate and deduplicate tickers, preserve exact quantities, and never submit a zero/negative or malformed order.
- Sell-all must use a fresh authoritative sellable position, not an offline holdings snapshot; never accidentally open a short position or sell reserved shares.
- Cancel queued-but-unsent operations on session/account switch, cancellation or expiry. Use asynchronous timers and generation guards.
- Distinguish an accepted/submitted order from a confirmed fill. A transport timeout after submission can mean unknown outcome, not a safe retry; never automatically duplicate an order with ambiguous submission state.
- Sanitize API failure reasons and never display/log credentials or tokens.
- Development verification uses synthetic responses and loopback requests only. This request authorizes implementing the feature, not executing real brokerage trades during development.

## Verification expected

- Parsing and deduplication parity with Publics; one-share buys; fresh-position sell-all including fractional shares and no-position/reserved cases.
- Every active brokerage account receives each ticker exactly once; verify account/ticker ordering and a freshly randomized 2–3-second delay AFTER each completed submission, including the final transaction. No concurrent dispatch, GUI blocking or pre-first-order delay; cancellation/expiry/account-isolation checks.
- Mixed success/rejection/transport-unknown results with safe reasons and correct submitted-versus-filled terminology.
- Request/header/payload tests against verified upstream primary source, not fabricated live responses.
- Desktop builds, QML regression coverage and independent review before delivery.
