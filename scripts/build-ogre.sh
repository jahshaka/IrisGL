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
    # Deployment floor. Unset, CMake stamps the dylibs with the SDK's own
    # version (minos 26.0 on an Xcode 17 box) and the redistributable bundle
    # refuses to launch on anything older. 13.0 is Qt's own floor (the Qt
    # 6.11.2 kit is built minos 13.0; MoltenVK ships 12.0), so it costs
    # nothing and is the lowest we can honestly claim.
    PLATFORM_FLAGS="$PLATFORM_FLAGS -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}"
else
    JOBS="${JOBS:-$(nproc)}"
    # $ORIGIN so the INSTALLED Ogre libraries find their siblings beside
    # themselves, with no LD_LIBRARY_PATH and no help from the consumer.
    #
    # WHY THIS IS NOT OPTIONAL (found 2026-09-03): DT_RUNPATH is NOT transitive.
    # A consumer's RUNPATH resolves the libraries the CONSUMER names, not the
    # ones those libraries name in turn. libOgreNextHlmsPbs has always NEEDED
    # libOgreNextMain and could never resolve it on its own — it only ever
    # worked because Jahshaka links Main directly too, so it was already in the
    # loaded set by SONAME. That accident broke the moment HlmsPbs gained a
    # NEEDED on libOgreNextPlanarReflections (the component pin below): every
    # already-built binary that did not also link the new library failed at
    # startup with "cannot open shared object file". $ORIGIN removes the whole
    # class of problem instead of the one instance of it.
    #
    # $ORIGIN/.. is for lib/OGRE-Next/*.so (RenderSystem_Vulkan and friends),
    # which are dlopen'd from a subdirectory and need to reach lib/ above them.
    # Single quotes: $ORIGIN must reach the linker literally, not be expanded
    # by this shell. CMake applies it at INSTALL time, so the build tree is
    # unaffected.
    PLATFORM_FLAGS="-DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=ON"
    PLATFORM_FLAGS="$PLATFORM_FLAGS -DCMAKE_INSTALL_RPATH=\$ORIGIN;\$ORIGIN/.."
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
# The component set is pinned EXPLICITLY (every OGRE_BUILD_COMPONENT_* that the
# pin defines) so the install is reproducible on every box. It used to name only
# three, and the rest rode upstream defaults -- two of which are machine
# dependent: OVERLAY is a cmake_dependent_option on FREETYPE_FOUND and DEAR_IMGUI
# on DearImgui_FOUND, so a box without libfreetype-dev silently produced a
# different install. OVERLAY is OFF by decision: we link neither the library nor
# any Overlay header, and turning it off removes the hidden freetype dependency.
# PLANAR_REFLECTIONS is ours (mirrors / glossy floors); note it is #ifdef-ed
# INSIDE OgreHlmsPbs.h, so it changes HlmsPbs's member layout -- consumers MUST
# recompile after a flip. generateAbiCookie() does not hash component defines,
# so nothing catches a stale consumer at runtime; we rely on CMake's -MD depfiles
# tracking the installed OgreBuildSettings.h (do not make those includes SYSTEM).
cmake -S "$SRC" -B "$SRC/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  $PLATFORM_FLAGS -DOGRE_BUILD_RENDERSYSTEM_VULKAN=ON \
  -DOGRE_VULKAN_WINDOW_NULL=ON \
  -DOGRE_BUILD_COMPONENT_HLMS_PBS=ON -DOGRE_BUILD_COMPONENT_HLMS_UNLIT=ON \
  -DOGRE_BUILD_COMPONENT_SCENE_FORMAT=ON \
  -DOGRE_BUILD_COMPONENT_PLANAR_REFLECTIONS=ON \
  -DOGRE_BUILD_COMPONENT_ATMOSPHERE=ON -DOGRE_BUILD_COMPONENT_MESHLODGENERATOR=ON \
  -DOGRE_BUILD_COMPONENT_PROPERTY=ON -DOGRE_BUILD_COMPONENT_OVERLAY=OFF \
  -DOGRE_BUILD_COMPONENT_PAGING=OFF -DOGRE_BUILD_COMPONENT_VOLUME=OFF \
  -DOGRE_BUILD_COMPONENT_DEAR_IMGUI=OFF \
  -DOGRE_BUILD_SAMPLES2=OFF -DOGRE_BUILD_TESTS=OFF -DOGRE_BUILD_TOOLS=ON

# libshaderc gotcha: a missing dep silently drops the Vulkan RenderSystem while
# configure still exits 0. Fail loudly instead.
grep -q "RenderSystem_Vulkan" "$SRC/build/build.ninja" || {
    echo "Vulkan RenderSystem was NOT configured — check dependencies (libshaderc-dev?)." >&2
    exit 1
}

# Same class of silent-drop guard for the PlanarReflections component: it is
# OFF by default upstream, and without it HlmsPbs compiles a different layout
# and OgrePlanarReflections.cpp will not build.
grep -q "OgreNextPlanarReflections" "$SRC/build/build.ninja" || {
    echo "PlanarReflections component was NOT configured — the explicit component pin above did not take." >&2
    exit 1
}

cmake --build "$SRC/build" -j"$JOBS"
# `cmake --install` overwrites, it never REMOVES: a component switched off (or
# a rename upstream) leaves its old .so behind for ever, and an orphan that no
# longer has its dependencies installed beside it fails the gate below with a
# problem nobody has. The build has succeeded by this point, so the shared
# prefix is safe to prune — this script owns it.
rm -f "$PREFIX"/lib/libOgreNext*.so* "$PREFIX"/lib/OGRE-Next/*.so*
cmake --install "$SRC/build" > /dev/null

# Self-containment gate. Every installed Ogre library must resolve its OWN
# dependencies in a clean environment (see the $ORIGIN note above): if this
# fails, binaries built against the install start failing at load time in ways
# that look like anything but a linker problem.
if [ "$(uname -s)" != "Darwin" ] && command -v ldd > /dev/null 2>&1; then
    missing=0
    for so in "$PREFIX"/lib/libOgreNext*.so.* "$PREFIX"/lib/OGRE-Next/*.so.*; do
        [ -f "$so" ] || continue
        n=$(env -u LD_LIBRARY_PATH ldd "$so" 2>/dev/null | grep -c "not found" || true)
        [ "$n" = "0" ] || { echo "UNRESOLVED deps in $(basename "$so"):" >&2
                            env -u LD_LIBRARY_PATH ldd "$so" | grep "not found" >&2
                            missing=$((missing + n)); }
    done
    [ "$missing" = "0" ] || {
        echo "Installed Ogre libraries do not resolve without LD_LIBRARY_PATH — the \$ORIGIN" >&2
        echo "install RPATH did not take. Do NOT paper over this with LD_LIBRARY_PATH." >&2
        exit 1
    }
fi

echo "Ogre-Next installed to $PREFIX"
