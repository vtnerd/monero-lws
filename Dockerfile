# Initial base from https://github.com/sethforprivacy/monero-lws/blob/588c7f1965d3afbda8a65dc870645650e063e897/Dockerfile

# Select ubuntu:22.04 for the build image base
FROM stagex/pallet-clang-cmake-busybox AS build
COPY --from=stagex/core-filesystem . /
COPY --from=stagex/core-ca-certificates . /
COPY --from=stagex/core-make . /
COPY --from=stagex/core-git . /
COPY --from=stagex/core-curl . /
COPY --from=stagex/core-openssl . /
COPY --from=stagex/user-libsodium . /
COPY --from=stagex/user-zeromq /usr/lib/. /usr/lib
COPY --from=stagex/user-zeromq /usr/lib64/. /usr/lib64
COPY --from=stagex/user-zeromq /usr/include/. /usr/include
COPY --from=stagex/user-eudev . /
COPY --from=stagex/user-libusb . /
COPY --from=stagex/core-sqlite3 . /

# Build expat (for unbound)
RUN <<-EOF
        curl -L -O https://github.com/libexpat/libexpat/releases/download/R_2_7_5/expat-2.7.5.tar.bz2
        echo "386a423d40580f1e392e8b512b7635cac5083fe0631961e74e036b0a7a830d77 expat-2.7.5.tar.bz2" | sha256sum -c
        tar -xf expat-2.7.5.tar.bz2
        cd expat-2.7.5
        ./configure --enable-static --disable-shared --prefix=/usr
        make -j$(nproc)
        make install
EOF

# Build unbound (not in stagex packages)
RUN <<-EOF
        cd /
        curl -L -O https://www.nlnetlabs.nl/downloads/unbound/unbound-1.24.2.tar.gz
        echo "44e7b53e008a6dcaec03032769a212b46ab5c23c105284aa05a4f3af78e59cdb unbound-1.24.2.tar.gz" | sha256sum -c
        tar -xzf unbound-1.24.2.tar.gz
        cd unbound-1.24.2
        ./configure \
                --disable-shared \
                --enable-static \
                --without-pyunbound \
                --with-libexpat=/usr \
                --with-ssl=/usr \
                --with-libevent=no \
                --without-pythonmodule \
                --disable-flto \
                --with-pthreads \
                --with-libunbound-only \
                --with-pic
         make -j$(nproc) 
         make install
EOF

# Build Boost with ability to strip functions (reduces file size heavily)
RUN <<-EOF
         cd /
         curl -L -O https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2
         echo "49551aff3b22cbc5c5a9ed3dbc92f0e23ea50a0f7325b0d198b705e8ee3fc305 boost_1_90_0.tar.bz2" | sha256sum -c
         tar -xf boost_1_90_0.tar.bz2
         cd boost_1_90_0
         ./bootstrap.sh
         ./b2 -j$(nproc) \
                 runtime-link=static \
                 link=static \
                 threading=multi \
                 variant=release \
                 --with-chrono \
                 --with-context \
                 --with-coroutine \
                 --with-date_time \
                 --with-filesystem \
                 --with-locale \
                 --with-program_options \
                 --with-regex \
                 --with-serialization \
                 --with-thread \
                 install
EOF

# Build rabbitmq with defaults
ARG CMAKE_GENERATOR=Ninja
RUN <<-EOF
        cd /
        curl -L -O https://github.com/alanxz/rabbitmq-c/archive/refs/tags/v0.15.0.tar.gz
        echo "7b652df52c0de4d19ca36c798ed81378cba7a03a0f0c5d498881ae2d79b241c2 v0.15.0.tar.gz" | sha256sum -c
        tar -xf v0.15.0.tar.gz
        cd rabbitmq-c-0.15.0
        mkdir build && cd build
        cmake \
                -DCMAKE_BUILD_TYPE=Release \
                -DBUILD_SHARED_LIBS=OFF \
                -DBUILD_STATIC_LIBS=ON \
                -DBUILD_TESTING=OFF \
                ../
        cmake --build . -j$(nproc) --target install
EOF

WORKDIR /monero-lws
COPY . .
ARG CMAKE_GENERATOR=Ninja
RUN <<-EOF
        git submodule update --init --recursive
        mkdir build && cd build
        cmake \
                -D CMAKE_BUILD_TYPE=Release \
                -D STATIC=ON \
                -D BUILD_TESTS=ON \
                -D WITH_RMQ=ON \
                -D DEAD_FUNCTION_REMOVAL=ON \
                /monero-lws
        cmake --build . -j$(nproc)
        ./tests/unit/monero-lws-unit
        strip --strip-debug /monero-lws/build/src/monero-lws*
EOF

# This step drops the static `.a` files and tries to keep symlinks
FROM stagex/pallet-clang-cmake-busybox AS staging
COPY --from=stagex/core-musl . /
COPY --from=stagex/core-musl . /musl
COPY --from=stagex/core-libcxx . /libcxx
COPY --from=stagex/core-libcxxabi . /libcxxabi
COPY --from=stagex/core-libunwind . /libunwind
RUN <<-EOF
        mkdir /staged
        cp -a /musl/usr/lib/*.so* /staged/
        cp -a /libcxx/usr/lib/*.so* /staged/
        cp -a /libcxxabi/usr/lib/*.so* /staged/
        cp -a /libunwind/usr/lib/*.so* /staged/
        strip --strip-debug /staged/*
EOF

FROM stagex/core-busybox
COPY --from=staging /staged/. /usr/lib/
COPY --from=build /monero-lws/build/src/monero-lws-daemon /usr/local/bin/
COPY --from=build /monero-lws/build/src/monero-lws-admin /usr/local/bin/
USER root
RUN <<-EOF
        deluser user
        rm -fr /home/user
        addgroup -g 1000 monero-lws
        adduser -g 1000 -D -h /home/monero-lws -G monero-lws -s /bin/sh monero-lws
EOF
USER monero-lws
ENV USER=monero-lws
ENV HOME=/home/monero-lws

EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s CMD /bin/sh -c "wget --spider http://127.0.0.1:8080/get_version || exit 1"
ENTRYPOINT ["/usr/local/bin/monero-lws-daemon"]
CMD ["--daemon=tcp://monerod:18082", "--sub=tcp://monerod:18083"]
