# The engine abstraction and its Ogre-Next backend.
# Studio links this; Studio never links Ogre directly.
# The backend is split across Ogre-private translation units; EnginePrivate.h
# holds the shared declarations and is never installed. The list is exported so
# the sanitised copy in tests/engine builds exactly the same sources.
set(JAHSHAKA_ENGINE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EnginePrivate.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreChain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreLights.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreMesh.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreMaterials.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreSky.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreGrid.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreShadow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreGi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgrePlanar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreParticles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreDecals.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreFog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreSkeleton.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreShaderCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreTextureCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreLogBridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreOverlayHud.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreClips.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreSockets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreFrameMonitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreCompute.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreRayQuery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreScreenProbeGather.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreSurfaceCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreVrSession.cpp)
set(JAHSHAKA_ENGINE_SOURCES "${JAHSHAKA_ENGINE_SOURCES}" PARENT_SCOPE)

# --- The hardware ray-query tier (PHOTON_SPEC §7 R1) -------------------------
# OgreRayQuery.cpp is the ONE TU that includes Vulkan itself: it needs the
# Vulkan render system's own headers (VulkanDevice, VulkanQueue,
# VulkanBufferInterface), the loader, and a compute shader compiled at SPIR-V
# 1.4 — which is what SPV_KHR_ray_query documents and what the pin's own GLSL
# path can never emit (OgreVulkanProgram.cpp never sets a target environment).
#
# NOTHING HERE IS FATAL. A platform without the Vulkan plugin (macOS: MoltenVK
# exposes neither VK_KHR_ray_query nor VK_KHR_acceleration_structure), without
# the Vulkan headers, or without glslang simply compiles the TU to nothing and
# takes the no-rays path — which is a supported picture, not a degraded build.
# (The S3 spike's find_library + FATAL_ERROR was audit C-6's macOS defect; this
# is the correction.)
set(JAH_RAY_QUERY_OK OFF)
if(UNIX AND NOT APPLE)
    # THE PLUGIN IS NOT A "lib" (both platforms). Ogre installs its render
    # systems UNPREFIXED — `RenderSystem_Vulkan.so` on Linux,
    # `RenderSystem_Vulkan.dylib` on macOS — and find_library() only ever tries
    # the prefixes in CMAKE_FIND_LIBRARY_PREFIXES, which is exactly `lib` here.
    # So a bare NAMES entry finds NOTHING on either platform, and the failure is
    # SILENT: the tier compiles out and the build stays green. (The S3 spike hid
    # this behind a `.so` literal that broke macOS — audit C-6; dropping the
    # literal to fix macOS broke Linux the same way. Name the file, with its
    # suffix, for each platform, and the question does not arise again.)
    unset(OGRE_NEXT_VULKAN_RS_LIB CACHE)
    find_library(OGRE_NEXT_VULKAN_RS_LIB
                 NAMES RenderSystem_Vulkan.so RenderSystem_Vulkan.dylib RenderSystem_Vulkan
                 PATHS "${OGRE_NEXT_PLUGIN_DIR}" "${OGRE_NEXT_PREFIX}/lib/OGRE-Next"
                       "${OGRE_NEXT_PREFIX}/lib"
                 NO_DEFAULT_PATH)
    find_package(Vulkan QUIET)
    find_program(JAH_GLSLANG_VALIDATOR NAMES glslangValidator glslang)
    if(OGRE_NEXT_VULKAN_RS_LIB AND Vulkan_FOUND AND JAH_GLSLANG_VALIDATOR
       AND EXISTS "${OGRE_NEXT_PREFIX}/include/OGRE-Next/RenderSystems/Vulkan/OgreVulkanDevice.h")
        set(JAH_RAY_QUERY_OK ON)
    endif()
    # SAY WHICH PICTURE THIS BUILD RENDERS. A tier that compiles out is a
    # supported outcome, but never a silent one: a lane that expected rays and
    # got the fallback must be able to see it in the configure log.
    if(JAH_RAY_QUERY_OK)
        message(STATUS "Jahshaka: hardware ray-query tier COMPILED IN "
                       "(${OGRE_NEXT_VULKAN_RS_LIB})")
    else()
        message(STATUS "Jahshaka: hardware ray-query tier COMPILED OUT — plugin="
                       "'${OGRE_NEXT_VULKAN_RS_LIB}' vulkan=${Vulkan_FOUND} "
                       "glslang='${JAH_GLSLANG_VALIDATOR}'")
    endif()
