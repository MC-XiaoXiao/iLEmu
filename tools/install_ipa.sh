#!/bin/sh

# Install a user IPA into an iPhone OS data-volume rootfs.  The firmware's
# MobileInstallation daemon normally performs this transaction, but a host
# preboot data volume has no guest-side package transport yet.  Keep this
# adapter limited to the same two persistent artifacts that SpringBoard uses:
# the application bundle and its installation-cache User map.

set -eu

usage() {
    echo "usage: $0 ROOTFS APPLICATION.ipa" >&2
    exit 2
}

[ "$#" -eq 2 ] || usage
rootfs=$1
ipa=$2

[ -d "$rootfs" ] || {
    echo "install-ipa: rootfs is not a directory: $rootfs" >&2
    exit 1
}
[ -f "$ipa" ] || {
    echo "install-ipa: IPA is not a file: $ipa" >&2
    exit 1
}

case "$rootfs" in
    */) rootfs=${rootfs%/} ;;
esac

app_root=$rootfs/private/var/mobile/Applications
cache_path=$rootfs/private/var/mobile/Library/Caches/com.apple.mobile.installation.plist
mkdir -p "$app_root" "$(dirname "$cache_path")"

stage=$(mktemp -d "${TMPDIR:-/tmp}/ilemu-ipa.XXXXXX")
cleanup() {
    rm -rf "$stage"
}
trap cleanup EXIT HUP INT TERM

# Reject absolute and parent-traversing archive names before invoking the
# firmware-independent unzip utility.  Ordinary iOS IPAs have a single
# Payload/*.app directory; extra top-level artwork/metadata is harmless.
archive_list=$stage/archive.list
unzip -Z1 "$ipa" > "$archive_list"
while IFS= read -r entry; do
    case "$entry" in
        /*|../*|*/../*|*/..)
            echo "install-ipa: unsafe archive entry: $entry" >&2
            exit 1
            ;;
    esac
done < "$archive_list"
unzip -qq "$ipa" 'Payload/*' -d "$stage"

app_dir=$(find "$stage/Payload" -mindepth 1 -maxdepth 1 -type d -name '*.app' -print -quit)
[ -n "$app_dir" ] || {
    echo "install-ipa: IPA has no Payload/*.app bundle" >&2
    exit 1
}

if [ "$(find "$stage/Payload" -mindepth 1 -maxdepth 1 -type d -name '*.app' -print | wc -l)" -ne 1 ]; then
    echo "install-ipa: IPA must contain exactly one top-level app bundle" >&2
    exit 1
fi

info_plist=$app_dir/Info.plist
[ -f "$info_plist" ] || {
    echo "install-ipa: app has no Info.plist" >&2
    exit 1
}

metadata=$(python3 - "$info_plist" <<'PY'
import plistlib
import sys

try:
    with open(sys.argv[1], "rb") as stream:
        info = plistlib.load(stream)
except Exception as error:
    raise SystemExit(f"install-ipa: cannot read Info.plist: {error}")

if not isinstance(info, dict):
    raise SystemExit("install-ipa: Info.plist root is not a dictionary")
bundle_id = info.get("CFBundleIdentifier")
executable = info.get("CFBundleExecutable")
if not isinstance(bundle_id, str) or not bundle_id:
    raise SystemExit("install-ipa: Info.plist has no CFBundleIdentifier")
if not isinstance(executable, str) or not executable:
    raise SystemExit("install-ipa: Info.plist has no CFBundleExecutable")
print(bundle_id)
print(executable)
PY
)
bundle_id=$(printf '%s\n' "$metadata" | sed -n '1p')
executable=$(printf '%s\n' "$metadata" | sed -n '2p')

case "$bundle_id" in
    */*|*..*|*" "*)
        echo "install-ipa: invalid bundle identifier: $bundle_id" >&2
        exit 1
        ;;
esac
case "$executable" in
    /*|*/..*|*"/"*)
        echo "install-ipa: invalid bundle executable: $executable" >&2
        exit 1
        ;;
esac
[ -f "$app_dir/$executable" ] || {
    echo "install-ipa: executable is missing from bundle: $executable" >&2
    exit 1
}

# Keep a metadata copy outside the bundle staging directory; the bundle is
# moved into its final UUID directory before the cache transaction below.
metadata_plist=$stage/Info.plist
cp "$info_plist" "$metadata_plist"

# A stable UUID makes reinstall idempotent without embedding an application,
# page, or firmware name in the emulator.  The UUID is derived from the IPA,
# not from its display name, so two different packages never share a target.
digest=$(sha256sum "$ipa" | awk '{print $1}')
uuid=$(printf '%s' "$digest" | cut -c1-32 | awk '{print toupper($0)}' | sed \
    's/^\(.\{8\}\)\(.\{4\}\)\(.\{4\}\)\(.\{4\}\)\(.\{12\}\)$/\1-\2-\3-\4-\5/')
