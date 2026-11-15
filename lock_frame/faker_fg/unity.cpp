// ============================================================================
//  unity.cpp - the translation unit fakerFG.dll is compiled from.
//
//  One TU for the same reason as the add-on: every unit keeps its state in an
//  anonymous namespace, so including them is safe, and the build script (and the
//  .def that maps the export names) only ever sees one object.
//
//  Order matters: faker_fg.cpp owns the record/API the rest publishes into,
//  provider.cpp calls into backend_xefg.cpp, and proxy_dxgi.cpp calls
//  xefg_start(). Headers make the dependencies explicit; this order is just the
//  one the MSVC single-pass compiler wants.
// ============================================================================
#include "faker_fg.cpp"
#include "producer_link.cpp"
#include "nvapi_arch.cpp"
#include "proxy_streamline.cpp"
#include "proxy_ffx.cpp"
#include "backend_xefg.cpp"
#include "mv_resample.cpp"
#include "passes.cpp"
#include "fg_swapchain_hooks.cpp"
#include "provider.cpp"
#include "proxy_nvngx.cpp"
#include "proxy_ngx.cpp"
#include "proxy_kernel.cpp"
#include "proxy_dxgi.cpp"
