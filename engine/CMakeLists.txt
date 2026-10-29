# The engine abstraction and its Ogre-Next backend.
# Studio links this; Studio never links Ogre directly.
# The backend is split across Ogre-private translation units; EnginePrivate.h
# holds the shared declarations and is never installed. The list is exported so
# the sanitised copy in tests/engine builds exactly the same sources.
# THE SPIR-V RULES READ GLSLANG'S DEPFILES (below) as CMake transforms them for Ninja (3.20's CMP0116; this directory is
# added from Studio's root, whose 3.16 minimum leaves it unset).
if(POLICY CMP0116)
    cmake_policy(SET CMP0116 NEW)
endif()
set(JAHSHAKA_ENGINE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EnginePrivate.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderThread.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SyncGetters.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderThread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreChain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreLights.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreMesh.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreMaterials.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreSky.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreAtmosphere.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreGpuCheckpoints.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Stillness.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ReadbackDecode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreCompute.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreVideoReadback.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreVoxelReaderParity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreEnvironmentCones.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GpuScene.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GpuSceneSubstrate.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/PhotonComponent.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/PhotonDispatch.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/PhotonComponent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/PhotonGi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/PhotonInjectBoxes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/atom/AtomComponent.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/atom/AtomComponent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SkinCache.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GpuVoxelGather.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreGpuCull.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreGpuScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreAtomDraw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgrePhotonView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreAtomIdPass.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreAtomDensity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreAtomCasterPass.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AlphaMasks.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreAlphaMasks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HlmsAtom.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HlmsAtom.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AtomPass.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreAtomPass.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreGpuVoxelGather.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreRayQuery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreScreenProbeGather.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreRayQueryHitSet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreSurfaceCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GpuUploadDelta.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GpuUploadDelta.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreScreenIntegrate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreAreaVis.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneLightList.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneLightList.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/OgreVrSession.cpp
    # PHOTON'S VOXEL GI, OURS (OWN-GI-1): the voxel cascades, their voxeliser and
    # material store, the irradiance field and the two debug drawables — an owned
    # copy of what was Ogre-Next's Vct/IrradianceField component, the way an
    # application owns Terra — and the listener half that binds them to every
    # PBS-family pass (HlmsPbs never holds them).
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonPassBinding.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonPassBinding.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelizerSourceBase.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelizerSourceBase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelizer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelizer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonMipChain.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonMipChain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelScroll.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelScroll.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelMaterial.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelMaterial.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonSpecularAlbedoBuffer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonSpecularAlbedoBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelLighting.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelLighting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelVisualizer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonVoxelVisualizer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonIrradianceField.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonIrradianceField.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonIfdProbeVisualizer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/photon/voxel/PhotonIfdProbeVisualizer.cpp)
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
    # THE FORK'S DIFFUSE LOBE FOR GLSLANG (PHOTON-CARDS-5): the card read restores
    # the lobe's view term with JahBrdf's and JahDiffuseAlbedo's own functions,
    # unwrapped from the fork's Pbs media at build time (never a copy).
    set(JAH_RQ_FORK_BRDF "${JAH_RQ_GEN_DIR}/jah_fork_brdf.glsl")
    add_custom_command(
        OUTPUT "${JAH_RQ_FORK_BRDF}"
        COMMAND ${CMAKE_COMMAND} -DUNWRAP=1 -DOUT=${JAH_RQ_FORK_BRDF}
                -DIN0=${OGRE_NEXT_HLMS_SOURCE_DIR}/Pbs/Any/JahBrdf_piece_all.any
                -DIN1=${OGRE_NEXT_HLMS_SOURCE_DIR}/Pbs/Any/JahDiffuseAlbedo_piece_all.any
                -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/WrapHlmsPiece.cmake"
        DEPENDS "${OGRE_NEXT_HLMS_SOURCE_DIR}/Pbs/Any/JahBrdf_piece_all.any"
                "${OGRE_NEXT_HLMS_SOURCE_DIR}/Pbs/Any/JahDiffuseAlbedo_piece_all.any"
                "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/WrapHlmsPiece.cmake"
        COMMENT "Unwrapping the fork's diffuse lobe for the ray jobs' card read"
        VERBATIM)
    # THE LOBE INTEGRALS FOR GLSLANG (METAL-BOUNCE-1; the fork's Pbs piece since PHOTON-3): unwrapped the
    # same way, so a ray's hit, the voxel store and the pixel's environment read one table through one text.
    set(JAH_RQ_SPEC_ALBEDO "${JAH_RQ_GEN_DIR}/jah_spec_albedo.glsl")
    add_custom_command(
        OUTPUT "${JAH_RQ_SPEC_ALBEDO}"
        COMMAND ${CMAKE_COMMAND} -DUNWRAP=1 -DOUT=${JAH_RQ_SPEC_ALBEDO}
                -DIN0=${OGRE_NEXT_HLMS_SOURCE_DIR}/Pbs/Any/JahSpecularAlbedo_piece_all.any
                -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/WrapHlmsPiece.cmake"
        DEPENDS "${OGRE_NEXT_HLMS_SOURCE_DIR}/Pbs/Any/JahSpecularAlbedo_piece_all.any"
                "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/WrapHlmsPiece.cmake"
        COMMENT "Unwrapping the metal's lobe integrals for the ray jobs' hit"
        VERBATIM)
    # BUILD-TIME SPIR-V, at spirv1.4 — never committed bytes (the spike
    # committed SPIR-V 1.5 headers, which are legal only on a device at
    # apiVersion >= 1.2 and which no reviewer can read: audit C-7/C-10).
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_rays_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_rays_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include"
                --vn krq_raysSpv -o "${JAH_RQ_GEN_DIR}/rq_rays_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_rays.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_rays.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_rays_spv.h.d"
        COMMENT "Compiling the ray-query compute shader at SPIR-V 1.4"
        VERBATIM)
    # R5's reflection trace, same rule (PHOTON_SPEC §7 R5).
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_reflect_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_reflect_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include" "-I${JAH_RQ_GEN_DIR}"
                --vn krq_reflectSpv -o "${JAH_RQ_GEN_DIR}/rq_reflect_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect.comp"
                "${JAH_RQ_FORK_BRDF}"
                "${JAH_RQ_SPEC_ALBEDO}"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_reflect_spv.h.d"
        COMMENT "Compiling the ray-traced reflection compute shader at SPIR-V 1.4"
        VERBATIM)
    # gi.card_read_parity's harness: the reflection's card read (jah_rq_card.glsl),
    # asked at a list of points - a test-only job, same rule.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_card_parity_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_card_parity_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include" "-I${JAH_RQ_GEN_DIR}"
                --vn krq_cardParitySpv -o "${JAH_RQ_GEN_DIR}/rq_card_parity_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_card_parity.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_card_parity.comp"
                "${JAH_RQ_FORK_BRDF}"
                "${JAH_RQ_SPEC_ALBEDO}"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_card_parity_spv.h.d"
        COMMENT "Compiling the card-read parity compute shader at SPIR-V 1.4"
        VERBATIM)
    # REFLECT-MOVERS-1's object motion for the screen march's resolve, same rule.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_motion_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_motion_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include"
                --vn krq_motionSpv -o "${JAH_RQ_GEN_DIR}/rq_motion_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_motion.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_motion.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_motion_spv.h.d"
        COMMENT "Compiling the screen march's object-motion compute shader at SPIR-V 1.4"
        VERBATIM)
    # ...and its POSED twin (SKINNED-VELOCITY-1, REFLECT-EDGE-2): the pixels the id
    # pass does not draw, identified through the TLAS while a pose moves; same rule.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_motion_skin_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_motion_skin_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include"
                --vn krq_motionSkinSpv -o "${JAH_RQ_GEN_DIR}/rq_motion_skin_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_motion_skin.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_motion_skin.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_motion_skin_spv.h.d"
        COMMENT "Compiling the march's posed object-motion compute shader at SPIR-V 1.4"
        VERBATIM)
    # ...and R5's spatial filter + composite (round C), same rule.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_reflect_filter_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_reflect_filter_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include"
                --vn krq_reflectFilterSpv -o "${JAH_RQ_GEN_DIR}/rq_reflect_filter_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect_filter.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect_filter.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_reflect_filter_spv.h.d"
        COMMENT "Compiling the reflection's spatial filter compute shader at SPIR-V 1.4"
        VERBATIM)
    # ...and the reflection's TEMPORAL pass (PHOTON-2 1b), with the shared reprojection header.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_reflect_temporal_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_reflect_temporal_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include"
                --vn krq_reflectTemporalSpv -o "${JAH_RQ_GEN_DIR}/rq_reflect_temporal_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect_temporal.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect_temporal.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_reflect_temporal_spv.h.d"
        COMMENT "Compiling the reflection temporal compute shader at SPIR-V 1.4"
        VERBATIM)
    # ...and the reflection's RESOLVE (PHOTON-7 stage 6 commit 6): each texel reconstructed from its neighbours' samples.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_reflect_resolve_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_reflect_resolve_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include" "-I${JAH_RQ_GEN_DIR}"
                --vn krq_reflectResolveSpv -o "${JAH_RQ_GEN_DIR}/rq_reflect_resolve_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect_resolve.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect_resolve.comp" "${JAH_RQ_SPEC_ALBEDO}"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_reflect_resolve_spv.h.d"
        COMMENT "Compiling the reflection resolve compute shader at SPIR-V 1.4"
        VERBATIM)
    # ...and THE BREAK'S COMPOSE (PHOTON-2 round 2, G2): the rest's answer composed into the half the trace reads.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_reflect_compose_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_reflect_compose_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include"
                --vn krq_reflectComposeSpv -o "${JAH_RQ_GEN_DIR}/rq_reflect_compose_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect_compose.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_reflect_compose.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_reflect_compose_spv.h.d"
        COMMENT "Compiling the reflection rest's compose compute shader at SPIR-V 1.4"
        VERBATIM)
    # THE SCREEN-PROBE GATHER'S FOUR JOBS (GATHER-1a; the filter PHOTON-GATHER-1b), same rule plus ONE
    # addition: `-I src/rayquery/include`, so the hit-radiance functions and the
    # parameter block are SHARED with the reflection trace rather than copied
    # (spec section 10 item 7 — glslang resolves `#include` under
    # GL_GOOGLE_include_directive, which each shader requires). EVERY RULE'S INCLUDES COME FROM GLSLANG'S OWN
    # DEPFILE (--depfile, DEPFILE: PHOTON-7 stage 5, the urgent list's #29 - a hand-listed include that was missing
    # rebuilt nothing on an edit and voided a negative control); DEPENDS keeps the source and the GENERATED inputs,
    # which a first build must order.
    set(JAH_RQ_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include")
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_probe_place_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_probe_place_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}"
                --vn krq_probePlaceSpv -o "${JAH_RQ_GEN_DIR}/rq_probe_place_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_place.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_place.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_probe_place_spv.h.d"
        COMMENT "Compiling the screen-probe placement job at SPIR-V 1.4"
        VERBATIM)
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_probe_place_select_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_probe_place_select_spv.h.d" --target-env spirv1.4 -S comp -x -DJAH_PLACE_SELECT
                "-I${JAH_RQ_INCLUDE}"
                --vn krq_probePlaceSelectSpv -o "${JAH_RQ_GEN_DIR}/rq_probe_place_select_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_place.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_place.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_probe_place_select_spv.h.d"
        COMMENT "Compiling the screen-probe placement's select pass at SPIR-V 1.4"
        VERBATIM)
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_probe_gather_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_probe_gather_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}" "-I${JAH_RQ_GEN_DIR}"
                --vn krq_probeGatherSpv -o "${JAH_RQ_GEN_DIR}/rq_probe_gather_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_gather.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_gather.comp"
                "${JAH_RQ_FORK_BRDF}"
                "${JAH_RQ_SPEC_ALBEDO}"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_probe_gather_spv.h.d"
        COMMENT "Compiling the screen-probe gather trace at SPIR-V 1.4"
        VERBATIM)
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_probe_filter_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_probe_filter_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}"
                --vn krq_probeFilterSpv -o "${JAH_RQ_GEN_DIR}/rq_probe_filter_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_filter.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_filter.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_probe_filter_spv.h.d"
        COMMENT "Compiling the screen-probe filter + SH projection at SPIR-V 1.4"
        VERBATIM)
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_probe_integrate_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_probe_integrate_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}" "-I${JAH_RQ_GEN_DIR}"
                --vn krq_probeIntegrateSpv -o "${JAH_RQ_GEN_DIR}/rq_probe_integrate_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_integrate.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_probe_integrate.comp"
                "${JAH_RQ_FORK_BRDF}"
                "${JAH_RQ_SPEC_ALBEDO}"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_probe_integrate_spv.h.d"
        COMMENT "Compiling the screen-probe integrate at SPIR-V 1.4"
        VERBATIM)
    # HARD SUN CONTACT SHADOWS (PHOTON-RAYS-1, RY-R3): one ray per pixel towards
    # the sun from the prepass' surface, geometry only — rq_rays.comp's shape.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_sun_contact_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_sun_contact_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}"
                --vn krq_sunContactSpv -o "${JAH_RQ_GEN_DIR}/rq_sun_contact_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_sun_contact.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_sun_contact.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_sun_contact_spv.h.d"
        COMMENT "Compiling the sun contact shadow job at SPIR-V 1.4"
        VERBATIM)
    # THE PIXEL'S AREA LIGHT VISIBILITY (PHOTON-7 stage 7, 7-A): the trace (one stratum of each forward LTC light a
    # texel a frame, the two ratio sums' temporal means) and the resolve (the joint-bilateral upsample and the ratio).
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_area_vis_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_area_vis_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}"
                --vn krq_areaVisSpv -o "${JAH_RQ_GEN_DIR}/rq_area_vis_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_area_vis.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_area_vis.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_area_vis_spv.h.d"
        COMMENT "Compiling the area light visibility trace at SPIR-V 1.4"
        VERBATIM)
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_area_vis_resolve_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_area_vis_resolve_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}"
                --vn krq_areaVisResolveSpv -o "${JAH_RQ_GEN_DIR}/rq_area_vis_resolve_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_area_vis_resolve.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_area_vis_resolve.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_area_vis_resolve_spv.h.d"
        COMMENT "Compiling the area light visibility resolve at SPIR-V 1.4"
        VERBATIM)
    # THE HIT WRITE-BACK (PHOTON-HIT-SHADE-1): the decoded radiance of every hit
    # record, scattered back into the reflection's mean and the gather's atlas.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_hit_composite_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_hit_composite_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include"
                --vn krq_hitCompositeSpv -o "${JAH_RQ_GEN_DIR}/rq_hit_composite_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_hit_composite.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_hit_composite.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_hit_composite_spv.h.d"
        COMMENT "Compiling the hit write-back at SPIR-V 1.4"
        VERBATIM)
    # THE MOVERS' SHADOW ON THE CARDS (PHOTON-CARDS-4): sun rays from the texels
    # of the cards a shadow-casting mover's footprint reaches, against the movers.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_card_movers_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_card_movers_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}" "-I${JAH_RQ_GEN_DIR}"
                --vn krq_cardMoversSpv -o "${JAH_RQ_GEN_DIR}/rq_card_movers_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_card_movers.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_card_movers.comp"
                "${JAH_RQ_FORK_BRDF}"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_card_movers_spv.h.d"
        COMMENT "Compiling the card mover-shadow job at SPIR-V 1.4"
        VERBATIM)
    # THE VOXEL SIDES' GATHER BY TLAS RAYS (PHOTON-7 stage 10, 10-2b): N fixed rays a side of the bricks a feedback pass
    # listed, by its indirect arguments, into the chain's side-gather scratch the bounce reads.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_voxel_sides_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_voxel_sides_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}" "-I${JAH_RQ_GEN_DIR}"
                --vn krq_voxelSidesSpv -o "${JAH_RQ_GEN_DIR}/rq_voxel_sides_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_voxel_sides.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_voxel_sides.comp"
                "${JAH_RQ_FORK_BRDF}"
                "${JAH_RQ_SPEC_ALBEDO}"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_voxel_sides_spv.h.d"
        COMMENT "Compiling the voxel sides' TLAS gather at SPIR-V 1.4"
        VERBATIM)
    # THE CARDS' INDIRECT BY TLAS RAYS (PHOTON-7 stage 10, 10-2a): N fixed rays a texel of the cards the relight
    # re-gathers, their hits read through the hit set, into the cache's Gather layer.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_card_gather_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_card_gather_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}" "-I${JAH_RQ_GEN_DIR}"
                --vn krq_cardGatherSpv -o "${JAH_RQ_GEN_DIR}/rq_card_gather_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_card_gather.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_card_gather.comp"
                "${JAH_RQ_FORK_BRDF}"
                "${JAH_RQ_SPEC_ALBEDO}"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_card_gather_spv.h.d"
        COMMENT "Compiling the cards' TLAS gather at SPIR-V 1.4"
        VERBATIM)
    # THE BLENDED PASSES' ESCAPES FOLLOW THE RAY (PHOTON-7 10-4 R4, design (D)): a record a blended fragment's open
    # specular share appended, traced from where the voxel chain's read ended, its hit read through the hit set; then
    # every pixel's fixed-point sum added to the pass's colour target.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_glass_escape_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_glass_escape_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}" "-I${JAH_RQ_GEN_DIR}"
                --vn krq_glassEscapeSpv -o "${JAH_RQ_GEN_DIR}/rq_glass_escape_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_glass_escape.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_glass_escape.comp"
                "${JAH_RQ_FORK_BRDF}"
                "${JAH_RQ_SPEC_ALBEDO}"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_glass_escape_spv.h.d"
        COMMENT "Compiling the blended passes' escapes at SPIR-V 1.4"
        VERBATIM)
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_glass_escape_add_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_glass_escape_add_spv.h.d" --target-env spirv1.4 -S comp -x
                "-I${JAH_RQ_INCLUDE}"
                --vn krq_glassEscapeAddSpv -o "${JAH_RQ_GEN_DIR}/rq_glass_escape_add_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_glass_escape_add.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_glass_escape_add.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_glass_escape_add_spv.h.d"
        COMMENT "Compiling the blended passes' escape add at SPIR-V 1.4"
        VERBATIM)
    # THE TOP-LEVEL INSTANCES BY COMPUTE (ATOM-CPU-WALKS-1): one thread per GPU scene
    # slot writes its near and far instance from the table.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/rq_tlas_write_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/rq_tlas_write_spv.h.d" --target-env spirv1.4 -S comp -x
                --vn krq_tlasWriteSpv -o "${JAH_RQ_GEN_DIR}/rq_tlas_write_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_tlas_write.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/rq_tlas_write.comp"
        DEPFILE "${JAH_RQ_GEN_DIR}/rq_tlas_write_spv.h.d"
        COMMENT "Compiling the top-level instance job at SPIR-V 1.4"
        VERBATIM)
    # THE VISIBILITY BUFFER'S ID PASS (ATOM S3-DRAW, OgreAtomIdPass.cpp): a
    # material-less vertex/fragment pair that pulls every vertex through the GPU
    # scene's geometry rows — jah_geom_rows.glsl, the ONE copy the voxeliser's
    # piece and the ray jobs read too. SPIR-V 1.0 (buffer references ride
    # SPV_KHR_physical_storage_buffer; nothing here needs 1.4).
    foreach(_stage vert frag)
        add_custom_command(
            OUTPUT "${JAH_RQ_GEN_DIR}/atom_id_${_stage}_spv.h"
            COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/atom_id_${_stage}_spv.h.d" -S ${_stage} -x
                    "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include"
                    --vn katomId_${_stage}Spv -o "${JAH_RQ_GEN_DIR}/atom_id_${_stage}_spv.h"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/atomdraw/atom_id.${_stage}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/atomdraw/atom_id.${_stage}"
            DEPFILE "${JAH_RQ_GEN_DIR}/atom_id_${_stage}_spv.h.d"
            COMMENT "Compiling the visibility buffer's id pass (${_stage})"
            VERBATIM)
    endforeach()
    # THE CASTER CUT (ATOM-SHADOWS-1, OgreAtomCasterPass.cpp): the same vertex pull, the
    # stock caster's bias; a fragment stage for a point light's cube face only.
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/atom_caster_vert_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/atom_caster_vert_spv.h.d" -S vert -x
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include"
                --vn katomCaster_vertSpv -o "${JAH_RQ_GEN_DIR}/atom_caster_vert_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/atomdraw/atom_caster.vert"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/atomdraw/atom_caster.vert"
        DEPFILE "${JAH_RQ_GEN_DIR}/atom_caster_vert_spv.h.d"
        COMMENT "Compiling the caster cut (vert)"
        VERBATIM)
    add_custom_command(
        OUTPUT "${JAH_RQ_GEN_DIR}/atom_caster_point_frag_spv.h"
        COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/atom_caster_point_frag_spv.h.d" -S frag -x
                --vn katomCasterPoint_fragSpv -o "${JAH_RQ_GEN_DIR}/atom_caster_point_frag_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/atomdraw/atom_caster_point.frag"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/atomdraw/atom_caster_point.frag"
        DEPFILE "${JAH_RQ_GEN_DIR}/atom_caster_point_frag_spv.h.d"
        COMMENT "Compiling the caster cut (point frag)"
        VERBATIM)
    # THE MASKED RANGES (ENGINE-POLISH-ATOM-1 stage 3): the same stages compiled with JAH_MASKED - the vertex stage
    # carries the texture coordinate, the fragment stage discards where the cut-out's mask is clear
    # (jah_alpha_mask.glsl) - and the directional/spot maps' masked fragment stage (atom_caster_masked.frag).
    foreach(_pair "atom_id.vert:vert:katomIdMasked_vertSpv:atom_id_masked_vert_spv.h"
                  "atom_id.frag:frag:katomIdMasked_fragSpv:atom_id_masked_frag_spv.h"
                  "atom_caster.vert:vert:katomCasterMasked_vertSpv:atom_caster_masked_vert_spv.h"
                  "atom_caster_point.frag:frag:katomCasterPointMasked_fragSpv:atom_caster_point_masked_frag_spv.h"
                  "atom_caster_masked.frag:frag:katomCasterMasked_fragSpv:atom_caster_masked_frag_spv.h")
        string(REPLACE ":" ";" _p "${_pair}")
        list(GET _p 0 _src)
        list(GET _p 1 _stage)
        list(GET _p 2 _vn)
        list(GET _p 3 _out)
        add_custom_command(
            OUTPUT "${JAH_RQ_GEN_DIR}/${_out}"
            COMMAND "${JAH_GLSLANG_VALIDATOR}" -V --depfile "${JAH_RQ_GEN_DIR}/${_out}.d" -S ${_stage} -x -DJAH_MASKED=1
                    "-I${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include"
                    --vn ${_vn} -o "${JAH_RQ_GEN_DIR}/${_out}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/atomdraw/${_src}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/atomdraw/${_src}"
            DEPFILE "${JAH_RQ_GEN_DIR}/${_out}.d"
            COMMENT "Compiling the Atom passes' masked ranges (${_src})"
            VERBATIM)
    endforeach()
    add_custom_target(JahshakaRayQuerySpv
                      DEPENDS "${JAH_RQ_GEN_DIR}/atom_id_vert_spv.h"
                              "${JAH_RQ_GEN_DIR}/atom_id_frag_spv.h"
                              "${JAH_RQ_GEN_DIR}/atom_caster_vert_spv.h"
                              "${JAH_RQ_GEN_DIR}/atom_caster_point_frag_spv.h"
                              "${JAH_RQ_GEN_DIR}/atom_id_masked_vert_spv.h"
                              "${JAH_RQ_GEN_DIR}/atom_id_masked_frag_spv.h"
                              "${JAH_RQ_GEN_DIR}/atom_caster_masked_vert_spv.h"
                              "${JAH_RQ_GEN_DIR}/atom_caster_point_masked_frag_spv.h"
                              "${JAH_RQ_GEN_DIR}/atom_caster_masked_frag_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_rays_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_sun_contact_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_area_vis_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_area_vis_resolve_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_hit_composite_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_card_movers_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_card_gather_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_glass_escape_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_glass_escape_add_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_voxel_sides_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_tlas_write_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_reflect_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_reflect_filter_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_reflect_temporal_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_reflect_resolve_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_reflect_compose_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_motion_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_motion_skin_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_card_parity_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_probe_place_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_probe_place_select_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_probe_gather_spv.h"
                              "${JAH_RQ_GEN_DIR}/rq_probe_filter_spv.h"
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
#  * JAHSHAKA_OGRE_FORK_COMMIT — the ogre-next submodule's HEAD sha. Since the
#    fork (2026-09-22) that ONE value is the whole identity of the engine source:
#    our changes are commits on github.com/jahshaka/ogre-next `jahshaka`, media and
#    C++ alike, so a different engine is a different sha and nothing else can vary.
#    It replaces the old JAHSHAKA_OGRE_PATCH_SERIES (a fold of the 88 patch files'
#    hashes plus the pin), which had exactly this job while the source was
#    "upstream's commit + a directory of patches". Ogre's own hashes cannot see
#    either: a media-only change produces identical Ogre-side hashes, so without
#    this term a stale cache looks perfectly valid and loads.
#  * JAHSHAKA_ENGINE_BUILD_ID — a hash of the engine's OWN sources. This TU set
#    decides which Hlms properties are set, which datablocks exist and which
#    listener extends the pass buffer. Editing OgreEngine.cpp changes what
#    shaders mean without changing one byte of any template.
#
# Configure-time, not build-time: an input to a cache whose only failure mode is
# "used a stale entry"; the submodule's HEAD files are configure dependencies (below),
# so CMake re-configures whenever the checkout moves.
#
# HEAD, not the recorded gitlink: the sha that must fence the cache is the one the
# engine was BUILT from. A lane that commits on the fork and builds before bumping
# the pin (the normal order) would otherwise get a cache fingerprint that still
# claims the old engine. `git -C thirdparty/ogre-next rev-parse HEAD` says what is
# actually there; a checkout so broken that it answers nothing falls back to the
# gitlink, and an unconfigurable "unknown" is better than a silently shared key.
execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/ogre-next" rev-parse HEAD
                OUTPUT_VARIABLE _ogre_fork_commit ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _ogre_fork_commit)
    execute_process(COMMAND git ls-tree HEAD thirdparty/ogre-next
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
                    OUTPUT_VARIABLE _ogre_fork_commit ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if(NOT _ogre_fork_commit)
    set(_ogre_fork_commit "unknown")
endif()
# ...AND IT IS RE-READ WHEN THE CHECKOUT MOVES (D6-FORK-TOOLING, audit V2-D F12; the
# bake key's pattern in irisgl/CMakeLists.txt). A configure-time value goes stale:
# a pin bump or a fork commit moves the submodule without touching a CMake input, so
# the term kept naming the previous engine until something unrelated re-configured —
# a new engine's shaders filed under the old engine's cache key. The inputs are the
# submodule's HEAD as git stores it: the HEAD file (a detached checkout rewrites it),
# the branch ref it names (a commit on a branch moves only that) and logs/HEAD
# (every HEAD move appends to it, packed refs included).
set(_ogre_git "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/ogre-next")
set(_ogre_head_inputs "")
execute_process(COMMAND git -C "${_ogre_git}" symbolic-ref -q HEAD
                OUTPUT_VARIABLE _ogre_head_ref ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
foreach(_p HEAD logs/HEAD ${_ogre_head_ref})
    execute_process(COMMAND git -C "${_ogre_git}" rev-parse --path-format=absolute --git-path ${_p}
                    OUTPUT_VARIABLE _f ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_f AND EXISTS "${_f}")
        list(APPEND _ogre_head_inputs "${_f}")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ogre_head_inputs})

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
    ${CMAKE_CURRENT_SOURCE_DIR}/include/jahshaka/engine/GpuSubstrate.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/jahshaka/engine/PhotonStatus.h
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
set(JAHSHAKA_OGRE_FORK_COMMIT "${_ogre_fork_commit}" PARENT_SCOPE)

