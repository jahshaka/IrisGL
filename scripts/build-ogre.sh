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
# PER-TREE INSTALL (owner decree 2026-09-06: "we should not have a shared
# engine"). The install lives beside its source inside THIS tree, so every
# checkout and every worktree is self-contained: no cross-lane mutation of a
# shared artifact, no stale-install ambiguity, and a worktree that patches its
# submodule gets exactly the engine it patched. The engine builds in ~2-3 min
# on this class of machine — the old shared install's rationale (expensive
# builds) is dead. OGRE_PREFIX still overrides for special layouts (the macOS
# workspace keeps its own convention until its docs migrate).
PREFIX="${OGRE_PREFIX:-$REPO_ROOT/thirdparty/ogre-next-install}"
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
    # honours Rapidjson_HOME. Look beside the (now per-tree) install prefix
    # first, then the legacy shared-workspace location the Mac vendored it to
    # before the per-tree move; RAPIDJSON_HOME overrides both.
    DEFAULT_RJ="$(dirname "$PREFIX")/deps/rapidjson"
    LEGACY_RJ="$HOME/Desktop/JahshakaDev/jahshakaclaude/engines/deps/rapidjson"
    [ -d "$DEFAULT_RJ" ] || { [ -d "$LEGACY_RJ" ] && DEFAULT_RJ="$LEGACY_RJ"; }
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
# different install. OVERLAY is ON by decision (STATS_OVERLAY_SPEC.md D1,
# 2026-09-05): the stats overlay + engine-drawn loading cover use it. That makes
# freetype a HARD dependency of this build -- the guard below fails loudly on a
# box without it, because cmake_dependent_option would otherwise silently force
# the component OFF and produce a different install (the exact machine-dependence
# this explicit pin exists to prevent). Runtime: libOgreNextOverlay NEEDS
# libfreetype.so.6 (DebugFont is type=truetype, rasterised at load).
# PLANAR_REFLECTIONS is ours (mirrors / glossy floors); note it is #ifdef-ed
# INSIDE OgreHlmsPbs.h, so it changes HlmsPbs's member layout -- consumers MUST
# recompile after a flip. generateAbiCookie() does not hash component defines,
# so nothing catches a stale consumer at runtime; we rely on CMake's -MD depfiles
# tracking the installed OgreBuildSettings.h (do not make those includes SYSTEM).
#
# OGRE_SHADER_COMPILATION_THREADING_MODE=2 (SPECS/THREADING_ADOPTION_SPEC.md P1)
# turns Ogre's MULTITHREADED SHADER/PSO COMPILATION on. It is not a performance
# knob with a default we happen to disagree with -- upstream's mode 1 (the
# default) enables the threaded path only via compiler TLS, and only when
# OGRE_STATIC is true (ogre-next/CMakeLists.txt:446-449). We build SHARED, so
# mode 1 means "off", for ever, on every platform: VulkanRenderSystem::
# supportsMultithreadedShaderCompilation() returns false, the parallel Hlms
# compile queue never starts, and HlmsDiskCache::applyTo runs single-threaded.
# Mode 2 drops the backwards-compatible tid-less overloads instead of using TLS
# -- API we use nowhere (we subclass Hlms nowhere, and our one HlmsListener
# overrides only the two tid-less hooks), which is what makes the flip free.
#
# PLATFORM-NEUTRAL ON PURPOSE: it belongs in this shared arg list, not in
# PLATFORM_FLAGS. macOS runs the same VulkanRenderSystem through MoltenVK and
# is in exactly the same shared-build situation, and so is MSVC.
#
# MEASURED RESIDUAL, recorded here because this flag is what creates it.
# ThreadSanitizer over an INSTRUMENTED Ogre (built once for this purpose, 2026-09-06)
# reports exactly one race in the parallel compile path: two
# ParallelHlmsCompileQueue threads inside Hlms::compileShaderCode -> gp->load()
# -> ResourceManager::_notifyResourceLoaded, both doing `mMemoryUsage += size`.
# `mMemoryUsage` is an Ogre::AtomicScalar whose operator+= is guarded by
# OGRE_AUTO_MUTEX -- a NO-OP at OGRE_THREAD_SUPPORT 0, which is what upstream's
# default (and our install) uses. So it is a genuine unsynchronised counter, by
# design: compileShaderCode holds msGlobalMutex only around createProgram, and
# the compile itself runs outside it, which is the entire point.
# CONSEQUENCE, bounded: mMemoryUsage feeds only ResourceManager::checkUsage(),
# whose branch is gated on mMemoryBudget -- SIZE_MAX by default, and Jahshaka
# never calls setMemoryBudget. Nothing reads the counter for a decision, so the
# cost is bookkeeping drift, not a crash or a leak. If a memory budget is ever
# set, this becomes real and the fix is upstream's (a std::atomic counter, or
# OGRE_THREAD_SUPPORT).
#
# THE FLIP CHANGES THE ABI. generateAbiCookie() hashes both threading macros
# (OgreAbiUtils.h:72-81), so a Studio compiled against the old
# OgreBuildSettings.h and linked against a mode-2 engine ABORTS at Root
# construction -- loud, not silent. On Linux and macOS the -MD depfiles track
# the installed header (Studio's cmake/IncludeOgre.cmake adds Ogre's include
# dirs deliberately NOT as SYSTEM), so `cmake --build` after this script picks
# the rebuild up by itself. EVERY TREE MUST RE-RUN THIS SCRIPT after pulling the
# commit that added the flag.
#
# OGRE_CONFIG_ENABLE_FINE_LIGHT_MASK_GRANULARITY=ON (LIGHT MASKS / lighting
# channels: "this light only affects these objects"). Upstream's option
# (ogre-next/CMakeLists.txt:573) defaults FALSE, which sets
# OGRE_NO_FINE_LIGHT_MASK_GRANULARITY=1 in OgreBuildSettings.h and COMPILES THE
# WHOLE FEATURE OUT: Light::setLightMask and MovableObject::setLightMask still
# exist and still store, but nothing ever reads them -- HlmsPbs never writes the
# mask into the light buffers (OgreHlmsPbs.cpp:2566/2644/2723/2832) nor the
# object's mask into the per-draw const buffer (:3685), ForwardPlusBase never
# writes it into the global light list (OgreForwardPlusBase.cpp:207), and the
# hlms_fine_light_mask / hlms_forwardplus_fine_light_mask shader properties are
# never set, so the generated shaders contain no test at all. The API is a
# silent no-op. ON makes it real, for BOTH light paths: forward (directional +
# shadow-casting + area approx + area LTC, HlmsPbs::setFineLightMaskGranularity)
# and Forward+ clustered (ForwardPlusBase::setFineLightMaskGranularity) -- both
# default to true once compiled in.
#
# COST, measured at the pin and bounded by construction: the light-buffer layout
# does NOT change (the mask rides the unused .w of each light's view-space
# position, and the ++ that skips it is compiled either way, see the #if blocks
# above), so no buffer grows and no upload gets bigger. The per-draw const
# buffer word at OgreHlmsPbs.cpp:3685 is likewise already reserved. What the
# flag adds is one uint AND-test per light per pixel inside the lighting loops,
# and only in the shader VARIANTS that get the property -- which is all of them
# while granularity is on. Upstream's own option text says the impact "may vary
# (may be slower, may be faster if you filter a lot of lights)".
#
# DEFAULTS ARE ALL-ON, so enabling changes no pixels: MovableObject's light mask
# is born 0xFFFFFFFF (OgreMovableObject.cpp:60 msDefaultLightMask, actually
# consulted at OgreObjectDataArrayMemoryManager.cpp:138 -- unlike query flags,
# whose msDefaultQueryFlags is dead code at this pin) and so is every Light's.
# `0xFFFFFFFF & 0xFFFFFFFF != 0` for every pair, so every light keeps affecting
# every object until somebody masks something. There are NO reserved bits in the
# light mask -- all 32 are ours.
#
# THIS FLIP IS NOT IN THE ABI COOKIE. generateAbiCookie() (OgreAbiUtils.h:58-98)
# hashes the two threading macros but NOT OGRE_NO_FINE_LIGHT_MASK_GRANULARITY --
# exactly like the component defines noted above. And the macro DOES change
# member layout: it adds `bool mFineLightMaskGranularity` to both HlmsPbs
# (OgreHlmsPbs.h:228) and ForwardPlusBase (OgreForwardPlusBase.h:154). So a
# consumer compiled against the old OgreBuildSettings.h and linked against a
# flag-ON engine is SILENTLY mismatched, not loudly aborted. What saves us is
# the same thing that saves the component pins: Studio's cmake/IncludeOgre.cmake
# adds Ogre's include dirs deliberately NOT as SYSTEM, so the -MD depfiles track
# the installed OgreBuildSettings.h and `cmake --build` rebuilds every Ogre-
# including TU by itself after this script re-runs. EVERY TREE MUST RE-RUN THIS
# SCRIPT after pulling the commit that added this flag -- and unlike the
# threading flag, forgetting is not self-announcing.
# Ogre's own 2.0 samples (Sample_PbsMaterials, Sample_LocalCubemaps, ...) are
# OFF by default, FOREVER: nothing in Studio links them, and they cost ~40 s and
# a few hundred MB in EVERY tree and EVERY worktree (the engine build is
# per-tree, owner decree 2026-09-06). Opt in with OGRE_SAMPLES=1 in the ONE tree
# doing side-by-side comparison work -- see SPECS/OGRE_SAMPLES_TAB_SPEC.md §7.
#   OGRE_SAMPLES=1 ./irisgl/scripts/build-ogre.sh
# Binaries land in $SRC/build/bin beside the generated resources2.cfg/plugins.cfg,
# whose paths point at the SOURCE tree -- so they must be run with that directory
# as the cwd, and they are NOT installed: OGRE_INSTALL_SAMPLES defaults TRUE
# upstream and would copy binaries + media into the prefix this script prunes.
# Missing SDL2 does not fail the build (Samples/2.0/CMakeLists.txt:15-18 prints
# "Could not find dependency for samples: SDL2" and skips), which is what makes
# the flag safe on the no-Homebrew macOS toolchain.
SAMPLE_FLAGS="-DOGRE_BUILD_SAMPLES2=OFF"
if [ "${OGRE_SAMPLES:-0}" = "1" ]; then
    echo "OGRE_SAMPLES=1: building Ogre's 2.0 samples into $SRC/build/bin (not installed)."
    SAMPLE_FLAGS="-DOGRE_BUILD_SAMPLES2=ON -DOGRE_INSTALL_SAMPLES=OFF -DOGRE_INSTALL_SAMPLES_SOURCE=OFF"
