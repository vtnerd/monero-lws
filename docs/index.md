# monero-lws Overview

[monero-lws](https://github.com/vtnerd/monero-lws) is a Monero light-wallet
server that adheres to the community ratified
[LWS spec](https://github.com/monero-project/meta/blob/master/api/lightwallet_rest.md).
The server does all "cryptographic scanning" that is required of a Monero
wallet, and is non-custodial (the "spend" keys are never sent to server). It is
compatible with [Skylight wallet](https://skylight.magicgrants.org), and 
[lwcli](https://github.com/cifro-codes/lwcli).

## Background-

Monero wallets require complex scanning/decryption against _every_ transaction
to determine if funds were sent to that wallet. This process can be offloaded
to an outside process **without** giving up the "spend" keys of the wallet. The
LWS API provides a standard mechanism for splitting the "scanning" process from
the "spend" process.

The only negative of splitting this process is privacy - the "LWS server" knows
all incoming funds and can typically infer outgoing funds as well. This privacy
issue can be mitigated by self-hosting a LWS server. `monero-lws` and
[OpenMonero](https://github.com/moneroexamples/openmonero)
are the two options for self-hosting.

## Clients

The two available wallets are [lwcli](/lwcli/) and
[Skylight wallet](https://skylight.magicgrants.org). `lwcli` is a TUI wallet
aimed at advanced users, whereas `Skylight` is a beginner wallet.

Programmers wishing to create a new LWS wallet should investigate the
[lwsf](https://github.com/vtnerd/lwsf) project which provides a simple C++
interface of typical wallet functions while performing all of the heavy lifting
of communicating to the server and creating transactions.

## Versions

The `develop` branch is considered "nightly", and the `master` branch is
considered the "alpha" branch. Only developers should use `develop` because
DB changes could be incompatible from commit to commit.

After changes are well tested, they are moved to the `master` branch which
should rarely see incompatible DB changes. Users of this branch should
check the `src/lws_version.h.in` file - a major (first) number change indicates
that a incompatible DB change has been made. You will be unable to "roll-back"
to a prior `master` commit unless you save your DB and re-use it with that
older commited.

The `release-v` branches indicate beta/stable versions of the software. The
major (first) number indicates the DB version. Upgrades from lower numbers
to higher numbers is possible, but the reverse is never true. The minor
(second) number refers to stability: new features are never imported into a
release branch, instead a new release with higher major/minor number are used.
