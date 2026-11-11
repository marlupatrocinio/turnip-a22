#!/usr/bin/env python3
"""Strip a Turnip build and package it as an AdrenoTools (.adpkg) zip.

Usage:
    python scripts/package_driver.py --so <path to libvulkan_freedreno.so> \
        --variant A --version 26.3.0 --out dist/

Produces a flat zip containing:
    meta.json
    libvulkan_freedreno.so   (llvm-strip --strip-unneeded)

See https://github.com/bylaws/libadrenotools/blob/master/tools/ADPKG.md
"""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import zipfile

VARIANTS = {
    "A": {
        "name": "Turnip A22 Balanced (Snapdragon G2 Gen 2)",
        "description": (
            "Mesa 26.3.0 Turnip for the Qualcomm Snapdragon G2 Gen 2 (SG6275P) "
            "Adreno A22 / gen7-11-0 GPU. Stable -O3 release build, generic "
            "ARMv8-A ISA, self-contained libc++, A22 device identity. "
            "Recommended default for emulators and Winlator."
        ),
        "packageVersion": "1",
    },
    "B": {
        "name": "Turnip A22 Performance (Snapdragon G2 Gen 2)",
        "description": (
            "Mesa 26.3.0 Turnip for the Qualcomm Snapdragon G2 Gen 2 (SG6275P) "
            "Adreno A22 / gen7-11-0 GPU. Performance variant: ARMv8.6-A ISA "
            "codegen (dotprod/fp16/i8mm/bf16/crypto), -fno-semantic-interposition, "
            "fully stripped. Same validated driver as Balanced."
        ),
        "packageVersion": "1",
    },
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--so", required=True)
    ap.add_argument("--variant", choices=sorted(VARIANTS), required=True)
    ap.add_argument("--version", default="26.3.0")
    ap.add_argument("--author", default="A22 Turnip Build")
    ap.add_argument("--strip-tool", default=None)
    ap.add_argument("--out", default="dist")
    ap.add_argument("--no-strip", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    meta_variant = VARIANTS[args.variant]
    stage = os.path.join(args.out, f"stage-{args.variant}")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)

    so_name = "libvulkan_freedreno.so"
    so_dst = os.path.join(stage, so_name)
    shutil.copy2(args.so, so_dst)

    strip = args.strip_tool or shutil.which("llvm-strip")
    if strip and not args.no_strip:
        subprocess.run([strip, "--strip-unneeded", so_dst], check=True)

    meta = {
        "schemaVersion": 1,
        "name": meta_variant["name"],
        "description": meta_variant["description"],
        "author": args.author,
        "packageVersion": meta_variant["packageVersion"],
        "vendor": "Mesa",
        "driverVersion": f"{args.version}-a22{args.variant.lower()}",
        "minApi": 28,
        "libraryName": so_name,
    }
    with open(os.path.join(stage, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    zip_name = f"turnip-a22-{args.version}-variant{args.variant}.adpkg.zip"
    zip_path = os.path.join(args.out, zip_name)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in ("meta.json", so_name):
            z.write(os.path.join(stage, name), name)

    size = os.path.getsize(zip_path)
    digest = sha256(zip_path)
    so_size = os.path.getsize(so_dst)
    print(f"variant {args.variant}: {zip_path}")
    print(f"  library  {so_size/1e6:.2f} MB (stripped)")
    print(f"  archive  {size/1e6:.2f} MB")
    print(f"  sha256   {digest}")
    with open(os.path.join(args.out, f"SHA256SUMS-{args.variant}.txt"), "w") as f:
        f.write(f"{digest}  {zip_name}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
