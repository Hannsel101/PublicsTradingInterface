# Findings

## Existing source boundaries
- RHNativeAuthClient already supports Pathfinder approved-device push and code challenges, fixed sanitized rejection summaries, origin/TLS/redirect guards, per-request16sec deadline, default120sec total and64poll bound. Remote opt-in must extend total/budget only, preserve desktop defaults and fail closed rather than trusting timeout.
- RHSessionManager stores nativeEmail/nativePassword only inside encrypted RHOAuthGrant envelope, validates user identity/primary grants/account scope, and commits through pinned credential revisions/reservations. New C++ callback APIs must keep credentials inside this layer.
- TelegramBotsApplet currently captures bot/request/broker with one global busy lease. Combined service holds Publics report settlement before starting RH. Add auth under same lease and separate non-boundary RH warning notices.
- Single Mode actual source: initializeSession deserializes valid grant then unconditionally invokes renewOAuthGrant; this explains token/expiry changing on every open. checkSessionRenewal additionally targets the last120sec. Verify original valid bearer without extending expiry, preserving actual provider lifetime rather than inventing seven days.

## Primary research
- Inspected current primary robin_stocks authentication source (jmfernandes/robin_stocks, robin_stocks/robinhood/authentication.py); endpoint contract matches existing native Pathfinder/push implementation. Its timeout comments that assume approval are NOT an acceptable implementation; local native implementation stays explicitly fail closed.
- Qt6.12 QTimer documentation confirms PreciseTimer never fires early (coarse may). Preserve existing2500ms PreciseTimer and use monotonic total deadlines/precise timers for remote eight-minute budget; asynchronous GUI lifecycle only.

## Integrated verification findings
- Independent bounded closure deleg_ed9491b8 confirms hard-status ordering fix and five known-status regressions resolve the transport P2, without claiming unavailable401 metadata is observable. No introduced issue found in that source-only inspection. Native/adapter P2 closure was independently confirmed by deleg_7650ae15.
- Auth notification scope must travel with queued attempts after final completion clears the command ticket. Recover one uncertain auth notification without retry or origin loss; generic/trading and known hard authorization failures still stop.
- Known HTTP401/403/409 must outrank recoverable response size/deadline failures. Actual Qt6.12 loopback diagnostics showed header-only401 remains status0 until body completes, consistent with primary Qt QHttpNetworkConnectionChannel::allDone handling; do not invent unavailable status. Temporary numeric diagnostics and speculative metadata/authentication-signal hooks were removed. Tests cover observable oversized401 and oversized/held-body403/409.
- Revoking an allowlist suppresses command/report authority without requiring the read-only polling transport to stop. Assert only getUpdates and zero new dispatch after revocation.
- Auto Mode's prior unit fixture assumed unexpired OAuth activation rotates; corrected to verify saved secret and actual expiry unchanged with zero renewal. The246-order all-session regression needs a larger test-only drain budget under parallel sanitizer kits; production deadlines/pacing unchanged.
