# Initial base from https://github.com/sethforprivacy/monero-lws/blob/588c7f1965d3afbda8a65dc870645650e063e897/Dockerfile

# Set monerod version to install from github
ARG MONERO_COMMIT_HASH=d32b5bfe18e2f5b979fa8dc3a8966c76159ca722

# Select ubuntu:22.04 for the build image base
FROM ubuntu:22.04 as build

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
    libgnutls30 \
    libldns-dev \
    liblzma-dev \
    libprotobuf-dev \
    librabbitmq-dev \
    libsodium-dev \
    libssl-dev \
    libudev-dev \
    libunwind8-dev \
    libusb-1.0-0-dev \
    pkg-config \
    wget \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Set necessary args and environment variables for building Monero
ARG MONERO_BRANCH
ARG MONERO_COMMIT_HASH
ARG NPROC
ARG TARGETARCH
ENV CFLAGS='-fPIC'
ENV CXXFLAGS='-fPIC -DELPP_FEATURE_CRASH_LOG'
ENV USE_SINGLE_BUILDDIR 1
ENV BOOST_DEBUG         1

# Build expat, a dependency for libunbound
RUN set -ex && wget https://github.com/libexpat/libexpat/releases/download/R_2_7_5/expat-2.7.5.tar.bz2 && \
    echo "386a423d40580f1e392e8b512b7635cac5083fe0631961e74e036b0a7a830d77 expat-2.7.5.tar.bz2" | sha256sum -c && \
    tar -xf expat-2.7.5.tar.bz2 && \
    rm expat-2.7.5.tar.bz2 && \
    cd expat-2.7.5 && \
    ./configure --enable-static --disable-shared --prefix=/usr && \
    make -j${NPROC:-$(nproc)} && \
    make -j${NPROC:-$(nproc)} install

# Build libunbound for static builds
WORKDIR /tmp
RUN set -ex && wget https://www.nlnetlabs.nl/downloads/unbound/unbound-1.24.2.tar.gz && \
    echo "44e7b53e008a6dcaec03032769a212b46ab5c23c105284aa05a4f3af78e59cdb unbound-1.24.2.tar.gz" | sha256sum -c && \
    tar -xzf unbound-1.24.2.tar.gz && \
    rm unbound-1.24.2.tar.gz && \
    cd unbound-1.24.2 && \
    ./configure --disable-shared --enable-static --without-pyunbound --with-libexpat=/usr --with-ssl=/usr --with-libevent=no --without-pythonmodule --disable-flto --with-pthreads --with-libunbound-only --with-pic && \
    make -j${NPROC:-$(nproc)} && \
    make -j${NPROC:-$(nproc)} install

# Build libzmq for static builds
WORKDIR /tmp
RUN set -ex && wget https://github.com/zeromq/libzmq/releases/download/v4.3.5/zeromq-4.3.5.tar.gz && \
    echo "6653ef5910f17954861fe72332e68b03ca6e4d9c7160eb3a8de5a5a913bfab43 zeromq-4.3.5.tar.gz" | sha256sum -c && \
    tar -xzf zeromq-4.3.5.tar.gz && \
    rm zeromq-4.3.5.tar.gz && \
    cd zeromq-4.3.5 && \
    ./configure --disable-shared --enable-static --without-libsodium --disable-libunwind --with-pic && \
    make -j${NPROC:-$(nproc)} && \
    make -j${NPROC:-$(nproc)} install

# Build boost for latest security updates
WORKDIR /tmp
RUN set -ex && wget https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2 && \
    echo "49551aff3b22cbc5c5a9ed3dbc92f0e23ea50a0f7325b0d198b705e8ee3fc305 boost_1_90_0.tar.bz2" | sha256sum -c && \
    tar -xf boost_1_90_0.tar.bz2 && \
    rm boost_1_90_0.tar.bz2 && \
    cd boost_1_90_0 && \
    ./bootstrap.sh && \
    ./b2 -j${NPROC:-$(nproc)} runtime-link=static link=static threading=multi variant=release \
      --with-chrono --with-context --with-coroutine --with-date_time --with-filesystem --with-locale \
      --with-program_options --with-regex --with-serialization --with-serialization install

# Switch to monero-lws source directory
WORKDIR /monero-lws

COPY . .

ARG NPROC
RUN set -ex \
    && git submodule update --init --recursive \
    && rm -rf build && mkdir build && cd build \
    && cmake -D CMAKE_BUILD_TYPE=Release -D STATIC=ON -D LWS_DEAD_FUNCTION_REMOVAL=ON -D BUILD_TESTS=ON -D WITH_RMQ=ON .. \
    && make -j${NPROC:-$(nproc)} monero-lws-admin monero-lws-daemon monero-lws-unit \
    && ./tests/unit/monero-lws-unit

# Begin final image build
# Select Ubuntu 20.04LTS for the image base
FROM ubuntu:22.04

# Add user and setup directories for monero-lws
RUN useradd -ms /bin/bash monero-lws \
    && mkdir -p /home/monero-lws/.bitmonero/light_wallet_server \
    && chown -R monero-lws:monero-lws /home/monero-lws/.bitmonero
USER monero-lws

# Switch to home directory and install newly built monero-lws binary
WORKDIR /home/monero-lws
COPY --chown=monero-lws:monero-lws --from=build /monero-lws/build/src/monero-lws-daemon /usr/local/bin/
COPY --chown=monero-lws:monero-lws --from=build /monero-lws/build/src/monero-lws-admin /usr/local/bin/

# Expose REST server port
EXPOSE 8443

ENTRYPOINT ["monero-lws-daemon"]
CMD ["--daemon=tcp://monerod:18082", "--sub=tcp://monerod:18083", "--log-level=4"]
