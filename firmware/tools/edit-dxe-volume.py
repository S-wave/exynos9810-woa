#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Edit the compressed DXE volume of a star2lte UEFI FD in place.

The FD holds one outer volume (FV0): the SEC file, then one FFS whose LZMA
GUID-defined section decompresses to the DXE volume (FV1), then free space.
FMMT can edit FV1 but re-encodes it with EDK2's LzmaCompress, which on this image
comes out ~5 KB larger than the shipped stream and overflows FV0 (only ~3.9 KB
free); FV0's size is fixed by the SEC binary, so it cannot grow.

So FV1 is edited uncompressed (FMMT on the bare volume) and re-compressed here
with the shipped stream's own LZMA parameters (lc=2 lp=2 pb=2, 16 MiB dictionary)
at maximum effort. The result is checked by decoding it with EDK2's LzmaCompress -d,
the decoder the firmware itself runs, before it is written back. FV0's header,
the SEC file and every other byte outside the rewritten FFS are left untouched.
"""
from __future__ import annotations

import argparse
import lzma
import os
import struct
import subprocess
import sys
import tempfile

LZMA_GUID = bytes.fromhex("98584eee143959429d6edc7bd79403cf")  # EE4E5898-3914-4259-9D6E-DC7BD79403CF
FFS_HEADER = 24
SECTION_HEADER = 24


def fail(message: str) -> None:
    sys.exit(f"edit-dxe-volume: {message}")


def find_compressed_file(fd: bytes):
    fv_len = struct.unpack_from("<Q", fd, 0x20)[0]
    if fd[0x28:0x2C] != b"_FVH":
        fail("the FD does not start with a firmware volume")
    if struct.unpack_from("<H", fd, 0x34)[0]:
        fail("FV0 has an extended header; not supported")
    off = (struct.unpack_from("<H", fd, 0x30)[0] + 7) & ~7
    while off + FFS_HEADER <= fv_len:
        if fd[off:off + FFS_HEADER] == b"\xff" * FFS_HEADER:
            break
        size = int.from_bytes(fd[off + 20:off + 23], "little")
        sec = off + FFS_HEADER
        if fd[off + 18] == 0x0B and fd[sec + 3] == 0x02 and fd[sec + 4:sec + 20] == LZMA_GUID:
            return fv_len, off, size
        off = (off + size + 7) & ~7
    fail("no LZMA-compressed volume file in FV0")


def edk2_decode(stream: bytes, tools: str) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = os.path.join(tmp, "in.lzma"), os.path.join(tmp, "out.bin")
        open(src, "wb").write(stream)
        r = subprocess.run([os.path.join(tools, "LzmaCompress.exe"), "-d", "-o", dst, src],
                           capture_output=True, text=True)
        if r.returncode != 0:
            fail("EDK2's LZMA decoder rejected the new stream: " + (r.stdout + r.stderr).strip())
        return open(dst, "rb").read()


def compress(raw: bytes, props: int, dict_size: int) -> bytes:
    lc, rest = props % 9, props // 9
    lp, pb = rest % 5, rest // 5
    body = lzma.compress(raw, format=lzma.FORMAT_RAW, filters=[{
        "id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": lc, "lp": lp, "pb": pb,
        "mode": lzma.MODE_NORMAL, "nice_len": 273, "mf": lzma.MF_BT4, "depth": 0,
    }])
    return bytes([props]) + struct.pack("<IQ", dict_size, len(raw)) + body


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--fd", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--replace", nargs=2, action="append", default=[], metavar=("GUID", "FFS"))
    ap.add_argument("--add", action="append", default=[], metavar="FFS")
    ap.add_argument("--tools", required=True, help="EDK2 BaseTools\\Bin\\Win32 (LzmaCompress.exe)")
    ap.add_argument("--fmmt", required=True, help="BaseTools\\Source\\Python\\FMMT\\FMMT.py")
    a = ap.parse_args()

    fd = bytearray(open(a.fd, "rb").read())
    fv_len, file_off, file_size = find_compressed_file(fd)
    sec = file_off + FFS_HEADER
    sec_len = int.from_bytes(fd[sec:sec + 3], "little")
    data_off = struct.unpack_from("<H", fd, sec + 20)[0]
    stream = bytes(fd[sec + data_off:sec + sec_len])
    props, dict_size = stream[0], struct.unpack_from("<I", stream, 1)[0]
    raw = edk2_decode(stream, a.tools)
    fv1 = raw.find(b"_FVH") - 0x28
    if fv1 < 0:
        fail("no volume inside the compressed section")
    fv1_len = struct.unpack_from("<Q", raw, fv1 + 0x20)[0]
    print(f"compressed volume file at 0x{file_off:x}: stream {len(stream)} bytes -> {len(raw)}, FV1 at +{fv1}, 0x{fv1_len:x} bytes")

    with tempfile.TemporaryDirectory() as tmp:
        cur = os.path.join(tmp, "fv1.0")
        open(cur, "wb").write(raw[fv1:fv1 + fv1_len])
        steps = [("-r", "FV0", guid, ffs) for guid, ffs in a.replace] + [("-a", "FV0", ffs) for ffs in a.add]
        for n, step in enumerate(steps, 1):
            nxt = os.path.join(tmp, f"fv1.{n}")
            r = subprocess.run([sys.executable, a.fmmt, step[0], cur, *step[1:], nxt], capture_output=True, text=True)
            if r.returncode != 0 or not os.path.exists(nxt):
                fail(f"FMMT {' '.join(step)} failed: {(r.stdout + r.stderr).strip()}")
            cur = nxt
        new_fv1 = open(cur, "rb").read()
    if len(new_fv1) != fv1_len:
        fail(f"the edited FV1 is {len(new_fv1)} bytes, not 0x{fv1_len:x}")
    new_raw = raw[:fv1] + new_fv1 + raw[fv1 + fv1_len:]

    new_stream = compress(new_raw, props, dict_size)
    if edk2_decode(new_stream, a.tools) != new_raw:
        fail("the new stream does not decode back to the edited volume")
    new_sec_len = data_off + len(new_stream)
    new_file_size = FFS_HEADER + new_sec_len
    if file_off + new_file_size > fv_len:
        fail(f"the edited volume needs {new_file_size} bytes; FV0 has room for {fv_len - file_off}")
    print(f"re-compressed: stream {len(new_stream)} bytes (was {len(stream)}), "
          f"FV0 free space left {fv_len - file_off - new_file_size} bytes")

    header = bytearray(fd[file_off:file_off + FFS_HEADER])
    header[20:23] = new_file_size.to_bytes(3, "little")
    header[16] = 0  # header checksum: computed with itself, State and the file checksum as zero
    file_checksum, state = header[17], header[23]
    header[17] = 0
    header[23] = 0
    header[16] = (0x100 - (sum(header) & 0xFF)) & 0xFF
    header[17], header[23] = file_checksum, state
    section = bytearray(fd[sec:sec + data_off])
    section[0:3] = new_sec_len.to_bytes(3, "little")

    fd[file_off:fv_len] = b"\xff" * (fv_len - file_off)
    fd[file_off:file_off + new_file_size] = bytes(header) + bytes(section) + new_stream
    open(a.output, "wb").write(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
