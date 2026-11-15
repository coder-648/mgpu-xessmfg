// ============================================================================
//  consumer_internal.h - everything xess_mfg_consumer's own .cpp files share.
//
//  The consumer is built as a unity translation unit (unity.cpp), the same
//  pattern as fakerFG.dll (src/faker_fg/unity.cpp): the modules live in
//  modules/*.cpp, keep their state in static/anonymous-namespace objects, and
//  are included in dependency order. Headers make the external dependencies
//  explicit; the order is the one MSVC's single-pass compiler needs.
//
//  Only the ABI headers under src/ (mgpu_shared.h, xefg_*.h) are shared with
//  bridge; no bridge module is compiled here.
// ============================================================================
#pragma once

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dcomp.h>
#include <dwmapi.h>   // DwmGetCompositionTimingInfo: the compositor's own refresh period
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dwmapi.h>      // DWMWA_*: keep the overlay out of Aero Peek / Task View thumbnails
#include <shobjidl.h>    // ITaskbarList: remove a task-view entry that already exists
#include <tlhelp32.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <roapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")     // CLSID_TaskbarList

// v22: Windows.Graphics.Capture is gone. The colour arrives through the producer's shared
// stream 0, so the consumer no longer needs WinRT capture interfaces at all.

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
// OptiScaler's Dynamic MFG count controller (verbatim port, GPL-3.0) - see the header.
#include "xefg_dynamic_count.h"
// (m,k,paced) 动态逐帧最优控制器（纯 C++，仿真见 tests/cadence_core）。
#include "cadence_core.h"
// 颜色<->发布 的精确配对（纯 C++，仿真见 tests/pair_core）。
#include "mgpu_pair_core.h"
// --unlock-mfg: the byte patch that lifts the non-Intel MFG clamp (src/xefg_unlock.h).
#include "xefg_unlock.h"

#include "mgpu_shared.h"

// --upsample: the consumer-side joint MV/depth reconstruction pass. The CSO is
// compiled from shaders/upsample_joint.hlsl by scripts/build_consumer.sh and
// embedded, so the exe never needs d3dcompiler at runtime.
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
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
