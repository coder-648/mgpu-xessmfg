#!/usr/bin/env python3
"""Extract the runtime-compiled HLSL string from a consumer module to a .hlsl file.

    extract_hlsl.py <module.cpp> <out.hlsl> [StringName]

The consumer embeds its convert/prepare shader source as `static const char* k_conv_hlsl
= R"HLSL( ... )HLSL";` and compiles it with D3DCompile at runtime, so a syntax error only
shows up as a "VS/PS compile FAILED" log line at startup. The build calls this script and
then runs fxc over every entry point to catch that before it ships.
"""
import re
import sys


def main():
    src_path, out_path = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else "k_conv_hlsl"
    text = open(src_path, encoding="utf-8", errors="replace").read()
    m = re.search(re.escape(name) + r'\s*=\s*R"HLSL\((.*?)\)HLSL";', text, re.S)
    if not m:
        sys.exit("no R\"HLSL(...)HLSL\" block named %s in %s" % (name, src_path))
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(m.group(1))
    print("[hlsl] %s -> %s (%d chars)" % (name, out_path, len(m.group(1))))


if __name__ == "__main__":
    main()
