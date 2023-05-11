# monero-lws Administration
The `monero-lws-admin` executable or `--admin-rest-server` option in the
`monero-lws-daemon` executable can be used to administer the database
used by `monero-lws-daemon`. Any number of `monero-lws-admin` instances can run
concurrently with a single `monero-lws-daemon` instance on the same database.
Administration is necessary to authorize new accounts and rescan requests
submitted from the REST API. The admin executable can also be used to list
the contents of the LMDB file for debugging purposes.

# monero-lws-admin

The `monero-lws-admin` utility is structured around command-line arguments with
JSON responses printed to `stdout`. Each administration command takes arguments
by position. Every available administration command and required+optional
arguments are listed when the `--help` flag is given to the executable.

The [`jq`](https://stedolan.github.io/jq/) utility is recommended if using
`monero-lws-admin` in a shell environment. The `jq` program can be used for
indenting the output to make it more readable, and can be used to
search+filter the JSON output from the command.

# Admin REST API
The `monero-lws-daemon` can be started with 1+ `--admin-rest-server` parameters
that specify a listening location for admin REST clients. By default, there is
no admin REST server and no available admin accounts.

An admin REST server can be merged with a regular REST server if path prefixes
are specified, such as
`--rest-server https://0.0.0.0:8443/basic --admin-rest-server https://0.0.0.0:8443/admin`.
This will start a server listening on one port, 8443, and requires clients to
specify `/basic/command` or `/admin/admin_command` when making a
request.

An admin account account can be created via `monero-lws-admin create_admin`
_only_ (this command is not available via REST for security purposes). The
`key` value returned in the `create_admin` JSON object becomes the `auth`
parameter in the admin REST API. A new admin account is put into the
`hidden` state - the account is _not_ scanned for transactions and is _not_
available to the normal REST API, but is available to the admin REST API.

Running `monero-lws-admin list_admin` will display all current admin
accounts, and their current state ("active", "inactive", or "hidden"). If
an admin account needs to be revoked, use the `modify_account` command
to put the account into the "inactive" state. Deleting accounts is not
currently supported.

Every admin REST request must be a `POST` that contains a JSON object with
an `auth` field (in default settings) and an optional `params` field:

```json
{
  "auth":"...",
  "params":{...}
 }
```
where the `params` object is specified below. The `auth` field can be omitted
if `--disable-admin-auth` is specified in the CLI arguments for the REST
server.

## Commands (of Admin REST API)
A subset of admin commands are available via admin REST API - the remainder
are initially omitted for security purposes. The commands available via REST
are:
  * [**accept_requests**](#accept_requests): `{"type": "import"|"create", "addresses":[...]}`
  * [**add_account**](#add_account): `{"address": ..., "key": ...}`
  * [**list_accounts**](#list_accounts): `{}`
  * [**list_requests**](#list_requests): `{}`
  * [**modify_account_status**](#modify_account_status): `{"status": "active"|"hidden"|"inactive", "addresses":[...]}`
  * [**reject_requests**](#reject_requests): `{"type": "import"|"create", "addresses":[...]}`
  * [**rescan**](#rescan): `{"height":..., "addresses":[...]}`
  * [**webhook_add**](#webhook_add): `{"type":"tx-confirmation", "address":"...", "url":"...", ...}` with optional fields:
    * **token**: A string to be returned when the webhook is triggered
    * **payment_id**: 16 hex characters representing a unique identifier for a transaction
  * [**webhook_delete**](#webhook_delete): `{"addresses":[...]}`
  * [**webhook_delete_uuid**](#webhook_delete_uuid): `{"event_ids": [...]}`
  * [**webhook_list**](#webhook_list): `{}`

where the listed object must be the `params` field above.

### accept_requests
Accepts new account and rescan from block 0 requests in the incoming
queue.

### add_account
Add account for view-key scanning. An example of the JSON:
```json
{
  "params": {
    "address": "9uTcr6T9GURRt7UADQc2rhjg5oMYBDyoQ5jgx8nAvVvs757WwDkc2vHLPJhwZfCnfVdnWNvuuKzJe8eMVTKwadYzBrYRG5j",
    "key": "deadbeef"
  },
  "auth": "f50922f5fcd186eaa4bd7070b8072b66fea4fd736f06bd82df702e2314187d09"
}
```

### list_accounts
Request a listing of all active accounts in the datbase. The request
should looke like:
```bash
curl -v -H "Content-Type: application/json" -d '{}' http://127.0.0.1:8081/list_accounts
```
when auth is disabled, and when enabled:
```bash
curl -v -H "Content-Type: application/json" -d '{"auth": "f50922f5fcd186eaa4bd7070b8072b66fea4fd736f06bd82df702e2314187d09"}' http://127.0.0.1:8081/list_accounts
```
The response will look something like:
```json
{
  "active": [
    {
      "address": "9wRAu3giCtKhSsVnkZJ7LLE6zqzrmMKpPg39S8aoC7T6F6GobeDpz8TcvVfTQT3ucW82oTYKG8v3ZMAeh8SZVXWwMdvwZew",
      "scan_height": 2220875,
      "access_time": 1681244149
    }
  ]
}
```

### list_requests
This is a listing of all pending new account requests and all requests
to import from genesis block requests. When auth is disabled usage
looks like:
```bash
curl -v -H "Content-Type: application/json" -d '{}' http://127.0.0.1:8081/list_requests
```
and with auth enabled looks like:
```bash
curl -v -H "Content-Type: application/json" -d '{"auth": "f50922f5fcd186eaa4bd7070b8072b66fea4fd736f06bd82df702e2314187d09"}' http://127.0.0.1:8081/list_requests
```
### modify_account_status
This can change an account status to `active`, `inactive` or `hidden`. The
`active` state is the normal state - the account is being scanned and
returned by the API. The `inactive` state is still returned by the API,
but is no longer being scanned. The `hidden` is the current way to
"delete" an account - it is not scanned nor returned by the API. Accounts
cannot currently be deleted due to internal DB requirements.

### reject_requests
This is the opposite of [`accept_requests`](#accept_requests) above. See
information from that endpoint on how to use this one.

### rescan
This tells the scanner to rescan specific account(s) from the specified
height.

### webhook_add
This is used to track a specific payment ID to an address or all general
payments to an address (where payment ID is zero). Using this endpint requires
a web address for callback purposes, a primary (not integrated!) address, and
finally the type ("tx-confirmation"). The event will remain in the database
until one of the delete commands ([webhook_delete_uuid](#webhook_delete_uuid)
or [webhook_delete](#webhook_delete)) is used to remove it.

> The provided URL will use SSL/TLS if `https://` is prefixed in the URL and
will use plaintext if `http://` is prefixed in the URL. SSL/TLS connections
will use the system certificate authority (root-CAs) by default, and will
ignore all authority checks if `--webhook-ssl-verification none` is provided
on the command line when starting `monero-lws-daemon`. The webhook will fail
if there is a mismatch of `http` and `https` between the two servers, and
will also fail if `https` verification is mismatched. The rule is: (1) if
the callback server has SSL/TLS disabled, the webhook should use `http://`,
(2) if the callback server has a self-signed certificate, `https://` and
`--webhook-ssl-verification none` should be used, and (3) if the callback
server is using "Let's Encrypt" (or similar), then `https://` with no
additional command line flag should be used.


#### Initial Request to server
Example where admin authentication is required (`--disable-admin-auth` NOT
set on start which is the default):
```json
{
  "auth": "f50922f5fcd186eaa4bd7070b8072b66fea4fd736f06bd82df702e2314187d09",
  "params": {
    "type": "tx-confirmation",
    "url": "http://127.0.0.1:7000",  
    "payment_id": "df034c176eca3296",
    "token": "1234",
    "address": "9uTcr6T9GURRt7UADQc2rhjg5oMYBDyoQ5jgx8nAvVvs757WwDkc2vHLPJhwZfCnfVdnWNvuuKzJe8eMVTKwadYzBrYRG5j"
  }
}
```

Example where admin authentication is not required (`--disable-admin-auth` set on start):
```json
{
  "params": {
    "type": "tx-confirmation",
    "url": "http://127.0.0.1:7000",  
    "payment_id": "df034c176eca3296",
    "token": "1234",
    "address": "9uTcr6T9GURRt7UADQc2rhjg5oMYBDyoQ5jgx8nAvVvs757WwDkc2vHLPJhwZfCnfVdnWNvuuKzJe8eMVTKwadYzBrYRG5j"
  }
}
```

As noted above - `payment_id` and `token` are both optional - `token` will
default to the empty string, and `payment_id` will default to zero.
##### Initial Response from Server
The server will replay all values back to the user for confirmation. An                 
additional field - `event_id` - is also returned which contains a globally
unique value (internally this is a 128-bit `UUID`).

Example response:
```json
{
  "payment_id": "df034c176eca3296",
  "event_id": "fa10a4db485145f1a24dc09c19a79d43",
  "token": "1234",
  "confirmations": 1,
  "url": "http://127.0.0.1:7000"
}
```

If you use the `debug_database` command provided by the `monero-lws-admin`
executable, the event should be listed in the
`webhooks_by_account_id,payment_id` field of the returned JSON object. The
event will remain in the database until an explicit
[`webhook_delete_uuid`](#webhook_delete_uuid) is invoked.

#### Callback from Server
When the event "fires" due to a transaction, the provided URL is invoked
with a JSON payload that looks like the below:

```json
{
  "event": "tx-confirmation",
  "payment_id": "df034c176eca3296",
  "token": "1234",
  "confirmations": 1,
  "id": "fa10a4db485145f1a24dc09c19a79d43",
  "tx_info": {
    "id": {
      "high": 0,
      "low": 5550229
    },
    "block": 2192100,
    "index": 0,
    "amount": 4949570000,
    "timestamp": 1678324181,
    "tx_hash": "901f9a2a919b6312131537ff6117d56ce2c0dc1f1341b845d7667299e1ef892f",
    "tx_prefix_hash": "89685cb7acb836fde30fae8be5d8b884e92706df086960d0508e146979ef80dc",
    "tx_public": "54c153792e47c1da8ceb3979560c424c1928b7b4a089c1c8b3ce99c563e1d240",
    "rct_mask": "f3449407dc3721299b5309c0c336a17daeebce55165ddd447ba28bbd1f46c201",
    "payment_id": "df034c176eca3296",
    "unlock_time": 0,
    "mixin_count": 15,
    "coinbase": false
  }
}
```
which is the same information provided by the user API. The database will
contain an entry in the `webhook_events_by_account_id,type,block_id,tx_hash,output_id,payment_id,event_id`
field of the JSON object provided by the `debug_database` command. The
entry will be removed when the number of confirmations has been reached.

### webhook_delete
Deletes all webhooks associated with a specific Monero primary address.

### webhook_delete_uuid
Deletes all references to a specific webhook referenced by its UUID
(`event_id`)

### webhook_list
This will list every webhook that is currently "listening" for
incoming transactions. If the server has auth disabled, the
request is simply:

```bash
curl -v -H "Content-Type: application/json" -d '{}' http://127.0.0.1:8081/webhook_list
```
and with auth enabled looks like:
```bash
curl -v -H "Content-Type: application/json" -d '{"auth": "f50922f5fcd186eaa4bd7070b8072b66fea4fd736f06bd82df702e2314187d09"}' http://127.0.0.1:8081/webhook_list
```

which returns a JSON object that looks like:
```json
{
  "webhooks": [
    {
      "key": {
        "user": 1,
        "type": "tx-confirmation"
      },
      "value": [
        {
          "payment_id": "9bc1a59b34253896",
          "event_id": "4dc201838af54dfe88686bea7e2b599f",
          "token": "12345",
          "confirmations": 5,
          "url": "http://127.0.0.1:8082"
        },
        {
          "payment_id": "9bc1a59b34253896",
          "event_id": "615171e477464401a1a23cdb45b3b433",
          "token": "12345",
          "confirmations": 5,
          "url": "http://127.0.0.1:8082"
        },
        {
          "payment_id": "9bc1a59b34253896",
          "event_id": "e64be3ad6d1647618fbd292be0485901",
          "token": "this is a fresh test",
          "confirmations": 1,
          "url": "http://127.0.0.1:8082/foobar"
        },
        {
          "payment_id": "9bc1a59b34253896",
          "event_id": "fe692cdf7de1453898ad453d8fabce42",
          "token": "12345",
          "confirmations": 5,
          "url": "http://127.0.0.1:8082/foobar"
        }
      ]
    }
  ]
}
```

# Examples

## Admin REST API

### Default Settings
```json
{
  "auth":"6d732245002a9499b3842c0a7f9fc6b2d657c77bd612dbefa4f7f9357d08530a",
  "params":{
    "status": "inactive",
    "addresses": ["9sAejnQ9EBR1111111111111111111111111111111111AdYmVTw2Tv6L9KYkHjJ2wd737ov8ZL5QU7CJ4zV6basGP9fyno"]
  }
 }
```
will put the listed address into the "inactive" state.

### `--disable-admin-auth` Setting
```json
{
  "params":{
    "status": "inactive",
    "addresses": ["9sAejnQ9EBR1111111111111111111111111111111111AdYmVTw2Tv6L9KYkHjJ2wd737ov8ZL5QU7CJ4zV6basGP9fyno"]
  }
 }
```

## monero-lws-admin

**List every active Monero address on a newline:**
  ```bash
  monero-lws-admin list_accounts | jq -r '.active | .[] | .address'
  ```

**Auto-accept every pending account creation request:**
  ```bash
  monero-lws-admin accept_requests create $(monero-lws-admin list_requests | jq -j '.create? | .[]? | .address?+" "')
  ```
# Debugging

`monero-lws-admin` has a debug mode that dumps everything stored in the
database, except the blockchain hashes are always truncated and viewkeys are
omitted by default (a command-line flag can enable viewkey output). Most of
the array outputs are sorted to accelerate `jq` filtering and search queries.

## Indexes

 - **blocks_by_id** - array of objects sorted by block height.
 - **accounts_by_status,id** - A single object where account status names are
   keys. Each value is an array of objects sorted by account id.
 - **accounts_by_address** - A single object where account addresses are keys.
   Each value is an object containing the status and account id for the account
   for lookup in `accounts_by_status,id`. The majority of account lookups should
   be done by this id (an integer).
 - **accounts_by_height,id** - An array of objects sorted by block height. These
   objects contain another array of objects sorted by account id.
 - **outputs_by_account_id,block_id,tx_hash,output_id** - An object where keys
   are account ids. Each value is an array of objects sorted by block height,
   transaction hash, then by output number.
 - **spends_by_account_id,block_id,tx_hash,image** - An object where keys are
   account ids. Each value is an array of objects sorted by block height,
   transaction hash, then by key image.
 - **requests_by_type,address** - An object where keys are request type, and
   each value is an array of objects sorted by address.

## Examples

**List every key-image associated with every account:**
  ```bash
  monenero-lws-admin debug_database | jq '."spends_by_account_id,block_id,tx_hash,output_id" | map_values([.[] | .image])'
  ```
will output something like:
  ```json
  {"1":["image1", "image2",...],"2":["image1","image2"...],...}
  ```

**List every account that received XMR in a given transaction hash:**
  ```bash
  monenero-lws-admin debug_database | jq '."outputs_by_account_id,block_id,tx_hash,output_id" | map_values(select([.[] | .tx_hash == "hash"] | any)) | keys'
  ```
will output somethng like:
  ```json
  {"1",...}
  ```

**Add total received XMR for every account**:
  ```bash
  monenero-lws-admin debug_database | jq '."outputs_by_account_id,block_id,tx_hash,output_id" | map_values([.[] | .amount] | add)'
  ```
will output something like:
  ```json
  {"1":6346,"2":45646}
  ```

# Extending Administration in monero-lws

## JSON via `stdin`

Some commands take sensitive information such as private view keys, and
therefore reading arguments from `stdin` via JSON array would also be useful for
those situations. This should be a relatively straightforward adaptation given
the design of the positional arguments.

## Administration via ZeroMQ

The LMDB database does account lookups by view-public only, so that CurveZMQ
(which uses curve25519) can be used to authenticate an administration account
without additional protocol overhead. The parameters to administration commands
can be sent via JSON or MsgPack array since the functions already use positional
arguments.
