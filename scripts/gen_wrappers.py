#!/usr/bin/env python3
"""gen_wrappers.py - 生成我们自己的 D3D12 薄包装（方案 B）

设计来自 ReShade（BSD-3-Clause, Copyright (C) 2014 Patrick Moulard）：
    只钩 D3D12CreateDevice，把游戏后续创建的 device / queue / command list 换成包装对象，
    于是每条 barrier、每次提交都必然经过我们 —— 不需要猜任何 vtable 槽号。

方法签名取自 ReShade 的 d3d12_{device,command_queue,command_list}.hpp（干净、无 SAL），
方法**个数**取自本机 SDK 的 d3d12.h（沿继承链累加），因为 ReShade 是对着更新的 SDK 写的。

输出：src/host/wrap/generated/wrapped_d3d12.{h,cpp}
"""
import pathlib
import re

SDK_HEADER = pathlib.Path(
    "/mnt/c/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um/d3d12.h")
RESHADE = pathlib.Path("third_party/reshade-main/source/d3d12")
OUT = pathlib.Path("src/host/wrap/generated")

TARGETS = [
    ("d3d12_device.hpp", "WrappedDevice", "ID3D12Device9",
     {"CreateCommandQueue", "CreateCommandList", "CreateCommandAllocator"},
     {"CreateCommittedResource", "CreateCommittedResource1", "CreateCommittedResource2",
      "CreateCommittedResource3", "CreatePlacedResource", "CreatePlacedResource1",
      "CreatePlacedResource2", "CreateReservedResource", "CreateReservedResource1",
      "CreateHeap", "CreateHeap1"}),
    ("d3d12_command_queue.hpp", "WrappedCommandQueue", "ID3D12CommandQueue",
     {"ExecuteCommandLists", "Signal"}, set()),
    ("d3d12_command_list.hpp", "WrappedCommandList", "ID3D12GraphicsCommandList9",
     {"ResourceBarrier", "Barrier", "Close", "Reset"}, set()),
]

DECL_RE = re.compile(
    r"^\s*(?P<ret>[A-Za-z_][\w\s\*]*?)\s+STDMETHODCALLTYPE\s+(?P<name>\w+)\s*"
    r"\((?P<args>(?:[^()]|\([^()]*\))*)\)\s*override;\s*$",
    re.M)

IFACE_RE = re.compile(
    r'MIDL_INTERFACE\("[^"]+"\)\s*\n\s*(\w+)\s*:\s*public\s+(\w+)')


def split_args(args):
    depth = 0
    cur = ""
    for ch in args:
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth -= 1
        if ch == "," and depth == 0:
            yield cur.strip()
            cur = ""
        else:
            cur += ch
    if cur.strip():
        yield cur.strip()


def split_param(raw):
    """('const FLOAT BlendFactor[4]', 'BlendFactor') - the declaration is kept verbatim so
    array parameters keep their bounds (C++ adjusts them to pointers either way)."""
    decl = " ".join(raw.split())
    # The name can be glued to the pointer/reference (ID3D12X *pThing), which is the norm in
    # d3d12.h - requiring whitespace there silently dropped those parameters.
    m = re.match(r"^(?P<type>.+?)\s*[*&]*\s*(?P<name>[A-Za-z_]\w*)\s*(?:\[[^\]]*\])?$", decl)
    if not m:
        return (decl, "")
    return (decl, m.group("name"))


def render_params(params):
    return "" if not params else ", ".join(p[0] for p in params)


def reshade_methods(path):
    """Ordered (ret, name, params) list from a ReShade interface header."""
    text = path.read_text(encoding="utf-8", errors="ignore")
    out = []
    for m in DECL_RE.finditer(text):
        name = m.group("name")
        if name in ("QueryInterface", "AddRef", "Release"):
            continue
        ret = " ".join(m.group("ret").split())
        args = m.group("args").strip()
        params = [split_param(a) for a in split_args(args)] if args and args != "void" else []
        out.append((ret, name, params))
    return out


SAL_RE = re.compile(r"\b_[A-Z]\w*_?\s*(\([^()]*(?:\([^()]*\))?[^()]*\))?|\b__\w+\s*(\([^()]*\))?")


def sdk_methods(iface):
    """Ordered (ret, name, params) for `iface`, straight from the SDK header.

    ReShade's headers are written against a newer SDK, and a signature that differs by one
    parameter type does not override anything ("method does not override any base class
    method") - so the signatures must come from the same header we compile against. SAL
    annotations are stripped; parameter names are preserved.
    """
    text = SDK_HEADER.read_text(encoding="utf-8", errors="ignore")
    chain = {}
    for m in IFACE_RE.finditer(text):
        name, base = m.group(1), m.group(2)
        bs = text.find("{", m.end())
        depth = 0
        i = bs
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = text[bs:i]
        decls = []
        for d in re.finditer(
                r"virtual\s+(?P<ret>[\w\s\*]+?)\s+STDMETHODCALLTYPE\s+(?P<name>\w+)\s*"
                r"\((?P<args>[^;]*?)\)\s*=\s*0\s*;", body, re.S):
            ret = " ".join(d.group("ret").split())
            name_ = d.group("name")
            args = SAL_RE.sub(" ", d.group("args"))
            args = " ".join(args.split())
            params = [split_param(a) for a in split_args(args)] if args and args != "void" else []
            decls.append((ret, name_, params))
        chain[name] = (base, decls)

    if iface not in chain:
        raise SystemExit(f"{iface} not found in {SDK_HEADER}")

    ordered = []
    cur, seen = iface, set()
    while cur in chain and cur not in seen:
        seen.add(cur)
        ordered = chain[cur][1] + ordered
        cur = chain[cur][0]
    return ordered