endif()

if(JAH_RAY_QUERY_OK)
    set(JAH_RQ_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/rayquery")
    file(MAKE_DIRECTORY "${JAH_RQ_GEN_DIR}")
    # BUILD-TIME SPIR-V, at spirv1.4 — never committed bytes (the spike
    # committed SPIR-V 1.5 headers, which are legal only on a device at
    # apiVersion >= 1.2 and which no reviewer can read: audit C-7/C-10).
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_rays_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --target-env spirv1.4 -S comp -x
                --vn krq_raysSpv -o "${JAH_RQ_GEN_DIR}/rq_rays_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_rays.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_rays.comp"
        COMMENT "Compiling the ray-query compute shader at SPIR-V 1.4"
        VERBATIM)
    # R5's reflection trace, same rule (PHOTON_SPEC §7 R5).
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_reflect_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --target-env spirv1.4 -S comp -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include"
                --vn krq_reflectSpv -o "${JAH_RQ_GEN_DIR}/rq_reflect_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect.comp"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include/jah_rq_hit.glsl"
        COMMENT "Compiling the ray-traced reflection compute shader at SPIR-V 1.4"
        VERBATIM)
    # ...and R5's spatial filter + composite (round C), same rule.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_reflect_filter_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --target-env spirv1.4 -S comp -x
                --vn krq_reflectFilterSpv -o "${JAH_RQ_GEN_DIR}/rq_reflect_filter_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect_filter.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect_filter.comp"
        COMMENT "Compiling the reflection filter/composite compute shader at SPIR-V 1.4"
        VERBATIM)
    # THE SCREEN-PROBE GATHER'S THREE JOBS (GATHER-1a), same rule plus ONE
    # addition: `-I src/rayquery/include`, so the hit-radiance functions and the
    # parameter block are SHARED with the reflection trace rather than copied
    # (spec section 10 item 7 — glslang resolves `#include` under
    # GL_GOOGLE_include_directive, which each shader requires). The headers are
    # named in DEPENDS or a change to one would not rebuild the SPIR-V.
    set(JAH_RQ_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include")
    set(JAH_RQ_HEADERS "${JAH_RQ_INCLUDE}/jah_rq_hit.glsl"
                       "${JAH_RQ_INCLUDE}/jah_probe_params.glsl")
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_probe_place_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}"
                --vn krq_probePlaceSpv -o "${JAH_RQ_GEN_DIR}/rq_probe_place_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_place.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_place.comp"
                ${JAH_RQ_HEADERS}
        COMMENT "Compiling the screen-probe placement job at SPIR-V 1.4"
        VERBATIM)
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_probe_gather_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}"
                --vn krq_probeGatherSpv -o "${JAH_RQ_GEN_DIR}/rq_probe_gather_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_gather.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_gather.comp"
                ${JAH_RQ_HEADERS}
        COMMENT "Compiling the screen-probe gather trace at SPIR-V 1.4"
        VERBATIM)
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_probe_integrate_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}"
                --vn krq_probeIntegrateSpv -o "${JAH_RQ_GEN_DIR}/rq_probe_integrate_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_integrate.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_integrate.comp"
                ${JAH_RQ_HEADERS}
        COMMENT "Compiling the screen-probe integrate at SPIR-V 1.4"
        VERBATIM)
    add_custom_target(JahshakaRayQuerySpv
                      DEPENDS "${JAH_RQ_GEN_DIR}/rq_rays_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_reflect_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_reflect_filter_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_probe_place_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_probe_gather_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_probe_integrate_spv.h")
    set(JAH_RQ_GEN_DIR "${JAH_RQ_GEN_DIR}" PARENT_SCOPE)
