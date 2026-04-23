#!/usr/bin/env python3
"""make_tile_bin.py — Build TCACHE05 tile binary from DUKE3D.GRP for the flash 'tiles' partition.

Fit tiles (both axes ≤ TC_MAX_DIM) and tiles in FORCE_FULL_TILES are stored at
their original dimensions.  All other oversized tiles are excluded; the runtime
GRP/SD path handles them via the existing PSRAM tile cache.

Entry format (TCACHE05): [31:28]=log2(w) [27:24]=log2(h) [23:0]=byte_offset
(4-bit log2 fields support dims up to 2^15=32768; 24-bit offset covers 16 MB.)

Usage:
  python3 tools/make_tile_bin.py --grp DUKE3D.GRP --out tools/tiles.bin
"""
import argparse
import os
import struct
import sys

TC_MAXTILES    = 9216
TC_MAX_DIM     = 128   # must match flash_tiles.cpp
TC_MAGIC_OK    = b"TCACHE05"
TC_MAGIC_WIP   = b"TCBUILD!"
TC_ABSENT      = 0xFFFFFFFF
TC_HEADER_SIZE = 16 + TC_MAXTILES * 4   # magic(8) + grp_size(4) + pad(4) + entry_table

# Oversized tiles stored at full original size in flash.
# Storing them in flash bypasses the tiny ~28 KB PSRAM tile cache (eviction causes artifacts).
FORCE_FULL_TILES = {
    144, 145, 209, 911, 1108, 1640, 1657, 1781, 1782, 1787,
    1962, 1963, 2047, 2051, 2052, 2300, 2324, 2325, 2326, 2327, 2457,
    2445, 2456,
    2462, 2465, 2492, 2493, 2494, 2497, 2499, 2521, 2522, 2528, 2529, 2536,
    2544, 2564, 2566, 2568, 2570, 2571, 2572, 2574, 2576, 2616,
    2617, 2618, 2619, 2632, 2641, 2642, 2646, 2647, 2661, 2662,
    2666, 2667, 2671, 2680, 2681, 2682, 2684, 2685,
    3240, 3241, 3242, 3243, 3244, 3260,
    3263, 3264, 3265, 3266, 3267, 3268,
    3281,  # LOADSCREEN — shown before every level load
}

# Sky tiles: w == TC_MAX_DIM, h > TC_MAX_DIM (38–51 KB each, exceed PSRAM tile cache).
# Stored with only the first TC_MAX_DIM rows per column; picsiz encodes log2(TC_MAX_DIM)
# so the wall renderer's picsiz-based column stride is correct.
# flash_tiles.cpp must update dim.height for these tile IDs to match the stored row count.
HEIGHT_TRUNCATE_TILES = {89, 90, 91, 92, 93, 95}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _ilog2(v):
    r = 0
    while v > 1:
        r += 1
        v >>= 1
    return r

def _tc_make(lw, lh, off):
    # [31:28]=log2(w)  [27:24]=log2(h)  [23:0]=offset
    return ((lw & 0xF) << 28) | ((lh & 0xF) << 24) | (off & 0x00FFFFFF)

def _truncate_columns(raw, w, h, stored_h):
    out = bytearray()
    for col in range(w):
        out.extend(raw[col * h: col * h + stored_h])
    return bytes(out)


# ---------------------------------------------------------------------------
# GRP parsing
# ---------------------------------------------------------------------------

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
        e[2] = off
        off += e[1]
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
        raw = data[pos: pos + w * h]
        acc.add(ts + i, w, h, raw)
        pos += w * h


# ---------------------------------------------------------------------------
# Accumulator
# ---------------------------------------------------------------------------

class _Acc:
    def __init__(self):
        self.entries = [TC_ABSENT] * TC_MAXTILES
        self.pixels  = bytearray()
        self.offset  = TC_HEADER_SIZE
        self.count   = 0
        self.skipped = 0
    def add(self, tile_idx, w, h, raw):
        if w <= 0 or h <= 0 or tile_idx >= TC_MAXTILES or len(raw) < w * h:
            return
        oversized = w > TC_MAX_DIM or h > TC_MAX_DIM
        if oversized and tile_idx not in FORCE_FULL_TILES \
                     and tile_idx not in HEIGHT_TRUNCATE_TILES:
            self.skipped += 1
            return
        stored_h = h
        if tile_idx in HEIGHT_TRUNCATE_TILES and h > TC_MAX_DIM:
            stored_h = TC_MAX_DIM
            trunc = bytearray()
            for col in range(w):
                trunc.extend(raw[col * h: col * h + stored_h])
            raw = bytes(trunc)
        if self.offset > 0x00FFFFFF:
            print(f"  WARNING: tile {tile_idx} offset exceeds 24-bit limit",
                  file=sys.stderr)
            return
        self.entries[tile_idx] = _tc_make(_ilog2(w), _ilog2(stored_h), self.offset)
        self.pixels.extend(raw[:w * stored_h])
        self.offset += w * stored_h
        self.count  += 1


def _is_cached(path, grp_size):
    try:
        with open(path, "rb") as f:
            magic  = f.read(8)
            stored, = struct.unpack("<I", f.read(4))
        return magic == TC_MAGIC_OK and stored == grp_size
    except Exception:
        return False

def _write_output(path, grp_size, acc):
    with open(path, "wb") as f:
        f.write(TC_MAGIC_WIP)
        f.write(struct.pack("<I", grp_size))
        f.write(b"\x00" * 4)
        f.write(b"\xff" * (TC_MAXTILES * 4))   # placeholder entries
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

    print(f"[make_tile_bin] building from {grp_path} ({grp_size} bytes)")
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
    print(f"[make_tile_bin] {acc.count} tiles ({acc.skipped} oversized skipped), "
          f"{total / 1024:.1f} KB")
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


if __name__ == "__main__":
    main()
