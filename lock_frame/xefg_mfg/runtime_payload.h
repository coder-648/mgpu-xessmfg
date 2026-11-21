// ============================================================================
//  lock_frame/xefg_mfg/runtime_payload.h
//
//  Payload for the vendored MFG-Unlocker ModelPort/ShaderPort shim.
//
//  MFG-Unlocker embeds the 54 portable XeSS MFG DXIL programs as RCDATA
//  resources. We keep the same contract (`xess_payload::Programs` keyed by the
//  numeric program id) but load them from a directory instead, so the asset set
//  can be dropped in without rebuilding:
//
//      <exe dir>\xefg_programs\programs.tsv      key<TAB>sha256
//      <exe dir>\xefg_programs\programs\<key>.dxil
//
//  The 54 program bytes are Intel-derived material (see the MFG-Unlocker
//  NOTICE); this project does not ship them. Without the directory the shim
//  fails closed and the unlock falls back to the 2x-model route.
// ============================================================================
#pragma once

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace xess_payload {

using Programs = std::map<std::string, std::vector<unsigned char>>;

struct Replacement
{
    std::string name;
    std::string nativeHash;
    size_t      nativeBytes{};
    std::vector<unsigned char> bytes;
};

inline std::filesystem::path& Root()
{
    static std::filesystem::path root;
    return root;
}

inline void SetRoot(const std::filesystem::path& root) { Root() = root; }

inline bool IsProgramKey(const std::string& key)
{
    if (key.empty() || key.size() > 32) { return false; }
    for (char c : key) {
        if (c < '0' || c > '9') { return false; }
    }
    return true;
}

inline bool ReadFileBytes(const std::filesystem::path& path,
                          std::vector<unsigned char>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0 || size > 8 * 1024 * 1024) { return false; }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return in.good() || in.eof();
}

inline uint32_t RotR(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// SHA-256 of the decoded program (ShaderPort logs it and would match it against
// a replacement's native hash; our replacement table is empty, but the digest
// still has to be the real algorithm for the log and future assets).
inline std::string Hash(const std::vector<unsigned char>& bytes)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::vector<unsigned char> msg = bytes;
    const uint64_t bits = static_cast<uint64_t>(msg.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) { msg.push_back(0); }
    for (int i = 7; i >= 0; --i) { msg.push_back(static_cast<unsigned char>(bits >> (i * 8))); }

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(msg[off + 4 * i]) << 24) | (uint32_t(msg[off + 4 * i + 1]) << 16) |
                   (uint32_t(msg[off + 4 * i + 2]) << 8) | msg[off + 4 * i + 3];
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = w[i - 16] + (RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
                   w[i - 7] + (RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10));
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    char out[65];
    for (int i = 0; i < 8; ++i) { std::snprintf(out + i * 8, 9, "%08x", h[i]); }
    return std::string(out, 64);
}

// Mirrors MFG-Unlocker's LoadPrograms(): the manifest is the authority on which
// keys exist; a program that is missing or does not start with DXBC is dropped,
// and ShaderPort's hasProgram() then rejects the port (safe fallback).
inline Programs LoadPrograms(HMODULE /*payload*/)
{
    Programs programs;
    if (Root().empty()) { return programs; }
    const std::filesystem::path manifest = Root() / L"programs.tsv";
    std::ifstream in(manifest);
    if (!in) { return programs; }

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.empty() || line[0] == '#') { continue; }
        const size_t tab = line.find('\t');
        const std::string key = line.substr(0, tab);
        if (!IsProgramKey(key)) { continue; }
        std::string digest = (tab == std::string::npos) ? std::string() : line.substr(tab + 1);
        while (!digest.empty() && (digest.back() == ' ' || digest.back() == '\r')) { digest.pop_back(); }
        for (char& c : digest) {
            if (c >= 'A' && c <= 'F') { c = static_cast<char>(c - 'A' + 'a'); }
        }

        std::vector<unsigned char> bytes;
        const std::filesystem::path path = Root() / L"programs" / (key + ".dxil");
        if (!ReadFileBytes(path, bytes) || bytes.size() < 4 ||
            std::memcmp(bytes.data(), "DXBC", 4) != 0) {
            continue;
        }
        if (!digest.empty() && Hash(bytes) != digest) { continue; }
        programs.emplace(key, std::move(bytes));
    }
    return programs;
}

