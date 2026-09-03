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

option(BUILD_BULLET2_DEMOS                      "" OFF)
option(BUILD_OPENGL3_DEMOS                      "" OFF)
option(BUILD_CPU_DEMOS                          "" OFF)
option(BUILD_EXTRAS                             "" OFF)
option(BUILD_UNIT_TESTS                         "" OFF)

add_subdirectory_with_folder("Dependencies/Bullet" thirdparty/bullet3)
add_subdirectory_with_folder("Dependencies/Zip" thirdparty/zip)

set(SRCS
    document/assets/mesh.cpp
    import/model.cpp
    document/materials/defaultmaterial.cpp
    document/scenegraph/meshnode.cpp
    import/graphicshelper.cpp
    document/scenegraph/cameranode.cpp
    document/assets/texture2d.cpp
    document/materials/material.cpp
    core/geometry/trimesh.cpp
    document/assets/vertexlayout.cpp
    document/assets/shader.cpp
    document/scenegraph/shadowmap.cpp
    document/animation/animation.cpp
    document/animation/keyframeset.cpp
    import/materialhelper.cpp
    document/scenegraph/viewernode.cpp
    document/scenegraph/particlesystemnode.cpp
    core/math/mathhelper.cpp
    document/materials/custommaterial.cpp
    document/materials/pbrmaterial.cpp
    document/materials/postprocessmanager.cpp
    document/animation/propertyanim.cpp
    document/scenegraph/lightnode.cpp
    document/scenegraph/decalnode.cpp
    document/animation/skeletalanimation.cpp
    document/animation/clipextractor.cpp
    document/assets/skeleton.cpp
    document/scenegraph/scene.cpp
    document/scenegraph/scenenode.cpp
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
    document/physics/environment.cpp
    document/physics/physicshelper.cpp
    document/physics/charactercontroller.cpp

)

set (HEADERS
    document/animation/nodekeyframe.h
    document/animation/keyframeanimation.h
    document/scenegraph/lightnode.h
    document/scenegraph/decalnode.h
    document/assets/texture2d.h
    document/assets/texture.h
    document/scenegraph/shadowmap.h
    document/assets/mesh.h
    import/model.h
    document/materials/material.h
    document/scenegraph/cameranode.h
    document/scenegraph/meshnode.h
    document/materials/defaultmaterial.h
    document/assets/vertexlayout.h
    document/assets/vertexbuffer.h
    core/viewport.h
    import/graphicshelper.h
    core/geometry/trimesh.h
    core/math/mathhelper.h
    irisglfwd.h
    document/assets/shader.h
    irisgl.h
    core/math/intersectionhelper.h
    document/animation/keyframeset.h
    core/math/bezierhelper.h
    document/animation/animation.h
    import/materialhelper.h
    core/irisutils.h
    core/ziphelper.h
    document/scenegraph/viewernode.h
    document/scenegraph/particle.h
    document/scenegraph/particlesystemnode.h
    document/scenegraph/iviewsource.h
    document/materials/custommaterial.h
    document/materials/pbrmaterial.h
    document/materials/postprocess.h
    document/materials/postprocessmanager.h
    document/animation/propertyanim.h
    document/animation/animableproperty.h
    document/assets/skeleton.h
    document/animation/skeletalanimation.h
    document/animation/clipextractor.h
    core/math/trs.h
    document/scenegraph/scene.h
    document/scenegraph/scenenode.h
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
    document/physics/environment.h
    document/physics/physicshelper.h
    document/physics/physicsproperties.h
    document/physics/charactercontroller.h
)

set(QRCS assets.qrc)

source_group("Resources" FILES ${QRCS})

# This will allow a .lib to be built alongside the dll, ((needed)) for MSVC
if(WIN32 AND MSVC)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 1)
    set(USE_MSVC_RUNTIME_LIBRARY_DLL 1)
endif()

add_library(IrisGL SHARED ${SRCS} ${HEADERS} ${QRCS})

# useful to move away from include_directories() - https://stackoverflow.com/a/38158071/996468
# Note the DIRs that are included, assimp generates a config that we need to find in the build dir. L2
target_include_directories(IrisGL PUBLIC
    "${PROJECT_SOURCE_DIR}/"
    "${PROJECT_BINARY_DIR}/thirdparty/assimp/include/"
    "${PROJECT_SOURCE_DIR}/thirdparty/assimp/include/"
    "${PROJECT_SOURCE_DIR}/thirdparty/bullet3/src"
    "${PROJECT_SOURCE_DIR}/thirdparty/zip/src"
)

target_link_libraries (IrisGL  assimp zip BulletDynamics BulletCollision LinearMath Bullet3Common Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Network Qt6::Concurrent)
target_compile_options(IrisGL PUBLIC)

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