# A GENERATED HEADER IS AN INPUT OF THE OBJECTS THAT INCLUDE IT (TOOLING-2 stage 3). The ray jobs' SPIR-V
# (rq_*_spv.h, atom_*_spv.h) and the engine build id are written by custom commands; add_dependencies() only ORDERS
# them before the engine's compiles, and the objects' own dependency on them was ninja's deps log alone — spelled
# through ccache's base_dir (~/Developer) relative to the REAL working directory (/mnt/work/Developer: ~/Developer is
# a symlink), `../../../../../../../../home/…/rq_reflect_spv.h`, a node no custom command produces. So an edit of a
# jah_rq_*.glsl include or a .comp regenerated the header and compiled OgreRayQuery.cpp / OgreScreenProbeGather.cpp
# against the OLD one in the same run — the binary linked the previous SPIR-V (PHOTON-1 lost two measurements,
# §2557; FAR-SUN-2's `.comp` edit, BUILD-GRAPH-COMP-LINK) — and every engine edit (a new build id) recompiled
# OgreTextureCache / OgreShaderCache and relinked in a SECOND build (BUILD-RELINK-TWICE-1). The includes are read from
# each source at configure, and the headers become OBJECT_DEPENDS of exactly those objects: one ninja run orders and
# rebuilds them, whatever the deps log spells. An existing source that NEWLY includes one gets the explicit dependency
# at the next configure only (an edit of a .cpp does not re-configure); until then the deps log carries it — spelled as
# the rule's node only when the build runs from its build dir with PWD naming it (the root cause: ccache's base_dir
# rewrites the depfile against $PWD; spec.sh's builds do, see scripts/lead/lib/util.py build()).
function(jahshaka_engine_generated_deps target)
    get_target_property(_srcs ${target} SOURCES)
    get_target_property(_sdir ${target} SOURCE_DIR)
    foreach(_s IN LISTS _srcs)
        if(NOT _s MATCHES "\\.(cpp|cc|cxx)$")
            continue()
        endif()
        if(NOT IS_ABSOLUTE "${_s}")
            set(_s "${_sdir}/${_s}")
        endif()
        file(STRINGS "${_s}" _incs
             REGEX "^[ \t]*#[ \t]*include[ \t]*\"(rayquery/[A-Za-z0-9_]+_spv\\.h|jahshaka_engine_build_id\\.h)\"")
        set(_deps "")
        foreach(_l IN LISTS _incs)
            string(REGEX REPLACE "^[^\"]*\"([^\"]+)\".*$" "\\1" _h "${_l}")
            if(_h MATCHES "^rayquery/")
                if(JAH_RAY_QUERY_OK)
                    list(APPEND _deps "${JAH_RQ_GEN_DIR}/../${_h}")
                endif()
            else()
                list(APPEND _deps "${JAHSHAKA_ENGINE_BUILD_ID_DIR}/${_h}")
            endif()
        endforeach()
        if(_deps)
            set_property(SOURCE "${_s}" TARGET_DIRECTORY ${target} APPEND PROPERTY OBJECT_DEPENDS ${_deps})
        endif()
    endforeach()
