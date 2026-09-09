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

**The list above is historical (2026-08-29); `scripts/build-ogre.sh` is the source of truth**
and pins every component explicitly. One argument there deserves calling out because it is an
*ABI* switch rather than a component:

```
-DOGRE_SHADER_COMPILATION_THREADING_MODE=2
```

Multithreaded shader/PSO compilation (`SPECS/THREADING_ADOPTION_SPEC.md` P1). Upstream's
default (1) enables the threaded path only through compiler TLS and only when `OGRE_STATIC`
is true — we build **shared**, so mode 1 means "off, on every platform, for ever". Mode 2
drops the backwards-compatible tid-less `Hlms` overloads instead; we use none of them (we
subclass `Hlms` nowhere, and our one `HlmsListener` overrides only the two hooks that carry no
`tid` in either mode), which is what makes the flip free of source changes.

Two consequences to know:

* **Every tree must re-run `build-ogre.sh` after pulling the commit that added it.**
  `generateAbiCookie()` hashes both threading macros, so a Studio compiled against the old
  `OgreBuildSettings.h` and linked against a mode-2 engine **aborts at `Ogre::Root`
  construction**. That is the loud failure; the quiet one is the reverse — an engine still
  built at mode 1, where everything works and `app.threading().multithreadedShaderCompilation`
  reads `false`.
* **The script fails loudly if the flag did not take**: after installing, it greps the
  installed `include/OGRE-Next/OgreBuildSettings.h` and exits non-zero while
  `OGRE_SHADER_THREADING_BACKWARDS_COMPATIBLE_API` is still defined. A stale CMake cache in
  `thirdparty/ogre-next/build` is the usual cause — delete it and re-run.

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

    (0012-0018 landed with later lanes — SMAA per-delegate viewport size, the FIFO
    latest-ready present mode, the overlay TextArea font load, the atomic id
    generator, the Forward+ light-collection warm-up, PCC hybrid probe weighting
    and the pass-light range fade. Each patch file documents itself.)

19. **0019-ssao-orthographic-position-reconstruction** — the same class of defect
    as 0011 and in the same three shaders: `getScreenSpacePos` (and the two blurs'
    `getLinearDepth`) assume a PERSPECTIVE frustum. The quad's interpolated
    view-space far corner is a ray only for one — OgreFrustum.cpp:884 takes
    ratio = 1 for PT_ORTHOGRAPHIC, so an ortho frustum's far corners have the same
    xy as its near ones — and `getProjectionParamsAB` returns a pair for which
    `B / (d - A)` is 1/t rather than t (OgreFrustum.cpp:128-141). Under an
    orthographic camera the reconstructed positions therefore MOVE WITH THE
    CAMERA: pan an editor's top/front/side view and the contact shadowing crawls
    over a scene that is not moving. The patch branches on a `jahOrthoParams`
    uniform the host pushes from the camera's projection type
    (chain::updateSsao); the perspective path is byte-for-byte unchanged. Jahshaka
    fixed the identical defect in its own SSR marcher at the same time
    (irisgl/engine/media/Hlms/Jahshaka/JahSsrRayMarch_ps.glsl), which is not a
    patch because that shader is ours.
