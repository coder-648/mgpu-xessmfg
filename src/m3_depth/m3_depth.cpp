// ============================================================================
// M3.1 - ReShade depth acquisition probe
//
// Goal: prove we can reach the game's depth buffer through ReShade, in the shape
// the transport needs (2560x1440), without touching the rendered image.
//
// Method: the companion effect (effects/mgpu_depth_tap.fx) declares a texture
// with the DEPTH semantic. ReShade binds the game's depth buffer into it, and
// an add-on can then fetch the real resource:
//
//     find_texture_variable("mgpu_depth_tap.fx", "MGPU_SceneDepthTex")
//     get_texture_binding(var, &srv, nullptr)
//     device->get_resource_from_view(srv)   -> the depth texture
//
// This build only observes and logs. Converting to R16_UNORM and pushing it
// into the mailbox is M3.2, once the shape is confirmed.
//
// Log: <game>/bin/x64/m3_depth.log
// ============================================================================

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "reshade.hpp"

using namespace reshade::api;

static FILE*            g_log = nullptr;
static HMODULE          g_self = nullptr;
static effect_runtime*  g_runtime = nullptr;
static uint64_t         g_present_count = 0;

// The effect file name as ReShade knows it (file name with extension).
static const char* kEffectName = "mgpu_depth_tap.fx";

static void logf_(const char* fmt, ...)
{
    if (!g_log) { return; }
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fflush(g_log);
}

static void open_log()
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(g_self, path, MAX_PATH);
    if (char* dot = strrchr(path, '.')) {
        strcpy_s(dot, MAX_PATH - (dot - path), ".log");
    }
    fopen_s(&g_log, path, "w");
    if (g_log) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        logf_("=== m3_depth  %04d-%02d-%02d %02d:%02d:%02d ===\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
}

static const char* format_name(format f)
{
    // Only the formats that matter here; everything else prints numerically.
    switch (f) {
    case format::unknown:          return "unknown";
    case format::r32_typeless:     return "R32_TYPELESS";
    case format::d32_float:        return "D32_FLOAT";
    case format::r32_float:        return "R32_FLOAT";
    case format::r16_unorm:        return "R16_UNORM";
    case format::r16g16_float:     return "R16G16_FLOAT";
    case format::r10g10b10a2_unorm:return "R10G10B10A2_UNORM";
    case format::r16g16b16a16_float:return "R16G16B16A16_FLOAT";
    case format::r8g8b8a8_unorm:   return "R8G8B8A8_UNORM";
    default:                       return "other";
    }
}

static void log_resource(const char* label, device* dev, resource res, resource_view srv)
{
    if (res.handle == 0) {
        logf_("    %-16s : <none> (srv=%llu)\n", label, (unsigned long long)srv.handle);
        return;
    }
    const resource_desc d = dev->get_resource_desc(res);
    logf_("    %-16s : res=%llu srv=%llu  %ux%u  fmt=%s(%d)  samp=%u  levels=%u\n",
          label, (unsigned long long)res.handle, (unsigned long long)srv.handle,
          d.texture.width, d.texture.height,
          format_name(d.texture.format), static_cast<int>(d.texture.format),
          d.texture.samples, d.texture.levels);
}

// --------------------------------------------------------------- the probe

static void probe_semantic(effect_runtime* rt, device* dev,
                           const char* variable_name, const char* label)
{
    const effect_texture_variable var = rt->find_texture_variable(kEffectName, variable_name);
    if (var.handle == 0) {
        logf_("    %-16s : variable not found (effect not loaded?)\n", label);
        return;
    }

    resource_view srv{};
    rt->get_texture_binding(var, &srv, nullptr);

    resource res{};
    if (srv.handle != 0) {
        res = dev->get_resource_from_view(srv);
    }
    log_resource(label, dev, res, srv);
}

static void probe(effect_runtime* rt)
{
    device* dev = rt->get_device();

    uint32_t w = 0, h = 0;
    rt->get_screenshot_width_and_height(&w, &h);
    logf_("    screenshot size  : %ux%u   back buffers=%u\n",
          w, h, rt->get_back_buffer_count());

    resource_view dummy{};
    log_resource("backbuffer", dev, rt->get_current_back_buffer(), dummy);
    probe_semantic(rt, dev, "MGPU_SceneDepthTex",  "depth");
    probe_semantic(rt, dev, "MGPU_MotionProbeTex", "motion probe");
}

// ------------------------------------------------------------------- events

static void on_init_effect_runtime(effect_runtime* rt)
{
    g_runtime = rt;
    uint32_t w = 0, h = 0;
    rt->get_screenshot_width_and_height(&w, &h);
    logf_("\ninit_effect_runtime : %p   buffer %ux%u   back buffers=%u\n",
          (void*)rt, w, h, rt->get_back_buffer_count());
}

static void on_destroy_effect_runtime(effect_runtime* rt)
{
    logf_("\ndestroy_effect_runtime : %p\n", (void*)rt);
    if (g_runtime == rt) { g_runtime = nullptr; }
}

static void on_reshade_present(effect_runtime* rt)
{
    ++g_present_count;
    if (g_present_count % 120 != 1) { return; }   // ~every 3 s at 40 fps
    logf_("\n-- present #%llu\n", (unsigned long long)g_present_count);
    probe(rt);
}

// ------------------------------------------------------------------ add-on

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
    g_self = addon_module;
    open_log();
    logf_("AddonInit (addon=%p reshade=%p)\n", (void*)addon_module, (void*)reshade_module);

    if (reshade::register_addon(addon_module, reshade_module)) {
        logf_("registered with ReShade\n");
    } else {
        logf_("reshade::register_addon FAILED\n");
    }

    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);

    logf_("events registered: init_effect_runtime, destroy_effect_runtime, reshade_present\n");
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
    logf_("AddonUninit (presents observed: %llu)\n", (unsigned long long)g_present_count);

    reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);

    reshade::unregister_addon(addon_module, reshade_module);

    if (g_log) { fclose(g_log); g_log = nullptr; }
}

