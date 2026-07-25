#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dmg="${1:-${project_root}/firmwares/iPhone1,1_1.0_1A543a/694-5262-39-decrypted.dmg}"
output="${2:-${project_root}/build/rootfs}"
image="${project_root}/build/firmware/iphoneos-1.0-hfsx.img"
archive="${project_root}/build/firmware/iphoneos-1.0-rootfs.tar"

mkdir -p "${project_root}/build/firmware" "${output}"

if [[ ! -x "${project_root}/tools/dmg2img/dmg2img" ]]; then
    echo "Build tools/dmg2img/dmg2img first (run make in tools/dmg2img)." >&2
    exit 1
fi
if [[ ! -x "${project_root}/tools/hfsfuse/hfstar" ]]; then
    echo "Build tools/hfsfuse/hfstar first (run make WITH_LZVN=none hfstar)." >&2
    exit 1
fi

"${project_root}/tools/dmg2img/dmg2img" -p 3 -i "${dmg}" -o "${image}"
"${project_root}/tools/hfsfuse/hfstar" \
    -W --rsrc-ext .ilemu-rsrc "${image}" "${archive}"
if tar --version 2>/dev/null | grep -q "GNU tar"; then
    tar --xattrs --xattrs-include='*' --warning=no-unknown-keyword \
        --no-same-owner --no-same-permissions \
        -xf "${archive}" -C "${output}"
elif command -v bsdtar >/dev/null 2>&1; then
    bsdtar --xattrs --no-same-owner --no-same-permissions \
        -xf "${archive}" -C "${output}"
else
    echo "A tar implementation with pax xattr support is required." >&2
    exit 1
fi

echo "Extracted iPhone OS root filesystem to ${output}"
