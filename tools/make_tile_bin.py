#!/usr/bin/env python3
"""make_tile_bin.py — Build TCACHE04 tile binary from DUKE3D.GRP for the flash 'tiles' partition.
All TILES*.ART entries are read; tiles > 128 px are halved per axis until they fit
(matching loadtile() in tiles.c). Skips rebuild if output grp_size already matches.
Usage: python3 tools/make_tile_bin.py --grp DUKE3D.GRP --out tools/tiles.bin
"""
import argparse
import os
import struct
import sys

TC_MAXTILES = 9216
TC_MAX_DIM = 128
TC_MAGIC_OK = b"TCACHE04"
TC_MAGIC_WIP = b"TCBUILD!"
TC_ABSENT = 0xFFFFFFFF
TC_HEADER_SIZE = 16 + TC_MAXTILES * 4

def _ilog2(v):
    r = 0
    while v > 1: r += 1; v >>= 1
    return r
def _fit_dim(d):
    while d > TC_MAX_DIM: d >>= 1
    return max(d, 1)
def _tc_make(lw, lh, off):
    return ((lw & 7) << 29) | ((lh & 7) << 26) | (off & 0x03FFFFFF)

def _downscale(src, orig_w, orig_h, new_w, new_h):
    """Nearest-neighbour downsample; column-major layout (x*h+y)."""
    dst = bytearray(new_w * new_h)
    for x in range(new_w):
        sx = (x * orig_w) // new_w
        for y in range(new_h):
            dst[x * new_h + y] = src[sx * orig_h + (y * orig_h) // new_h]
    return bytes(dst)

def _name_is_art(name):
    u = name.upper().rstrip('\x00')
    return u.startswith("TILES") and u.endswith(".ART")

def _read_grp_dir(f):
    if f.read(12) != b"KenSilverman":
        raise ValueError("Invalid GRP magic")
    n, = struct.unpack("<I", f.read(4))
    entries = []
    for _ in range(n):
        name = f.read(12).rstrip(b'\x00').decode('ascii', errors='replace')
        size, = struct.unpack("<I", f.read(4))
        entries.append([name, size, 0])
    off = 16 + n * 16
    for e in entries:
        e[2] = off; off += e[1]
    return entries

def _parse_art(data, acc):
    if len(data) < 16:
        return
    av, _an, ts, te = struct.unpack_from("<IIII", data, 0)
    if av != 1:
        return
    cnt = te - ts + 1
    if cnt <= 0 or cnt > 1024 or len(data) < 16 + cnt * 8:
        return
    pos = 16
    tsx = struct.unpack_from(f"<{cnt}H", data, pos); pos += cnt * 2
    tsy = struct.unpack_from(f"<{cnt}H", data, pos); pos += cnt * 2
    pos += cnt * 4
    for i, (w, h) in enumerate(zip(tsx, tsy)):
        acc.add(ts + i, w, h, data[pos: pos + w * h])
        pos += w * h

def _is_cached(path, grp_size):
    try:
        with open(path, "rb") as f:
            magic = f.read(8)
            stored, = struct.unpack("<I", f.read(4))
        return magic == TC_MAGIC_OK and stored == grp_size
    except Exception:
        return False

def _write_output(path, grp_size, acc):
    with open(path, "wb") as f:
        f.write(TC_MAGIC_WIP)
        f.write(struct.pack("<I", grp_size))
        f.write(b"\x00" * 4)
        f.write(b"\xff" * (TC_MAXTILES * 4))
        f.write(acc.pixels)
        f.seek(16)
        for e in acc.entries:
            f.write(struct.pack("<I", e))
        f.seek(0)
        f.write(TC_MAGIC_OK)

def build(grp_path, out_path):
    grp_size = os.path.getsize(grp_path)
    if _is_cached(out_path, grp_size):
        print(f"[make_tile_bin] up to date (grp_size={grp_size})")
        return
    print(f"[make_tile_bin] building {grp_path} ({grp_size} bytes)")
    with open(grp_path, "rb") as f:
        directory = _read_grp_dir(f)
        body = f.read()
    dir_end = 16 + len(directory) * 16
    acc = _Acc()
    for name, size, abs_off in directory:
        if not _name_is_art(name):
            continue
        print(f"  {name}")
        rel = abs_off - dir_end
        _parse_art(body[rel: rel + size], acc)
    total = TC_HEADER_SIZE + len(acc.pixels)
    print(f"[make_tile_bin] {acc.count} tiles, {total / 1024:.1f} KB")
    _write_output(out_path, grp_size, acc)
    print(f"[make_tile_bin] wrote {out_path}")

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--grp", required=True, metavar="PATH")
    p.add_argument("--out", required=True, metavar="PATH")
    args = p.parse_args()
    if not os.path.exists(args.grp):
        print(f"ERROR: GRP not found: {args.grp}", file=sys.stderr)
        sys.exit(1)
    build(args.grp, args.out)

class _Acc:
    """Accumulates tile entries and pixel data while building the binary."""
    def __init__(self):
        self.entries = [TC_ABSENT] * TC_MAXTILES
        self.pixels  = bytearray()
        self.offset  = TC_HEADER_SIZE
        self.count   = 0
    def add(self, tile_idx, w, h, raw):
        if w <= 0 or h <= 0 or tile_idx >= TC_MAXTILES:
            return
        if w > TC_MAX_DIM or h > TC_MAX_DIM:
            return  # oversized tiles fall back to SD/GRP at runtime
        nw, nh = w, h
        px = raw
        if self.offset > 0x03FFFFFF:
            print(f"  WARNING: tile {tile_idx} offset exceeds 26-bit limit", file=sys.stderr)
            return
        self.entries[tile_idx] = _tc_make(_ilog2(nw), _ilog2(nh), self.offset)
        self.pixels.extend(px)
        self.offset += len(px)
        self.count  += 1

if __name__ == "__main__":
    main()