endif()
set(JAH_RAY_QUERY_OK "${JAH_RAY_QUERY_OK}" PARENT_SCOPE)
set(OGRE_NEXT_VULKAN_RS_LIB "${OGRE_NEXT_VULKAN_RS_LIB}" PARENT_SCOPE)

# Applied to BOTH the engine library and the tests' sanitised copy of the same
# sources, so `test_engine_asan` instruments the tier too.
function(jahshaka_engine_ray_query target)
    if(NOT JAH_RAY_QUERY_OK)
        return()
    endif()
    target_compile_definitions(${target} PRIVATE JAH_RAY_QUERY=1)
    # BOTH Vulkan header roots: the install flattens the render system's own
    # headers into RenderSystems/Vulkan, but those headers include their
    # siblings as `Vao/OgreVulkanBufferInterface.h`, which only resolves under
    # RenderSystems/Vulkan/include.
    target_include_directories(${target} PRIVATE
        "${OGRE_NEXT_PREFIX}/include/OGRE-Next/RenderSystems/Vulkan"
        "${OGRE_NEXT_PREFIX}/include/OGRE-Next/RenderSystems/Vulkan/include"
        ${Vulkan_INCLUDE_DIRS}
        "${JAH_RQ_GEN_DIR}/..")
    target_link_libraries(${target} PRIVATE "${OGRE_NEXT_VULKAN_RS_LIB}" ${Vulkan_LIBRARIES})
    add_dependencies(${target} JahshakaRayQuerySpv)
endfunction()

# --- OpenXR (SPECS/VR_SPEC.md §4.1, phase 2) ---------------------------------
# OgreVrSession.cpp is the ONE TU that includes OpenXR and the second (after
# OgreRayQuery.cpp) that includes Vulkan. Same rule as the ray tier, for the
# same reason: a box without libopenxr-dev, or a platform without the Vulkan
# render system's headers (macOS), compiles the whole file to five refusals and
# builds green — `Engine::vrAvailable()` simply answers false.
#
# find_package(OpenXR) reads /usr/lib/<triplet>/cmake/openxr/OpenXRConfig.cmake
# from libopenxr-dev and exports OpenXR::openxr_loader. The loader is linked,
# never dlopen'd by us: the loader is itself the thing that dlopens a runtime,
# and it costs ~1 ms and no file access when no manifest resolves (phase 1a §6).
set(JAH_VR_OK OFF)
if(UNIX AND NOT APPLE)
    # BY PATH, NOT BY IMPORTED TARGET. `find_package(OpenXR)` creates
    # OpenXR::openxr_loader in the DIRECTORY that called it, and the sanitised
    # twin of these sources is configured from tests/engine — a sibling scope,
    # where that target does not exist ("the target was not found", measured).
    # A library path and a header probe cross scopes through PARENT_SCOPE like
    # OGRE_NEXT_VULKAN_RS_LIB already does.
    find_package(Vulkan QUIET)
    find_library(JAH_OPENXR_LOADER NAMES openxr_loader)
    find_path(JAH_OPENXR_INCLUDE_DIR openxr/openxr.h)
    if(JAH_OPENXR_LOADER AND JAH_OPENXR_INCLUDE_DIR AND Vulkan_FOUND AND OGRE_NEXT_VULKAN_RS_LIB
       AND EXISTS "${OGRE_NEXT_PREFIX}/include/OGRE-Next/RenderSystems/Vulkan/OgreVulkanDevice.h")
        set(JAH_VR_OK ON)
    endif()
    if(JAH_VR_OK)
        message(STATUS "Jahshaka: OpenXR (VR) COMPILED IN (${JAH_OPENXR_LOADER})")
    else()
        message(STATUS "Jahshaka: OpenXR (VR) COMPILED OUT — loader='${JAH_OPENXR_LOADER}' "
                       "headers='${JAH_OPENXR_INCLUDE_DIR}' vulkan=${Vulkan_FOUND} "
                       "rs='${OGRE_NEXT_VULKAN_RS_LIB}'")
    endif()
