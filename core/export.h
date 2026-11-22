// IRIS_CORE_API - WHAT libIrisCore EXPORTS (NOQT-CORE-1 stage 1; the lead's ruling 2026-11-22: IrisCore is ONE SHARED library,
// so every stateful symbol - the temp serial, the guid engine, the log sink - exists ONCE in a process, on every platform,
// by construction and not by link order). The library builds with hidden visibility (irisgl/core/CMakeLists.txt); only what
// carries this macro is exported - the iris:: API, never a helper of a .cpp. Header-only templates (iris::Signal, iris::bound)
// need none.
#pragma once

#if defined(_WIN32)
#  if defined(IRIS_CORE_BUILD)
#    define IRIS_CORE_API __declspec(dllexport)
#  else
#    define IRIS_CORE_API __declspec(dllimport)
#  endif
#else
#  define IRIS_CORE_API __attribute__((visibility("default")))
#endif
