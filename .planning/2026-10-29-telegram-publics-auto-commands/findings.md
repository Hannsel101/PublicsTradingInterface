# Findings

## Independent review follow-up
- Activation must obtain the bot token and current Chat ID policy from one authenticated locked catalog snapshot. Fresh token plus cached UI metadata can restore a policy revoked by another controller while stopped.
- UI/controller/store/runtime must share the supported positive Chat ID bound; signed-64-bit acceptance currently disagrees with the transport's exact 52-bit identity limit and can silently clear valid policies.
- Eagerly constructed Publics auth objects cache nicknames; refresh committed nickname metadata before report snapshots so later UI adds/renames appear correctly.
- Key-index loading=false is not proof of complete discovery. Authoritative QtKeychain index errors/malformed data must stop the all-keys batch before credentials or orders are loaded, rather than use potentially incomplete local settings labels.
- Parent ASan confirmed and fixed a separate startController callback-deletion UAF; retain that regression across follow-up edits.

- Existing AutoTradeController::submit snapshots all saved key labels, authorizes sequentially, aggregates per-account results and waits between keys; reuse unchanged.
- Existing PublicApiWorker owns BUY-one-share and SELL-all position handling and Release/preflight gates. Telegram transport must not duplicate brokerage HTTP code.
- Existing private-human eligibility checks from.id == chat.id; a positive per-bot private Chat ID allowlist therefore authorizes the same numeric sender identity, without admitting groups/channels.
- Existing Telegram attempt ledger is bounded RAM-only and insufficient for financial command replay prevention; add a durable admission gate without persisting recipient IDs.
- Existing TelegramBotsApplet injected-controller constructor is safe for fixtures and must not instantiate production keychain-backed brokerage services. Add a service-injected overload to prove real composition offline.
