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
# THE SECOND ID, JAHSHAKA_SHADER_CACHE_CODE_ID (TEST-STREAMLINE-1 1a, 2026-11-03): the hash of the C++ that
# WRITES the hlms layer's bytes (JAH_BUILD_ID_CACHE_SOURCES: OgreShaderCache.cpp - the container, kCacheFormat).
# It replaces JAHSHAKA_ENGINE_BUILD_ID in the HLMS LAYER's key: an entry of Ogre's Hlms disk cache is found by its
# MERGED properties and pieces, taken AFTER every C++ hook (OgreHlms.cpp, createShaderCacheEntry: the listener's and
# the Hlms' notifyPropertiesMergedPreGenerationStep run first), the root layout is made fresh at every compile
# (Hlms::compileShaderCode -> setupRootLayout, a cached source included), the PSOs are re-made at runtime
# (applyStrongBlockRules) and applyTo restores only pass property sets (OgreHlmsDiskCache.cpp) - so an engine edit
# that changes a property makes a NEW entry, never a wrong one. What can make a cached byte wrong is the templates
# (Ogre's checksum + the media= / photon= terms), the fork (ogre=) and the container itself (this id). A file of
# ours that writes shader SOURCE text outside properties and pieces belongs in this list; source.shader_cache_key
# keeps that true. The full id stays the texture cache's term and the warm-up's top-up signal.
#
# Run with:
#   cmake -DJAH_BUILD_ID_SOURCES=<|-list> -DJAH_BUILD_ID_CACHE_SOURCES=<|-list> -DJAH_BUILD_ID_OUT=<header> -P <this>
#
# Writes only when the value actually changes (via copy_if_different), so a
# rebuild that touched nothing relevant does not cascade a recompile of every
# translation unit that includes the header.

# The file list arrives '|'-separated: a ';' inside a -D value is eaten by
# CMake's own list handling before the script ever sees it.
# Each id is the sha256 of its files' contents in list order - never a path (a record serves every tree, #154).
function(_jah_id_of _list _out)
    string(REPLACE "|" ";" _files "${_list}")
    set(_blob "")
    foreach(_s IN LISTS _files)
        if(EXISTS "${_s}")
            file(SHA256 "${_s}" _h)
        else()
            set(_h "missing")
        endif()
        string(APPEND _blob "${_h};")
    endforeach()
    string(SHA256 _id "${_blob}")
    set(${_out} "${_id}" PARENT_SCOPE)
endfunction()
if(NOT JAH_BUILD_ID_CACHE_SOURCES)
    message(FATAL_ERROR "EngineBuildId.cmake: JAH_BUILD_ID_CACHE_SOURCES (the shader cache's own code) is required")
endif()
_jah_id_of("${JAH_BUILD_ID_SOURCES}" _id)
_jah_id_of("${JAH_BUILD_ID_CACHE_SOURCES}" _cache_id)

set(_tmp "${JAH_BUILD_ID_OUT}.tmp")
file(WRITE "${_tmp}"
"// GENERATED — do not edit, do not commit. See irisgl/cmake/EngineBuildId.cmake.\n\
#pragma once\n\
#define JAHSHAKA_ENGINE_BUILD_ID \"${_id}\"\n\
#define JAHSHAKA_SHADER_CACHE_CODE_ID \"${_cache_id}\"\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_tmp}" "${JAH_BUILD_ID_OUT}")
file(REMOVE "${_tmp}")
