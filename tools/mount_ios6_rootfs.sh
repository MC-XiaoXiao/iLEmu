#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: mount_ios6_rootfs.sh HFS_IMAGE [STATE_DIRECTORY]

Mount a decrypted iOS root filesystem read-only and print the path of its
writable copy-on-write view. The state directory holds the writable changes
and defaults to .cache-mem/n41-ios6.1.4 in this repository.

Set HFSFUSE and FUSE_OVERLAYFS to use binaries outside PATH.
EOF
}

fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    usage
    exit 0
fi
if (( $# < 1 || $# > 2 )); then
    usage >&2
    exit 2
fi

[[ -f $1 && -r $1 ]] || fail "Set HFS_IMAGE to a readable HFS image file."
image=$(realpath -e -- "$1")
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
state_dir=$(realpath -m -- "${2:-$repo/.cache-mem/n41-ios6.1.4}")
case $state_dir in
    *[:,]*) fail "The state directory cannot contain a colon or comma." ;;
esac

source_mount=$state_dir/source
upper_dir=$state_dir/upper
work_dir=$state_dir/work
rootfs=$state_dir/rootfs
image_record=$state_dir/image-path

mkdir -p -- "$state_dir"
exec 9>"$state_dir/.mount.lock"
flock 9
mkdir -p -- "$source_mount" "$upper_dir" "$work_dir" "$rootfs"

if [[ -e $image_record && $(<"$image_record") != "$image" ]]; then
    fail "The writable layer is associated with a different HFS image. Use a separate state directory."
fi

if findmnt -n -o TARGET -M "$source_mount" >/dev/null; then
    findmnt -n -o TARGET -M "$source_mount" -S "$image" -t fuse.hfs -O ro >/dev/null \
        || fail "Unexpected mount at $source_mount"
    source_mounted=true
else
    source_mounted=false
fi

if findmnt -n -o TARGET -M "$rootfs" >/dev/null; then
    if ! $source_mounted || ! findmnt -n -o TARGET -M "$rootfs" -t fuse.fuse-overlayfs -O rw >/dev/null; then
        fail "Unexpected mount at $rootfs"
    fi
    printf '%s\n' "$image" > "$image_record"
    printf '%s\n' "$rootfs"
    exit 0
fi

hfsfuse=${HFSFUSE:-hfsfuse}
overlayfs=${FUSE_OVERLAYFS:-fuse-overlayfs}
command -v -- "$overlayfs" >/dev/null || fail "Install fuse-overlayfs or set FUSE_OVERLAYFS."

if ! $source_mounted; then
    command -v -- "$hfsfuse" >/dev/null || fail "Install hfsfuse or set HFSFUSE."
    "$hfsfuse" -o ro "$image" "$source_mount" 9>&-
fi

printf '%s\n' "$image" > "$image_record"
ownership="squash_to_uid=$(id -u),squash_to_gid=$(id -g)"
if ! "$overlayfs" -o "lowerdir=$source_mount,upperdir=$upper_dir,workdir=$work_dir,$ownership" "$rootfs" 9>&-; then
    if ! $source_mounted; then
        fusermount3 -u "$source_mount" || true
    fi
    fail "Could not mount the writable iOS root filesystem at $rootfs"
fi

printf '%s\n' "$rootfs"