fi

# Compiler cache when installed (2026-09-11): each tree builds its own Ogre, so a shared
# ccache turns every tree's build after the first into cache hits.
CCACHE_FLAGS=""
if command -v ccache >/dev/null 2>&1 && [ "${JAH_NO_CCACHE:-0}" != "1" ]; then
    CCACHE_FLAGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
fi

cmake -S "$SRC" -B "$SRC/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo $CCACHE_FLAGS \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DOGRE_SHADER_COMPILATION_THREADING_MODE=2 \
  -DOGRE_CONFIG_ENABLE_FINE_LIGHT_MASK_GRANULARITY=ON \
  $PLATFORM_FLAGS -DOGRE_BUILD_RENDERSYSTEM_VULKAN=ON \
  -DOGRE_VULKAN_WINDOW_NULL=ON \
  -DOGRE_BUILD_COMPONENT_HLMS_PBS=ON -DOGRE_BUILD_COMPONENT_HLMS_UNLIT=ON \
  -DOGRE_BUILD_COMPONENT_SCENE_FORMAT=ON \
  -DOGRE_BUILD_COMPONENT_PLANAR_REFLECTIONS=ON \
  -DOGRE_BUILD_COMPONENT_ATMOSPHERE=ON -DOGRE_BUILD_COMPONENT_MESHLODGENERATOR=ON \
  -DOGRE_BUILD_COMPONENT_PROPERTY=ON -DOGRE_BUILD_COMPONENT_OVERLAY=ON \
  -DOGRE_BUILD_COMPONENT_PAGING=OFF -DOGRE_BUILD_COMPONENT_VOLUME=OFF \
  -DOGRE_BUILD_COMPONENT_DEAR_IMGUI=OFF \
  $SAMPLE_FLAGS -DOGRE_BUILD_TESTS=OFF -DOGRE_BUILD_TOOLS=ON

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

