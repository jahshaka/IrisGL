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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreClips.cpp)
set(JAHSHAKA_ENGINE_SOURCES "${JAHSHAKA_ENGINE_SOURCES}" PARENT_SCOPE)

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

# --- The DDGI piece copy, guarded rather than trusted ------------------------
# media/Hlms/Jahshaka/JahIfd_piece_ps.any carries a VERBATIM copy of upstream's
# `applyIrradianceField` body with one line changed (our diffuse-intensity
# multiply — GI_UNIFIED_SPEC.md §4 P1 / addendum 2; there is no hook to do it
# from, since the field's contribution is accumulated inside that piece's last
# statement and anything later would scale the probe and cubemap terms too).
#
# A copy of upstream code is a liability exactly once: at a pin bump, silently.
# So the configure hashes the original. When upstream's body changes, this FAILS
# LOUD with the two-minute fix attached, instead of leaving DDGI rendering last
# year's math. The escape hatch is for the person doing the re-sync, and it is
# deliberately a cache variable so nobody sets it by accident.
set(JAH_IFD_UPSTREAM_PIECE
    "${OGRE_NEXT_HLMS_SOURCE_DIR}/Pbs/Any/IrradianceField_piece_ps.any")
set(JAH_IFD_UPSTREAM_SHA256 "e6e1ddd1c833af2b3d2c3a811d3367334954bc705bfb7fe28045aa4308d993a1"
    CACHE STRING "sha256 of the upstream IrradianceField piece our copy was taken from")
option(JAH_IFD_PIECE_HASH_OK
       "Acknowledge that media/Hlms/Jahshaka/JahIfd_piece_ps.any has been re-synced against a \
changed upstream IrradianceField piece" OFF)
if(EXISTS "${JAH_IFD_UPSTREAM_PIECE}")
    file(SHA256 "${JAH_IFD_UPSTREAM_PIECE}" _jah_ifd_sha)
    if(NOT _jah_ifd_sha STREQUAL JAH_IFD_UPSTREAM_SHA256 AND NOT JAH_IFD_PIECE_HASH_OK)
        message(FATAL_ERROR
            "Ogre-Next's IrradianceField_piece_ps.any has CHANGED upstream.\n"
            "  expected ${JAH_IFD_UPSTREAM_SHA256}\n"
            "  found    ${_jah_ifd_sha}\n"
            "irisgl/engine/media/Hlms/Jahshaka/JahIfd_piece_ps.any holds a copy of that piece's "
            "body with one line changed (the DDGI diffuse intensity). Re-sync the body, re-apply "
            "the multiply on the final envColourD line, set JAH_IFD_UPSTREAM_SHA256 to the new "
            "hash — or configure once with -DJAH_IFD_PIECE_HASH_OK=ON if you have already done so.")
    endif()
endif()
add_dependencies(JahshakaEngine JahshakaEngineMedia)

# Compile-time DEFAULTS for the host. The engine never reads these; the host puts
# them into EngineConfig after trying <exe dir>/media and the env overrides.
target_compile_definitions(JahshakaEngine PUBLIC
    JAHSHAKA_OGRE_PLUGIN_DIR_DEFAULT="${OGRE_NEXT_PLUGIN_DIR}"
    JAHSHAKA_OGRE_MEDIA_DIR_DEFAULT="${JAHSHAKA_MEDIA_DIR}/")
