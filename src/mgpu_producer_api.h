// ============================================================================
//  mgpu_producer_api.h - the seam between fakerFG.dll and xess_mfg_producer.dll.
//
//  The producer is a plain DLL in the game directory. It is NOT a proxy, NOT a
//  ReShade add-on and it hooks nothing: fakerFG loads it, hands it the record API,
//  and calls present() from the one place every title shares (the Present detour
//  on the chain XeFG owns). Design: docs/design/XESS_MFG_PRODUCER.md.
//
//  Why present() is a separate call: the record arrives inside the upscaler's
//  Evaluate (the command list is still open, so the copies are recorded there),
//  but the fence may only be signalled after the title has submitted that list.
//  present() is that point.
// ============================================================================
#pragma once

#include <stdint.h>

#define MGPU_PRODUCER_VERSION      4u
#define MGPU_PRODUCER_DLL_NAME     L"xess_mfg_producer.dll"
#define MGPU_PRODUCER_API_EXPORT   "MgpuProducerGetApi"

#if defined(_WIN32)
#define MGPU_PRODUCER_CALL __cdecl
#else
#define MGPU_PRODUCER_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MgpuProducerApi
{
    uint32_t size;
    uint32_t version;

    // Subscribe to the fakerFG record. `fg_api` is the MgpuFgApi table
    // (src/faker_fg/mgpu_fg_api.h); the producer keeps it for the session.
    // Returns 0 on success.
    int  (*start)(const void* fg_api);

    // Called from fakerFG's Present path, after the title submitted its lists.
    // `queue` is the ID3D12CommandQueue the title presents on. Signals the
    // published slot's forward fence and advances the ring.
    void (*present)(void* queue);

    // v2: legacy CPU path (fakerFG mapped a readback and handed the bytes here).
    // Retired: kept in the table for ABI order only; the producer now sets it to
    // nullptr. Do not add new users.
    void (*marker_tile)(uint64_t seq, const void* tile, uint32_t bytes,
                        uint32_t format, uint32_t x, uint32_t y);

    // v3: retired. The marker is now a 32-bit code drawn by fakerFG in the colour
    // image (see docs/findings/PERF_ROUND_HANDOVER.md); no tile is copied.
    // Kept in the table for ABI order; the producer sets record_scar = nullptr.
    int  (*record_scar)(void* cmd_list, void* resource, uint32_t format,
                        uint32_t state, uint32_t origin_x, uint32_t origin_y);

    // Unsubscribe and drop the mapping. Called from the loader thread at exit.
    void (*shutdown)(void);

    // v4: tag-time UI synthesis. Called from slSetTag when the HUDless plane is
    // onlyValidNow, with the current chain backbuffer and the HUDless resource. The
    // producer records the synth dispatch on `cmd_list` immediately, so no full-res
    // HUDless snapshot is needed. Returns 1 when a pending UI texture is ready for the
    // next record. `backbuffer`/`hudless` are borrowed ID3D12Resource pointers.
    int  (*ui_tag)(void* cmd_list, void* backbuffer, uint32_t backbuffer_state,
                   void* hudless, uint32_t hudless_state);
} MgpuProducerApi;

typedef MgpuProducerApi* (MGPU_PRODUCER_CALL* MgpuProducerGetApiFn)(uint32_t min_version,
                                                                    uint32_t max_version);

#ifdef __cplusplus
} // extern "C"
#endif
