#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Replace the kernel of a known-good Android (v0, Samsung) boot image.

pack-uefi.ps1 wraps [bootshim][STAR2LTE_UEFI.fd] as the "kernel" of a boot image
sboot already accepts on star2lte. Everything else is kept from the reference:
header fields, ramdisk, second stage and the Samsung DT blob (whose size Samsung
stores in the header_version slot), the page alignment and the total length
(released images are padded to the 55 MiB BOOT partition). Only kernel_size and
the SHA-1 id are recomputed, the id the way AOSP mkbootimg does for this layout.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys

HEADER = struct.Struct("<8s10I16s512s32s1024s")
BOOT_PARTITION_BYTES = 14080 * 4096


def pages(length: int, page: int) -> int:
    return (length + page - 1) // page * page


def split(image: bytes):
    (magic, ksize, kaddr, rsize, raddr, ssize, saddr, taddr, page, dtsize, osver,
     name, cmdline, ident, extra) = HEADER.unpack_from(image, 0)
    if magic != b"ANDROID!":
        raise ValueError("not an Android boot image")
    off = page
    kernel = image[off:off + ksize]
    off += pages(ksize, page)
    ramdisk = image[off:off + rsize]
    off += pages(rsize, page)
    second = image[off:off + ssize]
    off += pages(ssize, page)
    dt = image[off:off + dtsize]
    return {
        "fields": (magic, ksize, kaddr, rsize, raddr, ssize, saddr, taddr, page, dtsize, osver,
                   name, cmdline, ident, extra),
        "page": page, "kernel": kernel, "ramdisk": ramdisk, "second": second, "dt": dt,
    }


def boot_id(kernel: bytes, ramdisk: bytes, second: bytes, dt: bytes) -> bytes:
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    if dt:
        sha.update(dt)
        sha.update(struct.pack("<I", len(dt)))
    return sha.digest().ljust(32, b"\0")


def build(ref: bytes, kernel: bytes) -> bytes:
    parts = split(ref)
    page = parts["page"]
    fields = list(parts["fields"])
    fields[1] = len(kernel)
    fields[13] = boot_id(kernel, parts["ramdisk"], parts["second"], parts["dt"])
    out = bytearray(HEADER.pack(*fields).ljust(page, b"\0"))
    for blob in (kernel, parts["ramdisk"], parts["second"], parts["dt"]):
        out += blob.ljust(pages(len(blob), page), b"\0") if blob else b""
    if len(out) > BOOT_PARTITION_BYTES:
        raise ValueError(f"image is {len(out)} bytes, larger than BOOT ({BOOT_PARTITION_BYTES})")
    # The released images are partition dumps: past the last section they carry
    # bytes sboot never reads. Keep the reference's own tail rather than zeros, so
    # an image with an unchanged kernel reproduces the reference byte for byte.
    if len(out) < len(ref):
        out += ref[len(out):]
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ref", required=True, help="known-good boot image to take everything but the kernel from")
    ap.add_argument("--kernel", required=True, help="new kernel ([bootshim][FD])")
    ap.add_argument("--output", required=True)
    ap.add_argument("--verify-ref", action="store_true",
                    help="also check that repacking the reference with its own kernel reproduces it exactly")
    a = ap.parse_args()

    ref = open(a.ref, "rb").read()
    if a.verify_ref and build(ref, split(ref)["kernel"]) != ref:
        print("reference does not round-trip; refusing to repack", file=sys.stderr)
        return 1
    image = build(ref, open(a.kernel, "rb").read())
    open(a.output, "wb").write(image)
    print(f"{a.output}: {len(image)} bytes, id {hashlib.sha1(image).hexdigest()[:12]}...")
    return 0


if __name__ == "__main__":
    sys.exit(main())
