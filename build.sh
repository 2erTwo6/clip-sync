#!/usr/bin/env bash
# Build helper. Requires podman and network, or adapt the zig/gcc commands.
set -e
cd "$(dirname "$0")"

podman start clipbuild >/dev/null 2>&1 || podman run -d --name clipbuild alpine:3.21 sleep infinity >/dev/null
podman exec clipbuild apk add --no-cache zig >/dev/null 2>&1 || true
podman exec clipbuild mkdir -p /root/src/android /root/src/common /root/src/pc
podman cp android/clipsync.c clipbuild:/root/src/android/clipsync.c
podman cp pc/clipsync.c clipbuild:/root/src/pc/clipsync.c
podman cp common/net.c clipbuild:/root/src/common/net.c
podman cp common/net.h clipbuild:/root/src/common/net.h

mkdir -p dist module/bin
podman exec clipbuild zig cc -target aarch64-linux-musl -static -O2 -s -I/root/src/common \
    -o /root/clipsync_android /root/src/android/clipsync.c /root/src/common/net.c
podman exec clipbuild zig cc -target x86_64-linux-musl -static -O2 -s -I/root/src/common \
    -o /root/clipsync_pc /root/src/pc/clipsync.c /root/src/common/net.c
podman cp clipbuild:/root/clipsync_android dist/clipsync_android
podman cp clipbuild:/root/clipsync_pc dist/clipsync_pc
cp dist/clipsync_android module/bin/clipsync
chmod 755 dist/clipsync_android dist/clipsync_pc module/bin/clipsync

rm -f dist/clipsync-module-0.1.0.zip
( cd module && zip -qr ../dist/clipsync-module-0.1.0.zip . )
echo "built:"
ls -l dist/
