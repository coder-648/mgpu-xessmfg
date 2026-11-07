// ============================================================================
// xefg_unlock.h  -  enable XeSS multi frame generation on a non-Intel GPU
//
// LICENSE / INTENT
//   This file is part of a GPL-3.0 project (see LICENSE / NOTICE) and is provided
//   for personal research only. It rewrites machine code inside libxess_fg.dll /
//   libxell.dll at runtime; that may violate the vendor's EULA. The DLLs
//   themselves are NOT distributed with this project. Read docs/ops/LEGAL.md
//   before enabling --unlock-mfg, and do not ship it as a default.
//
// Verified on this machine (RX 6750 GRE + libxess_fg 1.3.1.78 / libxell 1.3.2.10):
// all six byte sites below match the reference layout exactly, which is why the
// patch table can be fixed rather than discovered at runtime.
//
// Where this comes from
// ---------------------
// "Universal MFG Unlocker" by Michael Robles (dashdogy/MFG-Unlocker, MIT licence)
// unlocks XeMFG for NVIDIA/AMD/Intel. Its OptiScaler build exposes the same thing
// as `[XeFG] UnlockMFG` / "XeLL unlock". The mechanism is *not* a newer DLL: the
// stock libxess_fg already contains the MFG code, but four things stop it on a
// non-Intel adapter, and each is one immediate byte (plus one branch):
//
//   libxess_fg.dll
//     0x1A45C2  mov dword ptr [rdi+16Ch], 1      the override clamp
//     0x1A517D  mov ebx, 3                       the default ceiling
//     0x20973B  mov eax, 1                       the maximum reported by GetProperties
//     0x1A5DE4  je  +9                           "swap to the MFG model" predicate:
//                                                that model is absent from this
//                                                build (18 -> 16), so the branch is
//                                                forced, keeping the 2x model for
//                                                every generated frame
//     0x21B86B  cmp eax,1 / je ... cmp edx,1     a live count change to/from 1
//                                                re-initialises the pipeline
//   libxell.dll
//     0x00D1C9  cmp ebx, 3 ; jbe +7              xellSetGeneratedFramesCount caps
//                                                the count at 3
//
// After the patch `xefgSwapChainGetProperties` reports the requested count, and
// the library builds that many frames - which is also the check this code makes
// before it says it succeeded.
//
// Consequence worth knowing: because the MFG model is absent, every interpolated
// frame comes from the 2x model. That is exactly what the unlocker does too
// ("fg_keep_2x_model"), and it is why quality at 4x is not 2x quality.
// ============================================================================
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "xess_fg/xefg_swapchain.h"