# Overlay is a cmake_dependent_option on FREETYPE_FOUND: -DOVERLAY=ON on a box
# without freetype dev headers is SILENTLY forced OFF at configure. Fail loudly
# (STATS_OVERLAY_SPEC.md D1 makes the component load-bearing).
grep -q "OgreNextOverlay" "$SRC/build/build.ninja" || {
    echo "Overlay component was NOT configured — freetype missing? (libfreetype-dev / vendored freetype required)." >&2
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

# MULTITHREADED SHADER COMPILATION, on the INSTALL side (THREADING_ADOPTION_SPEC
# P1, gate G1-a). Same class of guard as the three above, and it needs to be at
# least as loud: a STALE CMake CACHE keeps mode 1 while everything still
# configures, builds, installs and RUNS. Nothing goes red — the editor simply
# compiles its shaders on one core for ever, and the whole phase evaporates
# silently. The installed header is the only honest witness (the CMake option is
# translated into these two macros at ogre-next/CMakeLists.txt:445-453), so read
# it rather than the cache.
_bs="$PREFIX/include/OGRE-Next/OgreBuildSettings.h"
[ -f "$_bs" ] || { echo "OgreBuildSettings.h missing from $PREFIX/include/OGRE-Next — the install did not land." >&2; exit 1; }
if grep -qE '^[[:space:]]*#define[[:space:]]+OGRE_SHADER_THREADING_BACKWARDS_COMPATIBLE_API' "$_bs"; then
    echo "OGRE_SHADER_THREADING_BACKWARDS_COMPATIBLE_API is STILL defined in $_bs —" >&2
    echo "the -DOGRE_SHADER_COMPILATION_THREADING_MODE=2 argument above did not take." >&2
    echo "Almost always a stale CMake cache in $SRC/build: delete it and re-run this" >&2
    echo "script. Do NOT ignore this — the build would otherwise succeed and run with" >&2
    echo "single-threaded shader compilation (SPECS/THREADING_ADOPTION_SPEC.md P1)." >&2
    exit 1
fi

# FINE LIGHT MASK GRANULARITY, on the INSTALL side (light masks / lighting
# channels). Strictly louder than the threading guard above, because this flag
# is NOT in the ABI cookie (see the long note beside the configure line): a
# stale cache, or a tree that simply never re-ran this script, produces an
# engine where Light::setLightMask and MovableObject::setLightMask compile,
# link, run, and store — and are read by nothing. Every light keeps lighting
# every object, the editor's channel checkboxes do nothing at all, and the only
# symptom is a feature that quietly is not there. The installed header is the
# only honest witness (ogre-next/CMake/ConfigureBuild.cmake:156-157 is what
# translates the option into this macro).
if ! grep -qE '^[[:space:]]*#define[[:space:]]+OGRE_NO_FINE_LIGHT_MASK_GRANULARITY[[:space:]]+0' "$_bs"; then
    echo "OGRE_NO_FINE_LIGHT_MASK_GRANULARITY is not 0 in $_bs —" >&2
    echo "the -DOGRE_CONFIG_ENABLE_FINE_LIGHT_MASK_GRANULARITY=ON argument above did not" >&2
    echo "take. Almost always a stale CMake cache in $SRC/build: delete it and re-run" >&2
    echo "this script. Do NOT ignore this — the build would otherwise succeed and run" >&2
    echo "with light masks silently compiled out (setLightMask becomes a no-op, and" >&2
    echo "every per-object lighting channel in the editor stops filtering anything)." >&2
    grep -n "FINE_LIGHT_MASK" "$_bs" >&2 || true
    exit 1
fi
unset _bs

# The Overlay component again, on the INSTALL side. The configure-time grep
# above cannot see an install-side prune or a rename, and the whole point of the
# guard is that a missing libOgreNextOverlay is otherwise indistinguishable from
# a working install until the editor silently stops drawing its loading cover.
# (Studio's cmake/IncludeOgre.cmake refuses to configure without it too; this is
# the half that fires on the machine that BUILT the engine.)
ls "$PREFIX"/lib/libOgreNextOverlay.so* > /dev/null 2>&1 ||
ls "$PREFIX"/lib/libOgreNextOverlay*.dylib > /dev/null 2>&1 || {
    echo "libOgreNextOverlay was configured and built but is NOT in $PREFIX/lib —" >&2
    echo "the install step dropped it. The stats overlay and the engine-drawn" >&2
    echo "loading cover (SPECS/STATS_OVERLAY_SPEC.md) cannot work without it." >&2
    exit 1
}

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