endif()
set(JAH_VR_OK "${JAH_VR_OK}" PARENT_SCOPE)
set(JAH_OPENXR_LOADER "${JAH_OPENXR_LOADER}" PARENT_SCOPE)
set(JAH_OPENXR_INCLUDE_DIR "${JAH_OPENXR_INCLUDE_DIR}" PARENT_SCOPE)

# Applied to BOTH the engine library and the tests' sanitised copy of the same
# sources, exactly like jahshaka_engine_ray_query.
function(jahshaka_engine_vr target)
    if(NOT JAH_VR_OK)
        target_compile_definitions(${target} PRIVATE JAH_VR=0)
        return()
    endif()
    target_compile_definitions(${target} PRIVATE JAH_VR=1)
    target_include_directories(${target} PRIVATE
        "${OGRE_NEXT_PREFIX}/include/OGRE-Next/RenderSystems/Vulkan"
        "${OGRE_NEXT_PREFIX}/include/OGRE-Next/RenderSystems/Vulkan/include"
        "${JAH_OPENXR_INCLUDE_DIR}"
        ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE "${JAH_OPENXR_LOADER}"
                          "${OGRE_NEXT_VULKAN_RS_LIB}" ${Vulkan_LIBRARIES})
endfunction()

# --- Shader-cache fingerprint terms (SHADER_CACHE_SPEC.md §4.2) --------------
# Two things no hash inside Ogre can see, baked in at configure time:
#
#  * JAHSHAKA_OGRE_PATCH_SERIES — a hash of irisgl/thirdparty/ogre-patches/*.patch
#    plus the submodule's pinned SHA. Four of our patches change SHADER media and
#    the rest change engine C++; a re-run of build-ogre.sh with a new patch
#    produces a different .so and identical Ogre-side hashes, so without this term
#    a stale cache would look perfectly valid to Ogre and load.
#  * JAHSHAKA_ENGINE_BUILD_ID — a hash of the engine's OWN sources. This TU set
#    decides which Hlms properties are set, which datablocks exist and which
#    listener extends the pass buffer. Editing OgreEngine.cpp changes what
#    shaders mean without changing one byte of any template.
#
# Configure-time, not build-time: both are inputs to a cache whose only failure
# mode is "used a stale entry", and CMake re-configures whenever these files
# change (the glob is CONFIGURE_DEPENDS and the source list is explicit).
set(_ogre_patch_dir "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/ogre-patches")
set(_fp_blob "")
file(GLOB _ogre_patches CONFIGURE_DEPENDS "${_ogre_patch_dir}/*.patch")
foreach(_p IN LISTS _ogre_patches)
    file(SHA256 "${_p}" _h)
    get_filename_component(_n "${_p}" NAME)
    string(APPEND _fp_blob "${_n}:${_h};")