21. **0021-vct-anisotropic-escape-fraction** — anisotropic voxel cone tracing
    (every Jahshaka GI quality above Low: `anisotropic = quality != Low`,
    OgreGi.cpp) saturates its cone alpha against the cone's OWN starting surface:
    the per-axis textures are front-to-back composited along their axis
    (AnisotropicMipVctStep1), so a one-voxel-thick floor reads as alpha 1.0 along
    ±Y at every coarser mip however little of the cell it fills — right for a ray
    crossing the cell, wrong for the diffuse cone that starts on that floor and
    re-enters its own cell as the footprint grows. The escape fraction that
    weights the ambient (`light.w`, computeVctProbe) collapses: measured on an
    open floor, isotropic keeps 93% of the ambient, anisotropic kept 4%
    (SPECS/OGRE_UPSTREAM_ISSUES.md, "Anisotropic VCT cone tracing saturates
    alpha"). The patch adds a second accumulator, `escapeAlpha`, that in the
    anisotropic loop takes the MIN of the three axis composites ONE MIP FINER
    than the colour samples (the axis textures are half-res, so `lodLevel` there
    is already a mip coarser than the isotropic march at the same distance) and
    that the ambient escape weight reads; colour/alpha accumulation (the bounce)
    is untouched, and the isotropic path computes the identical expression, so
    isotropic pixels are byte-for-byte unchanged. Measured: 4% → 65% of the raw
    ambient on the open floor — deliberately short of isotropic's 93%, because
    the exact route (the main texture's own isotropic mip alpha, which
    VctLighting builds in anisotropic mode above 32³) was measured at 95% AND
    restores the isotropic march's leak through one-voxel walls: the Mirror Room
    and Showroom samples flooded +46/+51 of 255 at Medium and clipped, where this
    patch moves them +7 (the isotropic march's own leak there is +14). Three
    alpha fetches per step in the anisotropic diffuse cones; frame cost under
    the noise floor. Media-only (Vct_piece_ps.any).
22. **0022-geometric-specular-antialiasing** — MEDIA-only (`800.PixelShader_piece_ps.any`).
    Folds the screen-space variance of the final shading normal into the GGX alpha
    (Kaplanyan 2016; Unity HDRP / Unreal runtime form, constants 0.25 / 0.18), gated on
    `normal_map_tex` so untextured normals and flat maps stay bit-identical. Cures the
    mip-collapse glints ("white dots") on low-roughness normal-mapped surfaces at
    distance. Toksvig is impossible on this pin: `getTSNormal` reconstructs Z so the
    sampled normal is always unit length. 0020 (samples config-dialog override) and 0021
    (VCT anisotropic escape fraction) are in flight on other lanes; numbers are claimed
    in order of landing.




23. **0023-ifd-raster-depth-grid-units** — the raster-fed IrradianceField
    (`IrradianceFieldRaster`, the path Jahshaka's `ddgiSource: raster` uses)
    writes its depth atlas in world units times the probe COUNT, while the pixel
    shader's Chebyshev visibility test compares against probe-GRID distances and
    the voxel path stores grid units. For any field larger than one unit per axis
    the stored depth dwarfs every cage distance, `r > mean` never fires and every
    probe reads unoccluded — the leak fix DDGI exists for is absent on the raster
    path. The patch adds an `invFieldSize` param to the CubemapToIfd job
    (default 1 = upstream's behaviour byte for byte) that the host sets to
    1 / the field's enlarged size per axis. Media-only. (22 is the lead's
    geometric-specular-antialiasing patch, landed on a later base than this
    lane's; numbered around it.)

24. **0024-pbs-ortho-view-dir** — SOURCE + MEDIA (`OgreHlmsPbs.h/.cpp` +
    `800.PixelShader_piece_ps.any`; every tree reruns `build-ogre.sh`). HlmsPbs
    computes `viewDir = normalize( -inPs.pos )`, the direction to a PINHOLE at the
    view-space origin — under an orthographic projection every pixel looks straight
    down -Z and the direction back to the eye is +Z for every fragment, so with the
    pinhole form NdotV, Fresnel, every light's half vector and the probe lookup vary
    with the fragment's SCREEN position and an axis-view pan slides highlights and
    reflections across a static scene (the owner's top-view report; 0019 closed the
    SSR/SSAO half). A pass property `hlms_ortho_camera` (`PbsProperty::OrthoCamera`)
    is set in `HlmsPbs::preparePassHash` BEFORE `preparePassHashBase` from the
    rendering camera's projection type, non-caster passes only (the caster shader
    never computes viewDir, and directional shadow cameras are orthographic — the
    property would only double the caster permutations for nothing); the
    LightingHeader piece takes `viewDir = (0, 0, 1)` under it. Compile-time branch:
    zero per-pixel cost. COST: the PBS pixel permutation space doubles along one axis
    — a scene first shown in an axis view compiles a second set of pixel shaders
    (measured: see the riders-lane report / JOURNAL entry); the Jahshaka shader-cache
    fingerprint hashes the staged Hlms tree, so every cache invalidates once.
    Gate: ssr.engine section 12 grew a PBS half (a glossy sphere's highlight centroid
    moves by exactly an 8 px ortho pan; the sphere window is the same picture
    translated).

Updating Ogre: bump the submodule pin, re-run scripts/build-ogre.sh. A patch that
no longer applies is the signal to review upstream's change and adapt. Media-only
patches (0003/0009/0011/0019/0021/0023) need no Ogre rebuild (0024 is SOURCE + media) — the Studio build stages the
media straight from the submodule — but the patch loop must have run in that tree,
and a tree whose media predates 0019 will THROW when chain::updateSsao pushes
`jahOrthoParams` at a shader that does not declare it (Ogre's setNamedConstant
raises on an unknown name), which is the loud failure that stale media deserves.
