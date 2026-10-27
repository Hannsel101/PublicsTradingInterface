# Findings
- Unknown is a local inability to verify the POST outcome, not proof that the broker rejected it. Existing halt/no-retry prevents duplicate purchases and must remain for true ambiguity.
- Current controller requires response.symbol and ref_id along with account/instrument/side/quantity/id/state. Test server normally echoes request fields, masking unsupported response-schema assumptions.
- Pinned primary upstream1b4ef98be36d1df127886889c843256799a1dfea find_stock_orders translates a symbol filter to an instrument URL; get_stock_order_info performs GET orders_url(orderID), and urls.py builds /orders/{orderID}/. Parent fetched these public sources with verified system curl, without authenticated provider traffic.
- A missing display-symbol echo alone reproduces the user's first-order Unknown/stop symptom in deterministic loopback tests, both queued and filled. This confirms a parser defect but does NOT establish the exact fields in the user's unseen live response.
- New verification uses safe server-issued candidate order id, not the client-generated Reference ID. No account-history scan/ref-id query contract is fabricated.
- Correlation remains strong: exact request reference plus account, instrument, side, quantity and safe id; optional present symbol still validated. A lookup cannot assert Filled unless broker state explicitly says filled.
- Current old batch cannot safely be replayed wholesale: first account may already hold the purchased share. New code must not claim it can resume historical in-memory state or skip based merely on holdings.
