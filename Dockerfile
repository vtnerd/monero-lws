FROM ubuntu:20.04 as monero-lws-build

# Set monerod and monero-lws versions to install from github
ARG MONERO_BRANCH=v0.17.3.0
ARG MONERO_COMMIT_HASH=ab18fea3500841fc312630d49ed6840b3aedb34d

ARG MONERO_LWS_BRANCH=release-v0.1_0.17
ARG MONERO_LWS_COMMIT_HASH=9d8a7e558aace21782217f69c3f083069538f38a

# install dependencies
RUN set -o errexit -o nounset \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get update \
    && apt-get upgrade -y -q \
    && apt-get install -y -q git build-essential cmake pkg-config libssl-dev libzmq3-dev libunbound-dev libsodium-dev libunwind8-dev \
    liblzma-dev libreadline6-dev libldns-dev libexpat1-dev libpgm-dev qttools5-dev-tools libhidapi-dev libusb-1.0-0-dev \
    libprotobuf-dev protobuf-compiler libudev-dev libboost-all-dev ccache doxygen graphviz \
    && apt-get autoclean

# install monerod
RUN git clone --recursive --depth 1 -b ${MONERO_BRANCH} https://github.com/monero-project/monero.git \
    && cd monero \
    && test `git rev-parse HEAD` = ${MONERO_COMMIT_HASH} || exit 1 \
    && make -j$(nproc) release

# install monero-lws
RUN git clone --recursive --depth 1 -b ${MONERO_LWS_BRANCH} https://github.com/vtnerd/monero-lws.git \
    && cd monero-lws \
    && test `git rev-parse HEAD` = ${MONERO_LWS_COMMIT_HASH} || exit 1 \
    && mkdir build && cd build \
    && cmake -DMONERO_SOURCE_DIR=/monero -DMONERO_BUILD_DIR=/monero/build/Linux/_no_branch_/release .. \
    && make -j$(nproc)

# expose REST server port
EXPOSE 8443

# expose commands `monero-lws-daemon` and `monero-lws-admin` via PATH
ENV PATH "$PATH:/monero-lws/build/src"

ENTRYPOINT ["monero-lws-daemon"]
