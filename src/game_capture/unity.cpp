// ============================================================================
// unity.cpp - the translation unit the add-on is actually compiled from.
//
// `build_addon.bat` compiles exactly one .cpp (cl cannot use a UNC cwd, and the
// staging directory only receives the sources), while this module is now the host
// plus one file per input source. Every unit here declares its state `static` /
// in an anonymous namespace, so including them into one TU is safe and keeps the
// build script (and every tests/* module) untouched.
//
// Adding a source = one more include + one make_*_source() call in AddonInit.
// ============================================================================
#include "game_capture.cpp"
#include "in_ngx.cpp"
#include "in_ffx.cpp"
#include "in_xess.cpp"
