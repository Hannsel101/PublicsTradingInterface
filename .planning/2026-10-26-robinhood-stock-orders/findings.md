# Findings
- Queued spec is ../2026-10-26-robinhood-account-information/next-task-stock-orders.md; user explicitly clarified all active brokerage accounts and delay AFTER every account/ticker result.
- Publics StockTrade.qml provides comma-separated single/multi-ticker input, Buy and Sell All; orderPayload uses MARKET and DAY. Publics actual live order calls are Release-only; Debug uses preflight. Publics parser strips all nonletters, which can change class-share tickers; Robinhood must not silently transform a ticker into a different security.
- Existing browser login stores only WebSecondaryBearer, with deliberately scoped read-only dispatch. Trading needs a distinct primary official-API bearer candidate validated by identity/accounts; a secondary cookie is not trading authorization.
## Verified upstream order contract
- Fetched actual upstream primary source over verified-TLS system curl: https://raw.githubusercontent.com/jmfernandes/robin_stocks/master/robin_stocks/robinhood/orders.py , urls.py and globals.py.
- Generic order payload includes account URL, instrument URL, symbol, quantity, ref_id UUID, side, type, trigger, time_in_force, regular market_hours, extended_hours=False, order_form_version=4; quoted ask/bid and a local bid_ask_timestamp are provided.
- For regular-hours market buy the SDK changes type to limit and adds preset_percent_limit='0.05'; do not claim a plain uncollared market-buy contract. Regular-hours market sell removes price. Use gfd to match Publics DAY rather than the SDK default gtc.
- orders_url(account_number) uses /orders/?account_numbers=<account_number>. Global headers are Accept */*, form-urlencoded Content-Type, X-Robinhood-API-Version 1.431.4, language/connection headers; Qt should negotiate compression itself.
- No live service acceptance or order permission verified by source inspection. No credentials/authenticated requests used.
