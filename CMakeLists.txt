cmake_minimum_required(VERSION 3.16 FATAL_ERROR)
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/MP>)
foreach(p CMP0071) # 3.10: Let AUTOMOC and AUTOUIC process GENERATED files
    if(POLICY ${p})
        cmake_policy(SET ${p} NEW)
    endif()
endforeach()
project(IrisGL)

# https://stackoverflow.com/a/45176243/996468
function(get_all_targets _result _dir)
    get_property(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        get_all_targets(${_result} "${_subdir}")
    endforeach()
    get_property(_sub_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
    set(${_result} ${${_result}} ${_sub_targets} PARENT_SCOPE)
endfunction()

function(add_subdirectory_with_folder _folder_name _folder)
    add_subdirectory(${_folder} ${ARGN})
    get_all_targets(_targets "${_folder}")
    foreach(_target IN LISTS _targets)
        set_target_properties(
            ${_target}
            PROPERTIES FOLDER "${_folder_name}"
        )
    endforeach()
endfunction()

#find_package(Qt6 COMPONENTS Core)

find_package(Qt6 COMPONENTS Core Gui Multimedia Network Concurrent)
message(STATUS "Qt6_VERSION: ${Qt6_VERSION}")
message(STATUS "Qt6_INSTALL_PREFIX: ${Qt6_INSTALL_PREFIX}")

set_property(GLOBAL PROPERTY USE_FOLDERS TRUE)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTORCC ON)
# The document library is QObject-free (audit §3.5) and the vendored trees have
# zero Q_OBJECT - moc has nothing to do anywhere under irisgl/ (audit §5.4.7).
set(CMAKE_AUTOMOC OFF)

set_property(GLOBAL PROPERTY AUTOGEN_TARGETS_FOLDER AutoMocFolder)

# BUILD_SHARED_LIBS must be decided BEFORE the first vendored add_subdirectory
# (assimp bakes it at configure time) - audit §5.4.5.
if (WIN32)
    set(BUILD_SHARED_LIBS 0)
    option(USE_MSVC_RUNTIME_LIBRARY_DLL         "" ON)
    # set(USE_MSVC_RUNTIME_LIBRARY_DLL 1)
else()
  set(BUILD_SHARED_LIBS 1)   # shared libs doesn't work with msvc (there aren't any dllexports defined)
endif()

option(ASSIMP_BUILD_ASSIMP_TOOLS                "" OFF)
option(ASSIMP_BUILD_TESTS                       "" OFF)
option(ASSIMP_NO_EXPORT                         "" ON)
option(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT    "" OFF)

# The importer ALLOWLIST. ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT is OFF above,
# so an importer that is not named here is not compiled at all (assimp's
# ADD_ASSIMP_IMPORTER macro -DASSIMP_BUILD_NO_<name>_IMPORTER's it away) and its
# extension is simply not readable — silently, at runtime. Adding a format is
# therefore a THREE-part change: the option here, the extension in
# Constants (MODEL_EXTS / ANIMATION_EXTS, which drive the sniffers and every
# file dialog), and a suite that proves the format loads through our paths.
option(ASSIMP_BUILD_OBJ_IMPORTER                "" ON)
option(ASSIMP_BUILD_FBX_IMPORTER                "" ON)
option(ASSIMP_BUILD_GLTF_IMPORTER               "" ON)
option(ASSIMP_BUILD_COLLADA_IMPORTER            "" ON)
# BVH is MOCAP: animation-only files with zero meshes, for the Avatar module's
# cross-file clip path (Studio tests/avatar). PLY and STL are mesh formats
# (scanners, photogrammetry, every 3D printer on earth) with no materials and,
# for STL, no UVs and no vertex normals worth trusting — the canonical preset's
# GenSmoothNormals is what makes them shadeable.
option(ASSIMP_BUILD_BVH_IMPORTER                "" ON)
option(ASSIMP_BUILD_PLY_IMPORTER                "" ON)
option(ASSIMP_BUILD_STL_IMPORTER                "" ON)
if(APPLE)
    # assimp's bundled zlib #defines fdopen on Mac (ancient upstream zlib bug),
    # which breaks the macOS SDK's stdio.h. The SDK always ships zlib: use it.
    option(ASSIMP_BUILD_ZLIB                    "" OFF)
else()
    option(ASSIMP_BUILD_ZLIB                    "" ON)
endif()

# Vendored trees are never edited in place: our changes to assimp live as patch
# files in thirdparty/assimp-patches/ and are applied HERE, before the tree is
# added, so a fresh clone cannot silently build unpatched sources.  Idempotent;
# a patch that no longer applies is a hard error.  See that directory's
# README.md (and irisgl/cmake/ApplyVendorPatches.cmake) for the whole law.
execute_process(
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/assimp
            -DPATCHES=${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/assimp-patches
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyVendorPatches.cmake
    RESULT_VARIABLE _assimp_patch_result
    OUTPUT_VARIABLE _assimp_patch_output
    ERROR_VARIABLE  _assimp_patch_output)
string(REGEX REPLACE "(^|\n)-- " "\\1" _assimp_patch_output "${_assimp_patch_output}")
string(STRIP "${_assimp_patch_output}" _assimp_patch_output)
if(_assimp_patch_output)
    message(STATUS "${_assimp_patch_output}")
endif()
if(NOT _assimp_patch_result EQUAL 0)
    message(FATAL_ERROR "assimp patches could not be applied — see above")
endif()

add_subdirectory_with_folder("Dependencies/Assimp" thirdparty/assimp)

# assimp's ~1,900 internal ai_assert()s are its OWN bounds/invariant checks on
# parsed FILE CONTENT, and ai_assert expands to NOTHING unless ASSIMP_BUILD_DEBUG
# is defined (include/assimp/ai_assert.h) — assimp's build never defines it, so
# until 2026-09-04 every Jahshaka build, Debug included, compiled them all away.
# That is how CVE-2025-70067's "guarded" strcpy shipped live (see
# thirdparty/assimp-patches/0002). Turn them on in DEBUG BUILDS ONLY, and only
# for assimp's own objects (PRIVATE): a malformed model then aborts loudly on a
# developer's machine — which is the point — while release builds keep upstream's
# shipping configuration. Multi-config safe via the generator expression.
target_compile_definitions(assimp PRIVATE $<$<CONFIG:Debug>:ASSIMP_BUILD_DEBUG>)

option(BUILD_BULLET2_DEMOS                      "" OFF)
option(BUILD_OPENGL3_DEMOS                      "" OFF)
option(BUILD_CPU_DEMOS                          "" OFF)
option(BUILD_EXTRAS                             "" OFF)
option(BUILD_UNIT_TESTS                         "" OFF)

# VENDORED-DEPENDENCY LINK POLICY (ENGINEERING_DEBT L4, lead 2026-09-09): bullet3
# and zip are STATIC, always, and position-independent (they link into the shared
# libIrisGL). Why it has to be said: bullet's own `OPTION(BUILD_SHARED_LIBS ... OFF)`
# runs under CMP0077 OLD (its cmake_minimum_required is 2.4.3). On a CLEAN configure
# that option() discarded our normal variable (bullet came out .a); on a RE-configure
# the cache entry already existed, the normal variable survived, and bullet came out
# .so — the link type flipped with configure HISTORY, libIrisGL silently gained three
# runtime .so deps, and the macOS bundle contents depended on how the tree had been
# configured (ADDENDUM 4). Pinning CMP0077 to NEW for the vendored subtrees means a
# normal variable is never clobbered by a vendored option(); saying OFF here means
# the answer is the same on every configure. assimp stays SHARED (decided above,
# baked at its add_subdirectory): it is the one vendored library the app loads as a
# runtime dependency by design.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(_jah_outer_shared_libs "${BUILD_SHARED_LIBS}")
set(_jah_outer_pic "${CMAKE_POSITION_INDEPENDENT_CODE}")
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory_with_folder("Dependencies/Bullet" thirdparty/bullet3)
add_subdirectory_with_folder("Dependencies/Zip" thirdparty/zip)
# meshoptimizer (ATOM stage 1, SPECS/NANITE_SPEC.md §7): the simplifier that
# builds a mesh's LOD chain at IMPORT time, inside MeshBake. Same policy as
# bullet3 and zip — STATIC and position-independent, because it links into the
# shared libIrisGL — and its own install rules are OFF: it is an implementation
# detail of the bake, not something a consumer of IrisGL links or includes.
# MESHOPT_BUILD_DEMO stays OFF; demo/clusterlod.h is vendored separately by COPY
# (thirdparty/meshoptimizer-clusterlod/, with its provenance) because it is an
# example file, not part of the library.
option(MESHOPT_INSTALL "" OFF)
option(MESHOPT_BUILD_DEMO "" OFF)
option(MESHOPT_BUILD_GLTFPACK "" OFF)
option(MESHOPT_BUILD_SHARED_LIBS "" OFF)
add_subdirectory_with_folder("Dependencies/MeshOptimizer" thirdparty/meshoptimizer)
set_target_properties(meshoptimizer PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS "${_jah_outer_shared_libs}")
set(CMAKE_POSITION_INDEPENDENT_CODE "${_jah_outer_pic}")

set(SRCS
    document/assets/mesh.cpp
    document/materials/defaultmaterial.cpp
    document/scenegraph/meshnode.cpp
    import/graphicshelper.cpp
    import/meshprewarm.cpp
    import/parsecensus.cpp
    import/meshbake.cpp
    # ATOM stage 2: the ONE TU that compiles clusterlod.h's implementation
    # (vendored copy, thirdparty/meshoptimizer-clusterlod/, pinned at meshoptimizer
    # v1.2 = 9d9890c73011d75920af614485296d1e03e95448 — PROVENANCE.md beside it).
    import/clusterlod.cpp
    import/importflags.cpp
    import/importsettings.cpp
    import/scenesource.cpp
    import/modelsceneinfo.cpp
    import/clipfileinfo.cpp
    document/scenegraph/cameranode.cpp
    document/scenegraph/cameralens.cpp
    document/assets/livetextures.cpp
    document/assets/texture2d.cpp
    document/materials/material.cpp
    core/geometry/trimesh.cpp
    document/assets/vertexlayout.cpp
    document/scenegraph/shadowmap.cpp
    document/animation/animation.cpp
    document/animation/keyframeset.cpp
    import/materialhelper.cpp
    document/scenegraph/particlesystemnode.cpp
    core/math/mathhelper.cpp
    document/materials/pbrmaterial.cpp
    document/materials/postprocessmanager.cpp
    document/animation/propertyanim.cpp
    document/scenegraph/lightnode.cpp
    document/scenegraph/decalnode.cpp
    document/animation/skeletalanimation.cpp
    document/animation/clipextractor.cpp
    document/animation/locomotion.cpp
    document/assets/skeleton.cpp
    document/assets/avatardefinition.cpp
    document/scenegraph/looks.cpp
    document/scenegraph/scene.cpp
    document/scenegraph/skybake.cpp
    document/scenegraph/scenenode.cpp
    document/scenegraph/nodegraph.cpp
    document/scenegraph/scenepicking.cpp
    document/scenegraph/socket.cpp
    core/geometry/plane.cpp
    core/geometry/frustum.cpp
    core/geometry/aabb.cpp
    core/logger.cpp
    document/assets/linemeshbuilder.cpp
    document/assets/shapehelper.cpp
    document/materials/postfx/fxaapostprocess.cpp
	document/materials/renderstates.cpp
    document/materials/rasterizerstate.cpp
    document/physics/avatarmovement.cpp
    document/physics/environment.cpp
    document/physics/physicshelper.cpp
    document/input/inputmap.cpp
    document/input/possession.cpp

)

set (HEADERS
    document/animation/nodekeyframe.h
    document/animation/keyframeanimation.h
    document/scenegraph/lightnode.h
    document/scenegraph/decalnode.h
    document/assets/livetextures.h
    document/assets/texture2d.h
    document/assets/texture.h
    document/scenegraph/shadowmap.h
    document/assets/mesh.h
    document/materials/material.h
    document/scenegraph/cameranode.h
    document/scenegraph/cameralens.h
    document/scenegraph/meshnode.h
    document/materials/defaultmaterial.h
    document/assets/vertexlayout.h
    document/assets/vertexbuffer.h
    core/viewport.h
    import/graphicshelper.h
    import/meshprewarm.h
    import/parsecensus.h
    import/meshbake.h
    import/importflags.h
    import/clipnaming.h
    import/importsettings.h
    import/scenesource.h
    import/modelsceneinfo.h
    import/clipfileinfo.h
    core/geometry/trimesh.h
    core/math/mathhelper.h
    irisglfwd.h
    irisgl.h
    core/math/intersectionhelper.h
    document/animation/keyframeset.h
    core/math/bezierhelper.h
    document/animation/animation.h
    import/materialhelper.h
    core/irisutils.h
    core/ziphelper.h
    document/scenegraph/particlesystemnode.h
    document/scenegraph/iviewsource.h
    document/materials/pbrmaterial.h
    document/materials/postprocess.h
    document/materials/postprocessmanager.h
    document/animation/propertyanim.h
    document/animation/animableproperty.h
    document/assets/skeleton.h
    document/assets/avatardefinition.h
    document/animation/skeletalanimation.h
    document/animation/clipextractor.h
    document/animation/locomotion.h
    core/math/trs.h
    document/scenegraph/looks.h
    document/scenegraph/scene.h
    document/scenegraph/simulationclock.h
    document/scenegraph/skybake.h
    document/scenegraph/scenenode.h
    document/scenegraph/nodegraph.h
    document/scenegraph/scenepicking.h
    document/scenegraph/socket.h
    core/properties/property.h
    core/geometry/boundingsphere.h
    core/geometry/plane.h
    core/geometry/frustum.h
    core/geometry/aabb.h
    core/math/transform.h
    core/logger.h
    core/performancetimer.h
    document/materials/renderstates.h
    document/assets/linemeshbuilder.h
    document/assets/shapehelper.h
    document/materials/postfx/fxaapostprocess.h
    document/materials/rasterizerstate.h
    document/physics/avatarmovement.h
    document/physics/environment.h
    document/physics/physicshelper.h
    document/physics/physicsproperties.h
    document/input/inputmap.h
    document/input/possession.h
)

set(QRCS assets.qrc)

source_group("Resources" FILES ${QRCS})

# This will allow a .lib to be built alongside the dll, ((needed)) for MSVC
if(WIN32 AND MSVC)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 1)
    set(USE_MSVC_RUNTIME_LIBRARY_DLL 1)
endif()

# --- Mesh-bake producer key (MESH_BAKE_SPEC.md phase 1; REDEFINED 2026-09-15) -
# The same mechanism the engine uses for its shader-cache fingerprint
# (irisgl/engine/CMakeLists.txt): a configure-time SHA256 over the source files
# that PRODUCE a bake. A bake is derived data whose validity depends on the code
# that built it, and nothing inside the blob can see a source edit — so the
# producer hash is a term of the bake's fingerprint, exactly as
# JAHSHAKA_ENGINE_BUILD_ID is a term of the shader cache's.
#
# Deliberately NOT the engine build id: a bake carries no engine-produced
# bytes (see the rationale at the top of import/meshbake.h), so an engine
# rebuild cannot invalidate one, and folding it in would throw the whole
# library's bakes away on every OgreEngine.cpp edit for no correctness gain.
#
# WHY THE LIST IS THREE FILES AND NOT TWELVE (BAKEKEY-1, measured 2026-09-15).
# The list used to carry the document's mesh/mesh-node/skeleton classes too.
# Those files are edited for reasons that have nothing to do with a bake's
# bytes — a getter, a render-side field, a comment — and the measurement said
# so: of the 408 irisgl commits since the bake landed (2026-09-05), 25 touched
# one of the twelve, on 10 of the 11 working days, document/scenegraph/
# meshnode.cpp alone on 12 of them. Every one of those builds produced a new
# producer id, and every new producer id stales EVERY bake in EVERY library:
# the whole point of the bake — an open is a file read — was being thrown away
# about once a day, for edits that could not change a single baked byte.
#
# So the key is now three parts, and each part answers a different question:
#
#   1. THE FORMAT VERSION (kFormatVersion in import/meshbake.cpp, the `v%1`
#      term of MeshBake::producerId) — hand-bumped. It covers the DOCUMENT-side
#      layout and MEANING of what the bake serializes: the vertex layout, the
#      skeleton, the material fields and the colour space they are recorded in.
#      A change in document/assets/mesh.{cpp,h}, document/assets/skeleton.*,
#      document/scenegraph/meshnode.cpp or core/geometry/trimesh.cpp that
#      really does move a baked byte is declared by bumping that constant —
#      which is exactly what v2, v4 and v5 already were.
#   2. THIS HASH, over the files that WRITE the bake and whose text change
#      means the produced bytes can change:
#        import/meshbake.cpp    — the builder AND the serializer: every byte of
#                                 the blob is written by this file, and the
#                                 aiScene walk that decides what goes in it.
#        import/meshbake.h      — the declarations the serializer walks
#                                 (MeshBake::Model, BakedNode): a field added
#                                 or removed here changes the written record
#                                 without meshbake.cpp having to change.
#        import/materialhelper.cpp — MeshBake::buildFromScene CALLS
#                                 MaterialHelper::extractMaterialData for every
#                                 material record it writes (spec-gloss →
#                                 metallic-roughness, texture-name resolution,
#                                 embedded-texture naming). Its numbers ARE
#                                 baked bytes.
#   3. A LINT, tests/hygiene/bake_key_guard.sh (ctest: source.bake_key_guard),
#      which fails when one of the document-side files that LEFT this hash is
#      touched without either bumping the format version or saying, in the
#      commit message, `bake-output: unchanged`. The version is a hand bump, so
#      it needs a gate that makes forgetting it loud.
#
# NOT HASHED, and why — each of these was in the old twelve:
#   import/importflags.{h,cpp} — the only thing in them that reaches a bake is
#       the VALUE of ImportFlags::Canonical, and producerId already hashes that
#       value NUMERICALLY (`flags%4`). Hashing the files adds nothing the exact
#       integer misses and would stale every library on a comment edit; the
#       headers are ~90% prose. (ClipNamesOnly never reaches a bake at all.)
#   import/graphicshelper.cpp — holds no conversion of its own: five thin
#       wrappers around `new Mesh(aiMesh*)`, Mesh::extractSkeleton/
#       extractAnimations and ReadFile(ImportFlags::…). MeshBake does its own
#       ReadFile and its own loop, so no baked byte passes through this file.
#       It is the PARSE twin the bake must agree with, which is what the lint
#       watches it for.
#   import/materialhelper.h — declarations only: no inline bodies, no
#       constants. A behaviour change there cannot avoid editing the .cpp.
#   document/assets/mesh.{cpp,h}, document/assets/skeleton.{cpp,h},
#   document/scenegraph/meshnode.cpp, core/geometry/trimesh.cpp — the
#       version's job (part 1), guarded by the lint (part 3). meshnode.cpp, the
#       biggest churner of the twelve, does not write a bake at all: it
#       CONSUMES one (MeshBake::buildFragment builds MeshNodes).
#
# The list is exported to the compiler as well, so the suite can prove that
# what this file hashes is what MeshBake::hashedSources() reports and that the
# C++ re-implementation of this algorithm agrees with CMake's output
# (tests/meshbake, section 6). ONE list, two consumers.
#   thirdparty/meshoptimizer/src/{meshoptimizer.h,simplifier.cpp} — ADDED by
#       ATOM stage 1 (2026-09-15). meshbake.cpp now calls meshopt_simplify* and
#       WRITES ITS OUTPUT into the bake, so the simplifier's text is as much a
#       producer of baked bytes as meshbake.cpp is: a submodule bump that
#       changes the algorithm changes every LOD level in every library, and
#       nothing else in the key could notice (the version constant describes the
#       LAYOUT, and the layout would not move). Two files, not the whole
#       library: `simplifier.cpp` IS the algorithm, and the header carries
#       MESHOPTIMIZER_VERSION plus every signature we call. They are vendored,
#       so they change only when someone deliberately moves the pin — exactly
#       when a re-bake is wanted.
#   thirdparty/meshoptimizer-clusterlod/clusterlod.h and the four library files
#       it drives — ADDED by ATOM stage 2 (lane ATOM-CLUSTER-1, 2026-09-23). The
#       bake now writes a CLUSTER DAG, and its bytes are clodBuild's output:
#       the header (the whole algorithm — partition, lock, simplify, re-split)
#       plus clusterizer.cpp (the meshlet builder), partition.cpp (the group
#       partitioner), spatialorder.cpp (partition sort) and meshletutils.cpp
#       (cluster bounds, local indices, meshlet optimisation) — every file a
#       DAG byte passes through, and nothing else. indexgenerator.cpp is NOT
#       here: its position remap is an equality test with one answer.
#       import/clusterlod.cpp is not either: it is three include lines and a
#       define, and cannot change a byte without the header changing.
set(_mesh_bake_hashed_sources
    import/meshbake.cpp
    import/meshbake.h
    import/materialhelper.cpp
    thirdparty/meshoptimizer/src/meshoptimizer.h
    thirdparty/meshoptimizer/src/simplifier.cpp
    thirdparty/meshoptimizer-clusterlod/clusterlod.h
    thirdparty/meshoptimizer/src/clusterizer.cpp
    thirdparty/meshoptimizer/src/partition.cpp
    thirdparty/meshoptimizer/src/spatialorder.cpp
    thirdparty/meshoptimizer/src/meshletutils.cpp)
# A CONFIGURE-TIME hash goes STALE (found by this lane's own suite, 2026-09-15):
# editing import/meshbake.cpp does not re-run CMake, so the compiled-in term
# kept describing the PREVIOUS contents of the file until something unrelated
# triggered a reconfigure — a build could write bakes under a key that does not
# name the code that wrote them. Three files can say "re-configure when I
# change" for nothing; the old twelve could not (every mesh.cpp edit would have
# re-run CMake as well as re-baked the library).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_mesh_bake_hashed_sources})

set(_mesh_bake_blob "")
foreach(_s ${_mesh_bake_hashed_sources})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_s}")
        message(FATAL_ERROR "mesh-bake key: ${_s} does not exist — fix the list "
                            "in irisgl/CMakeLists.txt (and tests/hygiene/bake_key_guard.sh)")
    endif()
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${_s}" _h)
    string(APPEND _mesh_bake_blob "${_h};")
