# Docker
Docker is the easiest way to get a recent copy of `monero-lws`. The paths are:
```bash
docker pull vtnerd/monero-lws
docker pull ghcr.io/vtnerd/monero-lws
```
which will download the latest stable release. A bleeding edge version can be
retrieved with:
```bash
docker pull vtnerd/monero-lws:master
docker pull ghcr.io/vtnerd/monero-lws:master
```
which is considered "alpha" software. The docker build comes with
`monero-lws-daemon` and `monero-lws-admin`.

