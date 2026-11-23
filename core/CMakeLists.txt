# IrisCore (NOQT-CORE-1, V2 plan 0.12): THE Qt-FREE BOTTOM of everything below the editor's UI - the standard library only,
# ONE SHARED library (the lead's ruling 2026-11-22: a static archive pulled into two images keeps two copies of a stateful
# symbol - the temp serial, the guid engine, the log sink - wherever the platform does not interpose, Windows always: one
# shared object makes ONE copy by construction), which the document library (IrisGL, PUBLIC) and the engine
# (JahshakaEngine, PRIVATE: its caches' file I/O) link. Its sources joined it as they lost Qt (NOQT-CORE-1 stages 1-4;
# the log and the picking geometry - TriMesh, TriangleBvh, AABB - in stage 2). It declares C++17 itself (no Qt usage
# requirement gives it one) and runs no Qt code generator.
# tests/hygiene/no_qt_below.py holds it: no Qt include directory, define or generator on its compile lines, no Qt symbol
# in it, no include outside core/ and the standard library, and no other image defines its stateful symbols.
add_library(IrisCore SHARED
    export.h
    files.cpp
    files.h
    geometry/aabb.cpp
    geometry/aabb.h
    geometry/plane.cpp
    geometry/plane.h
    geometry/trianglebvh.cpp
    geometry/trianglebvh.h
    geometry/trimesh.cpp
    geometry/trimesh.h
    guid.cpp
    guid.h
    json.cpp
    json.h
    log.cpp
    log.h
    math/scalar.h
    signal.h
    text.cpp
    text.h
    value.cpp
    value.h
)
target_compile_features(IrisCore PUBLIC cxx_std_17)
# THE PICKING INDEX'S BUILD AND WALK (SPEED-CPU): the bake reader builds a TriangleBvh per picking mesh on every open,
# and at -O0 a 233k-triangle scan's median-split build is the bake's own inner-loop story again. It computes only
# min/max boxes and box tests (no triangle arithmetic - the hit test stays in trimesh.cpp), so the optimisation level
# cannot change an answer; the same never-contract rule anyway (_jah_bake_fp: irisgl/CMakeLists.txt, set before this
# directory is added).
set_source_files_properties(geometry/trianglebvh.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>;${_jah_bake_fp}")
# Hidden by default: only what core/export.h's IRIS_CORE_API marks is exported (the iris:: API). Beside IrisGL in bin/.
set_target_properties(IrisCore PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
target_compile_definitions(IrisCore PRIVATE IRIS_CORE_BUILD)
# includes are path-qualified from irisgl/: "core/..."
target_include_directories(IrisCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
include(GNUInstallDirs)
install(TARGETS IrisCore
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