endforeach()
string(SHA256 _mesh_bake_producer_id "${_mesh_bake_blob}")
# '|' separated, not ';': a semicolon inside a compile definition is a CMake
# list separator and would become three defines.
string(REPLACE ";" "|" _mesh_bake_hashed_list "${_mesh_bake_hashed_sources}")

add_library(IrisGL SHARED ${SRCS} ${HEADERS} ${QRCS})

# useful to move away from include_directories() - https://stackoverflow.com/a/38158071/996468
target_include_directories(IrisGL PUBLIC
    "${PROJECT_SOURCE_DIR}/"
    "${PROJECT_SOURCE_DIR}/thirdparty/bullet3/src"
    "${PROJECT_SOURCE_DIR}/thirdparty/zip/src"
)

# assimp is PRIVATE (ENGINEERING_DEBT L4 part 3, 2026-09-10): no public IrisGL
# header includes it, and no consumer of libIrisGL links or includes it. The
# import-side TUs (document/assets/mesh.cpp, document/scenegraph/meshnode.cpp,
# import/*.cpp) are the only ones that see its headers; everything a consumer
# needs from a parse crosses as plain data (import/modelsceneinfo.h,
# import/clipfileinfo.h) or as the opaque import/scenesource.h. A translation
# unit outside this library that includes an assimp header fails to compile —
# that is the guard. (The build-dir include is assimp's generated config.h.)
target_include_directories(IrisGL PRIVATE
    "${PROJECT_BINARY_DIR}/thirdparty/assimp/include/"
    "${PROJECT_SOURCE_DIR}/thirdparty/assimp/include/"
)

