# Pinned candidate authentication protocol

Source: `jmfernandes/robin_stocks` commit `1b4ef98be36d1df127886889c843256799a1dfea`, committed 2026-02-11T02:42:31Z. Relevant upstream files: `authentication.py`, `helper.py`, `urls.py`, `globals.py`. Upstream MIT notice is retained in `UPSTREAM_LICENSE.txt`.

This describes observed client behavior, not a documented provider contract or proof of live acceptance.

| Stage | Method / fixed-origin path | Body / decision |
|---|---|---|
| Initial grant | POST `/oauth2/token/` | Form password grant; upstream public client ID, username/password, original device token, internal scope, requested expiry86400, try_passkeys=False, token_request_path=/login, create_read_only_secondary_token=True; optional upfront mfa_code. Honor actual returned expiry. |
| Workflow creation | POST `/pathfinder/user_machine/` | JSON device_id, flow=suv, input.workflow_id. Require returned machine ID. |
| Inquiry | GET `/pathfinder/inquiries/{machine_id}/user_view/` | Bounded periodic polling of context.sheriff_challenge. |
| App approval | GET `/push/{challenge_id}/get_prompts_status/` | Require challenge_status=validated, then confirm workflow. |
| SMS/email code | POST `/challenge/{challenge_id}/respond/` | Form response=<user code>; require status=validated, then confirm workflow. |
| Workflow confirmation | POST inquiry path | JSON sequence=0, user_input.status=continue. Require type_context.result OR verification_workflow.workflow_status equal workflow_status_approved. |
| Final grant | POST token path once | Repeat original password-grant payload only after explicit workflow approval; require genuine complete grant. |

Use per-request form/JSON headers, not mutable shared global content types. Selected compatibility headers now match the pinned client: Accept=*/*, Accept-Language=en-US,en;q=1, X-Robinhood-API-Version=1.431.4 and its literal User-Agent=*. Form requests specify charset=utf-8. These correct a measured fidelity mismatch, not an established cause of the user's HTTP400; Qt still owns compression/connection negotiation and its HTTP/TLS stack differs from Python Requests. Python form booleans are `False`/`True`. Qt negotiates compression rather than copying an unsupported Accept-Encoding list.

The experiment tightens upstream behavior: finite request/whole-attempt deadlines, bounded responses, cancellation/generation checks, no overlapping polls, no unbounded push loop, no inferred approval on timeout, no password/token persistence or Python pickle loading. Unknown states, denial and transport ambiguity fail closed.

Unsupported: legacy top-level challenge/mfa_required continuation schemas, passkeys/WebAuthn, CAPTCHA/browser handoff, recovery/resend and a newly issued workflow after the single approved retry. An authenticator code may be supplied on the initial request, as upstream supports; this is not support for arbitrary new MFA response protocols.

The initial-grant test does not establish account identity or trading permission. Production identity/account validation and secure persistence are separate integration steps and remain unchanged by this experiment.
