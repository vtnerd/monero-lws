# Initial base from https://github.com/sethforprivacy/monero-lws/blob/588c7f1965d3afbda8a65dc870645650e063e897/Dockerfile

# Set monerod version to install from github
ARG MONERO_BRANCH=v0.17.3.0
ARG MONERO_COMMIT_HASH=ab18fea3500841fc312630d49ed6840b3aedb34d

# Select ubuntu:20.04 for the build image base
FROM ubuntu:20.04 as build

# Install all dependencies for a static build
# Added DEBIAN_FRONTEND=noninteractive to workaround tzdata prompt on installation
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get upgrade --no-install-recommends -y

RUN apt-get install --no-install-recommends -y \
    build-essential \
    ca-certificates \
    ccache \
    cmake \
    doxygen \
    git \
    graphviz \
    libboost-all-dev \
    libexpat1-dev \
    libhidapi-dev \
    libldns-dev \
    liblzma-dev \
    libpgm-dev \
    libprotobuf-dev \
    libreadline6-dev \
    libsodium-dev \
    libssl-dev \
    libudev-dev \
    libunbound-dev \
    libunwind8-dev \
    libusb-1.0-0-dev \
    libzmq3-dev \
    pkg-config \
    protobuf-compiler \
    qttools5-dev-tools \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Set necessary args and environment variables for building Monero
ARG MONERO_BRANCH
ARG MONERO_COMMIT_HASH
ARG NPROC
ENV CFLAGS='-fPIC'
ENV CXXFLAGS='-fPIC -DELPP_FEATURE_CRASH_LOG'
ENV USE_SINGLE_BUILDDIR 1
ENV BOOST_DEBUG         1

# Switch to Monero source directory
WORKDIR /monero

# Git pull Monero source at specified tag/branch and compile monerod binary
RUN git clone --recursive --branch ${MONERO_BRANCH} \
    https://github.com/monero-project/monero . \
    && test `git rev-parse HEAD` = ${MONERO_COMMIT_HASH} || exit 1 \
    && git submodule init && git submodule update \
    && mkdir -p build/release && cd build/release \
    # Create make build files manually for release-static-linux-x86_64
    && cmake -D STATIC=ON -D ARCH="x86-64" -D BUILD_64=ON -D CMAKE_BUILD_TYPE=release -D BUILD_TAG="linux-x64" ../.. \
    # Build only monerod binary using number of available threads
    && cd /monero && nice -n 19 ionice -c2 -n7 make -j${NPROC:-$(nproc)} -C build/release daemon

# TODO: remove the need to manually make this static liblmdb_lib.a
RUN cd /monero/build/release/src/lmdb && make && cd /monero

# Switch to monero-lws source directory
WORKDIR /monero-lws

COPY . .

ARG NPROC
RUN set -ex \
    && git submodule init && git submodule update \
    && rm -rf build && mkdir build && cd build \
    && cmake -D STATIC=ON -D MONERO_SOURCE_DIR=/monero -D MONERO_BUILD_DIR=/monero/build/release .. \
    && make -j${NPROC:-$(nproc)}

# Begin final image build
# Select Ubuntu 20.04LTS for the image base
FROM ubuntu:20.04

# Added DEBIAN_FRONTEND=noninteractive to workaround tzdata prompt on installation
ENV DEBIAN_FRONTEND=noninteractive

# Upgrade base image
RUN apt-get update \
    && apt-get upgrade --no-install-recommends -y

# Install necessary dependencies
RUN apt-get install --no-install-recommends -y \
    ca-certificates \
    curl \
    jq \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Add user and setup directories for monero-lws
RUN useradd -ms /bin/bash monero-lws \
    && mkdir -p /home/monero-lws/.bitmonero/light_wallet_server \
    && chown -R monero-lws:monero-lws /home/monero-lws/.bitmonero
USER monero-lws

# Switch to home directory and install newly built monero-lws binary
WORKDIR /home/monero-lws
COPY --chown=monero-lws:monero-lws --from=build /monero-lws/build/src/* /usr/local/bin/

# Expose REST server port
EXPOSE 8443

ENTRYPOINT ["monero-lws-daemon", "--db-path=/home/monero-lws/.bitmonero/light_wallet_server"]
CMD ["--daemon=tcp://monerod:18082", "--sub=tcp://monerod:18083", "--log-level=4"]
