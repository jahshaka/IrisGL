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
set(BUILD_SHARED_LIBS "${_jah_outer_shared_libs}")
set(CMAKE_POSITION_INDEPENDENT_CODE "${_jah_outer_pic}")

set(SRCS
    document/assets/mesh.cpp
    import/model.cpp
    document/materials/defaultmaterial.cpp
    document/scenegraph/meshnode.cpp
    import/graphicshelper.cpp
    import/meshprewarm.cpp
    import/meshbake.cpp
    import/importflags.cpp
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
    import/modelloader.cpp
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
    import/model.h
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
    import/meshbake.h
    import/importflags.h
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
	import/modelloader.h
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

# --- Mesh-bake producer id (MESH_BAKE_SPEC.md phase 1) -----------------------
# The same mechanism the engine uses for its shader-cache fingerprint
# (irisgl/engine/CMakeLists.txt): a configure-time SHA256 over the TUs that
# PRODUCE a bake. A bake is derived data whose validity depends on the code
# that built it, and nothing inside the blob can see a source edit — so the
# producer hash is a term of the bake's fingerprint, exactly as
# JAHSHAKA_ENGINE_BUILD_ID is a term of the shader cache's.
#
# Deliberately NOT the engine build id: a bake carries no engine-produced
# bytes (see the rationale at the top of import/meshbake.h), so an engine
# rebuild cannot invalidate one, and folding it in would throw the whole
# library's bakes away on every OgreEngine.cpp edit for no correctness gain.
set(_mesh_bake_blob "")
foreach(_s import/meshbake.cpp import/meshbake.h
            import/importflags.h import/importflags.cpp
            import/materialhelper.cpp import/graphicshelper.cpp
            document/assets/mesh.cpp document/assets/mesh.h
            document/assets/skeleton.cpp document/assets/skeleton.h
            document/scenegraph/meshnode.cpp core/geometry/trimesh.cpp)
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${_s}" _h)
    string(APPEND _mesh_bake_blob "${_h};")
endforeach()
string(SHA256 _mesh_bake_producer_id "${_mesh_bake_blob}")
set(JAHSHAKA_MESH_BAKE_PRODUCER_ID "${_mesh_bake_producer_id}" PARENT_SCOPE)

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

# THE scene graph is Ogre-Next's (SPECS/SCENEGRAPH_SPEC.md D2), so the document
# library links it. PRIVATE, exactly like the engine does: Ogre must not reach
# Studio's include path, and the ONE translation unit here that includes an Ogre
# header is document/scenegraph/nodegraph.cpp. See nodegraph.h for the law.
target_link_libraries(IrisGL PRIVATE OgreNext)
target_compile_options(IrisGL PUBLIC)
target_compile_definitions(IrisGL PRIVATE
    JAHSHAKA_MESH_BAKE_PRODUCER_ID="${_mesh_bake_producer_id}")

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
