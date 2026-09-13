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

25. **0025-shadow-node-fixed-light-invalidates-cached-build** — SOURCE
    (`OgreCompositorShadowNode.cpp`; every tree reruns `build-ogre.sh`).
    `CompositorShadowNode::buildClosestLightList` rebuilds a node's light list at
    most once per (camera, compositor frame) — `mLastCamera`/`mLastFrame` — and
    that build is the ONLY place `mNumActiveShadowMapCastingLights` is computed,
    while `setLightFixedToShadowMap` writes the slot array without it. Hlms
    declares `hlms_num_shadow_map_lights` from the COUNT (OgreHlms.cpp:3269) and
    indexes shadow maps from the ARRAY (OgreHlms.cpp:3493-3505), so a lamp fixed
    after a node has already built for the frame makes the two disagree: measured
    on a reflection-probe capture as `declares 3 shadow maps but its lights index
    4` with `[0:dynamic dir][1:cached spot]`, which drops the shader out of the
    static-branching path, generates `hlms_shadowmap3` against three declared maps
    ("undeclared identifier") and — on a cold shader cache — used to SEGV in
    HlmsDiskCache over the entry the failed compile leaves behind. Upstream never
    sees it because its own sample fixes lights once before the first frame; a
    lamp-map CACHE re-assigns per frame (ENGINE_CACHE_POLICY_SPEC P2/P4). The
    patch drops the cached camera so the next build recomputes the count — one
    extra light-list build per ASSIGNMENT CHANGE, nothing at rest. There is no
    engine-side fix: both the count and the guard are private, and the window
    cannot be avoided by ordering (a probe captures from the frame's update half,
    around the cache's own work — measured with the assignment moved earlier and
    the mismatch still present). Gate: `scripting.e2e.shadow_cache_probes` (cold
    shader cache; 3/3 red without the patch, 5/5 green with it) plus
    `world.shadowStatus().shaderLightMismatches`, the engine's own self-check.

26. **0026-hlms-disk-cache-skip-entries-without-pso** — SOURCE
    (`OgreHlmsDiskCache.cpp`; every tree reruns `build-ogre.sh`).
    `Hlms::createShaderCacheEntry` adds its cache entry before the backend builds
    the PSO, so a shader that FAILS to compile leaves an entry whose
    `pso.macroblock`/`pso.blendblock` are null — and `HlmsDiskCache::Pso`'s
    constructor dereferences both, so saving the disk cache after any failed
    compile is a SIGSEGV at address 0. The patch skips such an entry and logs it
    at LML_CRITICAL with its hash; the cache still saves everything else. A
    compile failure is recoverable everywhere else in the engine and must not
    take the process down. Proven by running the 0025 defect with 0026 applied:
    "HlmsDiskCache: skipping shader cache entry 536872216 - it has no PSO" and no
    crash where the same state crashed before.

