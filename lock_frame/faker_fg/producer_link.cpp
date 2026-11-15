// ============================================================================
//  producer_link.cpp - loads xess_mfg_producer.dll and forwards the two calls it
//  needs: the record API (start) and the Present tick.
//
//  The producer is a plain module next to the game exe. It is optional: a
//  missing/older DLL logs one line and fakerFG keeps running exactly as before
//  (the FG chain does not depend on the transport).
//
//  Why fakerFG loads it instead of the title: fakerFG is the module that already
//  owns the swap chain and the queue, and its loading thread is the one place
//  where LoadLibrary can happen outside the loader lock (CAPTURE_RULES 3).
// ============================================================================
#include "fg_internal.h"

#include "mgpu_producer_api.h"

namespace fakerfg {
namespace {

HMODULE          g_producer_mod = nullptr;
MgpuProducerApi* g_producer_api = nullptr;
bool             g_producer_tried = false;

void exe_dir_local(wchar_t* out, size_t count)
{
    GetModuleFileNameW(nullptr, out, static_cast<DWORD>(count));
    if (wchar_t* slash = wcsrchr(out, L'\\')) { *slash = L'\0'; }
}

} // namespace

void producer_link_start()
{
    if (g_producer_tried) { return; }
    g_producer_tried = true;

    wchar_t path[MAX_PATH]{};
    exe_dir_local(path, MAX_PATH);
    wcscat_s(path, MAX_PATH, L"\\");
    wcscat_s(path, MAX_PATH, MGPU_PRODUCER_DLL_NAME);

    g_producer_mod = LoadLibraryW(path);
    if (g_producer_mod == nullptr) {
        logf("producer: %ls not present (err=%lu) - no transport module this session\n",
             path, GetLastError());
        return;
    }
    auto get = reinterpret_cast<MgpuProducerGetApiFn>(
        reinterpret_cast<void*>(GetProcAddress(g_producer_mod, MGPU_PRODUCER_API_EXPORT)));
    if (get == nullptr) {
        logf("producer: %ls has no %s\n", path, MGPU_PRODUCER_API_EXPORT);
        return;
    }
    // Ask for the widest range we understand: an older producer (v2) still gives us
    // MV/depth transport; record_scar() is then absent and the marker is skipped
    // instead of being left unrepaired.
    g_producer_api = get(2u, MGPU_PRODUCER_VERSION);
    if (g_producer_api == nullptr || g_producer_api->start == nullptr) {
        logf("producer: %ls refused the version request\n", path);
        g_producer_api = nullptr;
        return;
    }
    const int rc = g_producer_api->start(api_table());
    logf("producer: %ls -> start=%d (api v%u)\n", path, rc, g_producer_api->version);
    if (rc != 0) { g_producer_api = nullptr; }
}

void producer_present(void* queue)
{
    if (g_producer_api != nullptr && g_producer_api->present != nullptr) {
        g_producer_api->present(queue);
    }
}

// producer_record_scar() removed: the marker no longer transports a 16x16 tile.

} // namespace fakerfg
