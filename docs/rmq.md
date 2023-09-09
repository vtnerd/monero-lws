# monero-lws RabbitMQ Usage
Monero-lws uses RabbitMQ to provide JSON notifications of payment_id
(web)hooks. The feature is optional - by default LWS is _not_ compiled with
RabbitMQ support. The cmake option -DWITH_RMQ=ON must be specified during the
configuration stage to enable the feature.

The notification location is specified with `--rmq-address`, the login details
are specified with `--rmq-credentials` (specified as `user:pass`), the exchange
is specified with `--rmq-exchange`, and routing name is specified with
`--rmq-routing`. 

## `json-full-payment_hook`
Only events of this type are sent to RabbitMQ. They are always "simulcast" with
the webhook URL. If the webhook URL is `zmq` then no simulcast occurs - the
event is only sent via ZeroMQ and/or RabbitMQ (if both are configured then it
sent to both, otherwise it is only sent to the one configured).

Example of the "raw" output to RabbitMQ:

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
> `index` is a counter used to detect dropped messages. It is not useful to
RabbitMQ but is a carry-over from ZeroMQ (the same serialized message is used
to send to both).

> The `block` and `id` fields in the above example are NOT present when
`confirmations == 0`.