HEADER_TOP = '''// GENERATED by scripts/gen_wrappers.py - do not edit.
//
// Thin D3D12 wrappers, shaped after ReShade's (BSD-3-Clause, Copyright (C) 2014 Patrick
// Moulard): the objects the game creates come back wrapped, so every barrier and every
// submission passes through us. Only the intercepted methods differ from a plain forward.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <atomic>

'''

CPP_TOP = '''// GENERATED by scripts/gen_wrappers.py - do not edit.
#include "wrapped_d3d12.h"
#include "../wrap_hooks.h"

'''

KIND = {"WrappedDevice": "device", "WrappedCommandQueue": "queue", "WrappedCommandList": "list"}


def emit():
    OUT.mkdir(parents=True, exist_ok=True)
    h = [HEADER_TOP, "namespace mgpu_wrap {\n\n"]
    cpp = [CPP_TOP, "using namespace mgpu_wrap;\n\n"]

    for fname, cls, iface, hooked, counted in TARGETS:
        methods = sdk_methods(iface)
        note = f"    // {len(methods)} methods, exactly as declared by {iface} in this SDK.\n"

        h.append(f"class {cls} final : public {iface}\n{{\npublic:\n")
        h.append(f"    explicit {cls}({iface}* orig) : _orig(orig) {{ orig->AddRef(); }}\n")
        h.append("    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;\n")
        h.append("    ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }\n")
        h.append("    ULONG STDMETHODCALLTYPE Release() override;\n")
        h.append(f"    {iface}* original() const {{ return _orig; }}\n")
        h.append(note)
        for ret, name, params in methods:
            h.append(f"    {ret} STDMETHODCALLTYPE {name}({render_params(params)}) override;\n")
        h.append(f"\nprivate:\n    std::atomic<ULONG> _refs{{ 1 }};\n    {iface}* _orig = nullptr;\n}};\n\n")

        cpp.append(f"// ---------------------------------------------------------------- {cls}\n")
        cpp.append(f"HRESULT STDMETHODCALLTYPE {cls}::QueryInterface(REFIID riid, void** ppv)\n{{\n"
                   f"    if (ppv == nullptr) {{ return E_POINTER; }}\n"
                   f"    // Answer with the wrapper for every version we implement: returning the\n"
                   f"    // original would let the game bypass us for the rest of the session.\n"
                   f"    if (mgpu_wrap::supported_{KIND[cls]}_iid(riid)) {{\n"
                   f"        AddRef();\n"
                   f"        *ppv = static_cast<{iface}*>(this);\n"
                   f"        return S_OK;\n"
                   f"    }}\n"
                   f"    return _orig->QueryInterface(riid, ppv);\n}}\n\n")
        cpp.append(f"ULONG STDMETHODCALLTYPE {cls}::Release()\n{{\n"
                   f"    const ULONG left = --_refs;\n"
                   f"    if (left == 0) {{ _orig->Release(); mgpu_wrap::on_wrapper_released(this); delete this; return 0; }}\n"
                   f"    return left;\n}}\n\n")
        for ret, name, params in methods:
            args = ", ".join(p[1] for p in params if p[1])
            cpp.append(f"{ret} STDMETHODCALLTYPE {cls}::{name}({render_params(params)})\n{{\n")
            if name in hooked:
                tail = (", " + args) if args else ""
                if ret == "void":
                    cpp.append(f"    mgpu_wrap::on_{cls}_{name}(_orig{tail});\n")
                else:
                    cpp.append(f"    return mgpu_wrap::on_{cls}_{name}(_orig{tail});\n")
            elif name in counted:
                # Load signal: every resource/heap creation bumps a counter the host reads to
                # decide whether the game is still loading (see mgpu_host_glue.cpp).
                cpp.append("    mgpu_wrap::note_resource_creation();\n")
                cpp.append(f"    return _orig->{name}({args});\n" if ret != "void"
                           else f"    _orig->{name}({args});\n")
            else:
                cpp.append(f"    return _orig->{name}({args});\n" if ret != "void"
                           else f"    _orig->{name}({args});\n")
            cpp.append("}\n\n")

    h.append("WrappedDevice*       wrap_device(ID3D12Device* device);\n"
             "WrappedCommandQueue* wrap_queue(ID3D12CommandQueue* queue, WrappedDevice* owner);\n"
             "WrappedCommandList*  wrap_list(ID3D12GraphicsCommandList* list, WrappedDevice* owner);\n"
             "\n}  // namespace mgpu_wrap\n")

    (OUT / "wrapped_d3d12.h").write_text("".join(h), encoding="utf-8")
    (OUT / "wrapped_d3d12.cpp").write_text("".join(cpp), encoding="utf-8")
    print(f"wrote {OUT/'wrapped_d3d12.h'} ({sum(1 for line in h if 'override;' in line)} methods)")


if __name__ == "__main__":
    emit()
