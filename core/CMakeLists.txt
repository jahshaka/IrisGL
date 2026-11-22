# IrisCore (NOQT-CORE-1, V2 plan 0.12): THE Qt-FREE BOTTOM of everything below the editor's UI - the standard library only,
# a static library with position-independent code, which the document library (IrisGL, PUBLIC) links. Its sources join
# it as they lose Qt (NOQT-CORE-1 stages 1-4); trianglebvh.cpp joins with trimesh.cpp in stage 2 (its trimesh.h includes
# <QList> today). It declares C++17 itself (no Qt usage requirement gives it one) and runs no Qt code generator.
# tests/hygiene/no_qt_below.py holds it: no Qt include directory, define or generator on its compile lines, no Qt symbol
# in its archive, no include outside core/ and the standard library.
add_library(IrisCore STATIC
    files.cpp
    files.h
    geometry/plane.cpp
    geometry/plane.h
    guid.cpp
    guid.h
    math/scalar.h
    signal.h
    text.cpp
    text.h
)
target_compile_features(IrisCore PUBLIC cxx_std_17)
set_target_properties(IrisCore PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# includes are path-qualified from irisgl/: "core/..."
target_include_directories(IrisCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
