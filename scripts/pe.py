import struct, sys

def pe_sections(path):
    d = open(path, "rb").read()
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[e_lfanew:e_lfanew + 4] == b"PE\0\0"
    coff = e_lfanew + 4
    nsec = struct.unpack_from("<H", d, coff + 2)[0]
    optsize = struct.unpack_from("<H", d, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", d, opt)[0]
    image_base = struct.unpack_from("<Q", d, opt + 24)[0] if magic == 0x20B else struct.unpack_from("<I", d, opt + 28)[0]
    sec = opt + optsize
    out = []
    for i in range(nsec):
        o = sec + i * 40
        name = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, o + 8)
        out.append((name, vaddr, vsize, rawptr, rawsize))
    return d, image_base, out

def rva_to_off(sections, rva):
    for name, vaddr, vsize, rawptr, rawsize in sections:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None

def exports(path):
    d, image_base, sections = pe_sections(path)
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    opt = e_lfanew + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    exp_rva, exp_size = struct.unpack_from("<II", d, dd)
    off = rva_to_off(sections, exp_rva)
    nnames = struct.unpack_from("<I", d, off + 24)[0]
    names_rva = struct.unpack_from("<I", d, off + 32)[0]
    ords_rva = struct.unpack_from("<I", d, off + 36)[0]
    funcs_rva = struct.unpack_from("<I", d, off + 28)[0]
    noff = rva_to_off(sections, names_rva)
    foff = rva_to_off(sections, funcs_rva)
    ooff = rva_to_off(sections, ords_rva)
    out = {}
    for i in range(nnames):
        nr = struct.unpack_from("<I", d, noff + i * 4)[0]
        p = rva_to_off(sections, nr)
        name = d[p:d.index(b"\0", p)].decode("latin1")
        idx = struct.unpack_from("<H", d, ooff + i * 2)[0]
        out[name] = struct.unpack_from("<I", d, foff + idx * 4)[0]
    return out, image_base, sections, d

if __name__ == "__main__":
    path = sys.argv[1]
    exp, base, sections, d = exports(path)
    if len(sys.argv) > 2:
        for n in sys.argv[2:]:
            print(f"{n} RVA=0x{exp.get(n, -1):X}")
        sys.exit()
    print("image base", hex(base))
    print("sections:")
    for name, vaddr, vsize, rawptr, rawsize in sections:
        print(f"  {name:8s} rva=0x{vaddr:08X} vsize=0x{vsize:08X} raw=0x{rawptr:08X} size=0x{rawsize:08X}")
    print(f"exports: {len(exp)}")
