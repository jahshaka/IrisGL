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
# that changes a property makes a NEW entry, never a wrong one. What can make a cached byte wrong is the templates and
# the preprocessor (Ogre's template checksum, salted with the compile path's preprocessor group: Ogre RE-PARSES) and the
# container itself (this id). A file of
# ours that writes shader SOURCE text outside properties and pieces belongs in this list; source.shader_cache_key
# keeps that true. The full id stays the texture cache's term and the warm-up's top-up signal.
#
# THE COMPILE PATH's THREE IDS (SHADER-SYSTEM-1 stage 5; the lead's rulings 1-3, the start preflight's F3): the content hash
# of each group of irisgl/cmake/ShaderCompilePath.txt's fork files - JAHSHAKA_COMPILE_PATH_MICROCODE_ID (the SPIR-V
# layer's producer), _PREPROCESSOR_ID (the Hlms preprocessor: the template checksum's salt, and a term of the compute
# layer) and _COMPUTE_ID (the compute layer's producer). They replace the fork's COMMIT in those keys: a fork commit that
# changes no producer moves none of them.
#
# THE RAY SPIR-V's ID, JAHSHAKA_RAY_SPIRV_ID (SHADER-SYSTEM-1 stage 6a): the content hash of the ray tier's generated SPIR-V
# headers (JAH_RAY_SPIRV: irisgl/engine/CMakeLists.txt's JAH_RAY_SPIRV_HEADERS, the same list the JahshakaRayQuerySpv
# target builds), "none" when the tier compiles out. The `.comp` sources are no engine source: before this id a changed
# ray shader moved no id the startup pass or the warm-up keys on, and the ray pipelines it changes are the driver's
# (the pipeline layer, content-addressed by the driver), so nothing re-warmed them. The SPIR-V, not its sources: it is
# what the binary carries (an include, a define or glslang moving it moves this id; a comment does not).
#
# Run with:
#   cmake -DJAH_BUILD_ID_SOURCES=<|-list> -DJAH_BUILD_ID_CACHE_SOURCES=<|-list> -DJAH_RAY_SPIRV=<|-list, may be empty>
#         -DJAH_COMPILE_PATH_LIST=<list file> -DJAH_COMPILE_PATH_ROOT=<the fork's checkout> -DJAH_BUILD_ID_OUT=<header>
#         -P <this>
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
if(JAH_RAY_SPIRV)
    _jah_id_of("${JAH_RAY_SPIRV}" _ray_spirv_id)
else()
    set(_ray_spirv_id "none")
endif()

# The list's groups, each hashed over its files' content in list order (a missing file hashes as "missing": the source
# row reds on it).
if(NOT JAH_COMPILE_PATH_LIST OR NOT JAH_COMPILE_PATH_ROOT)
    message(FATAL_ERROR "EngineBuildId.cmake: JAH_COMPILE_PATH_LIST and JAH_COMPILE_PATH_ROOT are required")
endif()
file(STRINGS "${JAH_COMPILE_PATH_LIST}" _cp_lines)
set(_cp_group "")
foreach(_g microcode preprocessor compute)
    set(_cp_${_g} "")
endforeach()
foreach(_l IN LISTS _cp_lines)
    string(STRIP "${_l}" _l)
    if(_l STREQUAL "" OR _l MATCHES "^#")
        continue()
    elseif(_l MATCHES "^\\[([a-z]+)\\]$")
        set(_cp_group "${CMAKE_MATCH_1}")
    elseif(_cp_group)
        list(APPEND _cp_${_cp_group} "${JAH_COMPILE_PATH_ROOT}/${_l}")
    endif()
endforeach()
foreach(_g microcode preprocessor compute)
    if(NOT _cp_${_g})
        message(FATAL_ERROR "EngineBuildId.cmake: ${JAH_COMPILE_PATH_LIST} has no [${_g}] group")
    endif()
    string(REPLACE ";" "|" _cp_arg "${_cp_${_g}}")
    _jah_id_of("${_cp_arg}" _cp_id_${_g})
endforeach()

set(_tmp "${JAH_BUILD_ID_OUT}.tmp")
file(WRITE "${_tmp}"
"// GENERATED — do not edit, do not commit. See irisgl/cmake/EngineBuildId.cmake.\n\
#pragma once\n\
#define JAHSHAKA_ENGINE_BUILD_ID \"${_id}\"\n\
#define JAHSHAKA_SHADER_CACHE_CODE_ID \"${_cache_id}\"\n\
#define JAHSHAKA_RAY_SPIRV_ID \"${_ray_spirv_id}\"\n\
#define JAHSHAKA_COMPILE_PATH_MICROCODE_ID \"${_cp_id_microcode}\"\n\
#define JAHSHAKA_COMPILE_PATH_PREPROCESSOR_ID \"${_cp_id_preprocessor}\"\n\
#define JAHSHAKA_COMPILE_PATH_COMPUTE_ID \"${_cp_id_compute}\"\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_tmp}" "${JAH_BUILD_ID_OUT}")
file(REMOVE "${_tmp}")
