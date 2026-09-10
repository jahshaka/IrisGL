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
# Two appliers, chosen per SOURCE TREE, never per machine:
#   * `git apply`  — when SRC is the toplevel of its own git work tree (the
#     normal submodule checkout).  git's --check / --reverse --check are exact
#     and never touch the tree.
#   * GNU `patch`  — when SRC is not a git repository (a release tarball, a
#     vendored copy without .git) OR when it sits INSIDE some other repository
#     without being its toplevel: `git apply` run from a subdirectory of a
#     repository applies only the hunks whose paths fall under that
#     subdirectory and silently drops the rest, so it cannot be trusted there.
#     `patch -p1 -N` after a `--dry-run` in each direction gives the same
#     idempotent, loud-on-failure contract (a reversed dry-run that succeeds
#     means "already applied"; a forward dry-run that fails names the patch).
#     The flag pairing is load-bearing (measured, GNU patch 2.8): the REVERSE
#     probe needs --force (--batch "assumes reversed if it looks reversed" and
#     turns -R on an unapplied tree into a successful forward dry-run); the
#     FORWARD probe and the apply need --batch (--force makes -N re-apply an
#     already-applied pure-addition patch, duplicating its lines).  --fuzz=0
#     on every call: git apply is fuzz-0, and GNU patch's default fuzz of 2
#     applied 0004 onto a tree whose target line had been rewritten.
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

# --- Pick the applier for THIS tree ------------------------------------------
set(_use_git FALSE)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --show-toplevel
                    WORKING_DIRECTORY "${SRC}"
                    RESULT_VARIABLE _in_repo
                    OUTPUT_VARIABLE _toplevel
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(_in_repo EQUAL 0)
        # Compare as real paths: git resolves symlinks, ABSOLUTE does not.
        get_filename_component(_toplevel "${_toplevel}" REALPATH)
        get_filename_component(_src_real "${SRC}" REALPATH)
        if(_toplevel STREQUAL _src_real)
            set(_use_git TRUE)
        endif()
    endif()
endif()

if(NOT _use_git)
    find_program(PATCH_EXECUTABLE NAMES patch)
    if(NOT PATCH_EXECUTABLE)
        message(FATAL_ERROR
            "ApplyVendorPatches: ${SRC} is not a git checkout of its own, and no "
            "`patch` program was found, but ${_name} needs ${PATCHES} applied.\n"
            "  Install GNU patch (or git and a git checkout of ${_name}), or apply "
            "the patches by hand.")
    endif()
    message(STATUS "vendor patches (${_name}): not a git toplevel, using ${PATCH_EXECUTABLE}")
endif()

# --- Apply, in order, idempotently -------------------------------------------
foreach(_p IN LISTS _patches)
    get_filename_component(_base "${_p}" NAME)

    if(_use_git)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_p}"
                        WORKING_DIRECTORY "${SRC}"
                        RESULT_VARIABLE _reversible
                        OUTPUT_QUIET ERROR_QUIET)
    else()
        execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 --fuzz=0 -R --dry-run --force --silent
                                -i "${_p}"
                        WORKING_DIRECTORY "${SRC}"
                        RESULT_VARIABLE _reversible
                        OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(_reversible EQUAL 0)
        message(STATUS "vendor patch already applied: ${_base}")
        continue()
    endif()

    if(_use_git)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${_p}"
                        WORKING_DIRECTORY "${SRC}"
                        RESULT_VARIABLE _appliable
                        OUTPUT_QUIET ERROR_QUIET)
    else()
        execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 --fuzz=0 -N --dry-run --batch --silent
                                -i "${_p}"
                        WORKING_DIRECTORY "${SRC}"
                        RESULT_VARIABLE _appliable
                        OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(NOT _appliable EQUAL 0)
        message(FATAL_ERROR
            "PATCH DOES NOT APPLY: ${_base}\n"
            "  tree:  ${SRC}\n"
            "  Upstream changed the patched file, or the tree is dirty.\n"
            "  Diff their change and adapt the patch — never edit the vendored\n"
            "  source in place.")
    endif()

    if(_use_git)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${_p}"
                        WORKING_DIRECTORY "${SRC}"
                        RESULT_VARIABLE _applied
                        ERROR_VARIABLE _err)
    else()
        execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 --fuzz=0 -N --batch --silent -i "${_p}"
                        WORKING_DIRECTORY "${SRC}"
                        RESULT_VARIABLE _applied
                        ERROR_VARIABLE _err)
    endif()
    if(NOT _applied EQUAL 0)
        message(FATAL_ERROR "failed to apply ${_base}: ${_err}")
    endif()
    message(STATUS "vendor patch applied: ${_base}")
endforeach()
