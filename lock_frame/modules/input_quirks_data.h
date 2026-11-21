// GENERATED - do not edit by hand.
// Source: third_party/OptiScaler-master/OptiScaler/misc/GameConfigs.cpp
//         (GPL-3.0; keep this provenance and third_party/optiscaler/LICENSE).
// Generator: scripts/gen_input_quirks.py. Rows kept when they mention an FG-input
// setting/quirks or declare .ueExes (UE detection).
//
// Fields are OptiScaler's own text, unmodified: `defaults` = "Setting = value; ...",
// `quirks` = GameQuirk names. 025_input_quirks.inc maps them onto our switches.
#pragma once

struct OptiGameRow
{
    const char* exes;     // lowercase exe names, comma separated
    const char* ue_exes;  // UE project prefixes, comma separated
    const char* defaults; // OptiScaler "Setting = value" list
    const char* quirks;   // OptiScaler GameQuirk names
};

static const OptiGameRow kOptiGameRows[] = {
    { "rdr.exe, playrdr.exe", "", "DxgiSpoofing = false, FGPreserveSwapChain = false", "SkipFsr3Method, NoFSRFGFirstSwapchain" },
    { "", "visionsofmana", "Fsr2Pattern = true, DxgiSpoofing = false", "" },
    { "", "shf", "FGAlwaysCaptureFSRFGSwapchain = true", "" },
    { "", "beastofreincarnation", "EnableFsr2Inputs = false, EnableFsr3Inputs = false, FGAlwaysCaptureFSRFGSwapchain = true", "" },
    { "wwm.exe", "", "DxgiSpoofing = false", "ForceFGRenderSizeMVs" },
    { "endfield.exe", "", "UseFakenvapi = false, OnNonNvidia(VulkanSpoofing = true), OnNonNvidia(VulkanExtensionSpoofing = true)", "ForceCreateD3D12Device, VulkanDLSSBarrierFixup, IgnoreTagsWithoutHudlessForFG" },
    { "", "client", "UseNtdllHooks = false, FGPreserveSwapChain = false", "" },
    { "zenlesszonezero.exe", "", "", "IgnoreTagsWithoutHudlessForFG" },
    { "ninjagaiden4-steam.exe, ninjagaiden4-wingdk.exe", "", "DxgiSpoofing = false, FGSkipResizeBuffers = false, FGPreserveSwapChain = false, WithUpscalerFGInput(FGDisableHUDFix = true)", "" },
    { "doa6lr.exe", "", "DxgiSpoofing = false, FGSkipResizeBuffers = false, FGPreserveSwapChain = false", "" },
    { "tlou-i.exe, tlou-i-l.exe", "", "FGAllowedFrameAhead = 2, DxgiSpoofing = false", "" },
    { "dd2ccs.exe", "", "OnNonNvidia(RestoreComputeSignature = true), DxgiSpoofing = false, WithUpscalerFGInput(FGDisableHUDFix = true)", "" },
    { "dd2.exe", "", "RestoreComputeSignature = true, DxgiSpoofing = false, WithUpscalerFGInput(FGDisableHUDFix = true)", "PregmataFixDLSSModes" },
    { "pragmata_sketchbook.exe", "", "RestoreComputeSignature = true, DxgiSpoofing = false, FGAllowedFrameAhead = 2", "PregmataFixDLSSModes" },
    { "cyberpunk2077.exe", "", "WithUpscalerFGInput(FGDisableHUDFix = true), DxgiSpoofing = false, ReprojectionDepthCutoff = 0.02f", "CyberpunkHudlessState, FSRFGHudlessMismatchFixup" },
    { "afop.exe", "", "EnableFsr2Inputs = false, EnableFsr3Inputs = false, DxgiSpoofing = false, WithUpscalerFGInput(FGDisableHUDFix = true)", "" },
    { "talos2-win64-shipping.exe", "", "FGSkipResizeBuffers = false, FGPreserveSwapChain = false", "" },
    { "", "thecallistoprotocol", "FsrUseFsrInputValues = false, DxgiSpoofing = false, DisableReactiveMask = true, AutoExposure = true", "" },
    { "007firstlight.exe", "", "DxgiSpoofing = false, RestoreComputeSignature = true, ExtendedStateRestore = true, Fsr4DoNotLoadAmdxc64 = true", "IgnoreValidUntilEvaluateForFG" },
    { "", "returnal", "DxgiSpoofing = false, MVResourceBarrier = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, ColorResourceBarrier = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE", "" },
    { "", "b1", "SpoofRegistry = true", "" },
    { "", "avowed", "AutoExposure = true, ColorResourceBarrier = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, EnableFsr2Inputs = false, EnableFsr3Inputs = false", "" },
    { "spider-man.exe", "", "DxgiSpoofing = false, WithUpscalerFGInput(FGDisableHUDFix = true)", "FSRFGHudlessMismatchFixup" },
    { "milesmorales.exe, spider-man2.exe", "", "DxgiSpoofing = false, WithUpscalerFGInput(FGDisableHUDFix = true)", "" },
    { "ds2.exe", "", "DxgiSpoofing = false", "FSRFGHudlessMismatchFixup" },
    { "horizonforbiddenwest.exe", "", "DxgiSpoofing = false, FGAllowedFrameAhead = 2", "" },
    { "ghostoftsushima.exe", "", "DxgiSpoofing = false, WithUpscalerFGInput(FGDisableHUDFix = true)", "" },
    { "dead space.exe", "", "DxgiSpoofing = false, WithXeFG(OverrideVsync = true), WithXeFG(FGXeFGForceBorderless = true), FGSkipResizeBuffers = false", "" },
    { "metroexodus.exe", "", "DxgiSpoofing = false, WithXeFG(FGXeFGForceBorderless = true), AutoExposure = true, WithUpscalerFGInput(FGDisableHUDFix = true)", "" },
    { "outlaws.exe, outlaws_plus.exe", "", "EnableFsr2Inputs = false, EnableFsr3Inputs = false, DxgiSpoofing = false, WithUpscalerFGInput(FGDisableHUDFix = true)", "" },
    { "", "lop", "DxgiSpoofing = false", "" },
    { "acmirage.exe, acmirage_plus.exe", "", "DxgiSpoofing = false, WithUpscalerFGInput(FGDisableHUDFix = true)", "" },
    { "dyinglightgame_x64_rwdi.exe", "", "DxgiSpoofing = false, WithUpscalerFGInput(FGDisableHUDFix = true)", "" },
    { "alanwake2.exe, gotg.exe, u4.exe, u4-l.exe, tll.exe, tll-l.exe", "", "DxgiSpoofing = false, WithUpscalerFGInput(FGDisableHUDFix = true)", "" },
    { "witcher3.exe", "", "DxgiSpoofing = false", "CreateSLOnThe2ndDevice, XeFGCameraMotionFill" },
    { "", "hellbladegame, outriders, medium", "DxgiSpoofing = false, ColorResourceBarrier = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, MVResourceBarrier = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE", "" },
    { "", "sackboy, pumpkinjack, dungeonhaven", "DxgiSpoofing = false, AutoExposure = true, ColorResourceBarrier = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, MVResourceBarrier = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE", "" },
    { "", "pal7", "ColorResourceBarrier = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, MVResourceBarrier = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE", "" },
    { "", "deadisland, indiana", "DisableReactiveMask = true, DxgiSpoofing = false, AutoExposure = true", "" },
    { "reliccoh3.exe, saintsrow_dx12.exe", "scorn, plagueproject, caravansandwitch, genesis", "DxgiSpoofing = false", "" },
    { "f1_22.exe, bloom&rage.exe, flightsimulator.exe, f1manager24.exe, acr.exe", "fmf2, towers, metaleden, bates, bcg, banishers, game, supraworld, keeper, paganidol", "EnableFsr2Inputs = false, EnableFsr3Inputs = false", "" },
    { "", "redout2, recolored", "DxgiSpoofing = false", "" },
    { "dragon age the veilguard.exe, f1_2020_dx12.exe, f1_2021_dx12.exe, gta5_enhanced.exe, sottr.exe", "sb", "WithUpscalerFGInput(FGDisableHUDFix = true)", "" },
    { "gwt.exe", "persistence, mechwarrior, ghostrunner, ghostrunner2", "", "ForceUnrealEngine" },
    { "ff7rebirth_.exe", "", "WithUpscalerFGInput(FGDisableHUDFix = true)", "ForceUnrealEngine" },
    { "", "witchfire", "FsrUseFsrInputValues = false", "" },
    { "soulstice.exe", "", "AutoExposure = true", "ForceUnrealEngine" },
    { "nms.exe", "", "OnNonNvidia(VulkanSpoofing = true)", "KernelBaseHooks, VulkanDLSSBarrierFixup, FSRFGHudlessMismatchFixup" },
    { "wwzretail.exe", "", "UseFsr2VulkanInputs = true, OnNonNvidia(VulkanExtensionSpoofing = true), DxgiSpoofing = false", "ForceDepthD32S8" },
};
static const unsigned kOptiGameRowCount = sizeof(kOptiGameRows)/sizeof(kOptiGameRows[0]);
