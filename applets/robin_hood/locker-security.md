# Credential locker security and integration

> Legacy storage reference: Robinhood now uses official-site browser login and
> token-only QtKeychain sessions. The password-grant, pending-password and raw
> OAuth-response diagnostic descriptions below document the former flow; they
> are not enabled in the browser applet. New browser sign-in never reads or
> writes password records. See README.md for the current browser/token boundary.

`SecureCredentialLocker` is a QObject-thread-only asynchronous credential store.
It is compiled into `rh_sessions` and links OpenSSL Crypto and QtKeychain.

## API

```cpp
using Completion = std::function<void(bool, QString)>;
using ReadCompletion = std::function<void(bool, QString, QString, QString)>;
// ReadCompletion arguments: ok, email, password, error.
using KeyCompletion = std::function<void(bool, QByteArray, QString)>;
using KeyProvider = std::function<void(bool allowCreate, KeyCompletion)>;

explicit SecureCredentialLocker(QObject *parent = nullptr);
SecureCredentialLocker(QString path, KeyProvider provider, QObject *parent = nullptr);
void save(const QString &accountId, const QString &email,
          const QString &password, Completion);
void read(const QString &accountId, ReadCompletion);
void remove(const QString &accountId, Completion);
static void wipe(QString &);
static void wipe(QByteArray &);
```

Completions are deferred, even for an immediately completing provider. Each
operation requiring a key fetches it again; the locker does not cache a master key.
Call the locker and complete injected key-provider callbacks on the locker's
QObject thread. A provider must complete exactly once; duplicate completions are
ignored. Client callbacks can start another operation or delete the locker.
Destruction cancels pending client callbacks. Concurrent operations are rejected
with a busy error, not silently queued. Errors do not return credential values.
`remove` is idempotently successful, without fetching or creating a key, when
no credential file exists; this decision is made under the file lock. If a file
exists, removal requires an available key and valid authenticated map, even if
the requested account is absent. A missing key for an existing file is an error.

## Storage and master key

The production path is exactly:

```cpp
QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/profiles.dat"
```

The fixed QtKeychain service is
`PublicsTradingInterface.RobinHood.CredentialLocker`, and the fixed entry name is
`AES256GCM.MasterKey.v1`. Every QtKeychain job disables insecure fallback.
A 32-byte master key is generated with `RAND_bytes` only after `EntryNotFound`,
only for a save, and only when no credential file exists. A permission/backend
error, invalid-length key, or missing key with an existing file fails closed.
There is no automatic destructive recovery. Losing the key means the existing
file cannot be decrypted; preserve it rather than deleting or replacing it.

The injected provider is the test seam; it must obey `allowCreate` and must not
regenerate or substitute a key for an existing file. Tests use temporary files,
injected keys, and a subprocess helper. They do not contact the OS keychain or
Robinhood.

A nonblocking `QLockFile` at `profiles.dat.lock` is held from before key retrieval
or creation through the complete read-modify-atomic-write operation, including
any asynchronous keychain wait. The parent directory is canonicalized so path
aliases use the same lock. Locks are not broken merely because an operation is
old. Contention fails explicitly, allowing the caller to retry. All cooperating
processes must use this locker/lock convention for a given file. External file
edits, changes to directory access permissions, or manual keychain edits are not
transactional with the locker. Revoking directory write permission during an
operation can prevent lock-file cleanup as well as saving. While that directory
is not writable, the live locker retains its original QLockFile/native ownership
and retries release on its event loop (or before its next operation), once access
is restored. It does not force-remove other owners' locks. Keep the locker alive
and restore directory access for this recovery; destruction while access is still
revoked, or a permission change racing Qt's actual unlink, can still leave a lock
requiring external recovery. This does not protect against hostile lock-file
replacement by the same OS user.

## Encrypted format and limits

Binary version 1 is:

| Bytes | Meaning |
|---|---|
| 0–7 | `PTILOCK` followed by a zero byte |
| 8 | format version `1` |
| 9–20 | random 96-bit nonce, freshly generated for every write |
| 21–36 | 128-bit GCM authentication tag |
| 37 onward | AES-256-GCM encrypted UTF-8 JSON |

The first nine bytes are authenticated additional data. GCM authenticates the
nonce, ciphertext and tag; no decrypted JSON is consumed before authentication
succeeds. Unknown versions, truncated/oversized files, authentication failures,
invalid JSON, and invalid record schemas fail without rewriting the file.

