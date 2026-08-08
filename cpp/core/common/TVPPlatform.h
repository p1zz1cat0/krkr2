#pragma once

//---------------------------------------------------------------------------
// 平台宏（不依赖 cocos2d 头文件）
//---------------------------------------------------------------------------
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define TVP_PLATFORM_IOS 1
#endif
#endif

#if defined(__ANDROID__)
#define TVP_PLATFORM_ANDROID 1
#endif

#if defined(_WIN32)
#define TVP_PLATFORM_WINDOWS 1
#endif

#if defined(TVP_PLATFORM_IOS) || defined(TVP_PLATFORM_ANDROID)
#define TVP_PLATFORM_MOBILE 1
#endif
