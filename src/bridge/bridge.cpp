// ============================================================================
// bridge.exe - XeSS-FG on the second GPU, mirroring a captured game window
//
// Runs in its own process (ReShade's D3D12 hooks are process global, so a
// second-vendor device inside the game deadlocks - see docs/findings/milestones/M3.2_RESULTS.md).
//
// Inputs, all without adding GPU cost to the game:
//   colour : Windows.Graphics.Capture of the game window, R16G16B16A16_FLOAT
//            (scRGB, HDR preserved). DWM already copies the frame across to this
//            GPU to put it on screen, so we reuse that transfer.
//   mv     : shared memory slot, 2560x1440 R16G16_FLOAT   (from the producer)
//   depth  : shared memory slot, 2560x1440 R32_TYPELESS
//   jitter : shared memory frame record
//
// THIS FILE IS THE MODULE INDEX. The implementation lives in modules/ and is
// included in dependency order as one translation unit. Rationale and the
// module map are in docs/design/BRIDGE_ARCH.md; the rule is: a module may use
// symbols declared above it, never below it.
//
// Build: ./scripts/build_bridge.sh   (copies modules/ next to bridge.cpp)
// ============================================================================


#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dwmapi.h>      // DWMWA_*: keep the overlay out of Aero Peek / Task View thumbnails
#include <shobjidl.h>    // ITaskbarList: remove a task-view entry that already exists
#include <tlhelp32.h>
#include <wrl/client.h>
#include <wrl/implements.h>   // Microsoft::WRL::Callback for the WGC FrameArrived handler
#include <roapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")     // CLSID_TaskbarList

#include <winrt/windows.graphics.capture.h>
#include <winrt/windows.graphics.directx.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include "xell/xell.h"
#include "xell/xell_d3d12.h"
#include "osd_panel.h"
#include "xess_fg/xefg_swapchain.h"
#include "xess_fg/xefg_swapchain_d3d12.h"
#include "xess_fg/xefg_swapchain_debug.h"

// libxess_fg.dll is loaded at runtime instead of being linked: MFG support lives
// in that file (see src/xefg_dyn.h for the evidence), so the file has to be
// selectable and reportable.
#include "xefg_dyn.h"
// OptiScaler's Dynamic MFG count controller (verbatim port, MIT) - see the header.
#include "xefg_dynamic_count.h"
// --unlock-mfg: the byte patch that lifts the non-Intel MFG clamp (src/xefg_unlock.h).
#include "xefg_unlock.h"

#include "mgpu_shared.h"

// --upsample: the consumer-side joint MV/depth reconstruction pass. The CSO is compiled
// from src/bridge/shaders/upsample_joint.hlsl by scripts/build_bridge.sh and embedded, so
// the exe never needs d3dcompiler at runtime (same convention as the producer's shaders).
#include "upsample_joint_cso.h"

// AMD FidelityFX optical flow: compute the motion vectors locally on the AMD card
// instead of trusting the producer's MV stream (that stream measured as garbage on
// PBZ - no correlation with the real motion at any scale, docs/findings/HANDOFF.md 8.6).
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cwctype>
#include <deque>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureItem;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession5;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2;
using ABI::Windows::Graphics::SizeInt32;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;
using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

// ----------------------------------------------------------------- module index
// Dependency order. Keep this list in sync with docs/design/BRIDGE_ARCH.md.
#include "modules/010_platform.inc"
#include "modules/020_state_overlay.inc"
#include "modules/030_devices_capture_fg.inc"
#include "modules/040_cadence.inc"
#include "modules/050_shaders.inc"
#include "modules/060_frame_io.inc"
#include "modules/070_osd.inc"
#include "modules/080_warp_upsample.inc"
#include "modules/090_stream_auto.inc"
#include "modules/100_convert_warp.inc"
#include "modules/110_pair_dump.inc"
#include "modules/120_dump_of.inc"
#include "modules/130_input_cache.inc"
#include "modules/140_frame.inc"
#include "modules/150_probe_mfg.inc"
#include "modules/160_main.inc"
