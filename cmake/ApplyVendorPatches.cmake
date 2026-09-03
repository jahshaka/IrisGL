# ApplyVendorPatches.cmake — apply Jahshaka's patches to a vendored submodule.
#
# Vendored trees are NEVER edited in place and NEVER committed to (the Ogre-Next
# law, extended here to assimp): every local change is a self-documenting patch
# file, and the submodule's working tree ends up in the applied-not-committed
# state.  This script is that mechanism.
#
# Run it directly (any platform, no shell needed):
#
#   cmake -DSRC=<vendored source dir> -DPATCHES=<patch dir> \
#         -P irisgl/cmake/ApplyVendorPatches.cmake
#
# or let the build do it — irisgl/CMakeLists.txt invokes it at configure time,
# before add_subdirectory() of the vendored tree, so a fresh clone cannot
# silently build unpatched sources.  (Ogre-Next differs only in its hook:
# irisgl/scripts/build-ogre.sh applies irisgl/thirdparty/ogre-patches/, because
# Ogre is an out-of-tree prerequisite build we do not drive from this project's
# configure step.)
#
# Idempotent: a patch that reverse-applies cleanly is already in, and is
# skipped.  A patch that neither applies nor reverse-applies is a hard error —
# that failure IS the signal that upstream touched our lines: read their change,
# adapt (or drop) the patch, do not paper over it.
#
# Empty patch directories are fine and expected: the mechanism stands whether or
# not we currently carry a patch for a given tree.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SRC OR NOT DEFINED PATCHES)
    message(FATAL_ERROR "ApplyVendorPatches: SRC and PATCHES must both be set")
endif()

get_filename_component(SRC "${SRC}" ABSOLUTE)
get_filename_component(PATCHES "${PATCHES}" ABSOLUTE)
get_filename_component(_name "${SRC}" NAME)

if(NOT EXISTS "${SRC}/CMakeLists.txt")
    message(FATAL_ERROR
        "ApplyVendorPatches: no source at ${SRC}\n"
        "  run: git submodule update --init ${_name}")
endif()

file(GLOB _patches "${PATCHES}/*.patch")
list(SORT _patches)
if(NOT _patches)
    message(STATUS "vendor patches (${_name}): none")
    return()
endif()

find_package(Git QUIET)
if(NOT GIT_FOUND)
    message(FATAL_ERROR
        "ApplyVendorPatches: git not found, but ${_name} needs "
        "${PATCHES} applied. Install git, or apply the patches by hand.")
endif()

foreach(_p IN LISTS _patches)
    get_filename_component(_base "${_p}" NAME)

    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_p}"
                    WORKING_DIRECTORY "${SRC}"
                    RESULT_VARIABLE _reversible
                    OUTPUT_QUIET ERROR_QUIET)
    if(_reversible EQUAL 0)
        message(STATUS "vendor patch already applied: ${_base}")
        continue()
    endif()

    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${_p}"
                    WORKING_DIRECTORY "${SRC}"
                    RESULT_VARIABLE _appliable
                    OUTPUT_QUIET ERROR_QUIET)
    if(NOT _appliable EQUAL 0)
        message(FATAL_ERROR
            "PATCH DOES NOT APPLY: ${_base}\n"
            "  tree:  ${SRC}\n"
            "  Upstream changed the patched file, or the tree is dirty.\n"
            "  Diff their change and adapt the patch — never edit the vendored\n"
            "  source in place.")
    endif()

    execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${_p}"
                    WORKING_DIRECTORY "${SRC}"
                    RESULT_VARIABLE _applied
                    ERROR_VARIABLE _err)
    if(NOT _applied EQUAL 0)
        message(FATAL_ERROR "failed to apply ${_base}: ${_err}")
    endif()
    message(STATUS "vendor patch applied: ${_base}")
endforeach()