endforeach()
# The pin itself: a patch series applied to a different upstream commit is a
# different engine. `git ls-tree` reads the gitlink without needing the
# submodule to be checked out at all.
execute_process(COMMAND git ls-tree HEAD thirdparty/ogre-next
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
                OUTPUT_VARIABLE _ogre_pin ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
string(APPEND _fp_blob "pin:${_ogre_pin};")
string(SHA256 _ogre_patch_series "${_fp_blob}")

# JAHSHAKA_ENGINE_BUILD_ID IS GENERATED AT BUILD TIME, not here (LIGHTING_FIX
# fix 10 / F5). It used to be a configure-time `file(SHA256 ...)` fold, which
# could not do the one job it exists for: editing OgreEngine.cpp does not re-run
# CMake, so the term stayed identical across exactly the edits that change what
# the generated shaders MEAN, and the cache from the previous build looked
# perfectly valid. A custom command re-hashes the same file list on every build
# and writes the value into a generated header; `copy_if_different` inside the
# script keeps an unchanged value from cascading a rebuild.
set(JAHSHAKA_ENGINE_BUILD_ID_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/jahshaka/engine/Engine.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/jahshaka/engine/Types.h)
set(_build_id_inputs ${JAHSHAKA_ENGINE_SOURCES} ${JAHSHAKA_ENGINE_BUILD_ID_HEADERS})
set(JAHSHAKA_ENGINE_BUILD_ID_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_build_id_header "${JAHSHAKA_ENGINE_BUILD_ID_DIR}/jahshaka_engine_build_id.h")
file(MAKE_DIRECTORY "${JAHSHAKA_ENGINE_BUILD_ID_DIR}")
string(REPLACE ";" "|" _build_id_inputs_arg "${_build_id_inputs}")
add_custom_command(
    OUTPUT "${_build_id_header}"
    COMMAND ${CMAKE_COMMAND}
            "-DJAH_BUILD_ID_SOURCES=${_build_id_inputs_arg}"
            "-DJAH_BUILD_ID_OUT=${_build_id_header}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/EngineBuildId.cmake"
    DEPENDS ${_build_id_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/EngineBuildId.cmake"
    COMMENT "Hashing the engine sources for the shader-cache fingerprint"
    VERBATIM)
add_custom_target(JahshakaEngineBuildId DEPENDS "${_build_id_header}")
# Exported so tests/engine's sanitised (ASan) copy of the same sources compiles
# against the same generated header rather than a second, stale value.
set(JAHSHAKA_ENGINE_BUILD_ID_DIR "${JAHSHAKA_ENGINE_BUILD_ID_DIR}" PARENT_SCOPE)
set(JAHSHAKA_OGRE_PATCH_SERIES "${_ogre_patch_series}" PARENT_SCOPE)

add_library(JahshakaEngine STATIC ${JAHSHAKA_ENGINE_SOURCES})
add_dependencies(JahshakaEngine JahshakaEngineBuildId)
jahshaka_engine_ray_query(JahshakaEngine)
jahshaka_engine_vr(JahshakaEngine)
target_compile_definitions(JahshakaEngine PRIVATE
    JAHSHAKA_OGRE_PATCH_SERIES="${_ogre_patch_series}")
target_include_directories(JahshakaEngine
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${JAHSHAKA_ENGINE_BUILD_ID_DIR})
# OgreNext is PRIVATE on purpose: Ogre must not leak into Studio's include path.
target_link_libraries(JahshakaEngine PRIVATE OgreNext)
if(UNIX AND NOT APPLE)
    # Vulkan's XCB windowing path needs a Display* (SDL2x11 param) — backend detail.
    find_package(X11 REQUIRED)
    target_link_libraries(JahshakaEngine PRIVATE ${X11_LIBRARIES})
endif()
set_target_properties(JahshakaEngine PROPERTIES POSITION_INDEPENDENT_CODE ON)
# No Qt in here, ever; the root enables AUTOMOC globally.
set_target_properties(JahshakaEngine PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# --- Runtime data: stage the Hlms shader templates next to the executable ----
# The engine resolves `<exe dir>/media/Hlms/{Common,Pbs,Unlit}` at runtime; the
# build-machine path is only a fallback default. ~600K, copied when stale.
set(_hlms_stamp "${CMAKE_CURRENT_BINARY_DIR}/hlms-media.stamp")
# Also Ogre's common low-level material scripts (sky, DPSM point-light shadow maps,
# depth utilities): Samples/Media/2.0/scripts/materials/Common, ~540K.
get_filename_component(_ogre_media_root "${OGRE_NEXT_HLMS_SOURCE_DIR}" DIRECTORY)
set(OGRE_NEXT_COMMON_MATERIALS_DIR "${_ogre_media_root}/2.0/scripts/materials/Common")
# VCT voxelizer/lighting compute jobs, Ogre's ComputeTools (clearUav — the
# voxelizer clears its UAV textures through it) and the IBL specular integrator
# the PCC probe workspace uses (GI_SPEC.md phases 2-3), ~400K together.
# Post-chain effect media (POST_CHAIN_SPEC.md §4.1): the HDR/tonemap/bloom node's
# materials and shaders, SSAO's, and SMAA's (which also carries two DDS lookup
# textures). ~800K together; registered folder-by-folder like the rest.
set(OGRE_NEXT_HDR_MATERIALS_DIR "${_ogre_media_root}/2.0/scripts/materials/HDR")
set(OGRE_NEXT_SSAO_MATERIALS_DIR "${_ogre_media_root}/2.0/scripts/materials/Tutorial_SSAO")
set(OGRE_NEXT_SMAA_MATERIALS_DIR "${_ogre_media_root}/2.0/scripts/materials/Tutorial_SMAA")
set(OGRE_NEXT_VCT_MEDIA_DIR "${_ogre_media_root}/VCT")
set(OGRE_NEXT_COMPUTE_TOOLS_MEDIA_DIR "${_ogre_media_root}/Compute/Tools")
set(OGRE_NEXT_IBL_MEDIA_DIR "${_ogre_media_root}/Compute/Algorithms/IBL")
# The DDGI (IrradianceField) compute jobs — GI_UNIFIED_SPEC.md §4 P1. Five jobs
# (GenFromVct, depth + colour integration, two border mirrors) plus their .any
# pieces and the generation compositor, ~120K. `Visualizer/` travels with the
# folder (copy_directory) but is deliberately NOT registered as a resource
# location: it is upstream's debug probe-sphere material, which we do not draw.
#
# ORDINARY MEDIA STAGING, and the distinction matters (P0 spike finding 1,
# correcting the spec's own bullet): this is NOT the patch-0009/0017 class. The
# files are upstream's, unpatched, copied from OGRE_NEXT_SOURCE by the normal
# build step — a `cmake --build` picks them up and no tree has to re-run
# build-ogre.sh. A tree that has not rebuilt simply has no IFD media, and DDGI
# then fails LOUD (the compute jobs are missing) instead of rendering wrong.
set(OGRE_NEXT_IFD_MEDIA_DIR "${_ogre_media_root}/Compute/Algorithms/IrradianceFields")
# The overlay font (STATS_OVERLAY_SPEC §9.3 — patch-0009 class: staged from the
# SOURCE tree, so EVERY tree and every worktree re-stages, and a tree that has
# not rebuilt has no font and therefore no stats readout and no loading cover).
#
# Three files out of Ogre's DebugPack.zip, not the zip: it is 285 KB and carries
# Cube_d.mesh, Sphere1000.mesh, cuckoo.ttf, Inconsolata-Regular.ttf and a
# .material we will never use. `Inconsolate-OFL.txt` travels with the font
# because that is how the licence question is answered.
set(OGRE_NEXT_DEBUGPACK_ZIP "${_ogre_media_root}/packs/DebugPack.zip")
file(GLOB_RECURSE _hlms_inputs CONFIGURE_DEPENDS
     "${OGRE_NEXT_HLMS_SOURCE_DIR}/Common/*" "${OGRE_NEXT_HLMS_SOURCE_DIR}/Pbs/*"
     "${OGRE_NEXT_HLMS_SOURCE_DIR}/Unlit/*" "${OGRE_NEXT_COMMON_MATERIALS_DIR}/*"
     "${OGRE_NEXT_HDR_MATERIALS_DIR}/*" "${OGRE_NEXT_SSAO_MATERIALS_DIR}/*"
     "${OGRE_NEXT_SMAA_MATERIALS_DIR}/*"
     "${OGRE_NEXT_VCT_MEDIA_DIR}/*" "${OGRE_NEXT_COMPUTE_TOOLS_MEDIA_DIR}/*"
     "${OGRE_NEXT_IBL_MEDIA_DIR}/*" "${OGRE_NEXT_IFD_MEDIA_DIR}/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/media/Hlms/Jahshaka/*")
list(APPEND _hlms_inputs "${OGRE_NEXT_DEBUGPACK_ZIP}")
add_custom_command(OUTPUT "${_hlms_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${JAHSHAKA_MEDIA_DIR}/Hlms"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_HLMS_SOURCE_DIR}/Common" "${JAHSHAKA_MEDIA_DIR}/Hlms/Common"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_HLMS_SOURCE_DIR}/Pbs"    "${JAHSHAKA_MEDIA_DIR}/Hlms/Pbs"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_HLMS_SOURCE_DIR}/Unlit"  "${JAHSHAKA_MEDIA_DIR}/Hlms/Unlit"
    # Jahshaka's own Hlms pieces (fog colour + height fog, base-map UV tiling) and
    # the PCC/IBL compositors. WIPED first, unlike the Ogre folders: this one is an
    # Hlms LIBRARY path, and a renamed piece file left behind by copy_directory
    # would be parsed alongside its replacement and fail as a duplicate @piece.
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/media/Hlms/Jahshaka" "${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_COMMON_MATERIALS_DIR}" "${JAHSHAKA_MEDIA_DIR}/2.0/scripts/materials/Common"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_HDR_MATERIALS_DIR}" "${JAHSHAKA_MEDIA_DIR}/2.0/scripts/materials/HDR"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_SSAO_MATERIALS_DIR}" "${JAHSHAKA_MEDIA_DIR}/2.0/scripts/materials/Tutorial_SSAO"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_SMAA_MATERIALS_DIR}" "${JAHSHAKA_MEDIA_DIR}/2.0/scripts/materials/Tutorial_SMAA"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_VCT_MEDIA_DIR}" "${JAHSHAKA_MEDIA_DIR}/VCT"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_COMPUTE_TOOLS_MEDIA_DIR}" "${JAHSHAKA_MEDIA_DIR}/Compute/Tools"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_IBL_MEDIA_DIR}" "${JAHSHAKA_MEDIA_DIR}/Compute/Algorithms/IBL"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_IFD_MEDIA_DIR}" "${JAHSHAKA_MEDIA_DIR}/Compute/Algorithms/IrradianceFields"
    # The overlay font. `cmake -E tar` extracts named members, so only the three
    # files we ship land in bin/media/packs/DebugPack — which is exactly the
    # directory OgreEngine::registerCommonMaterials adds as a resource location
    # (hud::addFontLocation). Wiped first, like Hlms/Jahshaka: a renamed font
    # left behind would be parsed alongside its replacement.
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${JAHSHAKA_MEDIA_DIR}/packs"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${JAHSHAKA_MEDIA_DIR}/packs"
    COMMAND ${CMAKE_COMMAND} -E chdir "${JAHSHAKA_MEDIA_DIR}/packs"
            ${CMAKE_COMMAND} -E tar xf "${OGRE_NEXT_DEBUGPACK_ZIP}" --
            DebugPack/DebugFont.fontdef DebugPack/Inconsolata-Bold.ttf DebugPack/Inconsolate-OFL.txt
    COMMAND ${CMAKE_COMMAND} -E touch "${_hlms_stamp}"
    DEPENDS ${_hlms_inputs}
    COMMENT "Staging Ogre-Next Hlms templates into bin/media/Hlms")
add_custom_target(JahshakaEngineMedia DEPENDS "${_hlms_stamp}")

add_dependencies(JahshakaEngine JahshakaEngineMedia)

# Compile-time DEFAULTS for the host. The engine never reads these; the host puts
# them into EngineConfig after trying <exe dir>/media and the env overrides.
target_compile_definitions(JahshakaEngine PUBLIC
    JAHSHAKA_OGRE_PLUGIN_DIR_DEFAULT="${OGRE_NEXT_PLUGIN_DIR}"
    JAHSHAKA_OGRE_MEDIA_DIR_DEFAULT="${JAHSHAKA_MEDIA_DIR}/")
