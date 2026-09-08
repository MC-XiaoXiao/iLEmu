#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Prepare an original IPSW AppleLogo in the emulator's host cache.

Uses XPwn's xpwntool/imagetool for firmware codecs; never modifies the rootfs.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile


class FirmwareAssets:
    MAX_ASSET = 16 * 1024 * 1024

    def __init__(self, source):
        self.source = source
        self.archive = None if source.is_dir() else zipfile.ZipFile(source)

    def close(self):
        if self.archive:
            self.archive.close()

    def read(self, name):
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts:
            raise ValueError("invalid firmware asset path")
        if self.archive:
            info = self.archive.getinfo(name)
            if info.file_size > self.MAX_ASSET:
                raise ValueError("firmware asset exceeds 16 MiB")
            return self.archive.read(info)
        with (self.source / name).open("rb") as stream:
            data = stream.read(self.MAX_ASSET + 1)
        if len(data) > self.MAX_ASSET:
            raise ValueError("firmware asset exceeds 16 MiB")
        return data

    def plist(self, name):
        try:
            return plistlib.loads(self.read(name))
        except (KeyError, FileNotFoundError):
            return {}

    def select(self, product, board):
        manifest = self.plist("BuildManifest.plist") or self.plist("BuildManifesto.plist")
        restore = self.plist("Restore.plist")
        products = manifest.get("SupportedProductTypes", [])
        if not products and restore.get("ProductType"):
            products = [restore["ProductType"]]
        if product is None and len(products) == 1:
            product = products[0]
        if not product or (products and product not in products):
            raise ValueError("select a supported --device: " + ", ".join(products))
        build = manifest.get("ProductBuildVersion") or restore.get("ProductBuildVersion")
        for value in (product, build):
            if (not isinstance(value, str) or value in (".", "..") or
                    not re.fullmatch(r"[A-Za-z0-9,._-]+", value)):
                raise ValueError("missing or invalid firmware product/build metadata")
        paths = set()
        for identity in manifest.get("BuildIdentities", []):
            if board and identity.get("Info", {}).get("DeviceClass", "").lower() != board.lower():
                continue
            logo = identity.get("Manifest", {}).get("AppleLogo", {})
            if logo.get("Info", {}).get("Path"):
                paths.add(logo["Info"]["Path"])
        if not paths and not manifest.get("BuildIdentities"):
            names = (self.archive.namelist() if self.archive else
                     (p.relative_to(self.source).as_posix()
                      for p in (self.source / "Firmware").rglob("applelogo*")))
            for name in names:
                path = PurePosixPath(name)
                if (name.startswith("Firmware/") and path.name.startswith("applelogo")
                        and path.suffix in (".img2", ".img3")
                        and (not board or f"all_flash.{board.lower()}." in name.lower())):
                    paths.add(name)
        if len(paths) != 1:
            raise ValueError(f"expected one AppleLogo, found {len(paths)}; use --board to select the firmware identity")
        return product, build, paths.pop()


def run_tool(arguments):
    result = subprocess.run(arguments, capture_output=True, timeout=60)
    diagnostics = (result.stdout + result.stderr).decode(errors="replace")
    # Old XPwn sometimes reports a truncated decompression but exits zero and
    # writes a PNG using uninitialized tail pixels. Never publish that image.
    incomplete = any(message in diagnostics.lower() for message in (
        "uncompressed data shorter than expected", "decompression error",
        "error converting", "unsupported color type", "unsupported compression",
    ))
    if result.returncode or incomplete:
        raise ValueError(Path(arguments[0]).name + " failed: " +
                         diagnostics[-2000:])


def prepare(args):
    firmware = FirmwareAssets(args.source)
    try:
        product, build, member = firmware.select(args.device, args.board)
        data = firmware.read(member)
    finally:
        firmware.close()
    keys = json.loads(args.keys.read_text()) if args.keys else None
    if args.keys:
        if (not isinstance(keys, dict) or
                not isinstance(keys.get("iv"), str) or
                not isinstance(keys.get("key"), str) or
                not re.fullmatch(r"[0-9a-fA-F]{32}", keys.get("iv", "")) or
                not re.fullmatch(r"(?:[0-9a-fA-F]{32}|[0-9a-fA-F]{48}|[0-9a-fA-F]{64})", keys.get("key", ""))):
            raise ValueError("keys JSON must contain hexadecimal iv (16 bytes) and key (16/24/32 bytes)")
    destination = args.host_cache / "boot-logos" / product / (build + ".png")
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Conversion and atomic publication stay on the host-cache filesystem.
    with tempfile.TemporaryDirectory(prefix=".prepare-", dir=destination.parent) as temp:
        temp = Path(temp)
        source = temp / PurePosixPath(member).name
        source.write_bytes(data)
        if keys:
            decrypted = temp / "decrypted.img3"
            run_tool([args.xpwntool, str(source), str(decrypted), "-decrypt",
                      "-iv", keys["iv"], "-k", keys["key"]])
            source = decrypted
        png = temp / "logo.png"
        run_tool([args.imagetool, "extract", str(source), str(png)])
        if not png.exists() or png.stat().st_size > FirmwareAssets.MAX_ASSET:
            raise ValueError("imagetool did not produce a bounded PNG; check the asset keys")
        output = png.read_bytes()
        if (len(output) < 45 or output[:8] != b"\x89PNG\r\n\x1a\n" or
                output[12:16] != b"IHDR" or output[-8:-4] != b"IEND"):
            raise ValueError("imagetool did not produce a complete PNG; check the asset keys")
        width, height = struct.unpack_from(">II", output, 16)
        if not (0 < width <= 4096 and 0 < height <= 4096):
            raise ValueError("logo dimensions exceed 4096x4096")
        metadata = temp / "logo.json"
        metadata.write_text(json.dumps({
            "product": product, "build": build, "asset": member,
            "asset_sha256": hashlib.sha256(data).hexdigest(),
            "png_sha256": hashlib.sha256(output).hexdigest(),
            "width": width, "height": height,
        }, indent=2) + "\n")
        os.replace(metadata, destination.with_suffix(".json"))
        os.replace(png, destination)
    print(f"Prepared {member} ({width}x{height}) -> {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="IPSW or previously extracted IPSW directory")
    parser.add_argument("--host-cache", required=True, type=Path)
    parser.add_argument("--device", help="product type if the IPSW supports multiple products")
    parser.add_argument("--board", help="BuildManifest DeviceClass, for ambiguous identities")
    parser.add_argument("--keys", type=Path, help='AppleLogo keys JSON: {"iv": "hex", "key": "hex"}')
    parser.add_argument("--imagetool", default=shutil.which("imagetool") or "imagetool")
    parser.add_argument("--xpwntool", default=shutil.which("xpwntool") or "xpwntool")
    args = parser.parse_args()
    try:
        prepare(args)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"boot logo: {error}\n")


if __name__ == "__main__":
    main()
