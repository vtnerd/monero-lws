# '/feed' API

The `/feed` websocket API allows for faster synchronization on initalization (msgpack
with less redundant data compared to [wallet api](/api/wallet/)), and "push"
updates instead of polling. New block arrivals, new received/spent transactions,
and reorgs are all pushed to the client in real-time. This should provide an
enterprise-like experience where the (usually mobile) client is seamlessly
synchronized with `monerod` (but only if `monero-lws-daemon` is using ZMQ
PUB/SUB from `monerod`).

## Basics
The client initiates a `/feed` websocket connection, then the LWS server sends
the entire tx history, and then finally sends a continous stream of updates from
`monerod` (ZMQ PUB mempool+blocks messages) to the client.

## Flow
### Handshake
The client must attempt a websocket "upgrade" connection to `/feed`. The
initial handshake must have a HTTP/1.1 field, `Sec-WebSocket-Protocol`, which
is set to msgpack or json:

```
Sec-WebSocket-Protocol: lws.feed.v0.msgpack
Sec-WebSocket-Protocol: lws.feed.v0.json
```

This parameter signals the protocol version and payload type. All message
payloads must match the type set in the header. Msgpack is recommended as both
serialization and de-serialization is faster, and the messages are more
compact (especially since binary crypto blobs are not encoded as hex).

### Synchronization
After the websocket handshake, the client must send a [login](#login) message,
and the server must respond with a [tx-sync](#tx-sync) message OR
[error](#error)	message. An `error` message signals the websocket is now in the
"terminated" state, a `tx-sync` message indicates that server is now ready to
send real-time updates. In either case, the client shall not send any further
data - the server will terminate the connection if this is not followed.

### Updates
After the `tx-sync`, the server will then push [blocks](#blocks),
[mempool](#mempool), [warning](#warning), or [error](#error) messages until the
stream is terminated. An `error` message indicates the stream is immediately
terminated, whereas the other messages indicate a valid stream where clients
should expected further messages.

## Message Format
Every message has a prefix before the msgpack/json payload that indicates the
type. Current valid prefixes are `login:`, `tx-sync:`, `blocks:`, `mempool:`,
`warning:`, and `error:`. The `login:` prefix is from the client, and the
remainder are sent by the server. The root type of every payload is an object.
As an example, a client login message (in the json sub-protocol), will look
like:

```json
login:{
  "account":{
    "address": "47nPhxp2cJeKN2NjamupNUNA13XgzcYPzQBCzzsKcj717s8M2UpFVmmdwSuYwgyy8kPDwU7hpEqTTDvfe5LAb9Aj6nwmEzf",
    "view_key": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205"
  }
}
```
This format (with prefixes) allows for single-pass DOMless parsing.

### login
Messages of this type are sent (only) by the client. The message contains
proof the client has authority to receives updates for a given primary address,
along with optional restrictions on data sent/pushed by the server.

[schema](schemas/feed_login.json)

#### Example
```json
login:{
   "account":{
    "address": "47nPhxp2cJeKN2NjamupNUNA13XgzcYPzQBCzzsKcj717s8M2UpFVmmdwSuYwgyy8kPDwU7hpEqTTDvfe5LAb9Aj6nwmEzf",
    "view_key": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205"
  },
  "tx_sync": false,
  "receives_only": true
}
```

### tx-sync
Messages of this type are sent only by the server, and only in response to a
successful `login:` command from the client. The message contains everything
needed for the client to have a full tx synchronization with the backend
(unless the client requested no transactions).

[schema](schemas/feed_tx_sync.json)

#### Example
```json
tx_sync:{
  "scanned_block_height": 10,
  "blockchain_height": 10,
  "lookahead_fail": 5,
  "lookahead": {"maj_i": 50, "min_i": 200 },
  "transactions": [
    {
      "hash": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
      "prefix_hash": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
      "timestamp": 0,
      "fee": 0,
      "unlock_time": 0,
      "mixin": 0,
      "payment_id": "ee171270296bbc26",
      "coinbase": true,
      "mempool": true,
      "receives": [
        {
          "id": {"legacy": {"amount": 0, "index": 0}},
          "amount": 0,
          "public_key": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
          "index": 0,
          "tx_pub_key": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
          "rct": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
          "recipient": {"maj_i": 1, "min_i": 1}
        }
      ],
      "spends": [
        {
          "id": {"unified": 0},
          "key_image": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205"
        }
      ]
    }
  ]
}
```

### blocks
Messages of this type are sent only by the server when a new block has been
scanned on the account. If another block was added to the Monero chain, this
message type is not sent until a new block has been scanned against the
account. This will also give real-time updates when account is scanning
historical blocks (although the server can choose to group/aggregate messages
to reduce total number of messages).

[schema](schemas/feed_blocks.json)

#### Example
```json
blocks:{
  "scan_start": 150,
  "scan_end": 151,
  "blockchain_height": 151,
  "lookahead_fail": 0,
  "lookahead": {"maj_i": 50, "min_i": 200 },
  "transactions": [
    {
      "hash": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
      "prefix_hash": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
      "timestamp": 0,
      "fee": 0,
      "unlock_time": 0,
      "mixin": 0,
      "payment_id": "ee171270296bbc26",
      "coinbase": true,
      "receives": [
        {
          "id": {"legacy": {"amount": 0, "index": 0}},
          "amount": 0,
          "public_key": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
          "index": 0,
          "tx_pub_key": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
          "rct": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
          "recipient": {"maj_i": 1, "min_i": 1}
        }
      ],
      "spends": [
        {
          "id": {"unified": 0},
          "key_image": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205"
        }
      ]
    }
  ]
}
```

### mempool
If the account scanner has caught-up with the most recent block, the server may
send _received outputs_ that are still in the mempool. These outputs will be
re-sent in the `blocks:` message in a transaction; the ID numbers for outputs
are not assigned until the transaction is included in a block. Spends are
assumed to be under the control of the client, and are not sent in mempool
messages.

[schema](schemas/feed_mempool.json)

#### Full Example
```json
mempool:{
  "hash": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
  "prefix_hash": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
  "timestamp": 0,
  "fee": 0,
  "unlock_time": 0,
  "mixin": 0,
  "payment_id": "ee171270296bbc26",
  "amount": 0,
  "public_key": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
  "tx_pub_key": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205",
  "index": 0,
  "recipient": {"maj_i": 1, "min_i": 1},
  "rct": "ee171270296bbc26d5be4455b6313e1a2086a92080e205f77d6f861f8e5fd205"
}
```

### warning
The primary purpose of this message type is to alert the client that the server
has detected a block re-organization. The rollback point is transmitted in the
message, and the client is expected to rollback transactions that are part of
the re-organization. Other events, such as lost connection to `monerod` are also
sent as warnings. Most other events will be [error](#error).

[schema](schemas/feed_warning.json)

#### Example
```json
warning:{
  "msg": "Descriptive warning message",
  "code": 4,
  "counter": 10,
  "height": 1000
}
```

#### Codes
```c++
  unspecified_error = 0,
  account_not_found = 1,
  bad_address = 2,
  bad_view_key = 3,
  blockchain_reorg = 4,
  daemon_unresponsive = 5,
  parse_error = 6,
  protocol_error = 7,
  queue_error = 8,
  schema_error = 9
```

### error
Servers will send an error of this type when the websocket must be closed; no
further messages are to be sent after sending this type. The codes sent in this
message type are identical to the ones [specified aboved](#codes) for warning
messages.

[schema](schemas/feed_error.json)

#### Example
```json
error:{
  "msg": "Descriptive error message",
  "code": 8
}
```
