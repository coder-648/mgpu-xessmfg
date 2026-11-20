// ============================================================================
// lock_frame.exe - 锁帧版双卡补帧消费端（固定倍率 2x / 3x / 4x）
//
// 与 src/xess_mfg_consumer（v23 颜色入槽）不同，这一版把颜色放回 WGC：
//   colour : Windows.Graphics.Capture 抓游戏窗口（scRGB/HDR 原样）
//   mv     : 共享内存槽（producer 跨卡发布，默认 StreamPercent=150）
//   depth  : 同上
//   身份   : 颜色帧四角的 marker（6x6，32-bit code）与 frames[].frame_id + ms12
//            精确匹配；匹配不上就本帧 1X，绝不错配。
//
// 为什么是固定倍率：WGC 的到达时间被 DWM 量化到 2/3/4 vblank，动态逐帧
// 的相位累加器无法在这种输入上工作。锁帧让输入节拍固定，再用 2x / 3x / 4x
// 三个固定倍数插帧（120 Hz 面板分别对应 60 / 40 / 30 fps 输入）。
//
// 源码取自 src/bridge（原有 WGC 实现，踩坑成果），并把当前 consumer 的
// marker 精确配对移植回来；原版本的所有文件都没有改动。
//
// THIS FILE IS THE MODULE INDEX. 实现位于 modules/，按依赖顺序展开为一个
// 翻译单元。规则：模块只能使用它上面已经出现过的符号。
//
// Build: ./lock_frame/build.sh
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
#include "version.h"

// marker(seq12|ms12|CRC8) <-> publication 的精确配对，纯逻辑，单测在 tests/pair_core。
#include "mgpu_pair_core.h"

// --upsample: the consumer-side joint MV/depth reconstruction pass. The CSO is compiled
// from lock_frame/shaders/upsample_joint.hlsl by lock_frame/build.sh and embedded, so
// the exe never needs d3dcompiler at runtime (same convention as the producer's shaders).
#include "upsample_joint_cso.h"
// B mode 4: consumer-side UI unblend (HUDless = present - UI). The CSO is compiled from
// lock_frame/shaders/unblend_ui.hlsl by lock_frame/build.sh and embedded like the others.
#include "unblend_ui_cso.h"

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
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession3;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession5;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession6;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2;
using ABI::Windows::Graphics::SizeInt32;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;
using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

// ----------------------------------------------------------------- module index
// Dependency order. Keep this list in sync with lock_frame/docs/README.md.
#include "modules/010_platform.inc"
#include "modules/020_state_overlay.inc"
#include "modules/030_devices_capture_fg.inc"
#include "modules/040_cadence.inc"
#include "modules/050_shaders.inc"
#include "modules/060_frame_io.inc"
#include "modules/070_osd.inc"
#include "modules/080_warp_upsample.inc"
#include "modules/085_ui_unblend.inc"
#include "modules/090_stream_auto.inc"
#include "modules/110_pair_dump.inc"
#include "modules/115_marker.inc"     // WGC 颜色四角 marker -> 精确配对
#include "modules/100_convert_warp.inc"
#include "modules/120_dump_of.inc"
#include "modules/130_input_cache.inc"
#include "modules/140_frame.inc"
#include "modules/150_probe_mfg.inc"
#include "modules/160_main.inc"
