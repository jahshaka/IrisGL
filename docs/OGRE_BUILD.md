# Ogre-Next — build dependencies per platform

For Jahshaka's engine backend. **Linux is verified on this machine**; macOS and Windows are from
Ogre-Next's own setup docs (`Docs/src/SettingUpOgre/`) and are **not yet validated**.

Source: `~/Developer/engines/ogre-next` (master, `52d1a7a`, **4.0.0-unstable**).
Install prefix: `~/Developer/engines/ogre-next-install`.

> **Why master and not a release:** the last release is **v3.0.0 (2024-10-15), 22 months old**.
> We are greenfield — no existing Ogre code to break — and master carries fixes we need, notably
> the Vulkan/XCB external-window resize path. Revisit if master proves unstable.

---

## Linux (Ubuntu 26.04) — VERIFIED, builds

Already present on this machine: `libx11-dev` `libxt-dev` `libgl1-mesa-dev` `libglu1-mesa-dev`
`libfreetype-dev` `zlib1g-dev` `libvulkan-dev`

Installed for this build — **the complete list, install ALL of it before configuring**:
```bash
sudo apt-get install -y libxrandr-dev libxaw7-dev rapidjson-dev libzzip-dev \
                        libsdl2-dev glslang-tools spirv-tools vulkan-tools libshaderc-dev \
                        libfreeimage-dev libxcb-randr0-dev libx11-xcb-dev libxcb1-dev \
                        libxcb-keysyms1-dev
```

### ⚠ Three gotchas hit during the real build

1. **Install every dependency BEFORE the first `cmake` configure.** CMake caches not-found results.
   Installing `libfreeimage-dev` *after* configuring left the codec compiled in (`-DFREEIMAGE_LIB`)
   but the library absent from the link line — ~40 `undefined reference to FreeImage_*` at link
   time. The fix is to **re-run configure**, not to rebuild.
2. **`libshaderc-dev` silently disables Vulkan.** Without it CMake prints
   `Could NOT find Vulkan (missing: Vulkan_SHADERC_LIB_REL Vulkan_SHADERC_LIB_DBG)` and **still
   exits 0**, having dropped the Vulkan RenderSystem. Always check the `Building rendersystems:`
   block — do not trust the exit code.
3. **Vulkan's XCB windowing needs `libxcb-randr0-dev` + `libx11-xcb-dev`** (`xcb/randr.h`,
   `X11/Xlib-xcb.h`). Missing on a stock Ubuntu 26.04 desktop.
**`libshaderc-dev` is the non-obvious one** — without it CMake reports
`Could NOT find Vulkan (missing: Vulkan_SHADERC_LIB_REL Vulkan_SHADERC_LIB_DBG)` and **silently
drops the Vulkan RenderSystem** while still configuring successfully. Check the
`Building rendersystems:` block in the configure output, don't trust exit code 0.

Toolchain: CMake 4.2.3 · Ninja 1.13.2 · GCC 15.2 · pkg-config 2.5.1.

**No `ogre-next-deps` needed on Linux** — distro packages cover it.

### Configure
```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=$HOME/Developer/engines/ogre-next-install \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=ON -DOGRE_BUILD_RENDERSYSTEM_VULKAN=ON \
  -DOGRE_BUILD_COMPONENT_HLMS_PBS=ON -DOGRE_BUILD_COMPONENT_HLMS_UNLIT=ON \
  -DOGRE_BUILD_COMPONENT_SCENE_FORMAT=ON \
  -DOGRE_BUILD_SAMPLES2=OFF -DOGRE_BUILD_TESTS=OFF -DOGRE_BUILD_TOOLS=ON
```
`CMAKE_POLICY_VERSION_MINIMUM=3.5` is defensive — same reason as Jahshaka's build; Ogre-Next's own
root is `cmake_minimum_required(VERSION 3.13)`, but nested third-party CMake can be older.

### ✅ RESULT — built and installed clean, 2026-08-29

`554/554` targets, **zero errors**, install exit 0. Wall time ~10 min on 32 threads.

```
lib/         libOgreNextMain.so  libOgreNextHlmsPbs.so  libOgreNextHlmsUnlit.so
             libOgreNextSceneFormat.so  libOgreNextAtmosphere.so
             libOgreNextOverlay.so  libOgreNextProperty.so  libOgreNextMeshLodGenerator.so
lib/OGRE-Next/  RenderSystem_GL3Plus.so  RenderSystem_Vulkan.so  RenderSystem_NULL.so
                Plugin_ParticleFX.so  Plugin_ParticleFX2.so
lib/pkgconfig/  OGRE-Next.pc  (+ Hlms, Overlay, Property, MeshLodGenerator)
include/OGRE-Next/
bin/         OgreMeshTool  OgreCmgenToCubemap
```

**pkg-config resolves cleanly** — the claim that it consumes like an ordinary library, verified:
```bash
$ PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig pkg-config --modversion OGRE-Next
4.0.0unstable
```
Note `RenderSystem_NULL.so` — the headless backend that makes Stage 1 characterisation tests
runnable without a GPU or a window.

---

## macOS — NOT VALIDATED

O3DE's macOS story was experimental; Ogre-Next's Metal backend is the recommended path there, but
**there is no macOS CI in this repo** (only Linux + MSVC + clang-format). Treat as unproven.

