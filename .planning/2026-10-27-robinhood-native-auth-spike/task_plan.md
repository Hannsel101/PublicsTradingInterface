# Native Robinhood authentication compatibility spike
## Goal
Build an isolated, manually operated native Qt login experiment based on pinned robin_stocks wire behavior; verify shared desktop/Android compilation and offline state transitions before any production replacement.
## Scope
- New spikes/robinhood-native-auth/ only, plus this planning directory. Preserve production browser, trading, Publics, Android-disabled launcher and all prior dirty changes.
- No real credentials, provider authentication, keychain writes or trades during agent verification. User manually operates the standalone GUI when ready; no login on startup.
- No secret logs, raw responses, plaintext token persistence, credential CLI flags or arbitrary provider endpoints. Typed C++ grant handoff only; this experiment does not save credentials or tokens.
- Native password-grant compatibility is unofficial and contrary to modern OAuth recommendations; no MFA/device approval bypass, inferred success or guaranteed provider acceptance.
## Feasibility question
Given a shared Qt Core/Network client and a native interactive test UI, can the client follow token/verification protocols safely and compile for desktop and Android without a WebView?
## Phases
1. Pin upstream protocol and define interface. Status: complete.
2. Implement isolated client, offline regressions and manual test GUI. Status: complete.
3. Build desktop and Android, run offline tests and independent review. Status: complete.
4. Deliver runnable artifact, actual results and explicit live/device limits. Status: complete.
5. Add reviewed safe finite-vocabulary HTTP400 diagnostics and deliver rebuilt artifacts; no authentication protocol change or live agent retry. Status: complete.
6. Record user-confirmed desktop full-grant acquisition after header alignment; individual causal header not isolated. Status: complete.
7. Align verified upstream request headers with loopback regressions, review and rebuilt artifacts; provider rejection cause remains unproven. Status: complete.
## Deferred
Production integration, account/trading authorization, Android launcher enablement and the old renewal cancellation-review issue are not part of this compatibility experiment.
