#!/usr/bin/env python3
"""Extract the 54 portable XeSS MFG programs from MFGUnlocker.dll.

MFG-Unlocker V1.7.1 packs them into one LZMA-compressed RCDATA resource
(default id 3901) and keeps an asset table in .rdata:

    struct Asset { uint32 id; uint32 pad; const char* name; const char* hash; size_t bytes; };

Identical program bytes share one packed id, so several asset rows can name
different keys with the same id/hash. The packed stream is the concatenation of
the unique ids in ascending order.

Output (what lock_frame/xefg_mfg expects):

    <out>/programs.tsv          key<TAB>sha256
    <out>/programs/<key>.dxil   DXBC container

No Intel DLL is written; only the Intel-derived program data that the user
already has in their MFG-Unlocker release. See lock_frame/xefg_mfg/NOTICE.md.
"""
from __future__ import annotations

import argparse
import hashlib
import lzma
import re
import struct
from pathlib import Path


def parse_pe(b: bytes):
    if b[:2] != b"MZ":
        raise SystemExit("not a PE file")
    e = struct.unpack_from("<I", b, 0x3C)[0]
    if b[e:e + 4] != b"PE\0\0":
        raise SystemExit("missing PE signature")
    nsec = struct.unpack_from("<H", b, e + 6)[0]
    opt_size = struct.unpack_from("<H", b, e + 20)[0]
    opt = e + 24
    if struct.unpack_from("<H", b, opt)[0] != 0x20B:
        raise SystemExit("not PE32+")
    image_base = struct.unpack_from("<Q", b, opt + 24)[0]
    data_dir = opt + 112
    sections = []
    so = opt + opt_size
    for i in range(nsec):
        o = so + i * 40
        name = b[o:o + 8].rstrip(b"\0").decode(errors="replace")
        vs, va, rs, pr = struct.unpack_from("<IIII", b, o + 8)
        sections.append((name, va, vs, rs, pr))
    return image_base, data_dir, sections


def rva_to_off(rva: int, sections) -> int:
    for _name, va, vs, rs, pr in sections:
        if va <= rva < va + max(vs, rs):
            return pr + (rva - va)
    raise ValueError(f"RVA not mapped: {rva:#x}")


def c_string(b: bytes, rva: int, sections) -> bytes | None:
    try:
        off = rva_to_off(rva, sections)
    except ValueError:
        return None
    end = b.find(b"\0", off, off + 256)
    return b[off:end] if end > 0 else None


def resource_data(b: bytes, data_dir: int, sections, wanted_id: int) -> bytes:
    rsrc_rva, _rsrc_size = struct.unpack_from("<II", b, data_dir + 2 * 8)
    root = rva_to_off(rsrc_rva, sections)

    def entries(off):
        _chars, _ts, _maj, _min, named, ids = struct.unpack_from("<IIHHHH", b, off)
        return [struct.unpack_from("<II", b, off + 16 + i * 8) for i in range(named + ids)]

    for rtype, off in entries(root):
        if rtype != 10:  # RT_RCDATA
            continue
        for rid, off2 in entries(root + (off & 0x7FFFFFFF)):
            if rid != wanted_id:
                continue
            for _lang, off3 in entries(root + (off2 & 0x7FFFFFFF)):
                de = root + (off3 & 0x7FFFFFFF)
                drva, dsize, _cp = struct.unpack_from("<III", b, de)
                start = rva_to_off(drva, sections)
                return b[start:start + dsize]
    raise SystemExit(f"RCDATA {wanted_id} not found")


def asset_rows(b: bytes, image_base: int, sections, anchor: bytes, prefix: bytes):
    """Walk an Asset table, anchored on a known asset name."""
    needle = anchor + b"\0"
    name_off = b.find(needle)
    if name_off < 0:
        raise SystemExit(f"asset string not found: {anchor!r}")
    name_rva = None
    for _n, va, _vs, rs, pr in sections:
        if pr <= name_off < pr + rs:
            name_rva = va + (name_off - pr)
            break
    if name_rva is None:
        raise SystemExit("manifest string is outside the image sections")

    ptr = struct.pack("<Q", image_base + name_rva)
    for m in re.finditer(re.escape(ptr), b):
        start = m.start() - 8
        if start < 0:
            continue
        hptr = struct.unpack_from("<Q", b, start + 16)[0]
        hb = c_string(b, hptr - image_base, sections) if hptr >= image_base else None
        if hb is None or not re.fullmatch(rb"[0-9a-f]{64}", hb):
            continue

        # Walk backwards to the first row.
        row = start
        while row - 32 >= 0:
            prev = row - 32
            pn = struct.unpack_from("<Q", b, prev + 8)[0]
            ph = struct.unpack_from("<Q", b, prev + 16)[0]
            sn = c_string(b, pn - image_base, sections) if pn >= image_base else None
            sh = c_string(b, ph - image_base, sections) if ph >= image_base else None
            if not sn or not sn.startswith(prefix) or not sh or not re.fullmatch(rb"[0-9a-f]{64}", sh):
                break
            row = prev

        rows = []
        while True:
            ident = struct.unpack_from("<I", b, row)[0]
            n2 = struct.unpack_from("<Q", b, row + 8)[0]
            h2 = struct.unpack_from("<Q", b, row + 16)[0]
            size = struct.unpack_from("<Q", b, row + 24)[0]
            nm = c_string(b, n2 - image_base, sections) if n2 >= image_base else None
            hh = c_string(b, h2 - image_base, sections) if h2 >= image_base else None
            if not nm or not nm.startswith(prefix) or not hh or not re.fullmatch(rb"[0-9a-f]{64}", hh):
                break
            rows.append((ident, nm.decode(), hh.decode(), size))
            row += 32
        if rows:
            return rows
    raise SystemExit("Asset table not found")


