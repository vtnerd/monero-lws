# monero-lws Rate Limiting

monero-lws has built-in rate limiting by connection, IP address, or account
depending on the context. The server will return code `429` or `503` depending
how a limit has been hit. The `429` error codes can be turned off by setting
`--rate-limit 0` as a runtime option, OR by build time option by setting
`config::rate::max_calls_per_second = 0` in `src/config.h`. Disabling by build
time option has roughly a ~15% increase in performance, but this is difficult
to measure accurately (it is likely less).

The default is `--rate-limit=50`, which is ~50 API calls per-second per
account, or ~12 API calls per second by connection/IP. A weighting for
`/get_random_outs` and `/login` when `"create_account": true` restrict those
endpoints usage further.

Every `rate::config::max_calls_window` (default `5 seconds`) the
calls-per-second calculation is re-targeted. This technically allows for
`rate::config::max_calls_window * --rate-limit` bursts in the last second of
processing, but this gets immediately re-adjusted at the next re-targeting. The
current mechanism for "gaming" the system is to flood the system every
`rate::config::max_calls_window` seconds, but this just averages to
calls-per-second, so there is little to be gained other than co-ordinating a
DDoS this way. 

## Rate Limit Modes
### By-Connection
The rate limiting is done by connection when the remote IP is loopback or
private IP UNTIL a viewkey for an enabled account is provided. This helps with
Tor in particular - the hidden service can have PoW enabled to prevent DoS
attacks, and each connection is throttled further by the rate limiting
implementation. The limits by connection are `--rate-limit / 4`.

### By-IP
The rate limiting is done by IP when the remote IP is public UNTIL a viewkey
for an enabled account is provided. The limits by IP are
`--rate-limit / 4`. The rate limits per IP are slowly aged out after all
connections close, so an attacker cannot gain an advantage by slamming and
closing connections.

### By-Account
After a user provides a viewkey for an enabled account, the existing "call
count" per connection/IP/account is transferred to the "new" account such that
an attacker cannot gain an advantage by switching accounts. The limits per
account are `--rate-limit`. The rate limits per account are slowly aged out
after all connections close, so an attacker cannot gain an advantage by
slamming and closing connections.

## Rate Limits (`429` HTTP error code)
Each API call counts as `1` towards the per-second usage. `/get_random_outs`
counts as `1 + config::rate::get_random_outs_weight` which defaults to `21`,
and `/login` for a new account counts as
`1 + config::rate::account_creation_weight` which defaults to `41`.

### `/get_random_outs`
The default settings allow for ~1.6 `/get_random_outs` calls per-second unless
tracking by-account is being done, and then ~2.3 calls per second afterward.
Existing client implementations call `/get_unspent_outs` with a viewkey before
calling  `/get_random_outs`, so in practice all clients have enough limits for
constructing a TX (unless fee estimation is particularly difficult).

### `/login` with account creation
The default settings allow for ~0.31 account creations per second unless
tracking by acccount where ~1.25 account creations per second are permitted.
Creating an account after calling `/login` to an existing account is a
strange edge case but permitted.

## Hard Limits (`503` HTTP error code)
Several endpoints (`/daemon_status`, `/get_random_outs`, `/get_unspent_outs`,
and `/submit_raw_tx`) are asynchronous due to calls into `monerod` to
complete the response. All of these endpoints have build-time configurable
limits (in `src/config.h`) for buffering while the `monerod` calls complete. If
the hard-limit is reached a `503` error is returned. The goal to is prevent the
server from hitting a out-of-memory termination by the OS.

### `config::rate::daemon_wait_queue_max`
This build-time option, defaulted to `25,000`, limits the number of queued
requests to the `/daemon_status`, `/get_random_outs`, and `get_unspent_outs`
endpoints. If the user has never provided a viewkey to an enabled account, the
limit imposed is `config::rate::daemon_wait_queue_max / 4`. This allows for
valid account holders to continue service while unregistered users get
throttled.

### `config::rate::submit_tx_max`
This build-time option, defaulted to `100 MiB`, limits the bytes buffered for
transactions to be broadcast to the Monero network. If the user has never
provided a viewkey to an enabled account, the limit imposed is
`config::rate::submit_tx_max / 4`. This allows for valid account holders to
continue service while unregistered users get throttled.

## Connection Limits
There are no limits on the number of connections currently. If you need such
limits, configuration in the upstream proxy (nginx, tord, etc) are recommended.
A future release may have better built-in support; general DDoS protections
are likely better suited to other projects.

