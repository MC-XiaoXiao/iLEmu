#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
source_dir="$repo_root/external/cctools-port"
prefix="$repo_root/build/toolchain"
dependency_root="$repo_root/build/toolchain-deps/root"
target_ld="$prefix/bin/arm-apple-darwin9-ld"
jobs=${JOBS:-2}

if test -x "$target_ld"; then
    echo "ARMv6 ld64 already available: $target_ld"
    exit 0
fi

dependency_prefix=${ILEMU_LIBDISPATCH_PREFIX:-}
if test -z "$dependency_prefix" &&
   test -f /usr/lib/libdispatch.so &&
   test -f /usr/lib/libBlocksRuntime.so; then
    dependency_prefix=/usr
fi

if test -z "$dependency_prefix" && command -v pacman >/dev/null 2>&1; then
    package_url=$(pacman -Sp libdispatch)
    package="$repo_root/build/toolchain-deps/libdispatch.pkg.tar.zst"
    mkdir -p "$dependency_root"
    curl -L "$package_url" -o "$package"
    bsdtar -xf "$package" -C "$dependency_root"
    dependency_prefix="$dependency_root/usr"
fi

if test -z "$dependency_prefix" ||
   test ! -f "$dependency_prefix/lib/libdispatch.so" ||
   test ! -f "$dependency_prefix/lib/libBlocksRuntime.so"; then
    echo "libdispatch and BlocksRuntime are required to build cctools-port." >&2
    echo "Install them or set ILEMU_LIBDISPATCH_PREFIX." >&2
    exit 1
fi

if test ! -d "$source_dir/cctools"; then
    git clone --depth 1 \
        https://github.com/tpoechtrager/cctools-port.git "$source_dir"
fi

cd "$source_dir/cctools"
./configure \
    --prefix="$prefix" \
    --target=arm-apple-darwin9 \
    --with-libblocksruntime="$dependency_prefix" \
    --with-libdispatch="$dependency_prefix"
make -j"$jobs"
make install

echo "Installed ARMv6 ld64: $target_ld"
