#!/usr/bin/env python3
"""make_tile_bin.py — Build TCACHE04 tile binary from DUKE3D.GRP for the flash 'tiles' partition.

All TILES*.ART tiles are packed:
  - Tiles whose original dims both fit within TC_MAX_DIM are stored as-is.
  - Oversized tiles (either axis > TC_MAX_DIM) are downscaled to the largest
    power-of-2 ≤ TC_MAX_DIM on each axis using majority-vote mode downscaling
    (palette-correct area averaging: picks the most common palette index in each
    source block, far better than nearest-neighbour for indexed-colour images).

Usage:
  python3 tools/make_tile_bin.py --grp DUKE3D.GRP --out tools/tiles.bin
"""
import argparse
import os
import struct
import sys

TC_MAXTILES    = 9216
TC_MAX_DIM     = 128   # must match flash_tiles.cpp / tiles.c MAX_TILE_DIM
TC_MAGIC_OK    = b"TCACHE04"
TC_MAGIC_WIP   = b"TCBUILD!"
TC_ABSENT      = 0xFFFFFFFF
TC_HEADER_SIZE = 16 + TC_MAXTILES * 4   # magic(8) + grp_size(4) + pad(4) + entry_table


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _ilog2(v):
    r = 0
    while v > 1:
        r += 1
        v >>= 1
    return r

def _pow2_floor(v):
    """Largest power of 2 that is ≤ v (minimum 1)."""
    v = max(v, 1)
    r = 1
    while r * 2 <= v:
        r *= 2
    return r

def _target_dim(orig, max_dim):
    """Downscale target: original dim if ≤ max_dim, else largest power-of-2 ≤ max_dim."""
    if orig <= max_dim:
        return orig
    return _pow2_floor(max_dim)

def _tc_make(lw, lh, off):
    return ((lw & 7) << 29) | ((lh & 7) << 26) | (off & 0x03FFFFFF)


# ---------------------------------------------------------------------------
# Downscaling — majority-vote mode (palette-correct area averaging)
# ---------------------------------------------------------------------------

def _downscale_mode(src, orig_w, orig_h, new_w, new_h):
    """Downscale indexed-colour tile using majority-vote in each source block.

    For each output pixel (ox, oy) we collect all source pixels in the
    corresponding block of the column-major source (x*orig_h + y layout) and
    pick the most frequent palette index.  Ties are broken by lowest index for
    determinism.  This produces sharper, less aliased results than nearest-
    neighbour for large downscale factors while remaining palette-correct.
    """
    dst = bytearray(new_w * new_h)
    for ox in range(new_w):
        sx0 = (ox * orig_w) // new_w
        sx1 = ((ox + 1) * orig_w + new_w - 1) // new_w   # ceil
        sx1 = min(sx1, orig_w)
        if sx1 <= sx0:
            sx1 = sx0 + 1
        for oy in range(new_h):
            sy0 = (oy * orig_h) // new_h
            sy1 = ((oy + 1) * orig_h + new_h - 1) // new_h
            sy1 = min(sy1, orig_h)
            if sy1 <= sy0:
                sy1 = sy0 + 1
            counts = {}
            for sx in range(sx0, sx1):
                base = sx * orig_h
                for sy in range(sy0, sy1):
                    p = src[base + sy]
                    counts[p] = counts.get(p, 0) + 1
            # Most frequent; tie → smallest index
            best = max(counts, key=lambda k: (counts[k], -k))
            dst[ox * new_h + oy] = best
    return bytes(dst)


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
        self.scaled  = 0

    def _pack_tile(self, w, h, raw):
        """Return (nw, nh, pixels) — downscale oversized tiles to power-of-2."""
        if w > TC_MAX_DIM or h > TC_MAX_DIM:
            nw = _target_dim(w, TC_MAX_DIM)
            nh = _target_dim(h, TC_MAX_DIM)
            self.scaled += 1
            return nw, nh, _downscale_mode(raw, w, h, nw, nh)
        return w, h, bytes(raw)

    def add(self, tile_idx, w, h, raw):
        if w <= 0 or h <= 0 or tile_idx >= TC_MAXTILES or len(raw) < w * h:
            return
        if self.offset > 0x03FFFFFF:
            print(f"  WARNING: tile {tile_idx} offset 0x{self.offset:X} exceeds 26-bit limit",
                  file=sys.stderr)
            return
        nw, nh, px = self._pack_tile(w, h, raw)
        self.entries[tile_idx] = _tc_make(_ilog2(nw), _ilog2(nh), self.offset)
        self.pixels.extend(px)
        self.offset += len(px)
        self.count  += 1


# ---------------------------------------------------------------------------
# Binary I/O
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# Main build
# ---------------------------------------------------------------------------

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
    print(f"[make_tile_bin] {acc.count} tiles ({acc.scaled} downscaled), "
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
