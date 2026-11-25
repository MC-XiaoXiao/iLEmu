#!/usr/bin/env bash
# Boot the iPhone 5 game session with its existing firmware and saved user data.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
state=${ILEMU_IMAGE_STATE:-"$repo/.cache-mem/n41-ios6.1.4"}
binary=${ILEMU_BINARY:-"$repo/build/ilemu"}
if [[ ! -x $binary ]]; then
    echo "Build ilemu first or set ILEMU_BINARY to its path." >&2
    exit 1
fi
image=${ILEMU_HFS_IMAGE:-}
if [[ -z $image && -f $state/image-path ]]; then
    image=$(<"$state/image-path")
fi
if [[ -z $image ]]; then
    echo "Set ILEMU_HFS_IMAGE to the existing iPhone5,1 iOS 6 HFS image." >&2
    exit 1
fi
if [[ -x $state/tools/hfsfuse ]]; then
    export HFSFUSE=${HFSFUSE:-"$state/tools/hfsfuse"}
fi
if [[ -x $state/tools/usr/bin/fuse-overlayfs ]]; then
    export FUSE_OVERLAYFS=${FUSE_OVERLAYFS:-"$state/tools/usr/bin/fuse-overlayfs"}
fi
rootfs=$("$repo/tools/mount_ios6_rootfs.sh" "$image" "$state")
exec 8>"$state/game-session.lock"
if ! flock -n 8; then
    echo "Another game session is using this firmware." >&2
    exit 1
fi
session_cache=${ILEMU_HOST_CACHE:-"$state/.ilemu-cache/rootfs"}
mkdir -p -- "$session_cache"
exec "$binary" boot --rootfs "$rootfs" --device iPhone5,1 --display sdl \
    --gpu --gles-backend vulkan --activation activated --network isolated \
    --host-cache "$session_cache" --control-stdin --jit-profile-mode off \
    --jit-catalog-warming no-enqueue "$@"