endfunction()

# THE STILLNESS DOOR AT -O2 IN DEBUG (PHOTON-2 stage 0; the bake's TUs' precedent, irisgl/CMakeLists.txt): it diffs
# and classifies every pixel of every frame of its window — integer work whose answer no optimisation level changes —
# and at -O0 that alone is ~0.2 s a 1080p frame, the lab's whole budget.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/Stillness.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
# ...AND THE INSTRUMENTS' READBACK DECODES (PHOTON-2 round 2): at -O0 a 1080p history decode was a 15 ms hitch inside
# the frame after a stop (ReadbackDecode.h).
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/ReadbackDecode.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
add_library(JahshakaEngine STATIC ${JAHSHAKA_ENGINE_SOURCES})
add_dependencies(JahshakaEngine JahshakaEngineBuildId)
jahshaka_engine_generated_deps(JahshakaEngine)
jahshaka_engine_ray_query(JahshakaEngine)
jahshaka_engine_vr(JahshakaEngine)
target_compile_definitions(JahshakaEngine PRIVATE
    JAHSHAKA_OGRE_FORK_COMMIT="${_ogre_fork_commit}")
target_include_directories(JahshakaEngine
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${JAHSHAKA_ENGINE_BUILD_ID_DIR})
# THE ONE CORE BUDGET (THREADING-1): the engine sizes its pools (the texture multiload, the shader cache's boot apply)
# from IrisGL's header-only, Ogre- and Qt-free core/threadbudget.h — by include alone, PRIVATE; the engine still links no
# IrisGL and includes nothing else of it.
target_include_directories(JahshakaEngine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
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
# Ogre's ComputeTools (clearUav — the voxeliser clears its UAV textures through it)
# and the IBL specular integrator the PCC probe workspace uses (GI_SPEC.md phases
# 2-3).
# Post-chain effect media (POST_CHAIN_SPEC.md §4.1): the HDR node's tonemap and
# MSAA resolve (the fork's HDR folder), SSAO's, and SMAA's (which also carries two
# DDS lookup textures). ~800K together; registered folder-by-folder like the rest.
# The BLOOM is not the HDR folder's any more (BLOOM-1): its ladder is ours, in
# media/Hlms/Jahshaka/JahBloom*. The folder's bright pass and box blurs are still
# declared by its HDR.material (one file with the tonemap), so they stay staged
# and unused until the fork drops them.
set(OGRE_NEXT_HDR_MATERIALS_DIR "${_ogre_media_root}/2.0/scripts/materials/HDR")
set(OGRE_NEXT_SSAO_MATERIALS_DIR "${_ogre_media_root}/2.0/scripts/materials/Tutorial_SSAO")
set(OGRE_NEXT_SMAA_MATERIALS_DIR "${_ogre_media_root}/2.0/scripts/materials/Tutorial_SMAA")
set(OGRE_NEXT_COMPUTE_TOOLS_MEDIA_DIR "${_ogre_media_root}/Compute/Tools")
set(OGRE_NEXT_IBL_MEDIA_DIR "${_ogre_media_root}/Compute/Algorithms/IBL")
# PHOTON'S OWN MEDIA (OWN-GI-1): `media/Photon/Voxel` (the voxeliser's and the
# voxel lighting's compute jobs, Voxelizer.material.json, + `Visualizer/`, the photon
# view's Voxels picture), `media/Photon/IrradianceField` (the field's jobs and its
# generation compositor, + `Visualizer/`, the Probes picture) and `media/Photon/Hlms`
# (a PBS library: the voxel and field pixel pieces, which replace upstream's of the
# same names). Ours, copied from the fork when the component moved; Ogre-Next's VCT
# and IrradianceFields folders are not staged at all. Every folder is WIPED first: a
# renamed piece left behind by copy_directory would be parsed beside its replacement.
set(JAHSHAKA_PHOTON_MEDIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/media/Photon")
# The overlay font (STATS_OVERLAY_SPEC §9.3 — fork c290052de (was 0009) class: staged from the
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
     "${OGRE_NEXT_COMPUTE_TOOLS_MEDIA_DIR}/*"
     "${OGRE_NEXT_IBL_MEDIA_DIR}/*" "${JAHSHAKA_PHOTON_MEDIA_DIR}/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/media/Hlms/Jahshaka/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/media/Hlms/Atom/*")
list(APPEND _hlms_inputs "${OGRE_NEXT_DEBUGPACK_ZIP}")
# THE ONE VOXEL RADIANCE READER (PHOTON-READER-1). Two plain-GLSL files are the
# only source of the cone march and the voxel read: the ray jobs include them
# through glslang (`-I src/rayquery/include`), and the three Hlms consumers —
# the pixel shader's cones, the bounce-injection job, the irradiance field's
# generation job — insert them as two library pieces this step WRAPS them into
# at staging (`JahVoxelSample` = the finite test + the sample file, then
# `JahVoxelMarch`). Generated, never hand-copied; the wrapper also fails the
# build if either file carries the Hlms directive mark (the at-sign rule).
set(JAH_VOXEL_READER_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/include")
set(JAH_VOXEL_READER_SOURCES "${JAH_VOXEL_READER_INCLUDE}/jah_rq_finite.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_voxel_sample.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_voxel_march.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_ray_set.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_voxel_faces.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_inject_visibility.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_voxel_ray.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_voxel_cones.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_geom_rows.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_rq_finite.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_alpha_mask.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_rq_alpha.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_ray_origin.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_lamp_ray.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_oct_dir.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_card_sunvis.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_atom_id.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_voxel_parity.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_environment.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_field_window.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_field_read.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_cloud_shadow.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_atmosphere.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_height_fog.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_fog_along.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_card_view.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_area_vis_params.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_area_strata.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_reproject.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_area_vis_law.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_half_stochastic.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_half_round.glsl"
                             "${JAH_VOXEL_READER_INCLUDE}/jah_ssr_march.glsl"
                             "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/parity/jah_voxel_parity_ps_head.glsl"
                             "${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/parity/jah_voxel_parity_ps_main.glsl")
set(JAH_WRAP_HLMS_PIECE "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/WrapHlmsPiece.cmake")
list(APPEND _hlms_inputs ${JAH_VOXEL_READER_SOURCES} "${JAH_WRAP_HLMS_PIECE}")
add_custom_command(OUTPUT "${_hlms_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${JAHSHAKA_MEDIA_DIR}/Hlms"
    # THE FORK'S HLMS FOLDERS ARE WIPED FIRST TOO: every one of them is a template or
    # library path, and a piece file the fork RENAMED (ATOM S3-DRAW: the atmosphere's
    # vertex pieces became `_piece_vs_piece_ps`) left behind by copy_directory is
    # parsed beside its replacement — a duplicate piece in every vertex shader.
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${JAHSHAKA_MEDIA_DIR}/Hlms/Common" "${JAHSHAKA_MEDIA_DIR}/Hlms/Pbs"
            "${JAHSHAKA_MEDIA_DIR}/Hlms/Unlit"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_HLMS_SOURCE_DIR}/Common" "${JAHSHAKA_MEDIA_DIR}/Hlms/Common"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_HLMS_SOURCE_DIR}/Pbs"    "${JAHSHAKA_MEDIA_DIR}/Hlms/Pbs"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_HLMS_SOURCE_DIR}/Unlit"  "${JAHSHAKA_MEDIA_DIR}/Hlms/Unlit"
    # Jahshaka's own Hlms pieces (fog colour + height fog, base-map UV tiling) and
    # the PCC/IBL compositors. WIPED first, like the Ogre folders: this one is an
    # Hlms LIBRARY path, and a renamed piece file left behind by copy_directory
    # would be parsed alongside its replacement and fail as a duplicate @piece.
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/media/Hlms/Jahshaka" "${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka"
    # HLMS ATOM's data folder and its one library (ATOM-S3-PARITY), wiped first for
    # the same reason: Hlms/Atom/Any is a library path.
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${JAHSHAKA_MEDIA_DIR}/Hlms/Atom"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/media/Hlms/Atom" "${JAHSHAKA_MEDIA_DIR}/Hlms/Atom"
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahVoxelSample
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahVoxelSample_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_rq_finite.glsl
            -DIN1=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_sample.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahVoxelMarch
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahVoxelMarch_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_march.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE OCCUPANCY RAY (PHOTON-7 stage 10, 10-1): the ray the world caches gather by - the card relight, the bounce,
    # the field's probe rays - inserted after JahVoxelSample by the consumers that trace it; its head THE RAY SET and
    # its frame (10-2a, jah_ray_set.glsl: the TLAS gather's directions too).
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahVoxelRay
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahVoxelRay_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_ray_set.glsl
            -DIN1=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_faces.glsl
            -DIN2=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_ray.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE FACES A RAY CROSSES IN ONE VOXEL (PHOTON-7 10-4 R7): the one model the occupancy ray's transmissive cells and
    # the light injection's march share (jah_voxel_faces.glsl) - its own piece for the injection, which traces no ray.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahVoxelFaces
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahVoxelFaces_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_faces.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE VOXEL CHAIN'S SHADOW MARCH TO A LAMP (PHOTON-7 10-4 R9): one text for the light injection's direct term, the
    # no-ray area strata and the colour passes' Forward+ lamps at the no-ray tier (jah_inject_visibility.glsl)
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahInjectVisibility
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahInjectVisibility_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_inject_visibility.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE LAMP'S SHADOW RAY IN THE FRAGMENT (PHOTON-7 10-4 R9): the colour passes' Forward+ lamps, one TLAS ray each
    # (jah_lamp_ray.glsl), with the alpha machinery and the origin rule the compute rays use - their includes stripped
    # (the files are concatenated in their dependency order; each carries its own guard)
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahLampRayLib -DSTRIP_INCLUDES=1
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahLampRayLib_piece_ps.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_rq_finite.glsl
            -DIN1=${JAH_VOXEL_READER_INCLUDE}/jah_geom_rows.glsl
            -DIN2=${JAH_VOXEL_READER_INCLUDE}/jah_alpha_mask.glsl
            -DIN3=${JAH_VOXEL_READER_INCLUDE}/jah_rq_alpha.glsl
            -DIN4=${JAH_VOXEL_READER_INCLUDE}/jah_ray_origin.glsl
            -DIN5=${JAH_VOXEL_READER_INCLUDE}/jah_lamp_ray.glsl
            -DIN6=${JAH_VOXEL_READER_INCLUDE}/jah_oct_dir.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE ONE DIFFUSE CONE INTEGRATOR (PHOTON-CARDS-2): the cone frame, the cone
    # sets, the start bias and the escape weight - read by the pixel shader's
    # cones, the bounce-injection job and the surface cache's card job.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahVoxelCones
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahVoxelCones_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_cones.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE GEOMETRY ROW DECODE (PHOTON-CARDS-2): the vertex/index reads through a
    # geometry row's device addresses - the voxeliser's two jobs insert it, the
    # ray jobs' hit normal (jah_rq_geom.glsl) includes it.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahGeomRows
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahGeomRows_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_geom_rows.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE ID IMAGE'S ONE DECODE (ATOM-CLUSTER-CUT): the two words' packing — the id
    # pass includes it, the screen decode inserts it.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahAtomId
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahAtomId_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_atom_id.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE ONE ENVIRONMENT (PHOTON-ENV-1): what every escape sees — the cone
    # lookup on the prefiltered sky chain and the SH irradiance, one file, read
    # by the pixel shader's cones, the probe-array fallback, the bounce job and
    # (through glslang) the ray jobs' miss.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahEnvironment
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahEnvironment_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_environment.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE IRRADIANCE FIELD'S WINDOW (PHOTON-WRITER-1): which atlas tile holds which
    # probe, for the field's generation and integration jobs and the pixel's reader.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahFieldWindow
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahFieldWindow_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_field_window.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE IRRADIANCE FIELD'S CAGE (PHOTON-7 stage 7, 7-2): one text for the PBS pixel's reader
    # (PhotonIfd_piece_ps.any) and the screen-probe gather's integrate (through glslang).
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahFieldRead
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahFieldRead_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_field_read.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE CLOUD LAYER'S SHADOW (CLOUDS-2D-1/-2D-2): one transmittance, read by the
    # pixel (JahFog), the voxel light injection and the card relight.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahCloudShadow
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahCloudShadow_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_cloud_shadow.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE PLANET'S ATMOSPHERE (SKY-ATMOSPHERE-1): its arithmetic, one file — the
    # four table jobs and the pixel shader's aerial perspective insert the piece,
    # the sky quad's and the cloud sheet's plain GLSL programs include the raw copy.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahAtmosphere
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahAtmosphere_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_atmosphere.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    COMMAND ${CMAKE_COMMAND} -DRAW=1
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahAtmosphere.glsl
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_atmosphere.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE EXPONENTIAL HEIGHT FOG (SKY-DEFAULTS-1): its integral, one file — the
    # PBS pixel shader inserts the piece, the height fog's sky quad includes the
    # raw copy.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahHeightFog
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahHeightFog_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_height_fog.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    COMMAND ${CMAKE_COMMAND} -DRAW=1
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahHeightFog.glsl
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_height_fog.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE FOG LAW (PHOTON-I-1): the media between a surface and whoever looks at it,
    # one file — the PBS pixel shader inserts the piece (JahFog maps its pass buffer
    # onto it), the ray jobs include it through glslang, the screen reflection's
    # resolve includes the raw copy.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahFogAlong
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahFogAlong_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_fog_along.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    COMMAND ${CMAKE_COMMAND} -DRAW=1
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahFogAlong.glsl
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_fog_along.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE ONE RADIANCE CEILING AND THE NaN TESTS (PHOTON-7 stage 6, D12): the ray jobs include the file through
    # glslang, the screen reflection's raster history copy and resolve include this raw copy.
    COMMAND ${CMAKE_COMMAND} -DRAW=1
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahRqFinite.glsl
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_rq_finite.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE SCREEN TRACE'S CORE (PHOTON-7 stage 6, the one estimator): the march along a reflected ray through the depth
    # buffer and its fades, pure functions - the ray tier's trace includes the file through glslang, the no-ray chain's
    # raster march and its resolve (the fog law on a screen hit) include this raw copy.
    COMMAND ${CMAKE_COMMAND} -DRAW=1
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahSsrMarch.glsl
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_ssr_march.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # A CARD READ FROM A DIRECTION (PHOTON-CARDS-5): the relight writes the mean
    # light direction the ray jobs' card read restores the lobe's view term with.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahCardView
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahCardView_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_card_view.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE CARDS' SUNVIS TEXEL (PHOTON-7 10-4 R9 (3)): the sun, the fractional lamps and the 48-bit lamp mask - the relight
    # reads and writes it as the ray jobs do (jah_card_sunvis.glsl)
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahCardSunVis
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahCardSunVis_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_card_sunvis.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # THE PIXEL'S AREA LIGHT VISIBILITY'S LAW (PHOTON-7 stage 7, 7-6): one text for the ray tier's two jobs (through
    # glslang) and the no-ray route's Jahshaka/ScreenAreaVis and its resolve - the parameters, the strata's geometry,
    # the reprojection and the law itself, four pieces.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahAreaVisParams
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahAreaVisParams_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_area_vis_params.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahAreaStrata
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahAreaStrata_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_area_strata.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahReproject
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahReproject_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_reproject.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahAreaVisLaw
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahAreaVisLaw_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_area_vis_law.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # ...and the fp16 statistic's stochastic store (PHOTON_2_SPEC R1), the law's and the ray jobs' one text
    # THE fp16 STORE ROUNDED TO NEAREST (REDS-1): the relight's half-float layers take it as the ray jobs do
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahHalfRound
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahHalfRound_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_half_round.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahHalfStochastic
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahHalfStochastic_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_half_stochastic.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    # engine.voxel_reader_parity (tests/compute): the harness body as a piece
    # for its compute half, and its fragment half as a plain GLSL program built
    # from the SAME files - the suite asserts the two stages agree to the bit.
    COMMAND ${CMAKE_COMMAND} -DPIECE=JahVoxelParity
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahVoxelParity_piece_all.any
            -DIN0=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_parity.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    COMMAND ${CMAKE_COMMAND} -DRAW=1
            -DOUT=${JAHSHAKA_MEDIA_DIR}/Hlms/Jahshaka/JahVoxelReaderParity_ps.glsl
            -DIN0=${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/parity/jah_voxel_parity_ps_head.glsl
            -DIN1=${JAH_VOXEL_READER_INCLUDE}/jah_rq_finite.glsl
            -DIN2=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_sample.glsl
            -DIN3=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_march.glsl
            -DIN4=${JAH_VOXEL_READER_INCLUDE}/jah_ray_set.glsl
            -DIN5=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_faces.glsl
            -DIN6=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_ray.glsl
            -DIN7=${JAH_VOXEL_READER_INCLUDE}/jah_voxel_parity.glsl
            -DIN8=${CMAKE_CURRENT_SOURCE_DIR}/src/rayquery/parity/jah_voxel_parity_ps_main.glsl
            -P "${JAH_WRAP_HLMS_PIECE}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_COMMON_MATERIALS_DIR}" "${JAHSHAKA_MEDIA_DIR}/2.0/scripts/materials/Common"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_HDR_MATERIALS_DIR}" "${JAHSHAKA_MEDIA_DIR}/2.0/scripts/materials/HDR"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_SSAO_MATERIALS_DIR}" "${JAHSHAKA_MEDIA_DIR}/2.0/scripts/materials/Tutorial_SSAO"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OGRE_NEXT_SMAA_MATERIALS_DIR}" "${JAHSHAKA_MEDIA_DIR}/2.0/scripts/materials/Tutorial_SMAA"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${JAHSHAKA_MEDIA_DIR}/Photon"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${JAHSHAKA_PHOTON_MEDIA_DIR}" "${JAHSHAKA_MEDIA_DIR}/Photon"
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

# (The host's compile-time DEFAULTS for the plugin and media directories are
# JAHSHAKA_OGRE_{PLUGIN,MEDIA}_DIR_DEFAULT on src/bridge/enginehost.cpp alone —
# the root CMakeLists. They were PUBLIC here: the engine never read them, one
# Studio TU did, and 2,363 TUs carried two absolute paths — SPEED-CPU B2.)
