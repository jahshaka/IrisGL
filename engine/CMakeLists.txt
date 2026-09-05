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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreGi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgrePlanar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreParticles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreDecals.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreFog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreSkeleton.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreShaderCache.cpp
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

set(_engine_blob "")
foreach(_s IN LISTS JAHSHAKA_ENGINE_SOURCES)
    file(SHA256 "${_s}" _h)
    string(APPEND _engine_blob "${_h};")
endforeach()
foreach(_s include/jahshaka/engine/Engine.h include/jahshaka/engine/Types.h)
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${_s}" _h)
    string(APPEND _engine_blob "${_h};")
endforeach()
string(SHA256 _engine_build_id "${_engine_blob}")
# Exported so tests/engine's sanitised copy of the same sources compiles with
# the same fingerprint terms (it builds JAHSHAKA_ENGINE_SOURCES directly).
set(JAHSHAKA_ENGINE_BUILD_ID "${_engine_build_id}" PARENT_SCOPE)
set(JAHSHAKA_OGRE_PATCH_SERIES "${_ogre_patch_series}" PARENT_SCOPE)

add_library(JahshakaEngine STATIC ${JAHSHAKA_ENGINE_SOURCES})
target_compile_definitions(JahshakaEngine PRIVATE
    JAHSHAKA_ENGINE_BUILD_ID="${_engine_build_id}"
    JAHSHAKA_OGRE_PATCH_SERIES="${_ogre_patch_series}")
target_include_directories(JahshakaEngine
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
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
     "${OGRE_NEXT_IBL_MEDIA_DIR}/*"
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
