#!/bin/sh
# Builds the release archive rawview-<version>-gtk2-x86_64.tar.gz.
#
# LibRaw is compiled from source and linked statically, so the plugin does not
# depend on the distribution's libraw.so.<N>, whose number changes between
# releases. The build runs on Ubuntu 22.04 to keep the glibc requirement low:
# a binary runs on the system it was built on and on newer ones.
#
# Usage: ./build-release.sh <version>
#   Builds the committed source (git HEAD) in an ubuntu:22.04 Docker container
#   and writes the archive to dist/.
#
# Usage inside Ubuntu 22.04 (e.g. CI): ./build-release.sh --in-container <version> <out-dir>
#   Builds the current directory directly.
set -e

LIBRAW_VERSION=0.21.4
LIBRAW_SHA256=6be43f19397e43214ff56aab056bf3ff4925ca14012ce5a1538a172406a09e63
IMAGE=ubuntu:22.04

if [ "$1" != "--in-container" ]; then
    version=$1
    [ -n "$version" ] || { echo "usage: $0 <version>" >&2; exit 2; }
    cd "$(dirname "$0")"
    mkdir -p dist
    src=$(mktemp -d)
    trap 'rm -rf "$src"' EXIT
    git archive HEAD | tar -x -C "$src"
    docker run --rm -v "$src:/src:ro" -v "$PWD/dist:/out" -e HOST_UID="$(id -u)" \
        -e HOST_GID="$(id -g)" "$IMAGE" \
        sh -c 'cp -r /src /build && cd /build && ./build-release.sh --in-container "$0" /out &&
               chown "$HOST_UID:$HOST_GID" /out/*' "$version"
    exit
fi

version=$2
out=$3
[ -n "$version" ] && [ -n "$out" ] || { echo "usage: $0 --in-container <version> <out-dir>" >&2; exit 2; }

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends gcc g++ make pkg-config curl ca-certificates \
    libgtk2.0-dev libjpeg-dev liblcms2-dev zlib1g-dev

work=$(mktemp -d)
curl -fsSL -o "$work/libraw.tar.gz" "https://www.libraw.org/data/LibRaw-$LIBRAW_VERSION.tar.gz"
echo "$LIBRAW_SHA256  $work/libraw.tar.gz" | sha256sum -c -
tar -xzf "$work/libraw.tar.gz" -C "$work"

# -fPIC: the static library ends up inside a shared object (the .wlx).
(cd "$work/LibRaw-$LIBRAW_VERSION" &&
    CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC" ./configure --prefix="$work/libraw" \
        --disable-shared --enable-static --disable-examples --disable-jasper &&
    make -j"$(nproc)" && make install)

gcc -O2 -fPIC -Wall -Wextra -Wno-deprecated-declarations -c -o "$work/rawview.o" rawview.c \
    -I"$work/libraw/include" $(pkg-config --cflags gtk+-2.0)

# LibRaw is C++: g++ links it so that -static-libstdc++ takes effect and the
# C++ runtime is built in too. --exclude-libs keeps LibRaw's symbols private,
# so they cannot clash with another LibRaw loaded into the same process.
g++ -shared -o rawview.wlx "$work/rawview.o" "$work/libraw/lib/libraw.a" \
    $(pkg-config --libs gtk+-2.0) -static-libstdc++ -fopenmp -ljpeg -llcms2 -lz -lm -lpthread \
    -Wl,--exclude-libs,ALL -Wl,--as-needed -Wl,--no-undefined

if ldd rawview.wlx | grep -qE 'libraw|libstdc'; then
    echo "rawview.wlx still links a shared LibRaw or libstdc++" >&2
    exit 1
fi

name=rawview-$version
mkdir -p "$work/$name" "$out"
cp rawview.wlx README.md LICENSE "$work/$name/"
tar -czf "$out/$name-gtk2-x86_64.tar.gz" -C "$work" "$name"
echo "Built $out/$name-gtk2-x86_64.tar.gz"
