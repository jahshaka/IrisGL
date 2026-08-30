cmake_minimum_required(VERSION 3.3 FATAL_ERROR)
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

set_property(GLOBAL PROPERTY AUTOGEN_TARGETS_FOLDER AutoMocFolder)

option(ASSIMP_BUILD_ASSIMP_TOOLS                "" OFF)
option(ASSIMP_BUILD_TESTS                       "" OFF)
option(ASSIMP_NO_EXPORT                         "" ON)
option(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT    "" OFF)

option(ASSIMP_BUILD_OBJ_IMPORTER                "" ON)
option(ASSIMP_BUILD_FBX_IMPORTER                "" ON)
option(ASSIMP_BUILD_GLTF_IMPORTER               "" ON)
option(ASSIMP_BUILD_COLLADA_IMPORTER            "" ON)
option(ASSIMP_BUILD_ZLIB                        "" ON)

add_subdirectory_with_folder("Dependencies/Assimp" src/assimp)

option(BUILD_BULLET2_DEMOS                      "" OFF)
option(BUILD_OPENGL3_DEMOS                      "" OFF)
option(BUILD_CPU_DEMOS                          "" OFF)
option(BUILD_EXTRAS                             "" OFF)
option(BUILD_UNIT_TESTS                         "" OFF)

if (WIN32)
    set(BUILD_SHARED_LIBS 0)
    option(USE_MSVC_RUNTIME_LIBRARY_DLL         "" ON)
    # set(USE_MSVC_RUNTIME_LIBRARY_DLL 1)
else()
  set(BUILD_SHARED_LIBS 1)   # shared libs doesn't work with msvc (there aren't any dllexports defined)
endif()

add_subdirectory_with_folder("Dependencies/Bullet" src/bullet3)
add_subdirectory_with_folder("Dependencies/Zip" src/zip)

set(SRCS
    src/graphics/mesh.cpp
    src/graphics/model.cpp
    src/materials/defaultmaterial.cpp
    src/scenegraph/meshnode.cpp
    src/graphics/graphicshelper.cpp
    src/scenegraph/cameranode.cpp
    src/graphics/texture2d.cpp
    src/graphics/material.cpp
    src/geometry/trimesh.cpp
    src/graphics/vertexlayout.cpp
    src/graphics/shader.cpp
    src/graphics/shadowmap.cpp
    src/animation/animation.cpp
    src/animation/keyframeset.cpp
    src/materials/materialhelper.cpp
    src/scenegraph/viewernode.cpp
    src/scenegraph/particlesystemnode.cpp
    src/math/mathhelper.cpp
    src/materials/custommaterial.cpp
    src/materials/pbrmaterial.cpp
    src/graphics/postprocessmanager.cpp
    src/animation/propertyanim.cpp
    src/scenegraph/lightnode.cpp
    src/animation/skeletalanimation.cpp
    src/graphics/skeleton.cpp
    src/scenegraph/scene.cpp
    src/scenegraph/scenenode.cpp
    src/geometry/plane.cpp
    src/geometry/frustum.cpp
    src/geometry/aabb.cpp
    src/core/logger.cpp
    src/graphics/utils/linemeshbuilder.cpp
    src/graphics/utils/shapehelper.cpp
    src/postprocesses/fxaapostprocess.cpp
	src/graphics/renderstates.cpp
    src/graphics/rasterizerstate.cpp
    src/content/modelloader.cpp
    src/physics/environment.cpp
    src/physics/physicshelper.cpp
    src/physics/charactercontroller.cpp

)

set (HEADERS
    src/animation/nodekeyframe.h
    src/animation/keyframeanimation.h
    src/scenegraph/lightnode.h
    src/graphics/texture2d.h
    src/graphics/texture.h
    src/graphics/shadowmap.h
    src/graphics/mesh.h
    src/graphics/model.h
    src/graphics/material.h
    src/scenegraph/cameranode.h
    src/scenegraph/meshnode.h
    src/materials/defaultmaterial.h
    src/graphics/vertexlayout.h
    src/graphics/vertexbuffer.h
    src/graphics/viewport.h
    src/graphics/graphicshelper.h
    src/geometry/trimesh.h
    src/math/mathhelper.h
    src/irisglfwd.h
    src/graphics/shader.h
    src/irisgl.h
    src/math/intersectionhelper.h
    src/animation/keyframeset.h
    src/math/bezierhelper.h
    src/animation/animation.h
    src/materials/materialhelper.h
    src/core/irisutils.h
    src/core/ziphelper.h
    src/scenegraph/viewernode.h
    src/graphics/particle.h
    src/scenegraph/particlesystemnode.h
    src/graphics/iviewsource.h
    src/materials/custommaterial.h
    src/materials/pbrmaterial.h
    src/graphics/postprocess.h
    src/graphics/postprocessmanager.h
    src/animation/propertyanim.h
    src/animation/animableproperty.h
    src/graphics/skeleton.h
    src/animation/skeletalanimation.h
    src/scenegraph/scene.h
    src/scenegraph/scenenode.h
    src/core/property.h
    src/geometry/boundingsphere.h
    src/geometry/plane.h
    src/geometry/frustum.h
    src/geometry/aabb.h
    src/math/transform.h
    src/core/logger.h
    src/core/performancetimer.h
    src/graphics/renderstates.h
    src/graphics/utils/linemeshbuilder.h
    src/graphics/utils/shapehelper.h
    src/postprocesses/fxaapostprocess.h
    src/graphics/rasterizerstate.h
	src/content/modelloader.h
    src/physics/environment.h
    src/physics/physicshelper.h
    src/physics/physicsproperties.h
    src/physics/charactercontroller.h
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
    "${PROJECT_SOURCE_DIR}/src/"
    "${PROJECT_BINARY_DIR}/src/assimp/include/"
    "${PROJECT_SOURCE_DIR}/src/assimp/include/"
    "${PROJECT_SOURCE_DIR}/src/bullet3/src"
    "${PROJECT_SOURCE_DIR}/src/zip/src"
)

target_link_libraries (IrisGL  assimp zip BulletDynamics BulletCollision LinearMath Bullet3Common Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Network Qt6::Concurrent)
target_compile_options(IrisGL PUBLIC)

# A6: these two warning classes are undefined behaviour, not style - keep them fixed.
if (NOT MSVC)
    target_compile_options(IrisGL PRIVATE -Werror=return-type -Werror=delete-incomplete)
endif()
