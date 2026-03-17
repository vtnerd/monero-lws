# Webhooks / ZeroMQ
monero-lws can report scanner events via HTTP/1.1 webhooks or ZeroMQ PUB/SUB.
The ZeroMQ PUB is distinct from the `monerod` PUB socket;
`monero-lws-daemon --zmq-pub` creates a socket just for `monero-lws-daemon`
scanner updates. Webhooks can be "http" or "https" - the latter performing
standard system certificate authority checks (self-signed certificates NOT
allowed).

Events are ALWAYS published over ZMQ-PUB, whereas webhooks are only used when
a specific URL is given to `/webhook_add`. If the provided URL is `zmq` then
only ZMQ-PUB is used to push the event. All other URLs must start with
`http://` or `https://` as per [admin api spec](/api/admin).

## Registration
Registering for events requires `/webhook_add` from the
[admin api](/api/admin). Examples and full schema are provided on that page.

## Events
Every callback for an event has this structure:
```json
{
  "index": 0,
  "event": {...}
}
```
The `index` field is per event type, and tracks the number of events in that
type. This is useful only for ZeroMQ, where events can be dropped.

### Payments
A payment event occurs from 0-confirmations up to the user requested number of
confirmations. A payment event, even when using ZeroMQ, only occurs when one is
registered via `/webhook_add`. Payment registerations can only be done on
primary addresses and optionally a payment id. Registering a primary address
automatically registers all subaddresses; specifying a payment id only
registers that payment id. Leaving the payment id field blank during
registeration enables hooks from all payment ids.

The relevant topics for ZeroMQ-PUB are: `json-full-payment_hook`, and
`msgpack-full-payment_hook`.

Example payload of a webhook callback:
```json
{
  "index": 2,
  "event": {
    "event": "tx-confirmation",
    "payment_id": "4f695d197f2a3c54",
    "token": "single zmq wallet",
    "confirmations": 1,
    "event_id": "3894f98f5dd54af5857e4f8a961a4e57",
    "tx_info": {
      "id": {
        "high": 0,
        "low": 5666768
      },
      "block": 2265961,
      "index": 1,
      "amount": 3117324236131,
      "timestamp": 1687301600,
      "tx_hash": "ef3187775584351cc5109de124b877bcc530fb3fdbf77895329dd447902cc566",
      "tx_prefix_hash": "064884b8a8f903edcfebab830707ed44b633438b47c95a83320f4438b1b28626",
      "tx_public": "54dce1a6eebafa2fdedcea5e373ef9de1c3d2737ae9f809e80958d1ba4590d74",
      "rct_mask": "4cdc4c4e340aacb4741ba20f9b0b859242ecdad2fcc251f71d81123a47db3400",
      "payment_id": "4f695d197f2a3c54",
      "unlock_time": 0,
      "mixin_count": 15,
      "coinbase": false
    }
  }
}

```

### Spends
All of the information in the [payments](#payments) section is relevant to
here, except instead of payment confirmations, spend confirmations are
published. The relevant ZeroMQ-PUB topics are: `json-full-spend_hook` and
`msgpack-full_spend_hook`. Example payload of a webhook callback:

```json
{
  "index": 0,
  "event": {
    "event": "tx-spend",
    "token": "spend-xmr",
    "event_id": "7ff047aa74e14f4aa978469bc0eec8ec",
    "tx_info": {
      "input": {
        "height": 2464207,
        "tx_hash": "97d4e66c4968b16fec7662adc9f8562c49108d3c5e7030c4d6dd32d97fb62540",
        "image": "b0fe7acd9e17bb8b9ac2daae36d4cb607ac60ed8a101cc9b2e1f74016cf80b24",
        "source": {
          "high": 0,
          "low": 6246316
        },
        "timestamp": 1711902214,
        "unlock_time": 0,
        "mixin_count": 15,
        "sender": {
          "maj_i": 0,
          "min_i": 0
        }
      },
      "source": {
        "id": {
          "high": 0,
          "low": 6246316
        },
        "amount": 10000000000,
        "mixin": 15,
        "index": 0,
        "tx_public": "426ccd6d39535a1ee8636d14978581e580fcea35c8d3843ceb32eb688a0197f7"
      }
    }
  }
}
```

### New Accounts
When a new account is registered, regardless of the `--auto-accept-creation`
flag for `monero-lws-daemon`, the system will publish events. Registration
occurs in the [admin api](/api/admin). The ZeroMQ-PUB topics are
`json_full-new_account` and `msgpack-full-new_account`. Example of callback:

```json
{
  "index": 2,
  "event": {
    "event": "new-account",
    "event_id": "c5a735e71b1e4f0a8bfaeff661d0b38a",
    "token": "",
    "address": "9zGwnfWRMTF9nFVW9DNKp46aJ43CRtQBWNFvPqFVSN3RUKHuc37u2RDi2GXGp1wRdSRo5juS828FqgyxkumDaE4s9qyyi9B"
  }
}
```

### Updated Accounts
You can register to be notified when the scanner has updated _any_ account,
and the event will batch the notifications per-thread. The ZeroMQ-PUB topics
are `json-minimal-scanned` and `msgpack-minimal-scanned`. Example of
callback:

```json
{
  "index": 13,
  "event": {
    "height": 2438536,
    "id": "9197e1c6f3de28a98dfc579325903e5416ef1ba2681043c54b5fff0d39645a7f",
    "addresses": [
      "9xkhhJSa7ZhS5sAcTix6ozL14RwdgxbV7JZVFW4rCghN7GidutaykfxDHfgW45UPiCTXncuvZ91GNSGgxs3b2Cin9TU8nP3"
    ]
  }
}
```