// MFG-Unlocker's seven replacement shaders (splat + five U-Net conv programs;
// the file names are the ones its ShaderPort expects). The asset table in the
// release DLL names them `shaders/<name>.dxil`; we store them under
// <root>/shaders/<name>.dxil and verify them against shaders.tsv.
inline constexpr const char* kReplacementNames[] = {
    "shaders/splat_motion_aware.dxil",
    "shaders/splat_motion_aware_inverted.dxil",
    "shaders/unet_3ed03daf.dxil",
    "shaders/unet_748b0baf.dxil",
    "shaders/unet_cb391582.dxil",
    "shaders/unet_aa016078.dxil",
    "shaders/unet_efbe3788.dxil",
};

inline bool HaveReplacementShaders()
{
    if (Root().empty()) { return false; }
    for (const char* name : kReplacementNames) {
        if (!std::filesystem::is_regular_file(
                Root() / L"shaders" / std::filesystem::path(name).filename())) {
            return false;
        }
    }
    return true;
}

inline std::vector<Replacement> LoadReplacements(HMODULE /*payload*/)
{
    struct Native { const char* name; const char* hash; size_t bytes; };
    static const Native natives[] = {
        { "shaders/splat_motion_aware.dxil",
          "3f3f91d2ecd4345042f0519f7455776314be9e7730453a3fcb42c57f0acb49b5", 4008 },
        { "shaders/splat_motion_aware_inverted.dxil",
          "c3649191906bdd300462dbafe6add8f82b09e8e10c41e92784a209d99513224b", 4012 },
        { "shaders/unet_3ed03daf.dxil",
          "01c8beac27d3f3f82ecba8f3dc2d9d8b60d4c175a41df3c24a24e8643f5578f5", 35852 },
        { "shaders/unet_748b0baf.dxil",
          "8291bbc36b1100b07d0788f35a3a9fadfbfcc2267ff0ccb7afb114d76f5d69fe", 57376 },
        { "shaders/unet_cb391582.dxil",
          "a60f7488872a675d66afd2bec02b0b89e3cc38c1c4ed1d25d745a7b2fe677412", 57372 },
        { "shaders/unet_aa016078.dxil",
          "8c2efb9e955f39379a91887ed3d90412c4b7ae2c142a874e3da5f699de197f7c", 47728 },
        { "shaders/unet_efbe3788.dxil",
          "40e159414d5b839ba7e0e07cbf8ff4ef425ee026c216b2928ef3bb6dd0e20d40", 54224 },
    };

    std::map<std::string, std::string> digests;
    if (!Root().empty()) {
        std::ifstream in(Root() / L"shaders.tsv");
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') { line.pop_back(); }
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) { continue; }
            std::string name = line.substr(0, tab);
            std::string digest = line.substr(tab + 1);
            while (!digest.empty() && digest.back() == ' ') { digest.pop_back(); }
            for (char& c : digest) {
                if (c >= 'A' && c <= 'F') { c = static_cast<char>(c - 'A' + 'a'); }
            }
            digests.emplace(std::move(name), std::move(digest));
        }
    }

    std::vector<Replacement> result;
    for (const auto& native : natives) {
        std::vector<unsigned char> bytes;
        const std::filesystem::path path =
            Root() / L"shaders" / std::filesystem::path(native.name).filename();
        if (!ReadFileBytes(path, bytes) || bytes.size() < 4 ||
            std::memcmp(bytes.data(), "DXBC", 4) != 0) {
            continue;
        }
        const auto it = digests.find(std::filesystem::path(native.name).filename().string());
        if (it != digests.end() && !it->second.empty() && Hash(bytes) != it->second) {
            continue;
        }
        result.push_back({ native.name, native.hash, native.bytes, std::move(bytes) });
    }
    return result;
}

} // namespace xess_payload
