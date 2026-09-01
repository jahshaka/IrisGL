#!/usr/bin/env bash
# Builds Ogre-Next (IrisGL's engine backend) from the pinned submodule, with
# Jahshaka's patches applied. Run ONCE per machine (and again after a submodule
# bump); the normal IrisGL/Jahshaka build links the installed result and never
# recompiles Ogre.
#
#   ./scripts/build-ogre.sh              # build + install with defaults
#   OGRE_PREFIX=/opt/ogre ./scripts/build-ogre.sh
#
# Dependencies (Ubuntu — install ALL before first configure; CMake caches
# not-found results, see docs/OGRE_BUILD.md for the story and other platforms):
#   sudo apt-get install -y libxrandr-dev libxaw7-dev rapidjson-dev libzzip-dev \
#        libsdl2-dev glslang-tools spirv-tools vulkan-tools libshaderc-dev \
#        libfreeimage-dev libxcb-randr0-dev libx11-xcb-dev libxcb1-dev \
#        libxcb-keysyms1-dev libx11-dev libxt-dev libgl1-mesa-dev \
#        libglu1-mesa-dev libfreetype-dev zlib1g-dev libvulkan-dev
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${OGRE_SOURCE:-$REPO_ROOT/thirdparty/ogre-next}"
PREFIX="${OGRE_PREFIX:-$HOME/Developer/engines/ogre-next-install}"
PATCHES="$REPO_ROOT/thirdparty/ogre-patches"
if [ "$(uname -s)" = "Darwin" ]; then
    JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
    # macOS: Vulkan via MoltenVK (LunarG SDK — source its setup-env.sh first).
    # No X11 exists: OGRE_CONFIG_UNIX_NO_X11 drops XCB windowing (which would
    # otherwise default ON — CMake counts APPLE as UNIX) and forces the null
    # window on. GL3Plus is GLX-based; headless engine needs Vulkan only.
    # LIBS_AS_FRAMEWORKS defaults ON for APPLE but its header-copy steps emit
    # Xcode-generator $(VARS) that break Ninja — plain dylibs, like Linux .so.
    # No FreeImage on macOS (no package manager): bundled STBI codec instead.
    PLATFORM_FLAGS="-DOGRE_CONFIG_UNIX_NO_X11=TRUE -DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=OFF -DOGRE_BUILD_LIBS_AS_FRAMEWORKS=OFF -DOGRE_CONFIG_ENABLE_FREEIMAGE=OFF -DOGRE_CONFIG_ENABLE_STBI=ON"
    # rapidjson (header-only; apt's rapidjson-dev on Linux): FindRapidjson
    # honours Rapidjson_HOME. Default to <engines>/deps/rapidjson beside the
    # install prefix; RAPIDJSON_HOME overrides.
    DEFAULT_RJ="$(dirname "$PREFIX")/deps/rapidjson"
    if [ -n "${RAPIDJSON_HOME:-}" ] || [ -d "$DEFAULT_RJ" ]; then
        export Rapidjson_HOME="${RAPIDJSON_HOME:-$DEFAULT_RJ}"
    fi
    # Bake rpaths so the installed dylibs resolve @rpath/libvulkan (LunarG SDK)
    # and each other WITHOUT DYLD_LIBRARY_PATH — macOS SIP strips DYLD_* across
    # /bin/bash and friends, which silently broke plugin dlopen under ctest.
    [ -n "${VULKAN_SDK:-}" ] || { echo "VULKAN_SDK not set — source the LunarG setup-env.sh first" >&2; exit 1; }
    PLATFORM_FLAGS="$PLATFORM_FLAGS -DCMAKE_MACOSX_RPATH=ON -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH=$VULKAN_SDK/lib;@loader_path"
else
    JOBS="${JOBS:-$(nproc)}"
    PLATFORM_FLAGS="-DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=ON"
fi

[ -f "$SRC/CMakeLists.txt" ] || {
    echo "Ogre source not found at $SRC — run: git submodule update --init thirdparty/ogre-next" >&2
    exit 1
}

# --- Apply Jahshaka's patches (idempotent: skip any already applied) ---------
# Each patch documents itself; updating Ogre = bump the submodule pin, re-run
# this script, and fix whichever patch no longer applies (that failure is the
# signal upstream touched our files — review their change, adapt the patch).
for p in "$PATCHES"/*.patch; do
    if git -C "$SRC" apply --reverse --check "$p" 2>/dev/null; then
        echo "patch already applied: $(basename "$p")"
    elif git -C "$SRC" apply --check "$p" 2>/dev/null; then
        git -C "$SRC" apply "$p"
        echo "patch applied: $(basename "$p")"
    else
        echo "PATCH DOES NOT APPLY: $(basename "$p")" >&2
        echo "Upstream changed the patched file. Diff their change and adapt the patch." >&2
        exit 1
    fi
done

# --- Configure + build + install --------------------------------------------
cmake -S "$SRC" -B "$SRC/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  $PLATFORM_FLAGS -DOGRE_BUILD_RENDERSYSTEM_VULKAN=ON \
  -DOGRE_VULKAN_WINDOW_NULL=ON \
  -DOGRE_BUILD_COMPONENT_HLMS_PBS=ON -DOGRE_BUILD_COMPONENT_HLMS_UNLIT=ON \
  -DOGRE_BUILD_COMPONENT_SCENE_FORMAT=ON \
  -DOGRE_BUILD_SAMPLES2=OFF -DOGRE_BUILD_TESTS=OFF -DOGRE_BUILD_TOOLS=ON

# libshaderc gotcha: a missing dep silently drops the Vulkan RenderSystem while
# configure still exits 0. Fail loudly instead.
grep -q "RenderSystem_Vulkan" "$SRC/build/build.ninja" || {
    echo "Vulkan RenderSystem was NOT configured — check dependencies (libshaderc-dev?)." >&2
    exit 1
}

cmake --build "$SRC/build" -j"$JOBS"
cmake --install "$SRC/build" > /dev/null
echo "Ogre-Next installed to $PREFIX"
