# Findings

## Robinhood command extension
- Actual native RHSessionManager trading leases require an active, unexpired primary OAuth session and bind generation, stable saved-login owner, grant revision and complete brokerage scope; makeRHTradingDependencies already supplies current/currentLocal/currentAsync/release checks. Reuse this boundary rather than implement new order HTTP or change RH UI navigation/credential security.
- User clarified first saved login, all its brokerage accounts. Model row0 is the authority; saved slot IDs can be sparse, so literal slot0/current primary selection are not interchangeable.
- RHTradeController's existing native limits are20symbols and100 account-symbol rows; adapter must chunk all RH symbols while preserving one message-level scope/busy/report and refusing unsafe continuation, rather than silently truncate or relax native limits.
- Existing native diagnostics are display strings, not typed send proof. New initial-POST attempted/NoError+2xx/http/network fields must survive GET reconciliation unchanged; preview/accepted/sent/filled/unknown remain separate facts.
- Refreshed official Telegram Bot API sendMessage documentation: text limit remains4096 after entity parsing (https://core.telegram.org/bots/api#sendmessage). Result chunking must split safe decoded text before HTML escaping; incoming command work remains bounded to one ordinary4096char text message.
- Parent test-factory correction: TelegramAgent uses explicit URL-only constructors and setCommandLedgerPathForTests, not a generic Options/Transport API. Only the subsequently isolated missing applet constructor compile failure is API RED evidence.

## Existing architecture
- Telegram catalog currently uses a stored Robinhood/shared master key and authenticated encryption. New private policy/session fields must additionally require password-derived authenticated decryption; after migration no old protected-field copy may remain readable with that stored key alone.
- A single catalog rewrite can atomically replace legacy token/IDs with a password envelope while preserving other bots; failed writes must retain old records.
- Existing TelegramWebhookHost already owns bounded loopback QHttpServer, managed ngrok and per-bot routes/headers. Integrate the new manager without a second production port8080 bind or weakening late-registration/Stop handling.
- Existing status replies previously allowed private humans without a trading allowlist. New requirement applies allowlist validation to all received commands.
- Existing durable replay ledger deliberately persists only bot/update IDs; deletion must not remove replay history and permit old trade replay.

## Primary sources
- OWASP Password Storage Cheat Sheet (official, fetched this task): when Argon2id unavailable, scrypt N=2^17, r=8, p=1 is a recommended baseline; equivalent configurations exist. This is password-KDF guidance, not a reason to store a password hash for this encryption-only feature.
- Telegram Bot API setWebhook (official, fetched this task): uses HTTPS URL; secret_token 1..256 characters from A-Z/a-z/0-9/_/-; Telegram supplies the exact value in X-Telegram-Bot-Api-Secret-Token. Non-2xx is retried. getUpdates unavailable while webhook is registered. User IDs have at most52 significant bits.
- Canonical provider origin remains https://api.telegram.org/bot<TOKEN>/setWebhook; the user-provided telegram.org string is not the Bot API origin.
- Sources: https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html and https://core.telegram.org/bots/api#setwebhook.
