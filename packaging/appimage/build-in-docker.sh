#!/usr/bin/env bash
# Build a portable AppImage inside an old distribution (Ubuntu 22.04, glibc 2.35)
# so that it runs on every current Linux distribution.
# Usage: packaging/appimage/build-in-docker.sh   (from the repository root, needs docker)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE="${IMAGE:-ubuntu:22.04}"
docker run --rm -v "$ROOT":/src -w /src -e WORK=/src/build-appimage-docker \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" "$IMAGE" bash -c '
    set -e
    trap "mkdir -p /src/build-appimage-docker; chown -R \$HOST_UID:\$HOST_GID /src/build-appimage-docker" EXIT
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends build-essential cmake ninja-build pkg-config curl ca-certificates file \
      qt6-base-dev qt6-base-dev-tools qmake6 libqt6svg6 qt6-qpa-plugins qt6-wayland libgl1-mesa-dev \
      libpipewire-0.3-dev libsamplerate0-dev libasound2-dev git python3 \
      libflac-dev libogg-dev libvorbis-dev libopus-dev libmp3lame-dev libmpg123-dev >/dev/null
    # libsndfile 1.2 (MP3 read/write); Ubuntu 22.04 ships 1.0.31 without MP3
    git clone -q --depth 1 --branch 1.2.2 https://github.com/libsndfile/libsndfile.git /tmp/sndfile
    cmake -S /tmp/sndfile -B /tmp/sndfile/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
      -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DENABLE_MPEG=ON -DCMAKE_INSTALL_PREFIX=/usr/local >/dev/null
    cmake --build /tmp/sndfile/build >/dev/null && cmake --install /tmp/sndfile/build >/dev/null && ldconfig
    export QMAKE=$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake)
    bash packaging/appimage/build-appimage.sh
  '
ls -la "$ROOT"/build-appimage-docker/*.AppImage
