#!/usr/bin/env python3
"""Verify ISS's Android/Bionic executable set: never accept a glibc or foreign-game build."""
from pathlib import Path
import argparse
import re
import subprocess

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--ndk", type=Path, required=True)
    p.add_argument("--root", type=Path, default=Path("out/bionic"))
    p.add_argument("--complete", action="store_true")
    args = p.parse_args()
    readelf = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    required = [
        args.root / "bin/iss-probe", args.root / "bin/kgsl-chip-id",
        args.root / "bin/orbis-pkg-util", args.root / "lib/libbloodframe.so",
        args.root / "lib/libisscpu.so", args.root / "lib/libtlggpu.so",
        args.root / "lib/libisspresent.so", args.root / "lib/libopenxr_loader.so",
        args.root / "lib/libissopenxr.so", args.root / "lib/libc++_shared.so",
        args.root / "drivers/gen8/libvulkan_freedreno.so",
        args.root / "drivers/gen7/libvulkan_freedreno.so",
    ]
    if args.complete:
        required += [
            args.root / "python/bin/python3.13",
            args.root / "python/lib/libpython3.13.so",
            args.root / "python/lib/python3.13/encodings/__init__.py",
            args.root / "python/lib/python3.13/xml/etree/ElementTree.py",
        ]
    for file in required:
        if not file.is_file() or file.stat().st_size == 0:
            raise SystemExit(f"Missing ISS Bionic payload: {file}")
        if file.read_bytes()[:4] != b"\x7fELF":
            if file.suffix == ".py":
                continue
            raise SystemExit(f"Not an ELF binary: {file}")
        header = subprocess.check_output([str(readelf), "-h", str(file)], text=True)
        if "AArch64" not in header:
            raise SystemExit(f"Not ARM64: {file}")
        dynamic = subprocess.check_output([str(readelf), "-d", str(file)], text=True)
        needed = set(re.findall(r"NEEDED.*?\[(.*?)\]", dynamic))
        banned = {n for n in needed if n in {
            "libc.so.6", "libstdc++.so.6", "libX11.so.6", "libSDL3.so.0",
            "libvulkan.so.1", "libpthread.so.0", "ld-linux-aarch64.so.1"
        }}
        if banned:
            raise SystemExit(f"Linux or unexpectedly dynamic dependencies in {file}: {banned}")
        if file in (args.root / "bin/iss-probe", args.root / "bin/kgsl-chip-id",
                    args.root / "python/bin/python3.13", args.root / "bin/orbis-pkg-util"):
            interp = subprocess.check_output([str(readelf), "-l", str(file)], text=True)
            if "/system/bin/linker64" not in interp:
                raise SystemExit(f"Not an Android Bionic executable: {file}")
        print(f"Bionic AArch64 verified: {file.name}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
