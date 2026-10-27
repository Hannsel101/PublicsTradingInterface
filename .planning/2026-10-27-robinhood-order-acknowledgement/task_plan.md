# Robinhood order acknowledgement and safe verification
## Goal
Recognize valid provider-shaped order receipts, continue serial accounts only after confirmed acceptance, and reconcile ambiguous receipts using one bounded read-only order lookup without submitting duplicates.
## Scope and safety
Robinhood only. Preserve prior dirty work, auth/QtKeychain, penny pricing/frozen preflight, uppercase input, active-account scope, one-share buys/sell-all, serial order/final 2–3s delay, lease/generation guards, Debug preview/Release live policy and Android disablement. No commits or live provider/account/keychain/trade access by agent. Never retry an order POST or infer acceptance from changed holdings. Do not resume the user's old interrupted batch automatically.
## Investigation
User reports first account order appears in holdings but app marks Unknown and stops. Exact HTTP response is unavailable. Ranked hypotheses: (1) valid receipt lacks the currently mandatory display-symbol echo; (2) another required correlation/state field differs; (3) network/HTTP/parse ambiguity after broker acceptance. Parent reproduced hypothesis1 with two minimal receipt fixtures: actual Unknown versus expected Submitted/Filled, with strong account/instrument/ref/side/decimal-quantity matches.
## Contract
- Symbol may be absent; when present it must be a matching string. Account, instrument, request ref_id, side, exact quantity, safe broker order ID and recognized state remain mandatory.
- Ambiguous response with safe candidate broker id: one typed primary-only GET /orders/{id}/. Require exact id and full request correlation on the authoritative response before continuing. Never use the client Reference ID as the broker order ID.
- No safe id, failed/mismatched lookup, timeout or unrecognized state stays Unknown and stops. Cancellation/session expiry while looking up an already attempted POST remains Unknown, not unsent/cancelled.
- Diagnostics expose only allowlisted reason codes, HTTP/Qt error integers and field-category failures; no response bodies, headers, tokens or private field values.
## Ownership
Backend child: controller/network headers and sources plus unit controller/network and trading-context tests. Parent added the initial two-row RED test, then handed that file over; parent owns plans/docs/full builds/review/packages only.
## Current Phase
Complete
## Next Step
Deliver the reviewed archive with no-replay, live-order and offline-verification caveats.
## Phases
1. Evidence, tight reproduction and contract: complete.
2. Receipt/reconciliation implementation with RED/GREEN: complete.
3. Independent review and full desktop/Android verification: complete.
4. Rebuild package and deliver no-retry/old-batch caveats: complete.
