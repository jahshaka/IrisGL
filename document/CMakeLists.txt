# IrisDocument (NOQT-DOCUMENT-1 stage 6, V2 plan 0.15): THE DOCUMENT, A Qt-FREE SHARED LIBRARY - the scene graph, its nodes,
# assets, materials, animation, physics and input, on the standard library, IrisCore and Bullet. ONE SHARED library so that
# every process-wide table (the node graph's owners and journal, the live textures, the shipped meshes, the texture probe, the
# materials' revision) exists ONCE in a process, defined here; hidden by default, only what document/export.h's
# IRIS_DOCUMENT_API marks is exported. IrisGL (the importer, the bake, the mirror) links it PUBLIC, so no consumer's link
# line changes. It declares C++17 itself and runs no Qt code generator; tests/hygiene/no_qt_below.py holds it (no Qt include
# directory, define or generator on its compile lines, no Qt symbol in it, its stateful symbols defined in it alone).
add_library(IrisDocument SHARED
    animation/animableproperty.h
    animation/animation.cpp
    animation/animation.h
    animation/clipextractor.cpp
    animation/clipextractor.h
    animation/keyframeanimation.h
    animation/keyframeset.cpp
    animation/keyframeset.h
    animation/locomotion.cpp
    animation/locomotion.h
    animation/propertyanim.cpp
    animation/propertyanim.h
    animation/skeletalanimation.cpp
    animation/skeletalanimation.h
    assets/avatardefinition.cpp
    assets/avatardefinition.h
    assets/image.h
    assets/linemeshbuilder.cpp
    assets/linemeshbuilder.h
    assets/livetextures.cpp
    assets/livetextures.h
    assets/mesh.cpp
    assets/mesh.h
    assets/shapehelper.cpp
    assets/shapehelper.h
    assets/shippedmeshes.cpp
    assets/shippedmeshes.h
    assets/skeleton.cpp
    assets/skeleton.h
    assets/texture.h
    assets/texture2d.cpp
    assets/texture2d.h
    assets/vertexbuffer.h
    assets/vertexlayout.cpp
    assets/vertexlayout.h
    export.h
    input/inputmap.cpp
    input/inputmap.h
    input/keyvaluestore.h
    input/possession.cpp
    input/possession.h
    mapvalue.h
    materials/material.cpp
    materials/material.h
    materials/pbrmaterial.cpp
    materials/pbrmaterial.h
    physics/avatarmovement.cpp
    physics/avatarmovement.h
    physics/environment.cpp
    physics/environment.h
    physics/physicshelper.cpp
    physics/physicshelper.h
    physics/physicsproperties.h
    properties/property.h
    properties/valueshapes.h
    scenegraph/cameralens.cpp
    scenegraph/cameralens.h
    scenegraph/cameranode.cpp
    scenegraph/cameranode.h
    scenegraph/decalnode.cpp
    scenegraph/decalnode.h
    scenegraph/lightnode.cpp
    scenegraph/lightnode.h
    scenegraph/looks.cpp
    scenegraph/looks.h
    scenegraph/meshnode.cpp
    scenegraph/meshnode.h
    scenegraph/nodedirtyset.h
    scenegraph/nodegraph.cpp
    scenegraph/nodegraph.h
    scenegraph/particlesystemnode.cpp
    scenegraph/particlesystemnode.h
    scenegraph/scalelock.h
    scenegraph/scene.cpp
    scenegraph/scene.h
    scenegraph/scenenode.cpp
    scenegraph/scenenode.h
    scenegraph/scenepicking.cpp
    scenegraph/scenepicking.h
    scenegraph/shadowmap.cpp
    scenegraph/shadowmap.h
    scenegraph/simulationclock.h
    scenegraph/skybake.cpp
    scenegraph/skybake.h
    scenegraph/socket.cpp
    scenegraph/socket.h
    scenegraph/worldmodes.cpp
    scenegraph/worldmodes.h
    srgbcolor.h
)
target_compile_features(IrisDocument PUBLIC cxx_std_17)
set_target_properties(IrisDocument PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
target_compile_definitions(IrisDocument PRIVATE IRIS_DOCUMENT_BUILD)
target_compile_definitions(IrisDocument PUBLIC BT_THREADSAFE=1)   # Bullet's multi-threaded build (irisgl/CMakeLists.txt)
# includes are path-qualified from irisgl/: "document/...", "core/...", "irisglfwd.h"; Bullet's headers by their own names
target_include_directories(IrisDocument PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/.." "${PROJECT_SOURCE_DIR}/thirdparty/bullet3/src")
# THE ONE NODE-ID SPACE (ENGINE-UPGRADE-U3 stage 3, F6): the document takes its node ids from the engine's Ogre-free Ids.h
# (reserveNodeId); scenegraph/cameralens.cpp and worldmodes.cpp read PostFx.h, GiModes.h, GiParams.h. PRIVATE and
# include-only: no engine library is linked, and no document HEADER includes an engine header.
target_include_directories(IrisDocument PRIVATE "${PROJECT_SOURCE_DIR}/engine/include")
target_link_libraries(IrisDocument PUBLIC IrisCore BulletDynamics BulletCollision LinearMath Bullet3Common)
# THE PHYSICS TUs NEVER CONTRACT (BULLET-BATCHING-1; _jah_bake_fp is set in irisgl/CMakeLists.txt before this directory is
# added): they inline Bullet's header math, which Bullet's own targets compile with -ffp-contract=off.
set_source_files_properties(physics/avatarmovement.cpp physics/environment.cpp physics/physicshelper.cpp
    PROPERTIES COMPILE_OPTIONS "${_jah_bake_fp}")
if (NOT MSVC)   # A6: undefined behaviour, not style (IrisGL's rule)
    target_compile_options(IrisDocument PRIVATE -Werror=return-type -Werror=delete-incomplete)
endif()
include(GNUInstallDirs)
install(TARGETS IrisDocument
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
