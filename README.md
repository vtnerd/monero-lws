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
    - [Dependencies](#dependencies)


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


## License

See [LICENSE](LICENSE).


## Compiling Monero-lws from source

### Dependencies

The following table summarizes the tools and libraries required to build. A
few of the libraries are also included in this repository (marked as
"Vendored"). By default, the build uses the library installed on the system,
and ignores the vendored sources. However, if no library is found installed on
the system, then the vendored source will be built and used. The vendored
sources are also used for statically-linked builds because distribution
packages often include only shared library binaries (`.so`) but not static
library archives (`.a`).

| Dep          | Min. version  | Vendored | Debian/Ubuntu pkg    | Arch pkg     | Void pkg           | Fedora pkg          | Optional | Purpose         |
| ------------ | ------------- | -------- | -------------------- | ------------ | ------------------ | ------------------- | -------- | --------------- |
| GCC          | 4.7.3         | NO       | `build-essential`    | `base-devel` | `base-devel`       | `gcc`               | NO       |                 |
| CMake        | 3.1           | NO       | `cmake`              | `cmake`      | `cmake`            | `cmake`             | NO       |                 |
| Boost        | 1.58          | NO       | `libboost-all-dev`   | `boost`      | `boost-devel`      | `boost-devel`       | NO       | C++ libraries   |
| monero       | 0.15          | NO       |                      |              |                    |                     | NO       | Monero libraries|
| OpenSSL      | basically any | NO       | `libssl-dev`         | `openssl`    | `libressl-devel`   | `openssl-devel`     | NO       | sha256 sum      |
| libzmq       | 3.0.0         | NO       | `libzmq3-dev`        | `zeromq`     | `zeromq-devel`     | `zeromq-devel`      | NO       | ZeroMQ library  |
| Doxygen      | any           | NO       | `doxygen`            | `doxygen`    | `doxygen`          | `doxygen`           | YES      | Documentation   |
| Graphviz     | any           | NO       | `graphviz`           | `graphviz`   | `graphviz`         | `graphviz`          | YES      | Documentation   |

Install all dependencies (except `monero-project/monero`) at once on Debian/Ubuntu:

``` sudo apt update && sudo apt install build-essential cmake libboost-all-dev libssl-dev libzmq3-dev doxygen graphviz```

FreeBSD 12.1 one-liner required to build dependencies:
```pkg install git gmake cmake pkgconf boost-libs libzmq4```

### Cloning the repository

Clone recursively to pull-in needed submodule(s):

`$ git clone --recursive https://github.com/vtnerd/monero-lws.git`

If you already have a repo cloned, initialize and update:

`$ cd monero-lws && git submodule init && git submodule update`

### Build instructions

Monero uses the CMake build system and a top-level [Makefile](Makefile) that
invokes cmake commands as needed.

#### On Linux and macOS

* Install the dependencies. The [`monero`](https://github.com/monero-project/monero)
  dependency should be cloned and built according to the `README.md` of that
  project.
* Change to the root of the source code directory, change to the most recent develop branch, and build:

    ```bash
    cd monero-lws
    git checkout develop
    mkdir build && cd build
    cmake -DMONERO_SOURCE_DIR=~/monero -DMONERO_BUILD_DIR=~/monero/build ..
    make
    ```

    *Optional*: If your machine has several cores and enough memory, enable
    parallel build by running `make -j<number of threads>` instead of `make`. For
    this to be worthwhile, the machine should have one core and about 2GB of RAM
    available per thread.

    *Note*: The instructions above will compile the development release of the
    Monero-lws software.

* The resulting executables can be found in `build/src`

* Add `PATH="$PATH:$HOME/monero-lws/build/src"` to `.profile`

* Run Monero-lws with `monero-lws-daemon`

Dependencies need to be built with -fPIC. Static libraries usually aren't, so you may have to build them yourself with -fPIC. Refer to their documentation for how to build them.

* **Optional**: build documentation in `doc/html` (omit `HAVE_DOT=YES` if `graphviz` is not installed):

    ```bash
    HAVE_DOT=YES doxygen Doxyfile
    ```

## Running monero-lws-daemon

The build places the binary in `src/` sub-directory within the build directory
from which cmake was invoked (repository root by default). To run in
foreground:

```bash
./src/monero-lws-daemon
```

To list all available options, run `./src/monero-lws-daemon --help`.
