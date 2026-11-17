# Findings

- Independent source-only review confirmed the explicit four-type allowlist, nonempty IDs/deduplication, portfolio/manual/Auto/preflight routing, holdings safeguards, labels and README match the latest request, with no introduced P1/P2. Actual parent cross-kit six-suite results separately verified against one unchanged source fingerprint.

- PublicApiWorker::fetchAccounts remains the common eligibility boundary; adding ENTITY restores selected-key/manual/preflight and all-key Auto support without UI/Telegram/Robinhood protocol changes. Business display-label mapping was retained and needs no change. Unknown account types must remain excluded.
- Existing ENTITY exclusion tests encode the superseded request and must now assert positive native order/account/holdings results. New ENTITY-only regression covers BUY and SELL through both preflight and live loopback endpoints, one share BUY versus four held shares SELL. Auto preserves account order A/E/B/C and all four supported products.
