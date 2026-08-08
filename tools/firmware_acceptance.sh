#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${ILEMU_BUILD_DIR:-"$repo_root/build"}
build_jobs=${ILEMU_BUILD_JOBS:-8}
ticks=${ILEMU_ACCEPTANCE_TICKS:-110000000}
timeout_seconds=${ILEMU_ACCEPTANCE_TIMEOUT_SECONDS:-120}
ilemu="$build_dir/ilemu"

if [ -n "${ILEMU_ACCEPTANCE_CACHE_ROOT:-}" ]; then
    cache_root=$ILEMU_ACCEPTANCE_CACHE_ROOT
    mkdir -p "$cache_root"
else
    cache_root=$(mktemp -d -p /tmp ilemu-acceptance-XXXXXX)
fi

printf '%s\n' "[acceptance] repo=$repo_root"
printf '%s\n' "[acceptance] commit=$(git -C "$repo_root" rev-parse HEAD)"
dirty_count=$(git -C "$repo_root" status --porcelain | wc -l | tr -d ' ')
if [ "$dirty_count" -eq 0 ]; then
    printf '%s\n' "[acceptance] worktree=clean"
else
    printf '%s\n' "[acceptance] worktree=dirty paths=$dirty_count"
    git -C "$repo_root" status --short | sed -n '1,20p'
fi
printf '%s\n' "[acceptance] host-arch=$(uname -m)"
printf '%s\n' "[acceptance] compiler=$(c++ --version | sed -n '1p')"
if [ -f "$build_dir/CMakeCache.txt" ]; then
    cmake -LA -N "$build_dir" 2>/dev/null |
        rg -e '^BUILD_TESTING:' -e '^CMAKE_BUILD_TYPE:' || true
fi

if [ ! -x "$ilemu" ]; then
    cmake --build "$build_dir" --target ilemu --parallel "$build_jobs"
fi

summary_pattern='^\[(cpu|jit|host|boot|perf-artifact|perf-host-memory|perf-file-cache|perf)\]'

run_boot() {
    boot_label=$1
    boot_rootfs=$2
    boot_cache=$3
    boot_log=$4
    boot_cores=$5

    printf '%s\n' "[acceptance] case=$boot_label rootfs=$boot_rootfs cache=$boot_cache"
    if [ "$boot_cores" = faithful ]; then
        if timeout "$timeout_seconds" "$ilemu" boot \
            --rootfs "$boot_rootfs" \
            --display headless \
            --gles-backend software \
            --network isolated \
            --activation unactivated \
            --ticks "$ticks" \
            --perf-summary \
            --host-cache "$boot_cache" </dev/null >"$boot_log" 2>&1; then
            boot_status=0
        else
            boot_status=$?
        fi
    else
        if timeout "$timeout_seconds" "$ilemu" boot \
            --rootfs "$boot_rootfs" \
            --cores "$boot_cores" \
            --display headless \
            --gles-backend software \
            --network isolated \
            --activation unactivated \
            --ticks "$ticks" \
            --perf-summary \
            --host-cache "$boot_cache" </dev/null >"$boot_log" 2>&1; then
            boot_status=0
        else
            boot_status=$?
        fi
    fi
    rg -n -e "$summary_pattern" \
        -e 'abnormal-exit|fatal|exception|fork.*failed|spawn.*failed' \
        "$boot_log" || true
    if [ "$boot_status" -ne 0 ]; then
        printf '%s\n' "[acceptance] case=$boot_label status=$boot_status log=$boot_log"
        tail -n 40 "$boot_log"
        return "$boot_status"
    fi
    printf '%s\n' "[acceptance] case=$boot_label status=0 log=$boot_log"
}

run_firmware() {
    firmware_label=$1
    firmware_rootfs=$2
    if [ ! -d "$firmware_rootfs" ]; then
        printf '%s\n' "[acceptance] firmware=$firmware_label missing-rootfs=$firmware_rootfs" >&2
        return 2
    fi

    printf '%s\n' "[acceptance] firmware=$firmware_label rootfs=$firmware_rootfs"
    firmware_version_file="$firmware_rootfs/System/Library/CoreServices/SystemVersion.plist"
    if [ -f "$firmware_version_file" ]; then
        rg -n -A1 -e '<key>ProductVersion</key>' \
            -e '<key>ProductBuildVersion</key>' "$firmware_version_file" || true
    else
        printf '%s\n' "[acceptance] product-version=unavailable"
    fi
    stat -c '[acceptance] rootfs-stat device=%d inode=%i size=%s mtime=%Y ctime=%Z' \
        "$firmware_rootfs" 2>/dev/null || true

    firmware_cache=$(mktemp -d "$cache_root/${firmware_label}-XXXXXX")
    run_boot "$firmware_label-cold" "$firmware_rootfs" "$firmware_cache" \
        "$firmware_cache/cold.log" faithful
    run_boot "$firmware_label-warm" "$firmware_rootfs" "$firmware_cache" \
        "$firmware_cache/warm.log" faithful

    firmware_multicore_cache=$(mktemp -d "$cache_root/${firmware_label}-multi-XXXXXX")
    run_boot "$firmware_label-multicore" "$firmware_rootfs" "$firmware_multicore_cache" \
        "$firmware_multicore_cache/multicore.log" 2
}

overall_status=0
run_firmware 1A "$repo_root/../.cache-mem/1A543a-final-regression-20260804/rootfs" ||
    overall_status=$?
run_firmware 3A "$repo_root/../.cache-mem/3A109a-good-verbose-20260804/rootfs" ||
    overall_status=$?
run_firmware 5A "$repo_root/../.cache-mem/5A347-final-regression-20260804/rootfs" ||
    overall_status=$?
run_firmware 7A "$repo_root/../.cache-mem/7A341-final-regression-20260804/rootfs" ||
    overall_status=$?

printf '%s\n' "[acceptance] cache-root=$cache_root status=$overall_status"
exit "$overall_status"
