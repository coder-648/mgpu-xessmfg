// ============================================================================
//  lock_frame/version.h - 正式版版本号（消费者 / UI / 发布脚本共用）
//
//  协议版本仍在 include/mgpu_shared.h 的 kSharedVersion；这里只放产品版本。
//  发布脚本会读取 LOCK_FRAME_VERSION_STRING 生成 manifest 和包名。
// ============================================================================
#pragma once

#define LOCK_FRAME_VERSION_MAJOR 1
#define LOCK_FRAME_VERSION_MINOR 2
#define LOCK_FRAME_VERSION_PATCH 5
#define LOCK_FRAME_VERSION_STRING "1.2.5"
#define LOCK_FRAME_VERSION_WSTRING L"1.2.5"
