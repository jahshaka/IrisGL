# Generates the engine's shader-cache BUILD ID header, at BUILD time.
#
# WHY IT IS NOT A CONFIGURE-TIME `-D` ANY MORE (LIGHTING_FIX fix 10 / F5).
# JAHSHAKA_ENGINE_BUILD_ID is a term of the shader-cache fingerprint whose job
# is "the C++ that decides which Hlms properties get set has changed, so the
# cached shaders may be wrong". It used to be computed by `file(SHA256 ...)` at
# CMake CONFIGURE time — but editing OgreEngine.cpp does not re-run configure,
# so the very edit the term exists to notice left it unchanged. An incremental
# engine edit therefore shipped new shader-generation code against a cache that
# still looked valid: the exact failure the fingerprint is supposed to prevent,
# and the reason "delete the cache" was folk wisdom in this tree.
#
# Run with:
#   cmake -DJAH_BUILD_ID_SOURCES=<;-list> -DJAH_BUILD_ID_OUT=<header> -P <this>
#
# Writes only when the value actually changes (via copy_if_different), so a
# rebuild that touched nothing relevant does not cascade a recompile of every
# translation unit that includes the header.

# The file list arrives '|'-separated: a ';' inside a -D value is eaten by
# CMake's own list handling before the script ever sees it.
string(REPLACE "|" ";" JAH_BUILD_ID_SOURCES "${JAH_BUILD_ID_SOURCES}")
set(_blob "")
foreach(_s IN LISTS JAH_BUILD_ID_SOURCES)
    if(EXISTS "${_s}")
        file(SHA256 "${_s}" _h)
    else()
        set(_h "missing")
    endif()
    string(APPEND _blob "${_h};")
endforeach()
string(SHA256 _id "${_blob}")

set(_tmp "${JAH_BUILD_ID_OUT}.tmp")
file(WRITE "${_tmp}"
"// GENERATED — do not edit, do not commit. See irisgl/cmake/EngineBuildId.cmake.\n\
#pragma once\n\
#define JAHSHAKA_ENGINE_BUILD_ID \"${_id}\"\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_tmp}" "${JAH_BUILD_ID_OUT}")
file(REMOVE "${_tmp}")
