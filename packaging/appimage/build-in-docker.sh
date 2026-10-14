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
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends build-essential cmake ninja-build pkg-config curl ca-certificates file \
      qt6-base-dev qt6-base-dev-tools qmake6 libqt6svg6 qt6-qpa-plugins libgl1-mesa-dev \
      libpipewire-0.3-dev libsndfile1-dev libsamplerate0-dev libasound2-dev >/dev/null
    export QMAKE=$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake)
    bash packaging/appimage/build-appimage.sh
    chown -R "$HOST_UID:$HOST_GID" /src/build-appimage-docker
  '
ls -la "$ROOT"/build-appimage-docker/*.AppImage