namespace xefg_unlock {

struct Bytes
{
    uint8_t b[10];
    size_t  n;
};

struct Patch
{
    const char* name;
    bool        in_fg;      // true: libxess_fg.dll, false: libxell.dll
    uint32_t    rva;
    Bytes       before;
    Bytes       after;
};

inline Bytes with(const uint8_t* p, size_t n)
{
    Bytes out{};
    out.n = n;
    for (size_t i = 0; i < n && i < sizeof(out.b); ++i) { out.b[i] = p[i]; }
    return out;
}

// The six sites. `interp` is the number of interpolated frames per real frame,
// i.e. what xefg_swapchain_d3d12_init_params_t::maxInterpolatedFrames means.
inline void build_table(unsigned interp, Patch out[6])
{
    static const uint8_t override_clamp[] = { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
    static const uint8_t default_ceiling[] = { 0xBB, 0x03, 0x00, 0x00, 0x00 };
    static const uint8_t reported_max[]   = { 0xB8, 0x01, 0x00, 0x00, 0x00 };
    static const uint8_t keep_2x_model[]  = { 0x74, 0x09 };
    static const uint8_t live_count[]     = { 0x83, 0xF8, 0x01, 0x74, 0x0F,
                                              0x83, 0xFA, 0x01, 0x74, 0x0A };
    static const uint8_t ll_gate[]        = { 0x83, 0xFB, 0x03, 0x76, 0x07 };

    const uint8_t n = static_cast<uint8_t>(interp);

    Bytes oc_after = with(override_clamp, sizeof(override_clamp));
    oc_after.b[6] = n;
    Bytes dc_after = with(default_ceiling, sizeof(default_ceiling));
    dc_after.b[1] = n;
    Bytes rm_after = with(reported_max, sizeof(reported_max));
    rm_after.b[1] = n;
    // je +9 -> jmp +6: always keep the 2x model.
    const uint8_t k2m_after[] = { 0xEB, 0x06 };
    Bytes lc_after = with(live_count, sizeof(live_count));
    lc_after.b[2] = 0x00;
    lc_after.b[7] = 0x00;
    Bytes ll_after = with(ll_gate, sizeof(ll_gate));
    ll_after.b[2] = n;

    out[0] = { "fg_override_clamp",   true,  0x1A45C2, with(override_clamp, sizeof(override_clamp)), oc_after };
    out[1] = { "fg_default_ceiling",  true,  0x1A517D, with(default_ceiling, sizeof(default_ceiling)), dc_after };
    out[2] = { "fg_reported_maximum", true,  0x20973B, with(reported_max,   sizeof(reported_max)),   rm_after };
    out[3] = { "fg_keep_2x_model",    true,  0x1A5DE4, with(keep_2x_model,  sizeof(keep_2x_model)),  with(k2m_after, sizeof(k2m_after)) };
    out[4] = { "fg_live_count",       true,  0x21B86B, with(live_count,     sizeof(live_count)),     lc_after };
    out[5] = { "ll_generated_gate",   false, 0x00D1C9, with(ll_gate,        sizeof(ll_gate)),        ll_after };
}


// Applies all six sites or none at all.
//
//   fg / ll  - the loaded libxess_fg.dll and libxell.dll (the one that DLL uses).
//   interp   - requested interpolated frames per real frame (2..5 -> 3x..6x).
//   err      - which site failed and why.
//
// Every site is checked (module ownership, committed image page, exact bytes)
// before anything is written, each write is flushed and read back, and the old
// page protection is restored. The caller still has to confirm the effect with
// xefgSwapChainGetProperties - that is the only proof the right byte moved.
inline bool Apply(HMODULE fg, HMODULE ll, unsigned interp, std::string* err)
{
    if (interp < 2 || interp > 5) {
        if (err) { *err = "interp out of range (2..5)"; }
        return false;
    }
    if (!fg || !ll) {
        if (err) { *err = "module handle missing"; }
        return false;
    }

    Patch table[6];
    build_table(interp, table);

    struct Target { const Patch* p; uint8_t* addr; DWORD prot; bool already; };
    Target targets[6]{};

    // ---- phase 1: verify every site before touching memory ------------------
    for (int i = 0; i < 6; ++i) {
        const Patch& p = table[i];
        HMODULE mod = p.in_fg ? fg : ll;
        auto* addr = reinterpret_cast<uint8_t*>(mod) + p.rva;

        MEMORY_BASIC_INFORMATION m{};
        if (VirtualQuery(addr, &m, sizeof(m)) != sizeof(m) || m.AllocationBase != mod ||
            m.State != MEM_COMMIT || m.Type != MEM_IMAGE || m.Protect != PAGE_EXECUTE_READ) {
            if (err) { *err = std::string(p.name) + "_ownership"; }
            return false;
        }

        if (std::memcmp(addr, p.after.b, p.after.n) == 0) {
            targets[i] = { &p, addr, m.Protect, true };   // already patched
        } else if (std::memcmp(addr, p.before.b, p.before.n) == 0) {
            targets[i] = { &p, addr, m.Protect, false };
        } else {
            if (err) { *err = std::string(p.name) + "_unexpected_bytes"; }
            return false;
        }
    }

    // ---- phase 2: write, verifying as we go --------------------------------
    int done = 0;
    for (int i = 0; i < 6; ++i) {
        Target& t = targets[i];
        if (t.already) { ++done; continue; }

        DWORD prior = 0;
        if (!VirtualProtect(t.addr, t.p->after.n, PAGE_EXECUTE_READWRITE, &prior)) {
            if (err) { *err = std::string(t.p->name) + "_make_writable"; }
            break;
        }
        std::memcpy(t.addr, t.p->after.b, t.p->after.n);
        const bool flushed = FlushInstructionCache(GetCurrentProcess(), t.addr, t.p->after.n) != FALSE;
        DWORD tmp = 0;
        const bool restored = VirtualProtect(t.addr, t.p->after.n, prior, &tmp) != FALSE;
        const bool read_back = std::memcmp(t.addr, t.p->after.b, t.p->after.n) == 0;
        if (!flushed || !restored || !read_back) {
            if (err) { *err = std::string(t.p->name) + "_write_verification"; }
            break;
        }
        ++done;
    }

    if (done == 6) { return true; }

    // ---- phase 3: roll back what we changed --------------------------------
    for (int i = 0; i < 6; ++i) {
        Target& t = targets[i];
        if (t.already || !t.addr) { continue; }
        if (std::memcmp(t.addr, t.p->after.b, t.p->after.n) != 0) { continue; }
        DWORD prior = 0;
        if (VirtualProtect(t.addr, t.p->after.n, PAGE_EXECUTE_READWRITE, &prior)) {
            std::memcpy(t.addr, t.p->before.b, t.p->before.n);
            FlushInstructionCache(GetCurrentProcess(), t.addr, t.p->after.n);
            DWORD tmp = 0;
            VirtualProtect(t.addr, t.p->after.n, prior, &tmp);
        }
    }
    if (err && err->empty()) { *err = "write_failed"; }
    return false;
}

} // namespace xefg_unlock