27. **0027-vulkan-gpu-timestamp-samples** — SOURCE
    (`RenderSystems/Vulkan` only; every tree reruns `build-ogre.sh`).
    At this pin `VulkanRenderSystem::initGPUProfiling`, `deinitGPUProfiling`,
    `beginGPUSampleProfile` and `endGPUSampleProfile` are EMPTY BODIES, so the
    Vulkan render system can report no GPU time at all — not per pass, not per
    frame (D3D11 and Metal implement the same hooks; Vulkan does not). The patch
    fills them with `VK_QUERY_TYPE_TIMESTAMP` queries for the render-loop
    monitor (SPECS/RENDER_LOOP_MONITOR_SPEC.md; owner, 2026-09-12: "we want GPU
    more than CPU, but we need both"). Two alternating pools; results are read
    back TWO FRAMES LATE and non-blocking (`WITH_AVAILABILITY`), so the CPU
    never stalls on the GPU and a sample that is not back is reported as "not
    measured", never as zero. The design constraint is the RESET: a timestamp
    write is legal inside a render pass, `vkCmdResetQueryPool` is not, so the
    pool rotation, readback and reset happen at one host-called point at the top
    of the frame (`getCustomAttribute("JahGpuFrameBegin")`), outside every
    encoder. The readback rides `getCustomAttribute` — three new names,
    `JahGpuTimestamps` / `JahGpuFrameBegin` / `JahGpuSampleResults` — so the
    patch adds NO OgreMain ABI surface, and a build without it answers the
    host's probe by throwing, which is how the host learns there is no GPU
    timing.
    **TWO OFF-SWITCHES (owner decision D3).** BUILD: everything is inside
    `#ifdef JAH_GPU_TIMESTAMPS`, set by the new `JAH_GPU_TIMESTAMPS` CMake
    option, which **defaults OFF** — without it the four hooks compile to
    upstream's empty bodies byte for byte and the binary contains no query-pool
    code (verified: `strings RenderSystem_Vulkan.so | grep JahGpu` = 231 with
    the option on, 0 with `JAH_PRODUCTION=1`). `build-ogre.sh` passes
    `-DJAH_GPU_TIMESTAMPS=ON` for dev builds and `=OFF` when `JAH_PRODUCTION=1`
    (the release and packaging lanes). RUNTIME: even in a dev build no pool is
    created until a capture starts and both are destroyed when it stops, so a
    dev build with no capture running owns zero query pools — asserted by
    `test_engine`'s `monitor_gpu_timestamps`, which also passes on a
    production-configured engine by taking the "no patch in this build" branch.
    Upstream-reportable: the empty Vulkan hooks are an upstream gap.

28. **0028-pbs-probe-gate-on-material-reflectance** — SOURCE + media. A reflection
    probe only reaches a material that can reflect it (owner decision
    2026-09-13 Q1, `SPECS/REFLECTION_PROBE_AUDIT.md`). Upstream has NO
    material-side gate: once the pass sets `parallax_correct_cubemaps`, every
    datablock takes `use_envprobe_map` and every lit pixel of every object runs
    the per-pixel probe loop, however matte. The gate is the material's own
    reflectance — specular colour black AND no authored F0 — never a roughness
    threshold, and it is PIXEL EXACT rather than an approximation: the PBS
    specular term is `envColourS * pixelData.specular * (...)` with
    `pixelData.specular = material.kS`, so a black kS multiplies the whole
    environment term by zero whether or not it was sampled. CLEAR COAT IS
    INSIDE the gate: its own environment term is scaled by kS too
    (`Rs += clearCoatEnvColourS * pixelData.specular.xyz * (...) * clearCoat`,
    `200.BRDFs_piece_ps.any:334`), so a zero-kS clear-coated material reflects
    nothing either — the round-2 header claimed the opposite and was wrong
    (corrected 2026-09-13, A/B-measured pixel-identical in `gi.probe_gate` (g)).
    TWO EXCLUSIONS, both "a PASS feature writes the material's specular terms
    after the material has": `cubemaps_as_diffuse_gi`, where a probe also
    carries DIFFUSE light, which kS does not scale; and `hlms_decals_diffuse`,
    where a diffuse decal REWRITES `pixelData.specular` and `pixelData.F0`
    downstream of `material.kS`
    (`ForwardPlus_DecalsCubemaps_piece_ps.any:92-100`), so a decal laid on a
    matte black-kS floor is a reflective patch that must still see the room —
    the exactness argument does not hold under one, and Jahshaka feeds diffuse
    decals (`OgreDecals.cpp` setDecal requires a diffuse image). The gate is per
    DATABLOCK and a decal is per PIXEL, so a scene with diffuse decals bound
    keeps the probe loop on every material (clean-2 lane, 2026-09-13).
    THE F0 HALF OF THE PREDICATE READS WHAT THE SHADER READS: `setFresnel`
    writes `mFresnelG`/`mFresnelB` only in the SEPARATE form and `getFresnel()`
    returns all three regardless (the default datablock's stale G/B are 0.818),
    so testing all three unconditionally answered "reflective" for every
    zero-reflectance material in the non-separate Fresnel workflow and the gate
    never fired there at all — no pixel differed, which is why a picture-only
    suite could not see it (round-3 defect, 2026-09-13). The predicate now reads
    `mFresnelR` alone unless `hasSeparateFresnel()`, which is the shader's own
    rule (`500.Structs_piece_vs_piece_ps.any:283-287`).
    The MEDIA hunk is the one that saves the work: `forwardPlusDoCubemaps` is
    inserted from a PASS property, so it is made conditional on the DATABLOCK
    property `use_parallax_correct_cubemaps`.
    AN EDIT THAT CROSSES THE GATE REBUILDS THE SHADER: the predicate has ONE
    definition, `HlmsPbsDatablock::hasZeroSpecularResponse()`, and
    `setSpecular`/`setMetalness`/`setFresnel` call `flushRenderables()` only
    when it CHANGES (upstream's own `setClearCoat` idiom). Without that, a black
    material whose Specular Color the user raises kept the gated shader and went
    on reflecting nothing — `calculateHashForPreCreate` runs on a flush, and
    nothing in Jahshaka's per-frame material push flushes (OgreMaterials
    `applyPbr`). An ordinary edit that stays reflective costs no flush, and a
    DRAG of the Specular Color slider down through black and back crosses
    exactly twice, not once per frame. Both halves are measured rather than
    claimed: the engine counts crossings
    (`world.giStatus().probeGateCrossings`) and `gi.probe_gate` gates 100
    non-crossing pushes at zero and a 41-push drag at two.
    Covered by `gi.probe_gate`. (The decal permutation that never COMPILED is
    patch 0031, kept separate because it is an upstream bug that needs no probe
    grid and no gate — this entry's decal exclusion only makes Studio generate
    it far more often.)
29. **0029-pcc-probe-visibility-from-captured-depth** — MEDIA-only (two Hlms
    `.any` templates, both CRLF). A surface takes a probe's picture only if it
    is IN that picture (owner decision 2026-09-13 Q2: "the engine should not
    know if there is a room? Isn't it the layout of objects in a scene that
    matters?"). The only spatial tests at this pin are two axis-aligned boxes
    with three separate margins, and after Jahshaka's A2 shape clamp the
    parallax box is ~the whole probe region indoors, so the entry gate excludes
    nothing there: the outward face of a slab whose inward face a probe
    photographed is inside the same box. The probe already holds the answer —
    the DepthCompressor writes `min(0.5 * fDist/fApproxDist, 1)` into the cube's
    alpha and the IBL convolution preserves it at mip 0 (roughness 0 makes the
    GGX importance sample degenerate to the texel itself) — so the shader
    marches from the probe camera towards the shaded point, reads what the probe
    saw that way, and treats "it saw something nearer" as occlusion. The
    confidence multiplies the CONTRIBUTION (folded into `probeFade`) while
    `cubemapAccumWeight` keeps the full weight, so the unclaimed share returns to
    VCT / the irradiance field / the sky through patch 0017's blend instead of
    turning into black (lane E3's measured lesson). Slack is RELATIVE — 8% of
    the probe-to-point distance, floored at 2% of the captured distance —
    because the error is a texel's solid angle, 8-bit depth at the LDR tiers and
    filtering across silhouettes. AND ONLY A PROBE IN FRONT OF THE SURFACE IS
    ASKED (`saturate( dot( N, dirToProbe ) * 4 )`, both vectors in the probe's
    local space): the thing most often standing between a probe and a shaded
    point is the shaded point's OWN OBJECT — measured in `gi.probe_gate`'s room,
    whose witnesses' only weighted probes stand behind them, the chrome box's
    front face reads 79% of its own distance and the runtime plate 39%, and a
    depth-only rule takes both to black. A probe behind the surface keeps
    upstream's box reprojection; a probe in front is tested, and there the answer
    means something — a partition, a wall, a second room. (A SINGLE-SIDED
    material drawn with culling off keeps its front normal on its back face, so
    a probe in front of that face reads as "behind" and is not tested — the leak
    survives there; a two-sided material flips by `gl_FrontFacing` and is
    tested.) The limit, stated because it is real: a surface separated from its probe by nothing but a thin
    wall's own thickness is geometrically the mirror-box case and no depth rule
    of any tightness separates them; what answers that is the enclosure
    measurement that decides where probes are built at all, plus patch 0028's
    material gate.
    IT DEPENDS ON 0030 AND MUST NOT SHIP WITHOUT IT: this patch was designed and
    measured on 2026-09-13, then PARKED for a day because it also took a
    legitimate mirror in a sealed room to near-black — with 0030's encoding
    defect live, a probe reports a surface 2.256 m away as 1.058 m and this test
    honestly concludes "cannot see it". 0029 without 0030 = the leak closed and
    the mirror black; both = the leak closed and the mirror at r 1.000.
    Measured: a metal box sealed off from a red wall by a partition read
    r 1.000 g 0.055 (the wall it cannot see) and reads r 0.000 with the patch;
    the same room's legitimate mirror reads r 1.000.
    Covered by `gi.probe_visibility` (both phases).

30. **0030-pcc-depth-compressor-matrix-order** — MEDIA-only (a low-level
    material's four shader files). A probe's captured DEPTH was encoded against
    the WRONG DIRECTION on GLSL/Vulkan and Metal, so every probe that does not
    sit at the centre of its own parallax box lied about how far it could see
    along X and Y. `PccDepthCompressor_ps.any` does
    `mul( p_viewSpaceToProbeLocalSpace, probeToPosDir )`, and `mul` is
    `((x) * (y))` in those wrappers — but Ogre uploads a `Matrix3` ROW BY ROW
    into the padded COLUMNS of a GLSL `mat3`
    (`GpuProgramParameters::setNamedConstant(const String&, const Matrix3&)`,
    OgreGpuProgramParams.cpp:3208-3224), so `M * v` there evaluates the
    TRANSPOSE and `v * M` is the intended product. Upstream's own Hlms piece
    splits exactly this product by syntax (`Cubemap_piece_all.any`,
    `toProbeLocalSpace`); the compressor, a plain material rather than an Hlms
    template, does not. It hid because the transpose of a rotation is its
    inverse, the two Z faces' cubemap rotations are self-inverse (identity and
    180° about Y) and the only consumer is `fApproxDist` — the distance from the
    probe camera to its own box, which is symmetric for a centred probe. HLSL is
    left exactly as upstream shipped it and is SUSPECTED WRONG FOR THE SAME
    REASON — D3D11 packs cbuffer matrices column-major
    (`D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR`, OgreD3D11HLSLProgram.cpp:494/1580)
    and the Matrix3 upload has no transpose (OgreGpuProgramParams.cpp:1109-1122
    covers Matrix4 only), so `mul( m, v )` is most likely the transpose there
    too; what makes that form right in the Hlms pieces is that their matrix is
    ROW-CONSTRUCTED in the shader rather than uploaded. Unmeasured here (no
    D3D11 box), recorded in SPECS/OGRE_UPSTREAM_ISSUES.md, untouched by the
    patch — which adds one macro, `OGRE_MUL_M3V`, per wrapper.
    MEASURED by reading the probe cubes back texel by texel: in a sealed 20 m
    room with a 2x1x1 grid, probe 0 recorded a surface 2.256 m away at 1.058 m
    (47%) and probe 1 recorded the same surface 12.21 m off at 31.03 m (254%,
    saturated); with the patch, 2.275 m and 12.168 m. In an asymmetric coloured
    room the six face distances read 9.30 (saturated) / 0.49 / 2.00 / 2.00 /
    6.00 / 2.00 against a true 4.50 / 1.50 / 2.00 / 2.00 / 6.00 / 2.00, and
    every one of them is right with the patch. Three consumers were reading
    those numbers: `PccPerPixelGridPlacement::buildEnd`'s shrink-fit (the fit
    Jahshaka's A2 clamp exists to contain — with the patch the fitted shapes hug
    the room), `getPccVctBlendWeight`'s PCC-vs-VCT trust window, and patch 0029.
    Covered by `gi.probe_visibility` phase 2 (r 1.000 with, r 0.000 without).

31. **0031-pbs-decals-f0-scalar-swizzle** — MEDIA-only (one Hlms `.any`, CRLF).
    An UPSTREAM bug, reachable in Studio long before any patch of ours and found
    by `gi.probe_gate` on 2026-09-13, which was logging eight of these per run
    while passing: `ERROR: 'xyz' : vector swizzle selection out of range`, from
    `ForwardPlus_DecalsCubemaps_piece_ps.any:99` writing `pixelData.F0.xyz` in a
    permutation where `float_fresnel` is a SCALAR. F0 is a `midf3` only when
    `fresnel_scalar` is set (the FresnelSwizzle pair,
    `Main/500.Structs_piece_vs_piece_ps.any:283-287`), and the branch runs for
    `metallic_workflow || fresnel_workflow || fresnel_scalar` — the middle one,
    an authored F0 in the non-separate form, which is what
    `HlmsPbsDatablock::setFresnel` writes for an ordinary material, keeps it
    scalar. On Vulkan `VulkanProgram::compile` then throws and the permutation is
    lost, and A PERMUTATION THAT DOES NOT COMPILE RENDERS BLACK, SILENTLY:
    measured, the decal region of such a material reads 0.000/0.000/0.000 where
    it should reflect the room (50 compile errors in the suite's log), and
    r 0.071 g 0.012 b 0.012 with the patch (zero errors). Split on
    `fresnel_scalar`, the property that decides the type — upstream's own idiom.
    Kept OUT of 0028 deliberately: the bug needs no probe grid and no gate (0028
    only makes Studio generate the permutation far more often), and a tree that
    already carries 0028 must apply ONE NEW patch rather than reset the file.
    Covered by `gi.probe_gate` (h) and (i).

32. **0032-compute-indirect-dispatch** — SOURCE (OgreMain + the Vulkan render
    system; every tree must re-run `build-ogre.sh`). GPU-DRIVEN COMPUTE
    DISPATCH: `HlmsComputeJob::setIndirectDispatchBuffer( BufferPacked*,
    offsetBytes, issueBarrier = true )` + `RenderSystem::_dispatchIndirect` /
    `supportsIndirectDispatch()`, implemented on Vulkan as
    `vkCmdDispatchIndirect`. The base implementation throws
    ERR_NOT_IMPLEMENTED and reports false, so D3D11, Metal, GL3Plus, GLES2 and
    NULL compile unchanged.

    WHY: every Lumen-shaped stage compacts between passes, and Epic call
    indirect dispatch "essential" (up to a 50% tracing speedup from the
    compaction it enables). The pin had ONLY `vkCmdDispatch` with CPU-side
    counts; `setNumThreadGroupsBasedOn` is CPU-side arithmetic over a bound
    resource's DIMENSIONS and can never see a number a shader computed. It is
    the one mandatory piece of supporting technology for either Photon arm
    (SPECS/research/LUMEN_SUPPORTING_TECH_2026-09-13.md §6, NANITE_SPEC §4.2).

    THREE THINGS TO KNOW.
    (a) The argument buffer must be a `UavBufferPacked`, not an
    `IndirectBufferPacked`: `VulkanVaoManager` forces `mSupportsIndirectBuffers`
    to false (:183-184) and emulates indirect DRAW buffers in system memory, so
    such a buffer has no BufferInterface at all — while the ordinary VBO pools
    are already created with `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT` (:1122-1127),
    so no allocator change was needed.
    (b) THE BARRIER IS HAND-ROLLED AND DELIBERATELY NOT REGISTERED WITH THE
    SOLVER. `executeResourceTransition`'s buffer branch only ever sets
    SHADER_READ/SHADER_WRITE and `ogreToVkStageFlags` knows only the six shader
    stages, so `VK_ACCESS_INDIRECT_COMMAND_READ_BIT` at
    `VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT` is unreachable from the BarrierSolver
    — and `assumeTransition` exists only for textures, so it cannot be told
    after the fact either. `_dispatchIndirect` therefore issues a
    `VkBufferMemoryBarrier` over the twelve bytes it reads, covering both a
    compute SHADER_WRITE and a TRANSFER_WRITE (Ogre's copy encoder does not end
    with INDIRECT_COMMAND_READ in its destination mask either). It changes no
    state the solver tracks, so the solver stays coherent by construction; and
    routing it through `executeResourceTransition` would be wrong anyway —
    that function begins with `endAllEncoders()`, which would close the compute
    encoder the dispatch is being recorded into.
    (c) `HlmsCompute::compileShader` no longer demands a non-zero
    `num_thread_groups_*` for a job that carries an indirect buffer. That check
    exists because Metal needs the counts on the C++ side; an indirectly
    dispatched job has none by definition. The threads-per-group half is
    untouched.

    MEASURED by `compute.indirect_dispatch`: at 0, 7 and 4096 survivors the
    count the counting job wrote, the number of groups that ran and the count
    the groups read all agree, and the output is byte-identical to a CPU-sized
    dispatch of the same job. With `issueBarrier = false`, Vulkan
    synchronization validation reports SYNC-HAZARD-READ-AFTER-WRITE at
    `vkCmdDispatchIndirect` naming exactly the missing access/stage pair; with
    the barrier the layer is silent. (The data hazard itself did not reproduce
    on this driver — which is why the barrier is unconditional.)

    ALSO NOTE: a CPU-sized dispatch cannot express "run nothing" at all (Ogre
    refuses to compile a job whose group counts multiply to zero), so the
    empty-list case — the one a compaction hits most often — has no non-indirect
    equivalent short of a CPU-side branch the CPU has no information to take.

33. **0033-vct-cascade-escape-opacity-composite** — MEDIA-only (one Hlms `.any`).
    On a CASCADE CHAIN (`vct_num_probes > 1`, which only Photon's camera-centred
    cascade arm builds) the ambient escape weight collapsed to zero, so every
    surface the bounce did not reach rendered BLACK instead of ambient-lit.
    `voxelConeTraceDiff` starts its escape accumulator at the alpha it is handed
    (patch 0021), so the value it returns is already the running total; the
    cascade continuation loop then ADDED that total to the one it already held,
    compounding the opacity once per cascade. Measured on `gi.cascades` case 4
    (a ground lit by nothing but a flat ambient, four cascades at Epic): 0.0902
    luminance with GI off, 0.0000 with the chain, back to the GI-off value with
    the patch. Fix: assign instead of add. Nothing that ships today enters that
    loop, so no existing picture can move (selftest hash unchanged).

    UPSTREAM FINDING recorded with it: `result.alpha += newRes.alpha` on the
    line above has the same shape and doubles the colour alpha at every cascade
    hop — it both darkens the `( 1.0 - result.alpha )` composite and can exit
    the cascade walk early. Left alone deliberately (it changes the bounce) and
    reported for `SPECS/OGRE_UPSTREAM_ISSUES.md`.

Updating Ogre: bump the submodule pin, re-run scripts/build-ogre.sh. A patch that
no longer applies is the signal to review upstream's change and adapt. Media-only
patches (0003/0009/0011/0019/0021/0023/0029/0030/0031/0033) need no Ogre rebuild (0024 and 0028 are
SOURCE + media; 0025, 0026, 0027 and 0032 are SOURCE-only) — the Studio build stages the
media straight from the submodule — but the patch loop must have run in that tree,
and a tree whose media predates 0019 will THROW when chain::updateSsao pushes
`jahOrthoParams` at a shader that does not declare it (Ogre's setNamedConstant
raises on an unknown name), which is the loud failure that stale media deserves.