```bash
brew install cmake sdl2          # SDL2 is NOT built by ogre-next-deps on macOS
git clone --recurse-submodules --shallow-submodules https://github.com/OGRECave/ogre-next-deps
cd ogre-next-deps && mkdir build && cd build
cmake ../ -G Xcode
cmake --build . --target ALL   --config Debug   && cmake --build . --target install --config Debug
cmake --build . --target ALL   --config Release && cmake --build . --target install --config Release
```
Then symlink/copy `ogre-next-deps/build/ogrenext` into the Ogre source tree as `Dependencies/`.

- **Xcode generator is the supported one.**
- Unless static libs are used, **Xcode project dependencies are not wired correctly** — build the
  `ALL` target first or `RenderSystem_Metal` won't be built.
- Backend: **Metal**. The GL window subsystem is *not* ported to 3.0 on macOS.
- Jahshaka relevance: this is the exit from our GL 3.2 Core dead end, since Apple caps desktop GL
  at 4.1 and deprecates it.

---

## Windows — NOT VALIDATED

```
CMake 3.x · Git · Visual Studio 2015-2022 (MinGW discouraged)
Windows 10 SDK  (preferred; contains current DirectX SDK)
DirectX June 2010 SDK  (optional — only for older VS, or for its tools)
Python 3.x  — REQUIRED to build the shaderc dependency for Vulkan
```
Build `ogre-next-deps` first (FreeImage, freetype, OIS, zlib, zziplib) in **both** Debug and
Release, then **build its `INSTALL` project** — the docs stress this; it creates the folder
structure Ogre expects, producing `ogredeps/`. Then configure Ogre-Next with the *same* VS
generator.

Backends available: **D3D11** and **Vulkan**. (No D3D12 in Ogre-Next.)

---

## Runtime data — do not forget this

The Hlms shader templates are **required at runtime**, not optional samples:
`Samples/Media/Hlms/{Common,Pbs,Unlit}` must ship with the application and be registered via
`ResourceGroupManager::addResourceLocation()`. We configured with `OGRE_INSTALL_SAMPLES=OFF`, so
these must be copied from the source tree deliberately.


---

## Local patches to Ogre-Next (both upstream-reportable) — REQUIRED

The engine at `engines/ogre-next` carries two source patches. A stock checkout will not work.

### 1. `RenderSystems/Vulkan/CMakeLists.txt` — link glslang + SPIRV-Tools explicitly
Debian's `libshaderc_combined.a` is not combined; see the gotchas above. Without it the Vulkan
plugin dies at load with `undefined symbol: _ZN7glslang17InitializeProcessEv`.

### 2. `RenderSystems/Vulkan/src/OgreVulkanDevice.cpp` — clear static extension arrays
`VulkanInstance::enabledExtensions` / `enabledLayers` are `static` and were never cleared, so a
second `Ogre::Root` in one process (Engine destroyed and re-created) accumulated the previous
run's entries, whose pointers referenced a host string that `sortAndRelocate()` resets. Result:
garbage extension names → `vkCreateInstance` → `VK_ERROR_EXTENSION_NOT_PRESENT`. Fix: two
`clear()` calls at the top of `enumerateExtensionsAndLayers()`. Proven by
`tests/engine/test_engine_recreate` (3 create/render/destroy cycles).

### Also required: `-DOGRE_VULKAN_WINDOW_NULL=ON`
Enables the surfaceless `windowType=null` window used by every headless test. Coexists with XCB;
the interface is selected at runtime.

### 3. `Samples/Media/2.0/scripts/materials/Common/Sky.material` — drop `param_named sliceIdx`
On Vulkan the GLSL compiler strips the unused `sliceIdx` uniform and the script's `param_named`
then fails to parse (`Compiler error: invalid parameters in Sky.material(127)`). Jahshaka does not
use Ogre's sky (see the engine's own sky sphere/cube), so the line is commented out to keep the
log clean. This media is staged into `bin/media/2.0/scripts/materials/Common` by the engine build.

---

## Jahshaka's patches (thirdparty/ogre-patches/, applied by scripts/build-ogre.sh)

1. **0001-vulkan-cmake-debian-unbundled-glslang** — Debian/Ubuntu ship glslang and
   SPIRV-Tools unbundled; Ogre's Vulkan CMake expects the bundled layout. Adds the
   system libraries to the link.
2. **0002-vulkan-device-clear-static-extension-arrays** — VulkanDevice keeps static
   extension arrays that survive engine re-creation in one process; clears them so
   a second Engine::create after destroy does not abort (test_engine_recreate).
3. **0003-sky-material-silence-sliceidx-parse-error** — Sky.material's sliceIdx line
   throws on Vulkan AFTER the sky renderable is attached (null-datablock crash in
   the render queue). Jahshaka uses its own sky geometry; the line is commented out.

   (0004-0010 landed with the macOS and refraction lanes — CMake/FreeImage, MoltenVK
   portability, the Metal window, swapchain currentExtent, the equirect sky's
   sliceIdx sample and HlmsPbs' refraction max3. Each patch file documents itself.)

11. **0011-ssao-reject-far-plane-sky** — the Tutorial_SSAO march has no far-plane
    rejection: on sky pixels there is no geometry to occlude AND the normals
    G-buffer was never written (the sky quad writes colour only), so the 64 taps
    compare a uniform depth against itself and return ~half occlusion modulated by
    the rotation noise — a dithered sky, ~45% too dark, under any chain with SSAO.
    Ogre's own tutorial scene has no sky, which is why upstream never saw it.

Updating Ogre: bump the submodule pin, re-run scripts/build-ogre.sh. A patch that
no longer applies is the signal to review upstream's change and adapt. Media-only
patches (0003/0009/0011) need no Ogre rebuild — the Studio build stages the media
straight from the submodule — but the patch loop must have run in that tree.
