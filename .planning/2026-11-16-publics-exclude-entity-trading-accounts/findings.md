# Findings

- Independent bounded source review confirmed no introduced P1/P2; Business display label cannot grant eligibility. Actual parent cross-kit six-suite verification passed against the same unchanged source, including native Publics unit/Auto integration, Publics applet, Telegram Publics/composite services and actual bridge.

- PublicApiWorker::fetchAccounts is the shared eligibility boundary for selected-key manual/preflight and Publics Auto. It previously allowed ENTITY along with BROKERAGE/ROTH_IRA/TRADITIONAL_IRA. Publics Telegram services reuse that same worker, so the narrower policy follows naturally without remote-protocol edits.
- m_accountList is private, cleared on session replacement and populated from filtered discovery; addAccountToList/applyDiscoveredAccounts are private legacy helpers, not QML/public trading entry points. Business display-label formatting is presentation only and remains for historical rendering.
- Real loopback integration fixture already had mixed ENTITY accounts and positive assertions. Reversed those superseded expectations; assert no ENTITY portfolios, requests or result rows, preserve supported account BUY/SELL sizing/order, and test an ENTITY-only key has no eligible accounts.
