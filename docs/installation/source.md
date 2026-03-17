# From Source

## Branches
Building from source requires deciding on a branch to compile:

  * `develop` is where all merges go first. So this the "nightly" branch, and
    is generally only recommended for developers. This should compile against
    the `master` branch of `monerod`.
  * `master` is where tested merges go - making this the "alpha" branch.
    Recommended only if some new feature(s) are needed. This should compile
    against the `master` branch of `monerod`.
  * `release-v...` - The first number specifies the `monero-lws` version and
    the second number specifies the `monerod` version. Example:
    `release-v0.3_0.18` is the v0.3 version of `monero-lws` to be compiled
    against the  v0.18 version of `monerod`. The most recent release branch is
    recommended for the bulk of users. The supported release branches are:
      * release-v0.3_0.18
 
## Dependencies
After deciding on a branch, the next step is dependency installation. The only
dependencies are those required by `monerod` - go to
the [`monerod` dependency section](https://github.com/monero-project/monero/?tab=readme-ov-file#dependencies)
and follow those instructions for installing dependencies. Then come back to
this guide.

## Building
### Beginner

The "beginner" method for compilation lets the git submodules do all of the
hard work, but is only available on `develop` and `master` branches. The
process is simply:

```bash
git clone https://github.com/vtnerd/monero-lws.git
git checkout master
git submodule update --init --recursive
mkdir monero-lws/build && cd monero-lws/build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

If on macOS, replace `-j$(nproc)` with `-j8` (or the number of cores on your
system). This should produce three binaries in the `monero/build/src` folder:
`monero-lws-daemon`, `monero-lws-admin`, and `monero-lws-client`. You can now
move onto [usage](/monero-lws/usage/).

### Advanced

If you already have the `monerod` source tree on your system, you can save some
space by re-using that copy. You need to manually ensure that the `monerod`
branch is the one needed by `monero-lws`. The
[first instruction](#building-from-source) specifies the expected `monerod`
branch foreach `monero-lws` branch.

The process skips the submodule init and specifies the monero tree manually:

```bash
git clone https://github.com/vtnerd/monero-lws.git
git checkout origin/release-v0.3_0.18
mkdir monero-lws/build && cd monero-lws/build
cmake -DMONERO_SOURCE_DIR=/path/to/monero_0.18/source -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

Again, replace `-j$(nproc)` on macOS with appropriate core count. This should
produce three executables in the `monero-lws/build/src` directory. Move onto
[usage](/monero-lws/usage/) portion.

### Other Build Options

Advanced users/developers may wish to specify additional build-time options
supported by the project. The easiest is specifying `-DSTATIC=ON` at the
cmake stage, which tries to use static libraries where possible.

The other available option is `-DBUILD_TESTS=ON` which can also be specified at
the cmake stage. This creates an executable `tests/unit/monero-lws-unit`.

The last option is `-DSANITIZER=address` which can be specified at the cmake
stage. This builds both monero source tree and lws source tree with the address
sanitizer.