def decode_group(rows, raw: bytes, allow_text_id: int | None = None):
    """Split a packed group into {asset id: bytes} and verify each hash."""
    by_id: dict[int, tuple[int, str]] = {}
    for ident, _name, digest, size in rows:
        if ident in by_id and by_id[ident] != (size, digest):
            raise SystemExit(f"asset id {ident} has conflicting size/hash")
        by_id[ident] = (size, digest)
    if sum(size for size, _ in by_id.values()) != len(raw):
        raise SystemExit("packed asset sizes do not match the decoded group")

    chunks: dict[int, bytes] = {}
    off = 0
    for ident in sorted(by_id):
        size, digest = by_id[ident]
        chunk = raw[off:off + size]
        off += size
        if hashlib.sha256(chunk).hexdigest() != digest:
            raise SystemExit(f"asset {ident} hash mismatch")
        if ident != allow_text_id:
            if chunk[:4] != b"DXBC" or struct.unpack_from("<I", chunk, 24)[0] != len(chunk):
                raise SystemExit(f"asset {ident} is not a DXBC container")
        chunks[ident] = chunk
    return chunks


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", type=Path,
                    default=Path("third_party/MFGUnlocker-v1.7.1/MFGUnlocker.dll"))
    ap.add_argument("--out", type=Path, default=Path("build/xefg_programs"))
    ap.add_argument("--resource-id", type=int, default=3901)
    ap.add_argument("--shaders-resource-id", type=int, default=3902)
    args = ap.parse_args()

    b = args.dll.read_bytes()
    image_base, data_dir, sections = parse_pe(b)

    program_rows = asset_rows(b, image_base, sections, b"programs/programs.tsv", b"programs/")
    program_raw = lzma.decompress(resource_data(b, data_dir, sections, args.resource_id),
                                  format=lzma.FORMAT_ALONE)
    program_chunks = decode_group(program_rows, program_raw, 7030)  # 7030 = programs.tsv

    shader_rows = asset_rows(b, image_base, sections,
                             b"shaders/splat_motion_aware.dxil", b"shaders/")
    shader_raw = lzma.decompress(resource_data(b, data_dir, sections, args.shaders_resource_id),
                                 format=lzma.FORMAT_ALONE)
    shader_chunks = decode_group(shader_rows, shader_raw)

    manifest = [(name, digest, program_chunks[ident]) for ident, name, digest, _size in program_rows
                if name.endswith(".dxil")]
    if len(manifest) != 54:
        raise SystemExit(f"expected 54 programs, found {len(manifest)}")

    out = args.out
    (out / "programs").mkdir(parents=True, exist_ok=True)
    lines = []
    for name, digest, data in manifest:
        key = Path(name).stem
        (out / "programs" / f"{key}.dxil").write_bytes(data)
        lines.append(f"{key}\t{digest}")
    (out / "programs.tsv").write_text("\n".join(lines) + "\n", encoding="ascii")

    (out / "shaders").mkdir(parents=True, exist_ok=True)
    shader_lines = []
    for ident, name, digest, _size in shader_rows:
        data = shader_chunks[ident]
        (out / "shaders" / Path(name).name).write_bytes(data)
        shader_lines.append(f"{Path(name).name}\t{digest}")
    (out / "shaders.tsv").write_text("\n".join(shader_lines) + "\n", encoding="ascii")

    print(f"decoded {len(program_raw)} bytes -> {len(manifest)} programs in {out}")
    print(f"decoded {len(shader_raw)} bytes -> {len(shader_rows)} replacement shaders")
    print(f"packed program group sha256 {hashlib.sha256(program_raw).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
