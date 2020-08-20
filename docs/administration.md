# Using monero-lws-admin

The `monero-lws-admin` executable is used to administer the database used by
`monero-lws-daemon`. Any number of `monero-lws-admin` instances can run
concurrently with a single `monero-lws-daemon` instance on the same database.
Administration is necessary to authorize new accounts and rescan requests
submitted from the REST API. The admin executable can also be used to list
the contents of the LMDB file for debugging purposes.

# Basics

The `monero-lws-admin` utility is structured around command-line arguments with
JSON responses printed to `stdout`. Each administration command takes arguments
by position - the design makes it potentially compatible with a JSON or MsgPack
array (as used in JSON-RPC, etc). Every available administration command and
required+optional arguments are listed when the `--help` flag is given to the
executable.

The [`jq`](https://stedolan.github.io/jq/) utility is recommended if using
`monero-lws-admin` in a shell environment. The `jq` program can be used for
indenting the output to make it more readable, and can be used to
search+filter the JSON output from the command.

# Examples

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