target_link_libraries (IrisGL PUBLIC zip BulletDynamics BulletCollision LinearMath Bullet3Common Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Network Qt6::Concurrent)
target_link_libraries (IrisGL PRIVATE assimp)
# meshoptimizer is PRIVATE for the same reason assimp is: the LOD chain it builds
# crosses to the engine as plain data (iris::Mesh::lodIndices), never as one of
# its types, and no public IrisGL header includes it. import/meshbake.cpp is the
# only TU that does.
target_link_libraries (IrisGL PRIVATE meshoptimizer)
target_include_directories(IrisGL PRIVATE
    "${PROJECT_SOURCE_DIR}/thirdparty/meshoptimizer/src")

# THE scene graph is Ogre-Next's (SPECS/SCENEGRAPH_SPEC.md D2), so the document
# library links it. PRIVATE, exactly like the engine does: Ogre must not reach
# Studio's include path, and the ONE translation unit here that includes an Ogre
# header is document/scenegraph/nodegraph.cpp. See nodegraph.h for the law.
target_link_libraries(IrisGL PRIVATE OgreNext)
target_compile_options(IrisGL PUBLIC)
target_compile_definitions(IrisGL PRIVATE
    JAHSHAKA_MESH_BAKE_PRODUCER_ID="${_mesh_bake_producer_id}"
    JAHSHAKA_MESH_BAKE_HASHED_SOURCES="${_mesh_bake_hashed_list}")

# A6: these two warning classes are undefined behaviour, not style - keep them fixed.
if (NOT MSVC)
    target_compile_options(IrisGL PRIVATE -Werror=return-type -Werror=delete-incomplete)
endif()

# --- install (audit §5.4.8: IrisGL used to have no install story at all) ----
# Minimal but real: the shared document library plus its public headers, laid
# out exactly as the build consumes them (repo-root-relative includes).
include(GNUInstallDirs)
install(TARGETS IrisGL
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY core document import DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/irisgl
        FILES_MATCHING PATTERN "*.h")
install(FILES irisgl.h irisglfwd.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/irisgl)
