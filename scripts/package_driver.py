#!/usr/bin/env python3
"""Strip and package a Turnip build as an AdrenoTools (.adpkg) archive.

Outputs (deterministic: identical input bytes produce identical SHA256):
    dist/turnip-a22-<ver>.adpkg.zip    meta.json + stripped .so
    dist/turnip-a22-<ver>.debug.zip    separate debug symbols
    dist/SHA256SUMS.txt                hashes of both archives

Usage:
    python scripts/package_driver.py --so <libvulkan_freedreno.so> \
        --version 26.3.0 --out dist/ \
        --strip-tool $NDK/.../bin/llvm-strip \
        --objcopy-tool $NDK/.../bin/llvm-objcopy

Schema: https://github.com/bylaws/libadrenotools/blob/master/tools/ADPKG.md
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import zipfile

PACKAGE_NAME = "Turnip A22 (Snapdragon G2 Gen 2)"
PACKAGE_DESCRIPTION = (
    "Mesa 26.3.0 freedreno Vulkan HAL for the Qualcomm Snapdragon G2 Gen 2 "
    "(SG6275P), Adreno (TM) 22 / gen7-11-0 (chip_id 0x43030b00). Release "
    "build: -O3, ARMv8.6-A ISA, static libc++, stripped."
)
PACKAGE_VERSION = "3"

# 1980-01-01 is the earliest timestamp a zip can represent; a constant makes the
# archive byte-for-byte reproducible.
FIXED_DATE_TIME = (1980, 1, 1, 0, 0, 0)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_deterministic_zip(zip_path, entries):
    """entries: list of (arcname, source path). Sorted, fixed timestamps."""
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for arcname, src in sorted(entries):
            with open(src, "rb") as f:
                data = f.read()
            info = zipfile.ZipInfo(arcname, date_time=FIXED_DATE_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            info.create_system = 3  # unix
            z.writestr(info, data, compresslevel=9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--so", required=True)
    ap.add_argument("--version", default="26.3.0")
    ap.add_argument("--author", default="A22 Turnip Build")
    ap.add_argument("--strip-tool", default=None)
    ap.add_argument("--objcopy-tool", default=None)
    ap.add_argument("--out", default="dist")
    ap.add_argument("--no-strip", action="store_true")
    ap.add_argument("--no-debug", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    stage = os.path.join(args.out, "stage")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)

    so_name = "libvulkan_freedreno.so"
    so_dst = os.path.join(stage, so_name)
    shutil.copy2(args.so, so_dst)

    objcopy = args.objcopy_tool or shutil.which("llvm-objcopy")
    strip = args.strip_tool or shutil.which("llvm-strip")

    debug_path = None
    if objcopy and not args.no_debug:
        debug_path = os.path.join(stage, so_name + ".debug")
        subprocess.run([objcopy, "--only-keep-debug", so_dst, debug_path], check=True)

    if strip and not args.no_strip:
        subprocess.run([strip, "--strip-unneeded", so_dst], check=True)

    meta = {
        "schemaVersion": 1,
        "name": PACKAGE_NAME,
        "description": PACKAGE_DESCRIPTION,
        "author": args.author,
        "packageVersion": PACKAGE_VERSION,
        "vendor": "Mesa",
        "driverVersion": args.version,
        "minApi": 28,
        "libraryName": so_name,
    }
    meta_path = os.path.join(stage, "meta.json")
    with open(meta_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    zip_name = f"turnip-a22-{args.version}.adpkg.zip"
    zip_path = os.path.join(args.out, zip_name)
    write_deterministic_zip(zip_path, [(so_name, so_dst), ("meta.json", meta_path)])

    print(f"package: {zip_path}")
    print(f"  library  {os.path.getsize(so_dst)/1e6:.2f} MB (stripped)")
    print(f"  archive  {os.path.getsize(zip_path)/1e6:.2f} MB")
    print(f"  sha256   {sha256(zip_path)}")

    dbg_zip = None
    if debug_path:
        dbg_zip = os.path.join(args.out, f"turnip-a22-{args.version}.debug.zip")
        write_deterministic_zip(dbg_zip, [(so_name + ".debug", debug_path)])
        print(f"  symbols  {os.path.getsize(debug_path)/1e6:.2f} MB "
              f"-> {os.path.basename(dbg_zip)} ({os.path.getsize(dbg_zip)/1e6:.2f} MB)")

    sums = os.path.join(args.out, "SHA256SUMS.txt")
    with open(sums, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"{sha256(zip_path)}  {zip_name}\n")
        if dbg_zip:
            f.write(f"{sha256(dbg_zip)}  {os.path.basename(dbg_zip)}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