target=$app_root/$uuid/$(basename "$app_dir")
target_parent=$(dirname "$target")

# Keep a previous installation of the same bundle visible until the new User
# record has been atomically published.  The cache stores guest paths while
# the host rootfs keeps the data volume below private/; reject anything that
# cannot be proven to remain inside this application's directory.
old_install_path=$(python3 - "$cache_path" "$bundle_id" <<'PY'
import os
import plistlib
import sys

cache_path, bundle_id = sys.argv[1:]
if not os.path.exists(cache_path):
    print("")
    raise SystemExit(0)
with open(cache_path, "rb") as stream:
    cache = plistlib.load(stream)
if not isinstance(cache, dict):
    raise SystemExit("install-ipa: installation cache root is not a dictionary")
user = cache.get("User", {})
if not isinstance(user, dict):
    raise SystemExit("install-ipa: installation cache User map is not a dictionary")
record = user.get(bundle_id, {})
if not isinstance(record, dict):
    raise SystemExit("install-ipa: installation cache record is not a dictionary")
path = record.get("Path", "")
if path and not isinstance(path, str):
    raise SystemExit("install-ipa: installation cache Path is not a string")
if path:
    prefix = "/var/mobile/Applications/"
    if not path.startswith(prefix):
        raise SystemExit("install-ipa: previous application path is outside Applications")
    components = path[len(prefix):].split("/")
    if (len(components) != 2 or any(
            not component or component in {".", ".."}
            for component in components) or
            not components[1].endswith(".app")):
        raise SystemExit("install-ipa: previous application path is not an app bundle")
print(path)
PY
)
old_target_parent=
if [ -n "$old_install_path" ]; then
    case "$old_install_path" in
        /var/mobile/Applications/*) ;;
        *)
            echo "install-ipa: invalid previous application path: $old_install_path" >&2
            exit 1
            ;;
    esac
    case "$old_install_path" in
        *..*|*//*|*" "*)
            echo "install-ipa: invalid previous application path: $old_install_path" >&2
            exit 1
            ;;
    esac
    old_target=$rootfs/private$old_install_path
    case "$old_target" in
        "$app_root"/*) old_target_parent=$(dirname "$old_target") ;;
        *)
            echo "install-ipa: previous application path escapes data volume: $old_install_path" >&2
            exit 1
            ;;
    esac
fi

# Move the extracted tree into the data volume before publishing it.  A
# failed metadata transaction therefore leaves no partially visible bundle.
if [ -e "$target_parent" ]; then
    rm -rf "$target_parent"
fi
mkdir -p "$target_parent"
mv "$app_dir" "$target"
chmod -R u+rwX,go+rX "$target_parent"
chmod 755 "$target/$executable"

if ! python3 - "$metadata_plist" "$cache_path" "/var/mobile/Applications/$uuid/$(basename "$target")" <<'PY'
import os
import plistlib
import sys
import tempfile

info_path, cache_path, install_path = sys.argv[1:]
with open(info_path, "rb") as stream:
    info = plistlib.load(stream)
if not isinstance(info, dict):
    raise SystemExit("install-ipa: Info.plist root is not a dictionary")

if os.path.exists(cache_path):
    try:
        with open(cache_path, "rb") as stream:
            cache = plistlib.load(stream)
    except Exception as error:
        raise SystemExit(f"install-ipa: cannot read installation cache: {error}")
else:
    cache = {}
if not isinstance(cache, dict):
    raise SystemExit("install-ipa: installation cache root is not a dictionary")
user = cache.setdefault("User", {})
if not isinstance(user, dict):
    raise SystemExit("install-ipa: installation cache User map is not a dictionary")

bundle_id = info["CFBundleIdentifier"]
record = dict(info)
record["ApplicationType"] = "User"
record["Path"] = install_path
user[bundle_id] = record

parent = os.path.dirname(cache_path)
fd, temporary = tempfile.mkstemp(prefix=".ilemu-install.", dir=parent)
try:
    with os.fdopen(fd, "wb") as stream:
        plistlib.dump(cache, stream, fmt=plistlib.FMT_BINARY, sort_keys=False)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, cache_path)
except Exception:
    try:
        os.unlink(temporary)
    except FileNotFoundError:
        pass
    raise
PY

then
    # A different UUID means the old install is still the live one.  Do not
    # leave an unreferenced executable that a later catalog scan can mistake
    # for an installed application.
    if [ "$old_target_parent" != "$target_parent" ]; then
        rm -rf "$target_parent"
    fi
    exit 1
fi

if [ -n "$old_target_parent" ] &&
    [ "$old_target_parent" != "$target_parent" ] &&
    [ -e "$old_target_parent" ]; then
    if ! rm -rf "$old_target_parent"; then
        echo "install-ipa: warning: old application directory was retained: $old_target_parent" >&2
    fi
fi

echo "installed $bundle_id at /var/mobile/Applications/$uuid/$(basename "$target")"
