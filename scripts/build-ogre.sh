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
JOBS="${JOBS:-$(nproc)}"

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
  -DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=ON -DOGRE_BUILD_RENDERSYSTEM_VULKAN=ON \
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
