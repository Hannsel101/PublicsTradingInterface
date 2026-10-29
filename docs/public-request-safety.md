# Public request pacing and uncertain orders

No API versions or application versions are changed by this policy.

## Configuration

The worker reads the application's existing `QSettings` store at construction,
under the `PublicRequestPolicy` group. Restart the app after changing these
values. There is no new settings screen. Embedders and offline tests can call
`PublicApiWorker::setRequestPolicy()` between operations; changes during loading
or trading are ignored.

| Key | Default | Accepted range |
| --- | ---: | ---: |
| `portfolioPauseMs` | 500 | 0–60000 |
| `symbolPauseMs` | 100 | 0–60000 |
| `accountPauseMs` | 200 | 0–60000 |
| `gatewayCooldownMs` | 5000 | 0–300000 |
| `retryBaseMs` | 1000 | 1–60000 |
| `retryCapMs` | 30000 | base–300000 |
| `maxRetries` | 3 | 0–5 |
| `timeoutMs` | 30000 | 1–120000 |

Portfolios are loaded one at a time, preserving eligible account order and
removing duplicate account IDs. The pause begins after completion, including
before the first portfolio and after the last portfolio before trading starts.
Account-list and portfolio GETs retry HTTP 429/502/503/504 with exponential
backoff and up to 25% positive jitter, bounded by the cap. `Retry-After` delta
seconds or HTTP GMT date can lengthen that delay beyond the backoff cap.

A valid empty `positions` array is a known empty portfolio. Missing/malformed
positions, invalid equity quantities, duplicate equity positions, and failed
requests are **not** empty portfolios. Such accounts remain in result rows as
**Not submitted**, for buys as well as sells. All other eligible accounts still
run. Even when every portfolio fails, Auto mode retains the account rows and
continues to the next saved key. The existing inter-key pause is unchanged.

## Live submission safety

Each POST gets one UUID, retained as `orderId` in its result row. A successful
live acknowledgment must echo this ID, as required by the documented response;
a status-only or mismatched-ID response triggers read-only reconciliation.
POSTs are never
automatically retried, including preflight. Redirects are not followed.
Gateway failures and transport loss apply a longer cooldown before proceeding.
A timeout, 429, 408, 409, server error, malformed acknowledgment, or redirect
cannot prove a live order failed.

For an ambiguous live POST, the worker only performs the documented read:

`GET /userapigateway/trading/{accountId}/order/{orderId}`

It requires the original ID and a recognized order status to confirm the order.
404 may mean the asynchronous order is not visible yet; it is retried within the
same bounded read budget, never interpreted as permission to resubmit. A failed,
mismatched, malformed, or exhausted lookup ends as **Outcome uncertain**, not a
definite trade failure. The UI retains the order ID and tells the user to check
Public before submitting again. A confirmed acknowledgment is not a fill
confirmation. Cancelling a session also leaves in-flight live submissions
uncertain rather than pretending that aborting the HTTP request cancelled them.

The ID/result is held in memory, not a durable order journal. Application exit
or a subsequent batch replaces that in-memory history. Verify unresolved orders
in Public before another submission. Reconciliation is a bounded check, not an
ongoing fill monitor.

Reference contracts (documentation only; no production API calls used):
- https://public.com/api/docs/resources/order-placement/place-order
- https://public.com/api/docs/resources/order-placement/get-order

## Diagnostics and verification

Logs contain operation, HTTP status, numeric network error, latency, retry
number, account index/generation for portfolios, symbol/side for submissions,
and locally generated order-ID correlation. Every request also has a locally
generated request UUID (not an untrusted server header). They do not contain authorization
headers, tokens, API keys, account IDs, response bodies, or server error text.

Offline tests use a loopback TCP server only. Coverage includes partial and
all-failed portfolios, sequential completion-based pacing, bounded retries,
Retry-After, deterministic backoff clock/jitter inputs, gateway/timeouts and
malformed live responses, original-ID reconciliation, continued multi-account
and multi-key traversal, and the uncertain/not-submitted QML labels.
