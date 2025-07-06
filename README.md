# monero-lws

> This project is **NOT** a part of the official monero "core" code, but will
> hopefully be merged into that project as a new repository seperate from the
> [`monero-project/monero`](https://github.com/monero-project/monero)
> repository.

## Table of Contents

  - [Introduction](#introduction)
  - [About this project](#about-this-project)
  - [License](#license)
  - [Compiling Monero-lws from source](#compiling-monero-lws-from-source)


## Introduction

Monero is a private, secure, untraceable, decentralised digital currency. You are your bank, you control your funds, and nobody can trace your transfers unless you allow them to do so.

**Privacy:** Monero uses a cryptographically sound system to allow you to send and receive funds without your transactions being easily revealed on the blockchain (the ledger of transactions that everyone has). This ensures that your purchases, receipts, and all transfers remain absolutely private by default.

**Security:** Using the power of a distributed peer-to-peer consensus network, every transaction on the network is cryptographically secured. Individual wallets have a 25 word mnemonic seed that is only displayed once, and can be written down to backup the wallet. Wallet files are encrypted with a passphrase to ensure they are useless if stolen.

**Untraceability:** By taking advantage of ring signatures, a special property of a certain type of cryptography, Monero is able to ensure that transactions are not only untraceable, but have an optional measure of ambiguity that ensures that transactions cannot easily be tied back to an individual user or computer.

**Decentralization:** The utility of monero depends on its decentralised peer-to-peer consensus network - anyone should be able to run the monero software, validate the integrity of the blockchain, and participate in all aspects of the monero network using consumer-grade commodity hardware. Decentralization of the monero network is maintained by software development that minimizes the costs of running the monero software and inhibits the proliferation of specialized, non-commodity hardware.


## About this project

This is an implementation of the [Monero light-wallet REST API](https://github.com/monero-project/meta/blob/master/api/lightwallet_rest.md)
(i.e. MyMonero compatible). Clients can submit their Monero viewkey via the REST
API, and the server will scan for incoming Monero blockchain transactions.

Differences from [OpenMonero](https://github.com/moneroexamples/openmonero):
  - LMDB instead of MySQL
  - View keys stored in database - scanning occurs continuously in background
  - Uses ZeroMQ interface to `monerod` with chain subscription ("push") support
  - Uses amd64 ASM acceleration from Monero project, if available
  - Supports webhook notifications, including "0-conf" notification

## License

See [LICENSE](LICENSE).

## Compiling Monero-lws from source

### Dependencies

The first step, when buiding from source, is installing the [dependencies of the
monero project](https://github.com/monero-project/monero?tab=readme-ov-file#dependencies).
`monero-lws` depends on the monero project for building, so the dependencies of
that project become the dependencies of this project transitively. Only the
"non-vendored" dependencies need to be installed. There are no additional
dependencies that need installing.

### Beginner Build

The easiest method for building is to use git submodules to pull in the correct
Monero project dependency:

```bash
git clone https://github.com/vtnerd/monero-lws.git
mkdir monero-lws/build && cd monero-lws/build
git submodule update --init --recursive
cmake -DCMAKE_BUILD_TYPE=Release ../
make -j$(nproc)
```

  * On macOS replace `-j$(nproc)` with `-j8` or the number of cores on your
    system.
  * Each branch (`master`, and `develop`) should have the correct submodule for
    building that particular branch.
  * The `submodule` step is being run inside of the `monero-lws` directory, such
    that all of vendored dependencies are automatically fetched.
  * The instructions above will compile the `master` (beta) release of
    `monero-lws`
  * The resulting executables can be found in `monero-lws/build/src`

### Advanced Build

The monero source and build directories can be manually specified, which cuts
the build time in half and potentially re-uses the same source tree for multiple
builds. The process for advanced building is:

```bash
git clone https://github.com/monero-project/monero.git
git clone https://github.com/vtnerd/monero-lws.git
mkdir monero-lws/build
mkdir monero/build && cd monero/build
git submodule update --init --recursive
cmake -DCMAKE_BUILD_TYPE=Release ../
make -j$(nproc) daemon multisig lmdb_lib
cd ../../monero-lws/build
cmake -DCMAKE_BUILD_TYPE=Relase -DMONERO_SOURCE_DIR=../../monero -DMONERO_BUILD_DIR=../../monero/build ../
make -j$(nproc)
```

The `master`/`develop` branches of lws should compile against the `master`
branch of monero. The release branches specify which monero branch to compile
against - `release-v0.3_0.18` indicates that monero `0.18` should be used as
the source directory. The [beginner build process](#beginner-build) handles all
of this with git submodules.

  * On macOS replace `-j$(nproc)` with `-j8` or the numebr of cores on your
    system.
  * Notice that the `submodule` step is only being run in the `monero` project;
    `monero-lws` intentionally does not initialize submodules in the advanced
    mode of building.
  * The instructions above will compile the `master` (beta) release of
    `monero-lws`.
  * The resulting executables can be found in `build/src`

## Running monero-lws-daemon

The build places the binary in `src/` sub-directory within the build directory
from which cmake was invoked (repository root by default). To run in
foreground:

```bash
./src/monero-lws-daemon
```

To list all available options, run `./src/monero-lws-daemon --help`.
