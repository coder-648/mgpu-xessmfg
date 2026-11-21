// ============================================================================
//  lock_frame/xefg_mfg/xefg_mfg_port.h
//
//  The MFG-Unlocker "ported MFG model" route, adapted to this consumer:
//
//      ShaderPort  (54 portable DXIL programs, optional own shaders)
//          -> ModelPort (relabel donor model 15 as model 16 + metadata)
//          -> frame-count unlock WITHOUT fg_keep_2x_model
//
//  XeFG still does the generation; we only replace its model/program selection.
//  This is the recommended route in MFG-Unlocker V1.7.1 (`PreparePort`), and it
//  must be installed before the first XeFG context is created.
//
//  Include this from 150_probe_mfg.inc: it uses logf_ from 010_platform.inc.
// ============================================================================
#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime_payload.h"
#include "runtime_layout.h"

// The vendored ModelPort/ShaderPort call this unqualified. Keep it in the
// global namespace and route it through the consumer's own log.
inline void logEvent(const char* event)
{
    logf_("[lock] xefg-mfg: %s\n", event ? event : "");
}

inline void logEvent(const char* event, const std::string& fields)
{
    logf_("[lock] xefg-mfg: %s %s\n", event ? event : "", fields.c_str());
}

// ShaderPort uses this for its JSON log line (MFG-Unlocker keeps it in
// port_support.h, which pulls in D3D12/XeSS headers we do not need here).
inline std::string escape(const std::string& s)
{
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else if (c == '\t') { out += "\\t"; }
        else if (c >= 32) { out += char(c); }
    }
    return out;
}

#include "port/model_port.h"
#include "port/shader_port.h"

namespace xefg_mfg {

// The 16 reconstruction program pairs the ported model needs (MFG-Unlocker
// xess_mod.cpp kReconstructionPrograms). ShaderPort::hasProgram() must find all
// of them before ModelPort is allowed to touch the model catalog.
inline constexpr const char* kReconstructionPrograms[] = {
    "3518243144275597942266", "3518243145375109570477",
    "3519166589661394643131", "3519166588561883014920",
    "3515873413746975655382", "3515873414846487283593",
    "35115408591383412559519", "35115408590283900931308",
    "35118300971948070613363", "35118300970848558985152",
    "351747859676298538780", "351747860775810166991",
    "3517111067725881270903", "3517111066626369642692",
    "35110677435103722760616", "35110677436203234388827",
};

struct Port
{
    std::unique_ptr<ShaderPort> shaders;   // destroyed after model (see below)
    std::unique_ptr<ModelPort>  model;
};

inline std::unique_ptr<Port>& State()
{
    static std::unique_ptr<Port> state;
    return state;
}

inline void SetDataRoot(const std::filesystem::path& root) { xess_payload::SetRoot(root); }

// Checked before any memory is touched, so a missing asset set costs nothing.
inline bool AssetsPresent(std::string* err)
{
    try {
        if (xess_payload::Root().empty() ||
            !std::filesystem::is_regular_file(xess_payload::Root() / L"programs.tsv") ||
            !xess_payload::HaveReplacementShaders()) {
            if (err) { *err = "assets_incomplete"; }
            return false;
        }
        for (const char* key : kReconstructionPrograms) {
            const std::filesystem::path path =
                xess_payload::Root() / L"programs" / (std::string(key) + ".dxil");
            if (!std::filesystem::is_regular_file(path)) {
                if (err) { *err = std::string("reconstruction_program_missing:") + key; }
                return false;
            }
        }
    } catch (...) {
        if (err) { *err = "programs_path_error"; }
        return false;
    }
    return true;
}

// Installs ShaderPort then ModelPort. Returns false without leaving anything
// patched when the payload, the provider build or the model layout does not
// match; the caller then falls back to the 2x-model unlock.
inline bool Install(HMODULE fg, UINT adapterVendor, std::string* err)
{
    if (State()) { return true; }
    if (!fg) {
        if (err) { *err = "libxess_fg_missing"; }
        return false;
    }

    std::string why;
    if (!AssetsPresent(&why)) {
        if (err) { *err = why; }
        logEvent("model_port_rejected", "\"reason\":\"" + why + "\",\"fallback\":\"xefg_2x_model\"");
        return false;
    }

    try {
        auto port = std::make_unique<Port>();
        // Self() in MFG-Unlocker is the module that holds the RCDATA payload; our
        // payload loader reads the directory instead, so the module is unused.
        port->shaders = std::make_unique<ShaderPort>(fg, GetModuleHandleW(nullptr),
                                                     xess_payload::Root(), adapterVendor);
        for (const char* key : kReconstructionPrograms) {
            if (!port->shaders->hasProgram(key)) {
                throw std::runtime_error("reconstruction_program_pair_missing");
            }
        }
        port->model = std::make_unique<ModelPort>(fg, true, true);
        State() = std::move(port);
        logEvent("model_port_install", "\"donor_model\":15,\"target_model\":16,\"vendor\":" +
                 std::to_string(adapterVendor));
        return true;
    } catch (const std::exception& e) {
        State().reset();          // ShaderPort/ModelPort destructors restore
        if (err) { *err = e.what(); }
        logEvent("model_port_rejected", "\"reason\":\"" + std::string(e.what()) +
                 "\",\"fallback\":\"xefg_2x_model\"");
        return false;
    } catch (...) {
        State().reset();
        if (err) { *err = "model_port_unknown_error"; }
        logEvent("model_port_rejected", "\"reason\":\"unknown\",\"fallback\":\"xefg_2x_model\"");
        return false;
    }
}

inline void Restore()
{
    State().reset();
    logEvent("model_port_restored");
}

inline bool Installed() { return State() != nullptr; }

} // namespace xefg_mfg