The plaintext is a JSON object keyed by account ID. Each value has exactly two
string fields: `email` and `password`. IDs must contain 1–256 UTF-16 code units;
emails 1–4096; passwords 1–16384. The map is limited to 1024 accounts and its
serialized UTF-8 JSON to 1 MiB. File reads are bounded to that limit plus the
37-byte envelope. Existing records are validated before reads or mutations;
adding an account cannot create an unreadable out-of-bounds map.

Writes use `QSaveFile`, explicitly disable direct-write fallback, and set
owner-read/write-only Qt file permissions before writing or committing. Existing
files survive encryption or atomic-save failure. Symlink and non-file targets
are rejected. Permissions and atomic replacement rely on the filesystem and
platform implementation; this is not protection against the same OS user, an
administrator, a hostile directory owner, or a compromised running process.
The format does not detect rollback to an older valid encrypted file.

## Authentication boundary

The locker itself does not authenticate accounts. `RHSessionManager` must call
`save` only after a successful OAuth response has a usable access token, Bearer
token type and positive expiry. Before that boundary, manual sign-in uses
`PendingCredentials`, an AES-256-GCM RAM-only staging object with a random
ephemeral key. It never contacts QtKeychain or touches disk. Login/MFA retries
unseal transient values only for dispatch. Failure, cancellation and timeout clear
staging; unsuccessful attempts do not overwrite an existing persistent record.
Reauthentication reads an already saved record without rewriting it before success.
The independent non-secret device UUID may be committed to `sessions.json` before
a login request; this is not a credential-store write. It remains stable across
accounts/restarts while that metadata is retained. Removing the app data can lose
this identifier; the implementation does not reconstruct it from hardware.
Device creation and individual metadata commits share a file lock. This does
not make the complete session/keychain transaction multi-process atomic; use a
single writer for account save/delete/renewal operations, since token-store
writes are outside that metadata lock.

Sign-in UI errors expose HTTP status, transport failure category, JSON parse
shape and allowlisted OAuth error codes. Raw bodies, server detail/error
descriptions, unknown strings, account identifiers and challenge IDs are not
copied into UI errors or default logs. These diagnostics distinguish failures
but do not prove live compatibility.

### Temporary raw login logging

In a **Debug build only**, set `PUBLICS_RH_RAW_LOGIN_DIAGNOSTICS=1` in the
process environment (Qt Creator: Projects → Run → Environment) and restart.
The Login reply handler reads the response exactly once, prints
`LOGIN HTTP STATUS:` and `RAW LOGIN RESPONSE:` before JSON conversion, then
parses the same bytes. Compressed replies have already been decoded by Qt;
`qDebug` quotes/escapes the `QByteArray` without replacing its body fields.
Raw logging also works for alternate MFA JSON and non-JSON firewall/error pages.
Only Login is logged. The outgoing header log is limited to `Accept`,
`Content-Type`, `X-Robinhood-API-Version` and the final platform `User-Agent`;
request bodies, Authorization headers and cookies are never logged.

**The raw response can contain access/refresh tokens or personal information.**
Keep console output private, do not upload it unchanged, and remove the variable
or set it to `0` after diagnosis. Opt-in raw logging is compiled out of Release
builds, regardless of the variable. The raw logger is diagnostic, not evidence
that a request was accepted or a notification delivered.

## Memory handling

`wipe` uses `OPENSSL_cleanse` on the supplied mutable buffer, then clears it.
Shared Qt buffers detach, so other copies are not changed. Operation-owned input
buffers and temporary plaintext/key buffers are wiped on normal completion or
release where possible. This is best effort, not comprehensive secure-memory
handling: Qt JSON representations, keychain/framework internals, allocator
copies, caller-owned strings, and returned credential copies may remain in
memory. Do not log credentials or expose them through QML models.

## Dedicated verification

Build target: `test_securecredentiallocker`.

```sh
cmake --build build/architecture-qt612-release --target test_securecredentiallocker
ctest --test-dir build/architecture-qt612-release \
    -R '^test_securecredentiallocker$' --output-on-failure
```

Tests cover persistence and account isolation; key retrieval on every operation;
missing, denied, invalid and wrong keys; tampered format/version/nonce/tag/data;
invalid authenticated JSON/schema; file, field and account bounds; fresh nonces;
owner-only permissions; atomic-write failure preservation; immediate-provider
completion, busy rejection, callback reentrancy and destruction; and a real
subprocess contending during key creation, followed by a read-modify-write that
preserves the other process's account.
