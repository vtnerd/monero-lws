# monero-lws-daemon

`monero-lws-daemon` hosts the HTTP/1.1 server components of the
[wallet](/api/wallet) and [admin](/api/admin) API.
The wallet API is enabled by default on `http://127.0.0.1:8443`, and the admin
API is disabled by default. The daemon will listen for wallet API requests on
every `--rest-server` argument (multiple can be supplied). The same properties
apply to `--admin-rest-server` with the admin API.

The wallet API and admin API can be merged into one server instance through a
shared prefix: `--rest-server http://127.0.0.1:8443/wallet` combined with
` --admin-rest-server http://127.0.0.1:8443/admin` will listen on one port for
both APIs, and the client software must use `/wallet` or `/admin` to access
the correct API.

## SSL/TLS

Every REST endpoint that starts with `https://` will only communicate via
SSL/TLS. By default, a new certificate is created each time the lws daemon
starts. The commands `--rest-ssl-key` and `--rest-ssl-certificate` allow for
explicit server keys to be used, and they are applied to _every_ `https://`
REST argument (both wallet API and admin API).

## Subaddresses

Subaddresses are disabled by default in the lws daemon - all relevant endpoints
will fail. The option `--max-subaddresses`, when specified with a non-zero
value, will enable subaddress endpoints and impose restrictions on their
creation. If the value is lowered in the future, existing subaddresses are not
retroactively removed, so the limit is enforced purely at creation time.

If `--max-subaddresses` is zero or left unspecified, it will always disable
subaddresses, even if a user previously created some. So it is generally
recommended that once enabled, they always be enabled, even if the value
is lowered to `1` to discourage their use.

## ZeroMQ Sub

The daemon supports ZeroMQ SUB from `monerod` for instant mempool and block
notification. The `monerod` instance must use the option `--zmq-pub` and
`monero-lws-daemon` must use the `--sub` option. Both executables support
`tcp://` and `ipc://` for the rendevous location.

When a new block is processed by `monerod`, the lws daemon will immediately
begin processing as a result of the notification. Without proper `--sub`
usage, the lws daemon will poll the `monerod` instance at 20 second intervals.

The wallet API will **NOT** report mempool transactions due to a limitation
of the design. Webhooks **will** report mempool ("0-conf") transactions.

## Webhooks

The [admin API](/api/admin) has an endpoint, `/webhook_add`, with an `url`
field that must start with `http://`, `https://`, or `zmq`. See the link to
the API for specs/schema.

Every webhook is "simulcast" over an optional ZeroZQ PUB socket specified with
`--zmq-pub` (which is different from `monerod --zmq-pub`). If the webhook was
created with `params.url == zmq` then the notification occurs only over ZeroMQ.

The `params.address` field  of `/webhook_add`, when needed as per spec,  must
be a primary address. Subaddresses registered with the wallet API are
automatically reported to any primary address webhook. Omit `payment_id` to
report on _every_ transaction to that account.

See [webhooks documentation](/usage/webhooks) for more information.

## Remote Scanning

The view-key scanning process can be spread to multiple machines with the
`--lws-server-addr` option. The remote side should use the
[`monero-lws-client`](#monero-lws-client) executable, and connect back to the
`monero-lws-daemon` location. Usage of `--lws-server-pass` is highly
recommended when doing remote scanning, as it will prevent rogue processes
from hijacking the scan process. Also consider specifying the password in a
 `--config-file`, otherwise the process list will leak the password.

The protocol is a custom unencrypted binary format, so `ssh -L ...` should be
used on the `monero-lws-client` side to provide proper encryption, and
additional authentication).

