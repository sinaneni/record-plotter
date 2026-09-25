// App version and identity strings, shared by app.rc and the C++ sources.
#pragma once

// Version numbers.
#define APP_VER_MAJOR 0
#define APP_VER_MINOR 1
#define APP_VER_PATCH 0
#define APP_VER_BUILD 0

// Release channel suffix, with a leading space; empty for a final release.
#define APP_CHANNEL_STR  " preview"
#define APP_CHANNEL_STRW L" preview"

// ------------------------------------------------------------- which build is this
// Build commit id from build.bat, or "unknown".
#if defined(__has_include)
#  if __has_include("build_id.h")
#    include "build_id.h"
#  endif
#endif
#ifndef APP_BUILD_ID
#  define APP_BUILD_ID "unknown"
#endif

// ------------------------------------------------------------- derived strings
#define APP_STR_(x) #x
#define APP_STR(x)  APP_STR_(x)
#define WIDEN_(x) L##x
#define WIDEN(x)  WIDEN_(x)
#define APP_WSTR(x) WIDEN(APP_STR(x))

// Comma form for the version resource.
#define APP_VERSION_COMMAS APP_VER_MAJOR, APP_VER_MINOR, APP_VER_PATCH, APP_VER_BUILD

#define APP_VERSION_STR   APP_STR(APP_VER_MAJOR) "." APP_STR(APP_VER_MINOR) "." \
                          APP_STR(APP_VER_PATCH) "." APP_STR(APP_VER_BUILD)
#define APP_VERSION_SHORT APP_STR(APP_VER_MAJOR) "." APP_STR(APP_VER_MINOR) "." \
                          APP_STR(APP_VER_PATCH)
#define APP_VERSION_FULL  APP_VERSION_SHORT APP_CHANNEL_STR

#define APP_VERSION_SHORTW APP_WSTR(APP_VER_MAJOR) L"." APP_WSTR(APP_VER_MINOR) L"." \
                           APP_WSTR(APP_VER_PATCH)
#define APP_VERSION_FULLW  APP_VERSION_SHORTW APP_CHANNEL_STRW

// ------------------------------------------------------------------- identity
// UTF-8 literals.
#define APP_NAME_STR       "Record Plotter"
#define APP_COMPANY_STR    "Sina Neni"
#define APP_COPYRIGHT_STR  "Copyright © 2026 Sina Neni. All rights reserved."
#define APP_DESCRIPTION    "Record Plotter"

#define APP_NAME_STRW      L"Record Plotter"
#define APP_COMPANY_STRW   L"Sina Neni"
#define APP_COPYRIGHT_STRW L"Copyright © 2026 Sina Neni. All rights reserved."
#define APP_DESCRIPTIONW   L"Record Plotter"
