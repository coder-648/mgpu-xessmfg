// ============================================================================
//  unity.cpp - the translation unit xess_mfg_consumer.exe is compiled from.
//
//  Same pattern as fakerFG.dll (src/faker_fg/unity.cpp): the implementation is
//  split into modules/*.cpp for readability, each module keeps its state in
//  static/private objects, and this file includes them in dependency order.
//  There is no .inc file and no build step that concatenates sources.
//
//  Order matters: platform owns logging/exit, devices owns WGC/devices, the
//  input-cache modules publish the producer copy, frame owns run_frame(), and
//  main.cpp parses args, initializes everything and runs the M1 event loop.
// ============================================================================
#include "consumer_internal.h"

#include "modules/platform.cpp"
#include "modules/state_overlay.cpp"
#include "modules/devices_capture_fg.cpp"
#include "modules/cadence.cpp"
#include "modules/shaders.cpp"
#include "modules/frame_io.cpp"
#include "modules/osd.cpp"
#include "modules/warp_upsample.cpp"
#include "modules/stream_auto.cpp"
#include "modules/convert_warp.cpp"
#include "modules/pair_dump.cpp"
#include "modules/dump_of.cpp"
#include "modules/input_cache.cpp"
#include "modules/frame.cpp"
#include "modules/probe_mfg.cpp"
#include "modules/main.cpp"
