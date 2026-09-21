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
20. **0020-samples-env-suppress-config-dialog** — SOURCE, and the only patch in
    the stack that touches nothing Jahshaka ships: Ogre's sample framework
    (`Samples/2.0/Common/src/GraphicsSystem.cpp`) sets `mAlwaysAskForConfig`,
    which short-circuits `restoreConfig()` — so a seeded `ogre.cfg` is ignored
    and every sample run from a script or a headless shell stops on the config
    dialog. With `JAH_OGRE_SAMPLE_NO_CONFIG` set in the environment the flag is
    cleared and the seeded config is honoured. Kept because running an upstream
    sample against a pristine engine is how an upstream behaviour gets checked.

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




23. **DELETED 2026-09-17** (lane FIELD-RASTER-CRUD). Was
    `0023-ifd-raster-depth-grid-units`, a MEDIA-only patch that put the
    raster-fed IrradianceField's depth atlas into probe-GRID units
    (an `invFieldSize` param on the CubemapToIfd job) so the Chebyshev
    visibility test it feeds could fire at all. Its only consumer was the
    irradiance field's RASTERISED probe source (`ddgiSource: raster`), which was
    deleted the same day — no shipped tier ever selected it, the path cost
    ~3.4 ms per probe, and its first compute dispatch lost the device on the
    RTX 4080 / driver 595.84 about 20 % of the time (NVRM Xid 109 CTX SWITCH
    TIMEOUT). Nothing in Jahshaka calls `IrradianceFieldRaster` any more, so the
    pin's own copy of that class and of the `IrradianceFields` media is left
    exactly as upstream ships it. The number is NOT reused. (22 is the lead's
    geometric-specular-antialiasing patch, landed on a later base than the
    original 0023's lane; numbered around it.)

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
    **AMENDED IN PLACE 2026-09-14 (PHOTON E0, audit B5) — same number, and a
    tree that carries the older version must reset the file before re-running
    `build-ogre.sh`** (`git -C irisgl/thirdparty/ogre-next checkout -- . &&
    git -C irisgl/thirdparty/ogre-next clean -fd`, the usual loop).
    On a CASCADE CHAIN (`vct_num_probes > 1`, which only Photon's camera-centred
    cascade arm builds) the ambient collapsed to zero, so every surface the
    bounce did not reach rendered BLACK instead of ambient-lit. The cascade
    continuation is run TWICE per pixel — once over the diffuse cones and once
    over the specular one — and both loops carried the same two defects:
    (1) `voxelConeTrace*` starts its accumulators at the alpha it is handed
    (upstream's, and patch 0021's escape one), so the value each returns is
    already the running total for the whole cone; the loops then ADDED that
    total to the one they already held, doubling the opacity once per hop; and
    (2) the self-occlusion bias `computeVctProbe` applies before the first cone
    was never re-applied at a cascade ENTRY, so the surface occluded its own
    cone once per hop. Fix: assign instead of add, and re-apply the bias in the
    new cascade's units at every entry (on the specular walk along the GEOMETRIC
    NORMAL, measured by its own `normalBias` — the loop's `startBias` is
    upstream's seam term along the reflection ray and stays).
    Measured — diffuse, `gi.cascades` case 4 (a ground lit by nothing but a flat
    ambient, four cascades at Epic): 0.0902 luminance with GI off, 0.0000 with
    the chain, 0.0462 (0.51x) with the patch, the residual being upstream's
    march-distance restart (recorded for `SPECS/OGRE_UPSTREAM_ISSUES.md`).
    Specular, case 7 (a chrome plate lit by nothing but the ambient): at
    roughness 0.15 the plate read 0.0264 with one cascade, 0.0148 with two and
    0.0001 with four before the amendment.
    Nothing that ships today enters either loop (the flag is off in every scene),
    so no existing picture can move — selftest hash unchanged, A/B'd.

34. **0034-hdr-nan-must-not-latch-adapted-luminance** — MEDIA-only (one HDR GLSL
    shader). `HDR/DownScale03_SumLumEnd` is the ONLY recurrence in the HDR
    chain: it writes a 1x1 `keep_content` texture and mixes that same texture's
    previous value back in every frame. An +Inf measurement is survivable —
    `clamp()` pins it to `exposure.z` and the next frame carries on — but a NaN
    is not: `clamp()` is `min(max(x,lo),hi)` and `max(NaN,lo)` returns NaN on
    every driver measured, so `exp(NaN)`, then `mix(NaN, oldLum, w)`, and the
    adapted luminance is NaN for the rest of the workspace's life. Measured
    (SMOKE-ENGINE-1, 2026-09-14): entering the Player on a bright scene froze
    the exposure from frame one and only a WORKSPACE REBUILD — the one thing
    that re-runs this texture's initial clear — brought it back. Fix: two
    `x == x` guards (false only for a NaN), `fLumAvg` falling back to
    `exposure.y` (the log-luminance floor the very next line clamps to, i.e.
    "a dark frame") and `oldLum` falling back to `newLum`. A chain that was
    already producing numbers produces exactly the same numbers, Inf included —
    selftest hash unchanged, A/B'd.
    DELIBERATELY NOT IN THIS PATCH: `FilmicTonemap(Inf)` is `Inf/Inf == NaN` in
    `FinalToneMapping`, and `DownScale01_SumLumStart` takes `log()` of whatever
    the scene holds. Both were built and measured in the same lane and both MOVE
    EXISTING PICTURES — clamping the scene colour before the tonemapper removes
    black holes and replaces them with white sparkle over 20 % of the Shadow
    Maps port's viewport, because that port really does produce a large
    population of unrepresentable specular texels. That belongs upstream of the
    tonemapper and is recorded as a separate finding.
    GLSL only: this tree compiles Vulkan on every platform it ships on, so the
    HLSL and Metal copies are never built here and patching them would be
    unverifiable.

35. **0035-hlms-disk-cache-bounds-check-shader-hash-indices** — SOURCE
    (OgreMain; every tree must re-run `build-ogre.sh`). `HlmsDiskCache::copyFrom`
    unpacks a RENDERABLE index (21 bits) and a PASS index (8 bits) out of a
    32-bit shader hash and subscripts `mRenderableCache` / `mPassCache` with
    them, checking neither. On 2026-09-14 that killed the owner's editor and two
    rig instances inside forty minutes, always the same stack: `ShaderCache::save`
    -> `copyFrom` -> `Hlms::getProperty` -> `IdString::operator<`, SIGSEGV at
    0x0 — `std::lower_bound` walking a `HlmsPropertyVec` that is not one, out of
    the periodic save timer.
    The patch SKIPS an entry whose indices cannot be resolved and logs the hash,
    both indices against both sizes, and the Hlms TYPE the hash claims against
    the one it was filed under. Same contract as 0026 (which skips an entry whose
    compile failed and left a null PSO) — and 0026's guard sits AFTER this
    subscript, which is why it did not catch these three. Second hunk:
    `Hlms::preparePassHashFinal` says so, once and loudly, the moment `mPassCache`
    grows past what its 8 bits can address, because the only thing guarding that
    overflow today is an `assert()` release builds compile out and its only
    symptom would be exactly this crash.
    CAUSE NOT PROVEN, deliberately recorded as such: the lane could not
    reproduce the crash in 600+ saves across two harsh sessions. It ELIMINATED
    a worker-thread race (`RenderQueue::render` calls
    `ParallelHlmsCompileQueue::stopAndWait` before returning and
    `SceneManager::_fireWarmUpShadersCompile` syncs its barrier twice, so no
    compile thread outlives the call that started it) and MEASURED the pass-cache
    overflow as latent rather than current (86 of 256 after thirteen scene opens,
    every post row toggled and the player entered in each). The remaining
    hypothesis — a renderable hash filed under the wrong Hlms, where an index
    valid for HLMS_PBS's 33 entries is far out of range for HLMS_UNLIT's 8 — is
    exactly what the new log line answers.
    No picture can move: the patch only ever skips an entry that would have
    crashed the process. Selftest hash unchanged, A/B'd.

36. **0036-pbs-ssr-replaces-the-environment-term** — MEDIA-only (one Hlms Pbs
    piece; every tree needs the patch loop, no Ogre rebuild). Upstream composites
    a screen-space reflection into `pixelData.envColourS` two ways, chosen by the
    datablock property `use_envprobe_map`: a LERP when the material has a
    reflection cubemap or parallax-corrected probes, and `envColourS += ssr * w`
    when it has neither. The lerp is the physics — the probe and the screen are
    two estimates of ONE integral and `w` is how much of it the screen answered —
    and the add is the same specular lobe counted twice, because
    `applyVoxelConeTracing` adds its cone-traced SPECULAR to `envColourS` earlier
    in the same shader (Vct_piece_ps.any:647) without raising `use_envprobe_map`.
    It is the ONLY other writer of that term: the irradiance field and the
    irradiance volumes write `envColourD`. A VCT-lit scene with no sky cube and
    no probe grid therefore shaded every mirror as "the screen's answer PLUS the
    voxel cone's answer", and since this renderer's SSR is a closed loop (the
    shaded colour becomes the next frame's history) the error compounded
    through it.
    The patch makes the composite the lerp unconditionally.
    NO SHIPPED FRAME MOVES: where `envColourS` is zero, lerp(0, R, w) is exactly
    0 + R*w, and every scene with a sky or probes was already on the lerp branch
    (OgreSky.cpp binds the IBL cube when no automatic PCC is bound; the PCC path
    raises the property when it is). Selftest hash unchanged, A/B'd; the Mirror
    Room and the ShadowMapFromCode port are bit-identical across it.
    Gate: `ssr.engine` section 15 (a VCT-lit mirror with no sky — the exact
    configuration that takes the old branch) is RED on unpatched media.
    TRAP recorded in the patch header: an `@else` written inside a `///` COMMENT
    in a .any file is still read by the Hlms parser, unbalances the block and
    breaks the generated shader a thousand lines away.

37. **0037-vctlighting-setvoxelizer-and-three-lifetime-defects** — SOURCE
    (Components/Hlms/Pbs: every tree resets the submodule and re-runs
    `build-ogre.sh`). PHOTON-G12 (2026-09-15), the first patch under the "a patch
    beats a workaround" rule. The hook: a public `VctLighting::setVoxelizer()`
    that points an EXISTING lighting at a replacement voxeliser — it moves the
    texture listeners and re-creates the light voxels through the pin's own
    lost-residency recovery (`mVoxelizerTexturesChanged` +
    `mVoxelizerListenersRemoved` → `checkTextures()`), which is what lets a
    cascade chain rebuild ONE cascade's voxels without replacing its lighting
    (`VctLighting::mExtraCascades` is a FastArray walked by `fillConstBufferData`
    every pass, so replacing a lighting dangles every cascade inside it). Three
    pin lifetime defects fixed in the same patch: (1) `checkTextures()` never
    cleared `mVoxelizerTexturesChanged` (set on a lost residency, assigned false
    only in the ctor) — a voxeliser that lost residency once re-created its light
    voxels on EVERY later `update()` for ever; (2) `correct_area_light_shadows`
    on the SHARED "VCT/LightInjection" compute job was written only by
    `createTextures` from the last-built voxeliser, so a per-cascade rebuild
    left the outer cascade's value standing for the chain — re-asserted per
    injection from THIS lighting's voxeliser (the pin's own chain never
    re-voxelises a cascade alone, so it never noticed); (3)
    `IrradianceFieldRaster::destroyWorkspace` removed two of its three
    workspaces and then destroyed the `Camera*` the third was built against —
    one dangling disabled workspace per atlas rebuild. Replaces the engine's
    former `JahVctLighting` protected-member reach-in (deleted).
38. **0038-vulkan-ray-query-device-enablement** — SOURCE (one render-system
    file plus its header; every tree re-runs `build-ogre.sh`). Ogre-Next 4.0 has
    no ray-tracing support of any kind, and three things in `OgreVulkanDevice`
    block Vulkan's own: the instance is created at apiVersion 1.0.2, device
    extensions come from a hardcoded if/else whitelist, and the feature pNext
    chain is a fixed set of three structs. The patch raises the instance to 1.2
    ONLY when `vkEnumerateInstanceVersion` says the loader can (a 1.0 loader
    keeps 1.0.2 byte for byte), adds seven extension names each gated on the
    driver advertising it exactly like every name already there, and chains four
    feature structs held as a MEMBER of VulkanDevice (the chain must outlive
    `fillDeviceFeatures2`, which returns before `vkCreateDevice` reads it).
    THE FOOT-GUN EVERY LATER FEATURE PATCH MUST COPY: upstream uses the SAME
    chain to QUERY the driver and to ENABLE features, so whatever the query
    leaves set gets turned on — this patch clears every reported bit and puts
    back exactly three (accelerationStructure, rayQuery, bufferDeviceAddress).
    That mask is why enabling the tier moves no pixel. Adds
    `VulkanDevice::hasRayQuery()`, which Ogre never calls and 0039 does.
    Inert on a device without the extensions; that is the Mac's path.
    Gate: `gi.rayquery` + the unchanged `--engine-selftest` hash with the tier
    live and holding structures.
39. **0039-vulkan-vbo-pools-as-blas-build-input** — SOURCE. Two buffer usage
    bits and one memory-allocate flag on the DEVICE-LOCAL VBO pools
    (`vboFlag == CPU_INACCESSIBLE`), guarded on `hasRayQuery()`. Without it a
    bottom-level acceleration structure must be built from a COPY of every
    vertex and index buffer — a second resident copy of the world's geometry
    plus a readback per mesh. The guard is not cosmetic:
    SHADER_DEVICE_ADDRESS without the bufferDeviceAddress feature is invalid
    usage, and so is the usage bit without the matching allocate flag. Narrower
    than the S3 spike's `!= CPU_READ_WRITE`, which also decorated the
    host-visible staging pools that never feed a BLAS (audit C-9).
40. **0040-vulkan-export-onvulkanfailure** — SOURCE, one declaration. A PIN
    DEFECT found by building against the pin as its installed headers invite:
    `VulkanQueue` is declared `_OgreVulkanExport` and offers the PUBLIC INLINE
    `getCurrentCmdBuffer()` in an installed public header, whose device-lost
    branch expands `checkVkResult` → `Ogre::onVulkanFailure` — and that function
    is declared with NO export macro, so the render system's
    `-fvisibility=hidden` keeps it out of `RenderSystem_Vulkan.so`'s dynamic
    table. Any code outside the plugin calling the pin's own public accessor
    compiles and then fails to LINK (`undefined reference to
    Ogre::onVulkanFailure`); the out-of-line members of the same class
    (`endAllEncoders`, `getComputeEncoder`) export and link fine.
    The patch adds the macro and MOVES the declaration (with the `checkVkResult`
    macro) to the bottom of `OgreVulkanPrerequisites.h`, because upstream
    defines `_OgreVulkanExport` at the END of that same header — below where the
    function was declared. No behaviour change: same definition, same callers,
    same expansion. Worth reporting upstream; the same class of defect exists
    for every `checkVkResult` inside a public inline in an installed header.
    The alternative was reaching for `VulkanQueue::mCurrentCmdBuffer`, which is
    PROTECTED — a derived-class reach-in that lives only as long as the pin's
    layout. A patch beats a workaround (owner, 2026-09-15).

41. **0041-descriptor-cache-buffer-creation-serial** (SOURCE) — a descriptor-set
    cache keyed on a raw pointer must not outlive the pointee.
    `HlmsManager::getDescriptorSetTexture2`/`getDescriptorSetUav` cache whole
    descriptor sets keyed on `BufferSlot`, whose identity is the raw
    `BufferPacked *` + offset + size, and NOTHING tells that cache when a buffer
    dies. An entry outlives its buffer for as long as any job still references it,
    so a new buffer handed the ADDRESS of a destroyed one gets the DEAD buffer's
    API view — wrong suballocation offset, wrong pixel format. MEASURED: VCT's
    `VCT/AabbCalculator` read IrradianceField's destroyed integration-taps buffer
    (`PFG_RG32_FLOAT`) as its `PFG_RGBA32_UINT` mesh table — Vulkan core
    validation `VUID-vkCmdDispatch-format-07753` names it — so every mesh AABB
    came back as the sentinel and the voxelization wrote ZERO voxels: a scene with
    no bounce light at all whenever a GI re-solve landed one frame after a cold
    inline shader compile (ledger §373, PHOTON_SPEC §7 E1 item 0). The fix is a
    process-wide monotonic creation serial on `BufferPacked` that the two
    `BufferSlot`s carry and compare, STAMPED by
    `HlmsComputeJob::setTexBuffer`/`_setUavBuffer` while the buffer is alive (never
    read through the pointer inside the comparison — that would be the very
    use-after-free it prevents). Two more hunks of the same bug class ride with it:
    `VulkanRenderSystem::_descriptorSetSamplerDestroyed` stored the dying object's
    own address instead of clearing the table slot (the compute twin eight lines
    below has always been right), and `HlmsComputeJob::analyzeBarriers` registered
    no transition at all for a plain `TexBufferPacked` read by a compute job.
    Verified: `--engine-selftest` hash UNCHANGED (`b55e2d5d…`), `gi.*` + `engine.*`
    35/35, `test_engine` + every ASan twin 12/12, and `threading.gi_resolve*` —
    the guard this patch exists for — green 3/3 cold on both compile branches.
    Guarded by `threading.gi_resolve` / `.gi_resolve_serial` / `.gi_resolve_pixels`.

42. **0042-hdr-luminance-meter-must-not-be-poisoned** (MEDIA, GLSL) — THE DRAG
    SHIMMER. The HDR chain's exposure is a MEAN of `log( luminance )` over a
    sparse grid of the scene target, so ONE unusable sample makes the whole
    FRAME's measurement unusable — and two things produce one, both of which come
    and go while a light turns: the RGBA16F target stores `+Inf` where a punctual
    light's specular lobe (`1 / (pi * alpha^2)` at the roughness floor) blows out,
    and the measurement's bilinear fetch weights that texel by zero on some frames
    and not others (`0 * Inf` is a NaN); and a filtered fetch across a blown
    neighbourhood comes back BELOW ZERO, whose `log()` is a NaN too. Patch 0034
    then read an unusable measurement as `exposure.y`, the log-luminance FLOOR —
    i.e. the chain's BRIGHTEST exposure — so every measurement failure yanked the
    grade towards its limit. MEASURED on the live editor viewport (Ogre's
    ShadowMapFromCode port, the sun stepped 0.35 deg/frame, the 1x1 adaptation
    history and the 4x4 `rtIter2` read back every frame): 24 of 120 dragging
    frames with SSR on and 9 with SSR off carried a NaN, the exposure lurched on
    every one, and the adapted luminance's range over a 60-frame drag fell from
    12.0 % to 2.3 % (SSR on) and 5.0 % to 1.0 % (SSR off) with the patch, 0.363 %
    to 0.060 % at rest. AND: **patch 0034's `x == x` guards NEVER FIRED** — the
    shader compiler folds `x == x` to true on this stack (NVIDIA 595.84,
    Vulkan/SPIR-V), proved with an absurd value on its NaN branch that never
    appeared; what produced 0034's constant was the DRIVER's `clamp( NaN, lo, hi )`
    returning `lo`. Every guard here is on the BITS
    (`(floatBitsToUint(x) & 0x7FFFFFFF) < 0x7F800000`), which no optimiser may
    remove — **an `x == x` NaN test anywhere in this tree's shaders is a no-op.**
    Bit-identical for every frame whose samples were all finite and non-negative;
    `--engine-selftest` hash UNCHANGED (`b55e2d5d…`). Guarded by `hdr.drag_stable`.
    **INERT ON JAHSHAKA'S FRAME PATH SINCE EXPOSURE-2 (2026-09-17), AND KEPT
    DELIBERATELY.** 0034 and 0042 patch the two GLSL files of the pin's
    64/16/4/1 luminance LADDER, and Jahshaka's chain does not run that ladder any
    more: the automatic exposure is a compute HISTOGRAM of our own
    (`engine/media/Hlms/Jahshaka/JahHdrMeter*_cs.glsl`), where an unusable sample
    is not BOUNDED but simply NOT BINNED — a mean had to clamp one because it
    could not drop it, and a histogram can. Both patches stay applied because
    they are correct fixes to upstream media that upstream's own HDR sample and
    compositor still use, and because the measurement in this entry is the
    record that produced the bit-level NaN rule the new meter also follows.
    Neither is a candidate for deletion on cost grounds (they are MEDIA-only:
    no Ogre rebuild). If the stack is ever squashed (PATCHES-2), they belong
    together.
43. **0043-prepass-hands-back-the-roughness-it-wrote** (MEDIA) — in PrePassUse
    mode the shading pass reads its normal and its shadow term out of the
    G-buffer, but upstream lets it read the ROUGHNESS back only when the material
    carries a roughness MAP. Jahshaka patch 0022 broke that premise: it makes the
    GGX alpha a PER-PIXEL quantity on every normal-mapped surface (geometric
    specular anti-aliasing), the prepass computes it and writes it, and the
    shading pass threw it away — so a normal-mapped material with a CONSTANT
    roughness lost its anti-aliasing for as long as SSR was switched on and got it
    back when SSR was switched off (lane SSR-1 round 2 measured the gate alone at
    1.48 % of the picture, peak 161 of 255). The gate is now `roughness_map ||
    normal_map_tex`. AND the encoded RANGE moved with it: upstream packs alpha
    over `[0.02, 1]` while `SampleRoughnessMap`'s own floor is `0.001`, so a
    mirror-smooth material came back through the G-buffer WIDENED — measured at
    8.60 % of the picture on the fixture in `tests/ssr`, worse than the defect
    being fixed; over `[0.001, 1]` the round trip is faithful at the same 16 bits
    (1.54 %, which is the prepass' R10G10B10A2 NORMALS on a near-delta lobe and
    nothing else). `--engine-selftest` hash UNCHANGED — the default scene has no
    SSR. Guarded by `ssr.engine` case 13.

44. **0044-irradiance-field-movable-volume-and-lighting-rebind** (SOURCE — every
    tree re-runs `build-ogre.sh`) — Photon E1 puts the irradiance field on the
    INNERMOST cascade of a camera-centred chain, so the field's volume has to
    follow that cascade as it scrolls. Upstream's only placement API is
    `initialize()`, which destroys and re-creates both atlases (a field
    re-initialised on every step is born black on every step), and the members
    a placement needs are protected — the reach-in the patch rule forbids. Two
    hooks on `IrradianceField`: `setFieldVolume(origin, size)` moves/resizes the
    volume in place with the same one-probe-block enlargement `initialize()`
    applies and re-derives the generation params (the probe counts are settings
    and untouched, so the atlases stay valid); and `setVctLighting(l)`, which is
    also a PIN-DEFECT fix: `createTextures()` binds the light voxel textures
    ONCE by pointer, and a `VctLighting` re-creates them on LostResidency and on
    `setVoxelizer()` (patch 0037) — after which a bound field integrates from
    DESTROYED textures, silently. The setter re-binds the generation job's
    slots (the only consumer; the integration jobs read the atlases) and
    re-reads the light voxel width for the start bias. Recorded in
    OGRE_UPSTREAM_ISSUES ("IrradianceField binds the light voxels by pointer").
    Guarded by `gi.field_follows` (the field rides cascade 0; a 100 m walk =
    20 follows; the raster source follows) and `gi.leak_room`.

45. **0045-vct-cone-diffuse-yields-to-a-partial-field** (MEDIA — `Vct_piece_ps.any`;
    OVERLAPS 0021 and 0033 in that file: a tree carrying them fails
    `build-ogre.sh`'s reverse-check with "Upstream changed the patched file" —
    the documented blind spot, cured by the submodule reset) — upstream sets
    `vct_disable_diffuse` whenever a field is bound because ITS field covers the
    whole voxel volume. Under a Photon chain the field covers only cascade 0, and
    "field instead of cones" would leave every pixel in the ring with no diffuse
    bounce at all. The engine's Hlms listener re-opens the gate when
    `irradiance_field && vct_num_probes > 1` (both already in the pass
    properties, so the pass hash and the code-cache key cover a chain on/off);
    this patch makes the cone diffuse no longer ACCUMULATED where a field is
    bound (it is still computed), and Jahshaka's `JahIfd` piece adds
    `vctDiffuse × (1 − ifdConfidence)` under a chain — the cone term arrives
    exactly once in all four permutations (chain/single × field/no field). The
    single-volume arm keeps the hemisphere fallback; `--engine-selftest` hash
    UNCHANGED (one volume). Guarded by `gi.cascades` case 4's field variant
    (0.51× of the single volume's bounce beyond cascade 0, bar ≥ 0.40) and the
    red-on-grey bounce (0.0960 with the gate open, 0.0001 without).

46. **0046-hlms-shader-hash-field-sizes** (SOURCE) — THE SHADER HASH'S FIELDS,
    MEASURED AND REBALANCED. Every shader lookup is a 32-bit hash of
    `[type][renderable][pass]`, upstream 3/21/8, and the PASS index's eight bits
    are 256 distinct sets of pass properties for the life of a process, guarded
    by an `assert` every release build compiles out. Entry 256 carries into the
    renderable field beside it, which is the 2026-09-14 crash (patch 0035's guard
    and lane SHADERCACHE-2's recycled capture name fixed the symptom and the
    driver; the field stayed). MEASURED on the instrumented pin, in one session
    that opened the eight shipped samples, walked Low→Epic on each, drove every
    World Mode row through every option, changed the sky forty times, played and
    screenshotted: HlmsPbs peaked at **115** pass entries (45 % of the field),
    HlmsLowLevel 94, HlmsUnlit 12; renderable peaks 24 / 104 / 16; and a WARM
    boot replays 79 of the previous session's pass entries out of the disk cache
    before a frame is drawn. The split is now **3 / 16 / 13** — 8,192 pass
    entries (71× the peak) and 65,536 renderable ones (630×), the fields tiling
    the uint32 exactly under a `static_assert`. Both caches now REFUSE rather
    than overflow, through ONE appender (`Hlms::findOrAddPassCache`) that
    replaced three open-coded copies — HlmsUnlit's had no warning at all — and
    that throws on the render path, returns false while a disk cache is being
    replayed (a slow start, never a dead boot) and warns once at three quarters
    full; `getMaterial` asserts the two halves of the hash cannot overlap. The
    RENDER QUEUE's 10-bit shader sort field is part of the patch: it used to be
    the LOW bits of the renderable hash, which is the hole the pass field lives
    in (two arbitrary bits before, a constant at any wider pass field), and is
    now composed from the hash's type and renderable FIELDS — what
    `RqBits::ShaderBits`' comment always claimed. Draw order changes; opaque
    pixels cannot (depth-tested), transparents sort by depth above this term so
    only ties inside one depth bucket move — measured identical on the selftest,
    13 sample ports and every deterministic shipped sample. CRUD: `InputLayoutShift`/`InputLayoutMask` were declared and never
    defined; deleted. Adds `Hlms::getPassCacheSize`/`getRenderableCacheSize` +
    capacities, which Jahshaka reports through `app.shaderCache()` and
    `shadercache.app` run 5 now asserts as a NUMBER instead of the absence of a
    warning. On-disk formats are layout-INDEPENDENT (HlmsDiskCache stores
    property vectors and re-derives the indices; the Vulkan microcode map is
    keyed by shader source text), so `c_hlmsDiskCacheVersion` stays at 6.
    `--engine-selftest` hash UNCHANGED (`b55e2d5d…`); 13/13 Ogre sample ports and
    7/8 shipped samples byte-identical (Particles differs by the same pixels
    between two runs of the UNPATCHED binary). **It touches the same files as
    0035, so a tree carrying 0035 must reset its ogre-next submodule before
    `build-ogre.sh`.**

47. **0047-pcc-placement-keeps-the-depth-it-measured** (SOURCE — lane R5-ROOM;
    this entry was transcribed from the patch's own header by lane
    SKY-FALLBACK-1, which found the list stopping at 46 while the stack held 47)
    — `PccPerPixelGridPlacement::buildEnd` already takes one averaged 1x1 depth
    value per cube face and throws it away after fitting the probe's shape. The
    fit cannot be read backwards into it (1 % padding, `snapToFullRegion`,
    `snapToSides`, and a saturation at twice the region distance), so a host
    cannot tell "this probe sees a wall just inside the region" from "this probe
    saw nothing and was snapped back to it" — which is exactly the distinction
    the per-probe keep rule has to make. The patch KEEPS the six floats per
    probe, in CubemapSide order, and adds the accessor; the fit itself is
    byte-for-byte the code it was. Upstream-worthy as it stands.

48. **0048-where-no-probe-is-the-sky-is** (MEDIA — `ForwardPlus_DecalsCubemaps_piece_ps.any`
    and `Vct_piece_ps.any`; OVERLAPS 0017, 0029 and 0031 in the first and 0021,
    0033 and 0045 in the second (0028 names that file in its prose but has no
    hunk in it): a tree carrying them fails
    `build-ogre.sh`'s reverse-check with "Upstream changed the patched file" —
    the documented blind spot, cured by the submodule reset) — the PBS env-probe
    slot has ONE occupant, and under automatic PCC it is the probe cube ARRAY, so
    the engine takes the sky cubemap off every datablock the moment one probe
    exists (a manual cube there does not compile — three ways, listed on
    `OgreScene::reflectionTexFor`). Harmless while a grid meant a room; a
    REGRESSION since patch 0047 made the grid a per-probe decision, because a
    PARTIAL grid is now the normal case (one crate in a new project keeps 2 of
    18) and every pixel no surviving probe's box contains was left with cone
    tracing alone — measured as `scripting.e2e.default_ground`'s grazing margin
    falling 5/255 → 3/255. The sky gets a SECOND slot at the pass level through
    the extra-pass-texture route HlmsPbs honours for it
    (`getNumExtraPassTextures` / `propertiesMergedPreGenerationStep` /
    `hlmsTypeChanged`; upstream's Terra sample is the reference use, and the
    route needs no engine change — the slot lands inside `set0_texture_slot_end`
    and so inside the root layout HlmsPbs already builds). The host half is
    `FogHlmsListener` and the declaration is in Jahshaka's own
    `JahFog_piece_vs_piece_ps.any`; THIS patch is only the composite, which has
    to live in upstream's probe loop because nothing else can see whether a probe
    answered. The test is the loop's own and BINARY — did any probe's box contain
    this point (`getProbeFade > 0`) — because both quantities that look like
    coverage were built and measured NOT to be one: `cubemapAccumWeight` is an
    inter-probe blend weight that is divided out again (0.006 at the centre of a
    four-probe room), and the raw depth inside a probe's box is ~0.001 on every
    wall and floor, the box being shrink-fitted to those very surfaces. So a
    pixel any probe covers is bit-identical to before, and only the pixels with
    no probe answer at all change — to the real sky, at the Sky Light's gain and
    the sky cube's own mip LOD (both in Jahshaka's pass float4, because
    `ambientUpperHemi.w` and `envMapNumMipmaps` belong to the probe array here).
    Specular and clear coat only; `cubemaps_as_diffuse_gi` stays masked off. AND
    IT IS A SWAP, NOT A REPLACEMENT: a pixel no probe covers may still be inside
    the voxel volume, where the cone has a real answer, so `Vct_piece_ps.any`
    exports what the cone's ESCAPE contributed (the flat ambient at
    `specAlpha × blendWeight`, per cascade) and the composite is
    `cone − ambient×escape + sky×escapeFraction` with the cone's hit untouched —
    the colour removed carries upstream's 1/π because that is what was added, the
    weight the sky enters at does not, because every other cubemap sample in this
    shader is in radiance units. Inert for upstream (every line is inside
    `@property( jah_sky_env_probe )`). KNOWN LIMIT, recorded: the test is binary,
    so the sky arrives at once at the outermost box face — 10/255 measured on a
    reflective ground in a new project with a crate; the fix is a margin on the
    FIT, a later lane. `--engine-selftest` hash UNCHANGED (`b55e2d5d…`).

49. **0049-cubemapprobe-set-is-idempotent** (SOURCE — `OgreCubemapProbe.{h,cpp}`;
    **every tree resets its ogre-next submodule and re-runs `build-ogre.sh`, and
    that rerun COMPILES**) — `CubemapProbe::set` applies its 1.005 padding on
    EVERY call. The padding is right for the CALLER's boxes (adjacent probe areas
    that tile a region exactly would leave a crack; a shape must contain its
    area) and is kept. It is wrong for a caller that must re-publish a probe
    WITHOUT re-authoring it, which Jahshaka now has: after the grid is placed at
    the scout's 32 px and the candidates that saw nothing are dropped, the cube
    array is re-created at the kept count and the tier's resolution, and that
    drops each probe's internal probe — the GPU-side copy of these very values —
    so every survivor is published again. Handing a probe back its own
    `getArea()`/`getProbeShape()` grew both boxes half a percent per call:
    measured, the probe union left the region (gi.pcc_bounds' A2 invariant) and
    gi.budget's paused re-capture read (g−r) +0.259 where it reads +0.380. The
    patch adds a trailing `bValuesAlreadyPadded`, defaulted to false — every
    existing call site is byte-identical — and the one caller that passes true is
    the re-create. Upstream-worthy as it stands. The alternative was dividing by
    1.005 in our own code, i.e. copying a private constant out of the pin, which
    is the workaround this tree refuses.

50. **0050-irradiance-field-const-buffer-size-and-vct-zero-multiplier**
    (SOURCE — `OgreIrradianceField.cpp`, `OgreVctLighting.cpp`; OVERLAPS 0044
    and 0037 on those files; **every tree resets its ogre-next submodule and
    re-runs `build-ogre.sh`, and that rerun COMPILES**). **AMENDED AND RENAMED
    2026-09-17** (lane FIELD-RASTER-CRUD): items (2) and (3) below existed only
    for the rasterised probe source deleted that day, so their hunks — the
    public `switchToRasterSource()` with its four accessors, and patch 0023's
    C++ half in `IrradianceFieldRaster` — are gone from the patch, which no
    longer touches `OgreIrradianceField.h` or
    `OgreIrradianceFieldRaster.{h,cpp}` at all. Three engine-side compensations
    remain, turned into the pin fixes they were compensating for (audit
    `SPECS/audits/ENGINE_WORKAROUNDS_TO_PATCHES_2026-09-15.md` F2/F3/F4/F10 plus
    the `unused0` nit). (1) `IrradianceField::getConstBufferSize()` returned 20
    floats while `fillConstBufferData` writes 24 — the size of the struct the
    generated shader declares — so HlmsPbs reserved 16 bytes too few and
    advanced its write pointer 16 bytes short: upstream's samples overrun their
    pass-buffer map, and with an HlmsListener next in line (ours) the listener's
    first float4 landed on the field's irradiance-atlas parameters and every
    DDGI UV collapsed onto texel 0. The engine's answer had been to reserve four
    extra floats and deliberately SKIP them, which worked only because HlmsPbs
    fills the field's block before calling the listener; ~60 lines of
    `OgreFog.cpp`/`EnginePrivate.h` are deleted with it. (2) [WITHDRAWN
    2026-09-17 with the raster source — was the public `switchToRasterSource()`
    plus four accessors.] (3) [WITHDRAWN 2026-09-17 — was patch 0023's C++ half,
    the `invFieldSize` push in `IrradianceFieldRaster`.]
    (4) `IrradianceFieldGenParams::unused0` was uploaded uninitialised on
    every voxel-sourced update. (5) `VctLighting::update`'s auto multiplier
    inverted a zero maximum radiance into +inf, so a scene with no visible
    light contributed NO GI at all (and, since a bound VctLighting kills the PBS
    ambient inside its volume, went black); the zero case now falls back to
    `mBakingMultiplier`, which is what the engine was choosing from outside — by
    walking EVERY node before every injection (`OgreScene::hasVctLights`, three
    call sites, deleted).

51. **0051-reserved-texture-pool-is-resident-next-too** (SOURCE — overlaps 0056 in `OgreTextureGpuManager.cpp`;
    `OgreTextureGpuManager.cpp`; **rerun COMPILES**) —
    `TextureGpuManager::reservePoolId` transitions the pool master to Resident
    but never sets its NEXT residency, because `_transitionTo` only syncs the two
    for a ManualTexture. The master therefore read "Resident, on its way to
    storage", and the next `scheduleTransitionTo( Resident )` on it queued a FILE
    load for a texture with no file — the streaming worker memcpy's from a null
    mip pointer. The engine carried the one line from outside after its own
    `reservePoolId` call for the decal atlases; every caller needs it, so it
    belongs here. Audit F12.

52. **0052-planar-reflections-active-actor-slots** (SOURCE — a header:
    `OgrePlanarReflections.h`; **rerun COMPILES, and every consumer of the header
    recompiles**) — `getNumActiveActorSlots()` / `getActiveActorWorkspace( slot )`
    over the `protected` `mActiveActorData`. Each active actor slot renders
    through its own workspace, which instantiates its own shadow node and pass
    list, and the lamp-map cache and the render-loop monitor both have to reach
    those; the engine derived `JahPlanarReflections` purely to expose two lines,
    and that class is deleted. Audit F13.

53. **0053-setsky-records-the-method-it-was-given** (SOURCE —
    `OgreSceneManager.cpp`, far from 0016's hunk in the same file; **rerun
    COMPILES**) — `mSkyMethod` was assigned in the constructor and nowhere else,
    so `getSkyMethod()` answered `SkyCubemap` for the life of the scene manager
    whatever sky was up — including in the round trip
    `setSky( false, getSkyMethod(), … )` the API invites. The engine's own
    `mSkyIsEquirect` mirror (three writes, one read — the equirect sky needs a
    WRAPPED u axis and a cube sky must not get it) is deleted. Audit F14.

54. **0054-atmospherenpr-names-its-sky-quad** (SOURCE — a header:
    `OgreAtmosphereNpr.h`; **rerun COMPILES**) — `getSky( sceneManager )` returns
    the `Rectangle2D` the component created for that scene, from the `protected`
    `mSkies` map. Jahshaka must move that quad to render queue 0 (upstream's 212
    is after our on-top overlays, which write no depth) and give it the engine's
    visibility bit (Instant Radiosity's rays must never hit the sky), and with no
    accessor the engine found it by diffing the SceneManager's Rectangle2D set
    across the `setSky` call — eighteen lines and two movable-object iterations,
    deleted. Audit F15.

55. **0055-rectangle2d-is-born-with-geometry** (SOURCE — `OgreRectangle2D2.cpp`;
    **rerun COMPILES**) — the constructor left `mPosition`, `mSize` and
    `mNormals` UNINITIALISED; `initialize()` fills the vertex buffer from them
    and `setGeometry()` only raises a dirty flag, while the only `update()` call
    in the whole engine is `SceneManager::_renderPhase02` on its OWN sky quad. So
    every other Rectangle2D in a process drew from whatever the stack held,
    silently, unless its author knew to call `update()` by hand (which is why
    Jahshaka's sun disc does, after the symptom cost a lane an afternoon). Born
    as the full-screen quad `SceneManager::setSky` sets, rays zeroed. A trap fix,
    not a line saving: the engine keeps its explicit `update()`. Audit F16.

56. **0056-groupless-load-must-not-call-a-null-listener** (SOURCE —
    `OgreTextureGpuManager.cpp`, ~500 lines from 0051's hunk; **rerun COMPILES**)
    — both load paths detect "no archive and no loading listener", log
    `"ERROR: Did you call createTexture with a valid resourceGroup?"` at
    LML_CRITICAL, and then call `grouplessResourceLoading()` on that null
    listener two lines later: the diagnostic upstream wrote for the mistake is
    followed by a null-pointer call on a worker thread. The deref is guarded and
    `data` stays null, which takes each path's existing no-stream route — the
    multiload worker passes the request on ("as if multiload was turned off", its
    own comment for the archive-throws case) and the streaming worker substitutes
    `mErrorFallbackTexData`. A missing resource group becomes a logged error and
    a visible error texture instead of a process death. Audit F17.

57. **0057-a-cascade-added-after-bounces-were-enabled** (SOURCE —
    `OgreVctLighting.cpp`, overlapping 0037 and 0050 in that file; **rerun
    COMPILES**) — `runBounce()` sets `hlms_num_vct_cascades` from the chain's
    length on every injection and the generated compute shader declares one
    light-voxel texture per cascade (four with anisotropy), but the job's texture
    UNIT COUNT is only ever derived in
    `setAllowMultipleBounces()`/`resetTexturesFromBuildRelative()`. Chaining the
    cascades AFTER enabling bounces — an order the header does not forbid, and
    the order this engine uses — therefore produced
    `'ogre_t6' : unrecognized layout identifier`, the bounce program failed to
    compile, and the WHOLE VCT arm stayed unbound: Photon's cascade chain with
    multiple bounces rendered no GI at all, every time, with one log line to say
    so (measured, `spikes/patches-1/gi-reenable-before-0057.log`). `addCascade()`
    now re-derives the job when bounces are on, so either order works. The
    shared-job residual (one `HlmsComputeJob` found by name for every
    `VctLighting` in the process, the class of 0037's defect 2) is recorded in
    the patch header; the gate is `gi.reenable`.

58. **0058-irradiance-field-piece-hooks** (MEDIA —
    `Samples/Media/Hlms/Pbs/Any/IrradianceField_piece_ps.any`; no Ogre recompile,
    but Studio stages Hlms media from the SOURCE tree, so **the patch loop must
    have run in that tree**) — five `@insertpiece` hook points in upstream's
    `applyIrradianceField` (prologue / probe-origin / per-probe / resolve /
    post) plus one defect fix, `float r = max( length( dir ), 1e-6 )` (a probe
    sitting exactly under the shaded pixel made `dir` NaN and poisoned the whole
    cage sum). It replaces a WHOLE-BODY COPY of that piece in
    `irisgl/engine/media/Hlms/Jahshaka/JahIfd_piece_ps.any`, which existed
    because what Jahshaka adds to DDGI is not a post-multiply: the sample point
    moves before the cage is derived (the paper's normal/view bias), the cage's
    probe indices must be clamped to the grid (upstream's own comment promises
    it and upstream cannot do it — only Nx and Nx*Ny are in the render params,
    so the per-axis counts a clamp needs are not in the shader; unclamped, the
    Showroom 2 roof took 77 % of its cage weight from the FLOOR layer), two
    quantities are accumulated per probe, and the field's answer is scaled and
    then complemented. The copy was guarded by a configure-time sha256 of
    upstream's file that FAILED the build when upstream moved — the guard was
    the tell. Both are gone: ~200 copied lines out of our media file, 31 lines
    of CMake (the hash check and its `JAH_IFD_PIECE_HASH_OK` escape hatch) out
    of `irisgl/engine/CMakeLists.txt`. PIXEL-EXACT, measured rather than argued:
    the `--engine-selftest` hash is byte-identical with the hooks replacing the
    copy, and the default scene has a field bound, so that hash covers every
    term in the piece; 50/50 on gi.*, scripting.e2e.gi_*, page_gi, lights.* and
    test_engine, `gi.ddgi_raster` included. Audit F6 — whose premise (a "2-line
    hook before the final multiply") the file refuted: three of the five
    divergences MODIFY upstream statements rather than add to them, which is why
    the composite's scale is a variable instead of a post-multiply (it keeps the
    arithmetic and its order identical to the copy's).

59. **0059-mesh2-set-lod-values** (SOURCE — `OgreMain/include/OgreMesh2.h`,
    `OgreMain/src/OgreMesh2.cpp`; **every tree re-runs `build-ogre.sh`**; no
    other patch touches those two files, so there is no overlap to reset for) —
    adds `void Mesh::_setLodValues( const LodValueArray & )`, ~10 lines. There
    is otherwise NO public way to give a v2 mesh built in memory its LOD levels:
    `mLodValues` is protected with only a const getter (OgreMesh2.h:417),
    `_setLodInfo`'s entire body is commented out (OgreMesh2.cpp:360-373) so
    calling it does nothing silently, and the only writers left are the
    serializer (a friend) and `importV1`. Everything else already works — a
    SubMesh takes as many VAOs as you push into `mVao[VpNormal]`,
    `SceneManager::updateAllLods` runs every frame, `LodStrategy::lodSet`
    binary-searches the array and `RenderQueue` indexes the VAO list with the
    result — so an importer-driven engine can build a perfect LOD chain that
    Ogre will never select from. Required by ATOM stage 1 (SPECS/NANITE_SPEC.md
    §7), which bakes the chain at import; the header documents the contract
    (one entry per level in VAO order, entry 0 the strategy's base value, sorted
    the way the active strategy sorts, set before any Item is created because
    `Item::_initialise` caches the array's address). It restores a capability
    upstream commented out and is a good upstream PR as it stands.
    `--engine-selftest` byte-identical (`b55e2d5d…`).


60. **0060-bounce-bindings-per-dispatch** (SOURCE —
    `Components/Hlms/Pbs/{include,src}/Vct/OgreVctLighting.{h,cpp}`; **every tree
    re-runs `build-ogre.sh`**; it overlaps 0037/0050/0057 in the .cpp and 0050 in
    the header, different functions, so a per-patch reverse-check reports it
    "unapplied" for those files on a tree that carries them — judge by content)
    — `runBounce()` re-asserts the injection job's texture bindings before it
    dispatches. The pin writes the EXTRA CASCADES' light-voxel slots ONCE, in
    `setupBounceTextures()`, and every cascade swaps its own `mLightVoxel[0]` at
    the end of every bounce iteration it runs; after an ODD number of them the
    job would read the texture that cascade has just stopped writing (a swap is
    an involution, so even counts come back to where they started). On the
    shipped four-cascade table at three total bounces the counts used to resolve to
    1/2/4/8 (measured under JAHSHAKA_GI_DEBUG — an earlier "1/3/7" was
    arithmetic on the wrong precedence; since PHOTON-M1 every cascade runs the
    document's own count, so three bounces is 2/2/2/2 and two is 1/1/1/1), so only cascade 0 was odd and nobody's
    bounce reads cascade 0 (the irradiance FIELD does, which is why the engine
    re-binds it after every bouncing update). THE DEFECT THAT ACTUALLY RED THE
    SUITE is the other one this hunk closes — patch 0057's recorded residual and
    the shared-job class of 0037's defect 2: the job is found BY NAME, so its
    bindings belonged to whichever `VctLighting` called setup last; the engine
    builds a chain outermost-first, so cascade 0 configured the shared job LAST
    and every outer cascade dispatched its bounces through cascade 0's voxels —
    a whole-picture corruption at any count above one under a chain. Now the
    bindings belong to the one dispatching. `setupBounceTextures` gained a `bSetSamplerRefs` parameter
    (default true — every existing caller is unchanged) so the per-dispatch call
    skips the OpenGL-only samplerblock reference counting, which would otherwise
    leak a reference per bounce; the GLSL unit list's own change guard is upstream's (it fires every dispatch once extras exist; inert on Vulkan).
    Suite: `gi.cascade_bounce_bindings` — the same light reached by two
    different histories must render the same picture.

61. **0061-voxelizer-octants-follow-the-region** (SOURCE —
    `Components/Hlms/Pbs/{include,src}/Vct/OgreVctVoxelizer.{h,cpp}`; **every tree
    re-runs `build-ogre.sh`**) — `dividideOctants()` COPIES the region into every
    octant, and `build()` uses that copy twice: it culls the items against it and
    passes its minimum as the voxelisation shader's world origin.
    `setRegionToVoxelize()` replaced the region and left the octants describing the
    box it had just replaced, so a voxeliser that MOVES — which is every
    camera-following cascade — voxelised the OLD box's geometry at the OLD origin
    into a texture the shader maps onto the new one. It is the class's own
    invariant broken in one of two siblings: `VctImageVoxelizer::setRegionToVoxelize`
    ends with `mOctants.clear()` and its caller re-divides. Re-derives (rather than
    clears) so that no existing caller is left with an empty octant list — an
    assert in debug and a black volume in release. Measured: a document rendered
    42,42,42 in the session that created it and 47,47,47 after a reopen.
    Suites: `gi.cascade_determinism` case 1 (220/255 before, 0 after),
    `scene.reopen_fidelity`.

62. **0062-voxel-merge-and-dispatch-order** (SOURCE **and** MEDIA —
    `.../Vct/OgreVctVoxelizer.{h,cpp}`, `.../Vct/OgreVctMaterial.{h,cpp}`,
    `Samples/Media/VCT/Voxelizer_piece_cs.any`; **every tree re-runs
    `build-ogre.sh`**; shares the two VctVoxelizer files with 0061, different
    functions) — two halves of one defect. MEDIA: the per-voxel merge wrote
    `voxelNormal.a = max( voxelNormal.a, voxelNormal.a )`, an upstream no-op typo
    for `max( flag, origNormal.a )`, so the double-sided flag at a voxel is the
    LAST dispatch's answer and a dispatch that adds nothing there clears it; the
    flag is read per light (`abs(NdotL)` against `saturate(NdotL)`), so junction
    voxels are systematically mis-lit. SOURCE: which dispatch is last was the
    iteration order of a `std::map` keyed on buffer POINTERS — the same scene in
    two processes is two orders. Ordered by what a bucket IS instead (the job's
    variant, the material's pool and slot);
    `VctMaterial::DatablockConversionResult` gains the pool index it already knew.
    Upstream knows the class: `OGRE_FORCE_VCT_VOXELIZER_DETERMINISTIC` in the same
    header orders MeshPtrs by name for the same reason and does not cover this map.
    Measured: the same geometry attached in the opposite order rendered 6/255
    apart before and 1/255 (the voxel's own 8-bit step) after.
    Suite: `gi.cascade_determinism` case 3.

63. **0063-microcode-cache-for-reflected-array-bindings** (SOURCE —
    `OgreMain/include/OgreRootLayout.h`,
    `RenderSystems/Vulkan/include/OgreVulkanProgram.h`,
    `RenderSystems/Vulkan/include/OgreVulkanRootLayout.h`,
    `RenderSystems/Vulkan/src/OgreVulkanProgram.cpp`; **every tree re-runs
    `build-ogre.sh`**; no overlap with any other patch) — the microcode cache
    can now hold a shader whose root layout was DISCOVERED by reflecting its own
    SPIR-V. Symptom: with Photon's VCT cascade chain on, every launch after the
    first two recompiled exactly three shaders forever —
    `20/33/44LightVctBounceInject_cs`, the 4-, 3- and 2-cascade permutations of
    the bounce injection. Cause: `LightVctBounceInject_cs.glsl:13` sets
    `uses_array_bindings` for any cascade count above one,
    `HlmsCompute::compileShader` answers with
    `setAutoReflectArrayBindingsInRootLayout(true)`, and upstream's microcode
    read (`OgreVulkanProgram.cpp:325`) and write (`:751`, its own TODO) are both
    gated on `!mReflectArrayRootLayouts`. Such a shader is compiled TWICE cold —
    once to find its arrays, once against the layout they imply — and the second
    compile's preamble (the microcode key is source + preamble) depends on what
    the first found, so an entry stored under it could never be looked up. The
    patch captures the key in `loadFromSource()` before anything compiles, and
    frames the blob as `magic | array bindings | SPIR-V` so a hit can put the
    reflected bindings back into the root layout instead of recompiling. The
    frame is length-checked, which is also the only bounds check between that
    file and `vkCreateShaderModule`. The blob format changes, so every existing
    shader cache is one generation stale — Jahshaka's cache fingerprint carries
    `JAHSHAKA_OGRE_PATCH_SERIES` and discards it automatically. Upstream's second
    case (no custom root layout at all) is deliberately NOT taken: that layout
    lives in the shader's own source, which is not parsed when the key is built.
    Measured: `shadercache.app` run 3 compiled 3 of 110 before, 0 of 113 after.
    Suite: `shadercache.app` (its unchanged `compiledThisRun == 0` assertion).

64. **0064-voxelizer-lod-level** (SOURCE —
    `Components/Hlms/Pbs/include/Vct/OgreVctVoxelizer.h`,
    `Components/Hlms/Pbs/src/Vct/OgreVctVoxelizer.cpp`; **every tree re-runs
    `build-ogre.sh`**; the same two files as 0061 and 0062, applied after both,
    no overlapping hunks) — `VctVoxelizer::addItem` can be told WHICH MESH LOD to
    voxelize. The pin read `mVao[VpNormal].front()` — the finest level — in all
    five places it walks geometry (`countBuffersSize`,
    `prepareAabbCalculatorMeshData`, `convertMeshUncompressed`, `addItem`'s own
    index check, `placeItemsInBuckets`) and took no argument that could say
    otherwise; the only alternative, forcing the Item's live `mCurrentMeshLod`,
    belongs to the CAMERA (`updateAllLods` rewrites it every frame for what is
    drawn) and would not have been read by the voxelizer anyway. Why a voxelizer
    wants one: a voxel grid cannot represent detail finer than its own cell, so
    Photon's outermost cascade (1.875 m per voxel at High) voxelizes a mesh
    simplified to a 0.9 m error to exactly the same voxels for a fraction of the
    raster dispatch — that is ATOM stage 1's far-field proxy
    (SPECS/NANITE_SPEC.md §7). The shape: a new `QueuedMesh::lodLevel` (the level
    belongs to the MESH inside one voxelizer, because its buffers are downloaded
    and converted once per mesh; when items disagree the FINEST wins, the same
    precedence `bCompressed` uses), a trailing `lodLevel = 0u` argument on
    `addItem`, and ONE static helper `getLodVao( subMesh, lodLevel )` replacing
    the five `.front()` reads, clamped to the levels the mesh has. Every existing
    caller passes nothing, gets 0 and produces the buffers it always did; a mesh
    with no LOD chain is unaffected. The sibling `VctImageVoxelizer` is NOT
    patched: it caches a voxelized mesh in `VoxelizedMeshCache` keyed by the mesh,
    so a level would have to join that key, and Jahshaka's cascade chain drives
    the rasterizing voxelizer only. Consumer: `OgreScene::cascadeVoxelLod`, whose
    caller resolves the level per MESH (the MIN over the cascade's attach set)
    before it adds anything, so the engine's reading describes what the voxelizer
    HOLDS rather than what an item asked for.
    Measured (gi.cascade_lod's fixture): the outermost cascade voxelizes 1,676 of
    32,780 triangles, 5.1 %; on 1,000 imported 6,768-triangle spheres its rebuild
    falls 3,017 -> 225 ms of GPU. `--engine-selftest` UNCHANGED — the default
    scene's meshes are document primitives, which carry no chain. Suite:
    `gi.cascade_lod`. THE FRACTION OF THE CELL IT ASKS FOR CHANGED IN ATOM-3
    (the render audit's A2): half a cell moved that lattice's picture by up to
    112/255 because a voxel's occupancy is a binary triangle-box test, so it is
    a measured 1/256 now and the outer cascade takes level 2 (4x) instead of
    level 4 (16x). The patch itself is untouched — it is the hook, not the rule.
    RESIDUAL, recorded (NANITE_SPEC §7 stage 1): one level per mesh per voxelizer
    means a mesh instanced at SEVERAL SCALES in one cascade is voxelized at the
    finest of their levels, so the proxy is worth less on mixed-scale instances
    of one imported mesh; the real shape is a `(mesh, level)` cache key — days of
    work, and its own item.

65. **0065-order-independent-voxel-merge** (SOURCE **and** MEDIA —
    `.../Vct/OgreVctVoxelizer.{h,cpp}`, `Samples/Media/VCT/Voxelizer.material.json`,
    `Samples/Media/VCT/Voxelizer_cs.{glsl,hlsl,metal}`,
    `Samples/Media/VCT/Voxelizer_piece_cs.any`, and ONE NEW media file
    `VoxelMerge_piece_cs.any`; **every tree re-runs `build-ogre.sh`**; shares the
    two VctVoxelizer files with 0061, 0062 and 0064 and `Voxelizer_piece_cs.any`
    with 0062, different regions, applied after all of them) — the voxelisation's
    per-voxel merge is ORDER-INDEPENDENT, so a bucket can be a material POOL again.
    THE COST IT REMOVES: `build()` issues one compute dispatch per bucket per
    octant, each sized by the WHOLE OCTANT however few instances it holds. Patch
    0062 put the material SLOT in the bucket key to buy determinism from an
    order-dependent merge; that is one whole-volume dispatch PER MATERIAL, and the
    editor gives every primitive its own material, so on the 8,026-instance
    lattice the outermost cascade's rebuild went **86 -> 325 ms** (3.8x). Scenes
    that share materials paid nothing.
    THE MECHANISM IT FIXES: the merge was a RUNNING MEAN into the 8-bit voxel
    textures (`mixAverage3/4`) — an 8-bit round trip per dispatch, so which
    instances landed in which dispatch decided the picture; and the normal's
    "these surfaces face opposite ways" test was made WHILE summing, against the
    sum so far, so the first triangle a thread reached decided which way a voxel
    faced. Both are order dependences no re-solve can correct, because the VOXELS
    differ.
    AMENDED IN PLACE (fix round 1, the lead's second read): the double-sided flag
    was a MEMBERSHIP test and is now the 120-degree one (F1 below), the
    accumulator lost the three channels that never carried anything, and the two
    unsigned sums saturate. The patch was unpushed; a tree carrying the earlier
    0065 resets its ogre-next submodule and re-runs `build-ogre.sh`, which is the
    recipe a SOURCE patch asks for anyway.
    THE SHAPE: a dispatch adds its triangles to a new transient accumulator
    (`mMergeAccumTex`, PFG_R32_UINT, THIRTEEN texels per voxel interleaved in Z:
    albedo sum rgba, raw normal sum xyz, emissive sum rgb, FOLDED normal sum xyz;
    upstream's `voxelAccumVal` stays the triangle counter) as exact
    fixed-point integer SUMS (12 fractional bits, a contribution clamped to
    [0, 16], so a contribution is at most 65,536 and 65,535 of them are the whole
    uint32 bar one — the two unsigned sums therefore SATURATE rather than wrap,
    and saturating addition is still commutative and associative; the snap is 6 %
    of one 8-bit step), then writes the mean of the TOTAL the accumulator holds into the
    three voxel textures — so whichever dispatch is last leaves the complete
    answer and no separate resolve pass is needed. The double-sided decision is
    made there from the two normal sums: folded = U - F and raw = U + F (U the
    normals the fold left alone, F the ones it turned round, in their original
    directions), so U = (raw + folded)/2 and F = (raw - folded)/2.
    **F1, THE FOLD'S SEAM — the decision is GEOMETRY, not membership.** "Both
    groups are non-empty" is the wrong question: the fold's seam runs along the
    six arcs where a normal's largest component changes, and a smooth or faceted
    surface crosses them everywhere — (0.72, 0, -0.69) and (0.69, 0, -0.72) are
    six and a half degrees apart and land one in each group. Membership calls that
    voxel double-sided, the injection takes abs(NdotL), and a single-sided surface
    is LIT FROM BEHIND: the leak class, on every sphere, cylinder, character and
    curved wall. So the test is upstream's own 120 degrees asked of the two group
    MEANS — `dot(U, F) < -0.5 |U| |F|`, compared squared. The normal is then the
    RAW mean when it is one surface (upstream's answer when its test excluded
    nothing) and the LARGER group's mean when it is two (what upstream's exclusion
    left behind). Taking the larger and not U is load-bearing: a voxel whose
    normals ALL lie in the folded half-space (a wall facing -X) has U = 0 and
    would be stored black — four `gi.leak_room` assertions caught it.
    RESIDUALS: two normals ~170 degrees apart can both land in U and leave a
    near-zero normal (the dark side, not the leaking one); and on a voxel holding
    THREE clusters — nearly every voxel of a 0.2 m wall at an outer cascade's
    1.9 m cell — upstream keeps whichever cluster its first triangle seeded and
    this keeps the larger fold group, which on an edge voxel differ by a
    reflection. Neither is more right; only one is the same answer twice.
    Measured on gi.field_follows' far red bounce: 0.0210 for the pin, 0.0174 here,
    against a GI-off control of 0.0000 — that suite's bar is re-anchored from 0.02
    to 0.012 with the three numbers written into it.
    `VoxelizerBucket::materialSlotIdx` is RETIRED (0062's source half, and only
    that half; its media half is superseded by the resolve computing the flag
    rather than merging it), and the key keeps 0062's pointer-free ordering.
    `getNumBuckets()/getNumOctants()` are exposed so the host can report the
    dispatch count (`GiStatus::CascadeStatus::voxelDispatches`,
    `world.giStatus().cascades[].voxelDispatches`).
    **R32_UINT AND NOT RGBA32_UINT — THE CAUSE CORRECTED, THE CHOICE RE-MEASURED
    (VOXMERGE-2 / render audit ON-6, 2026-09-18; no logic moves, but two hunk
    PAYLOADS do — the class comment and the media piece's layout comment — so
    EVERY TREE RESETS THE SUBMODULE AND RE-RUNS `build-ogre.sh`, as for any 0065
    amendment: the earlier text fails the reverse check on three files, the staged
    media differs, and the voxel-merge permutations take one microcode-cache miss
    on the first warm launch after it).** This entry used
    to say that clearing a PFG_RGBA32_UINT 3D uav through
    `ComputeTools::clearUavUint` HANGS THE GPU on NVIDIA 595.84 (`Xid 109 CTX
    SWITCH TIMEOUT`, 8/8 on `samples.cleanstart.Showroom{,_2}`, 0/4 with that call
    removed) and that the thirteen-scalar interleave was bought against it. THE
    ATTRIBUTION WAS WRONG: patch 0067 established that the hazard was a
    device-local block recycled ONE FRAME early while three were in flight — the
    accumulator's whole-volume clear was simply the first and biggest write into
    recycled memory — and patch 0071 then stopped this accumulator being recycled
    at all (it is created with the voxel textures and stays Resident).
    RE-MEASURED ON THAT STACK, with a full RGBA32_UINT arm built and verified
    (four 128-bit texels per voxel, one clear; `gi.cascade_determinism`,
    `gi.leak_room`, `gi.field_follows`, `gi.cascades`, `gi.cascade_dirty` green on
    it and both `--engine-selftest` poses byte-identical, so it was a working
    implementation): **the 128-bit clear is CLEAN — 16/16 rounds of
    `samples.cleanstart.Showroom{,_2}` with ZERO Xid, against 16/16 for the
    shipped layout.** And the four-texel layout buys nothing: on the same
    8,026-instance lattice at Photon High (clocks locked, GPU timestamps, three
    runs an arm, mean GPU ms per rebuild) R32 read c0 11.7/14.5/11.3,
    c1 14.9/15.4/14.5, c2 20.2/18.3/16.6, c3 77.3/80.4/79.5 and RGBA32 read
    c0 14.3/15.5/12.5, c1 18.3/15.5/15.3, c2 20.2/19.1/19.0, c3 85.3/80.9/75.9 —
    the run-to-run spread is larger than any difference. Which is what the
    arithmetic says: the merge touches only voxels a dispatch contributed to,
    while the CLEAR touches every voxel of the volume and writes MORE bytes in the
    RGBA32 layout (64 per voxel against 52). So the thirteen-texel layout stays on
    its own merits — 12 MB less per 64^3 volume and 25 MB less per 128^3, RESIDENT
    since 0071, which at Photon High is 245 MB held against 301. The RGBA32 arm is
    kept as a measured artifact in `spikes/engine-small-b/`.
    WHAT IT COSTS: 52 bytes per voxel — 13.6 MB at 64^3, 109 MB at 128^3 —
    RESIDENT for the voxeliser's life since patch 0071 (it was transient here;
    the per-build round trip of an image that size was the second Xid 109 site),
    plus thirteen scalar texels read and written per touched voxel. The
    voxelisation job gains ONE uav slot (7 instead of 6) and nothing else about
    its bindings moves.
    Measured (lane VOXMERGE-1, GPU clocks locked, the lattice at Photon High, GPU
    timestamps, mean/max ms per rebuild, two runs per arm): c0 13.7/20.4 and
    14.3/22.7 -> 13.8/19.6 and 13.4/16.5; c1 34.3/90.1 and 29.8/73.4 -> 17.3/30.7
    and 17.1/30.4; c2 73.9/175.5 and 68.5/164.0 -> 18.8/29.3 and 18.3/27.6;
    **c3 324.8/333.6 and 329.1/331.1 -> 85.5/86.8 and 84.8/85.3**, 3.9x and at the
    86.8/88.9 E2 measured before 0062 landed. Showroom 2, whose materials are
    shared, pays the accumulator's traffic on its near cascades and is repaid on
    its far one: c0 4.21 -> 4.81, c1 4.31 -> 4.85, c2 3.77 -> 3.80,
    c3 7.84 -> 5.06.
    `gi.cascade_determinism` case 3 goes 6.00/255 -> **0.00/255** — exact, not
    merely inside the 8-bit floor. `gi.leak_room`'s new seam case (two quads 2 mm
    apart whose normals straddle the fold) reads **0.0000 of bounce on the side
    away from the lamp against 1.0000 for a genuinely two-sided pair**, and
    **1.0000** on the membership test this replaces. `--engine-selftest`
    byte-identical (`ead9a2ce...`), and so is the single-volume picture of the
    same scene.
    Suites: `gi.cascade_determinism` (which gains case 4 — 200 objects with 200
    materials in two attach orders), `gi.cascades` case 14 (the dispatch count is
    a handful, not one per material), `gi.field_follows`, `gi.leak_room`,
    `samples.cleanstart.Showroom{,_2}` and `scripting.e2e.movable_lamp_rest`
    (both of which the RGBA32_UINT shape reddened and this one does not).


66. **0066-vct-cascade-march-carries-the-cone** (MEDIA —
    `Samples/Media/Hlms/Pbs/Any/Vct_piece_ps.any`; **every tree resets the
    ogre-next submodule and re-runs `build-ogre.sh`** for the staging, no engine
    recompile of its own; it OVERLAPS 0021, 0033, 0045 and 0048 in the same file,
    so the per-patch reverse-check reports it "unapplied" on a tree that already
    carries those — the documented blind spot, judge by content) — a CASCADED
    CONE MARCH MUST CARRY THE CONE ACROSS THE HOP. PHOTON-E3 measured a staircase
    in the ambient a surface receives under a camera-centred chain: one step per
    cascade face, 1.15-1.24x each, travelling with the camera, a factor 1.9 from
    the far world to the eye, the same RATIO under a flat ambient and under the
    analytic sky at noon and at 5 degrees (so not a spherical-harmonic band
    mismatch), and absent from the single fitted volume. SEAM-1 found the two
    terms in `computeVctProbe`'s continuation loop. (a) The ESCAPE opacity is
    re-seeded from the COLOUR opacity at every hop (`float escapeAlpha =
    startingAlpha;`, and `startingAlpha` is the caller's `result.alpha`) —
    patch 0021's own "small approximation only that path pays", which is in fact
    the staircase, because `escapeAlpha <= alpha` always and every hop raises the
    escape to the colour opacity. (b) The cone's AGE is reset at every hop:
    `dist` both positions the samples and sizes the cone, and restarting it at
    the new cascade's entry re-anchors the cone's apex ~0.87 cells behind where
    it belongs, so the whole march runs at a footprint that does not match the
    cone's solid angle. The fix carries both — `VctResult::travelled`, and
    `startingEscapeAlpha` / `startingTravelled` arguments, with the age converted
    into the next cascade's units by `dot( abs( dir ),
    fromPreviousProbeToNext[j-1][0].xyz )`, the same directional scale upstream
    uses for `vctInvResolution`. The first cascade is arithmetically identical
    (the trace floors the age at `vctInvResolution`, which is what `dist` starts
    at), and that is measured, not asserted: the default scene with the chain OFF
    is sha256-IDENTICAL between the two medias
    (`7407fff2ded7f6462fb2ca3f23f11d292484b6395d56931922814132a25a5799`).
    Measured (`gi.chain_face`, the 400 m slab at High, the +/-20 m reading):
    the cascade-0 face 0.591 -> 1.000x (flat), 0.604 -> 1.003x (hemisphere);
    the cascade-2 face 1.138 -> 1.000x; the ring nearest the eye went from 0.567x
    to 0.959x of the ambient a pixel outside the chain receives — PHOTON_SPEC
    §9's "cascade march residual (0.51x)", measured at 0.96x. Four faces became
    one, and the one left (0.821-0.839x) sits at the tier table's 128^3 -> 64^3
    cell-size jump, not at a cascade hop. `--engine-selftest` MOVES (chain ON):
    `ead9a2ce...` -> `1fd91da9...`, max 4/255, mean 1.06, 36.9 % of pixels over
    2/255, every changed channel DARKER. Suite: `gi.chain_face`, re-anchored to
    0.75-1.35x. NOT DONE, each measured first: terminating the march on the
    escape opacity instead of the colour opacity is bit-identical (the march
    never reaches alpha 0.95 in these scenes), and dropping upstream's second
    `( 1.0 - result.alpha )` de-amplification moves an interior hall by 0.6 %.
    THE SPECULAR WALK TAKES THE ESCAPE CARRY AND NOT THE AGE CARRY, measured:
    the specular trace runs to `maxLod = 11` (it leaves the BOX rather than
    handing over at a matched footprint) and its ambient rides the RAW `alpha`,
    not patch 0021's `min3` estimate, so a true age there is right geometry fed
    to an opacity estimate that is already too high — `gi.cascades` case 7 fell
    from 1.00 / 0.85 / 0.85 to 1.00 / 0.43 / 0.34. Passing 0 restores upstream's
    arithmetic exactly on that walk. The honest cure for the specular side
    (give `specAlpha` the same `min3` estimate: measured 1.00 / 0.88 / 0.87, and
    better than the pin at every roughness) moves the SINGLE-VOLUME reflection
    too, so it is its own lane.
    UPSTREAM-REPORTABLE: (b) is upstream's, and its own
    `LightVctBounceInject_piece_cs.any` carries the same reset plus the
    `result.alpha += newRes.alpha` opacity doubling that patch 0033 fixed on the
    pixel side.

67. **0067-delayed-blocks-wait-for-their-frame** (SOURCE —
    `RenderSystems/Vulkan/src/Vao/OgreVulkanVaoManager.cpp`; **every tree re-runs
    `build-ogre.sh`**; it shares that file with 0039 but not its regions) — A
    FREED GPU BLOCK IS REUSED ONE FRAME TOO EARLY. `deallocateVbo` delays a free
    as `DirtyBlock( mFrameCount, ... )` and `flushGpuDelayedBlocks` returns it to
    the pool once `( frameCount - frameIdx ) >= 1u` — "GPU -> GPU resources are
    safe to reuse after 1 frame of synchronization" — behind a
    `waitForTailFrameToFinish()`. But this render system keeps
    `mDynamicBufferMultiplier` (3) frames in flight and that wait covers the TAIL
    frame, not the frame the block was freed in: a block freed during frame N is
    handed to a new resource at N+1 while frame N's command buffer may still be
    executing, and the two then ALIAS the same device memory with no
    synchronisation. On NVIDIA 595.84 the second resource's writes do not fault —
    they HANG THE CHANNEL: `NVRM: Xid 109 CTX SWITCH TIMEOUT` followed by
    `vkWaitForFences failed VK_ERROR_DEVICE_LOST` and a device-lost recreate that
    never returns. Patch 0065's merge accumulator is what made it constant: a
    transient 109 MB (at 128^3) R32_UINT 3D uav paged OnStorage at the end of
    every `VctVoxelizer::build()` and back to Resident at the start of the next,
    whose first traffic is `clearVoxels`' whole-volume clear — the biggest
    immediate write into just-recycled memory this engine performs, once per
    cascade, and Photon rebuilds a cascade per frame. Measured on `mcp.e2e`
    (lane XID-1, cold home every run, the kernel log followed and every app pid
    matched): tip 7 fails / 8 with an Xid each and none on a pass; `0065` removed
    0/8; only its `clearUavUint` removed 0/8; that clear writing 1 instead of 0
    5/8; the accumulator reshaped X-major at the same texel count 6/8; the
    accumulator kept RESIDENT 0/8; `JAHSHAKA_GI_SWEEPS=2` 0/8 and patch 0066
    removed 4/8 (both only change how much of frame N is still running when N+1
    recycles). With this patch — accumulator still transient, clear still there —
    **0 fails / 16, zero Xid**. Both tests become `>= mDynamicBufferMultiplier`,
    which is exactly the window the caller's wait already establishes; it costs
    two extra frames of retention on blocks that are already delayed, and nothing
    on the `flushAllGpuDelayedBlocks` path (that one frees behind a full memory
    barrier on purpose). `--engine-selftest` unchanged (`1fd91da9...`): the patch
    moves no pixels, only when memory is recycled. UPSTREAM-REPORTABLE, and not
    VCT-specific: any consumer that frees and re-requests a large device-local
    block every frame or two can hit it, which is where Jahshaka's standing
    Xid 109 sightings (VOXMERGE-1's two, the scoped gate's log.perf red) came
    from.

68. **0068-external-device-renders-and-is-created-honestly** (SOURCE —
    `RenderSystems/Vulkan/src/OgreVulkanRenderSystem.cpp`,
    `RenderSystems/Vulkan/src/OgreVulkanDevice.cpp`,
    `RenderSystems/Vulkan/include/OgreVulkanDevice.h`; **every tree re-runs
    `build-ogre.sh`**; it shares those files with 0002/0007/0013/0038/0040 in
    different regions, so the per-patch reverse-check can report the overlap
    false positive — judge by content and reset the submodule first) — THE
    EXTERNAL-DEVICE ROUTE, WHICH VR NEEDS, RENDERED NOTHING AND WAS CREATED ON A
    LIE. Under `XR_KHR_vulkan_enable2` the OpenXR runtime creates the VkInstance
    and the VkDevice and Ogre is handed both (`VulkanExternalInstance` /
    `VulkanExternalDevice`). Two defects at the pin, both inside code the render
    system keeps to itself:
    **(1) the frame veto.** `VulkanRenderSystem::validateDevice()` returned
    `false` for ANY external device or instance, lost or not, and
    `Root::_fireFrameStarted()` treats `false` as a veto — so an engine booted on
    the runtime's device rendered NOTHING, every frame, silently. It now returns
    `!mDevice->isDeviceLost()` for the external case. Recovery stays impossible
    on purpose: `handleDeviceLost()` (reachable only through `validateDevice`)
    recreates the instance, the device and every resource, which is meaningless
    for a device the runtime owns and whose swapchain images it holds — a lost
    external device ends the XR session and the process restarts. The plain path
    is byte-for-byte unchanged.
    **(2) the device Ogre would have built, exported, and the enabled set read
    back honestly.** `createDevice()` is SKIPPED on the external path, so its ~17
    device extensions and its five-struct `VkPhysicalDeviceFeatures2` chain (16-bit
    storage, shaderFloat16/int8, pipeline cache control, 0038's ray-query set,
    0013's fifo_latest_ready) were the caller's to reproduce — and a copy of that
    list in our own TU would rot the day a patch changed it. The selection is now
    exported and the plain path calls the same code:
    `VulkanDevice::fillDeviceExtensionRequest()` (createDevice's extension loop),
    `fillDeviceFeaturesFor()` (fillDeviceFeatures' opt-in set),
    `buildFeatureChain()` (fillDeviceFeatures2's whole body) and
    `buildDeviceCreationRequest()`, which fills a caller-owned
    `VulkanDeviceCreationRequest` whose `extensions` / `pNext()` / `features` feed
    a `VkDeviceCreateInfo` verbatim (that is what `xrCreateVulkanDeviceKHR` is
    handed). And the matching half: on the external path `fillDeviceFeatures2()`
    queried the PHYSICAL DEVICE and recorded what the hardware SUPPORTS as though
    it had been ENABLED — `shaderFloat16` + `storageInputOutput16` decide
    `RSC_SHADER_FLOAT16`, which moves both the Hlms variants and the shader-cache
    fingerprint — so `VulkanExternalDevice` gains an OPTIONAL `creationRequest`
    pointer: supply the request the device was created from and Ogre records the
    enabled set from it; omit it and the old behaviour stands, with a
    `LML_CRITICAL` line saying so. Default-initialised to null, so upstream's own
    `Tutorial_VulkanExternal` is unaffected.
    MEASURED (lane VR-1A, `spikes/openxr-vulkan`, Monado 25.0.0's simulated HMD
    on this box): before the patch the external route draws the clear colour for
    ever; after it, 551,969 of 902,272 pixels of a lit scene, 60 `xrEndFrame`s,
    and — the parity that matters — the picture the runtime-created device renders
    is **sha256-identical** to the picture Ogre's own device renders at the same
    pose (`3b2a7e48…`), with `DeviceInfo` and the whole capability line equal
    (17 extensions, `shaderFloat16` 1, `storageInputOutput16` 0, cache control 1,
    ray query 1, `RSC_VP_AND_RT_ARRAY_INDEX_FROM_ANY_SHADER` 1). Jahshaka's
    `--engine-selftest` is unchanged (`1fd91da9…`): both hunks are dead on the
    non-external path. FIX ROUND 1 added three things a second read asked for:
    `VulkanDeviceCreationRequest` is non-copyable by declaration (its pNext chain
    points into itself, so a copy would hand `vkCreateDevice` a chain that walks the
    original); the BASE `VkPhysicalDeviceFeatures` is taken from the request too, not
    only the chain's bits, with `mSupportedStages` re-derived from it (`mDeviceFeatures`
    was still read from what the GPU SUPPORTS, and geometry/tessellation are exactly the
    two bits that stage mask is built from); and the external path no longer re-logs
    "Found device extension" for every extension on the device, while the "hardware ray
    query" verdict is logged after the override rather than only from the support query
    before it.

69. **0069-device-loss-must-not-abort-from-a-destructor** (SOURCE —
    `RenderSystems/Vulkan/src/Vao/OgreVulkanStagingBuffer.cpp`, one file, touched by no
    other patch in the stack; **every tree re-runs `build-ogre.sh`**) — EVERY DEVICE
    LOSS THAT REACHES THE RECOVERY PATH ABORTS THE PROCESS. `~VulkanStagingBuffer`
    waits on its last fence unconditionally (`:58-59`) and `wait()` ends in
    `checkVkResult`, which THROWS on a bad `VkResult` (`:142-150`); a destructor is
    implicitly `noexcept` since C++11, so a throw leaving it is `std::terminate`. And
    `handleDeviceLost()` — the render system's ONLY recovery path — destroys every
    staging buffer on its way through `destroyVkResources` →
    `VaoManager::deleteStagingBuffers`. So a lost device does not produce a clean
    exception and does not produce a recreate: it produces SIGABRT, by construction,
    not by luck. Found by lane VR-1A's gate in a suite with nothing to do with VR:
    `scripting.e2e.physics` died "Subprocess aborted" with a kernel-confirmed
    `NVRM: Xid 109 CTX SWITCH TIMEOUT` naming that app's own pid, and the backtrace is
    `_fireFrameStarted → validateDevice → handleDeviceLost → destroyVkResources →
    deleteStagingBuffers → ~VulkanStagingBuffer → wait → __cxa_call_terminate → SIGABRT`
    (evidence `~/Developer/spikes/openxr-vulkan/xid-physics-crash.log`). THE FIX is one
    guard: skip the wait when the device is already lost. A fence on a lost device can
    never be signalled, so the wait is not merely dangerous but meaningless;
    `vkDestroyFence` and the pool return below it are both legal on a lost device, so
    nothing else changes, and the skipped wait logs one `LML_CRITICAL` line so the loss
    is never silent. The device is reached exactly as `wait()` reaches it (through the
    VaoManager, whose pointer the destructor already computed further down and which is
    simply hoisted). On a healthy device the condition is false and behaviour is
    identical — `--engine-selftest` unchanged (`1fd91da9…`). The rest of that teardown
    path already uses `stallIgnoringDeviceLost` for exactly this reason; this was the
    only throwing wait left on it. HONEST RESIDUAL: whether a fresh `vkCreateDevice`
    succeeds after an Xid 109 — i.e. whether `handleDeviceLost` can RECOVER rather than
    merely fail cleanly — is a separate question this patch does not answer and could
    not answer without provoking a device loss deliberately; what it establishes is the
    floor, that a device loss ends in a clean throw or a working recreate and never in
    an abort. The Xid 109 class itself is lane XID-2's. UPSTREAM-REPORTABLE.
    **AND THAT RESIDUAL IS NOW OBSERVED, NOT SPECULATED** (one sample, VR-1A's fix-round
    gate): with this patch in, `log.perf` met an Xid 109 (kernel line, its own pid) and
    its log runs `vkWaitForFences … VK_ERROR_DEVICE_LOST` → the "Deleting mapped buffer"
    warnings → **nothing**. No `VulkanStagingBuffer::wait failed`, no crash file, no
    SIGABRT — and no return either: ctest reported a TIMEOUT. The same suite class
    before this patch (`scripting.e2e.physics`, same gate, same box, pre-0069) aborted
    at exactly the staging-buffer destructor. So the destructor defect is fixed at its
    cause and the NEXT one on that path is exposed: `handleDeviceLost`'s recreate does
    not return after an Xid 109. A hang is not better than an abort for a user — it is
    only better for a diagnosis — so XID-2 owns bounding or abandoning that recreate.
    Neither failure mode is provokable on demand, so neither claim is a rate; both are
    single observations with their kernel lines beside them
    (`~/Developer/spikes/openxr-vulkan/`).

  0071-the-merge-accumulator-stays-resident — THE SECOND Xid 109 SITE (lane XID-2,
    2026-09-17), SOURCE, one file, amends 0065's own lines. Patch 0065 gave the
    order-independent merge accumulator upstream's transient treatment: OnStorage at
    the end of every `VctVoxelizer::build()`, Resident at the start of the next. That
    volume is thirteen texels per voxel (13.6 MB at 64^3, 109 MB at 128^3) and under a
    Photon cascade chain a build happens every frame or two, so a LARGE 3D storage
    image was created and destroyed continuously while the dispatches that wrote the
    previous one may still have been executing. On NVIDIA 595.84 that hangs the
    channel: NVRM Xid 109 CTX SWITCH TIMEOUT -> VK_ERROR_DEVICE_LOST. Measured on the
    owner's own sequence scripted (hide and show a plane in an Epic scene): 4/4 and
    6/6 runs lost the device with the round trip, 0/6, 0/6 and 0/16 without it, every
    failure pid-matched to a kernel Xid line and no passing run ever carrying one. It
    is NOT patch 0067's delayed-block window (16 frames still hangs 4/6), NOT the
    512 MB force-flush (disabled: 6/6), NOT the cached image views (purged on
    residency loss: 6/6) and NOT the ray-query tier (rays off: 6/6). The cost is one
    accumulator per voxeliser held for its lifetime — memory the next build asked for
    again a frame later anyway. Sharing one scratch volume across a chain's cascades
    is a Photon optimisation, not a correctness matter.

  0072-a-lost-device-ends-the-session — THE OTHER HALF OF 0069 (lane XID-2,
    2026-09-17), SOURCE, four files in the Vulkan render system. 0069 stopped a device
    loss from ABORTING out of a destructor and uncovered that the recreate never
    returns. XID-2 caught it with gdb on a live hang: the block is `vkDestroyDevice`
    ITSELF (`VulkanDevice::destroy` <- `setPhysicalDevice` <- `handleDeviceLost` <-
    `validateDevice` <- `Root::_fireFrameStarted`), spinning at 99.4 % of a core inside
    libnvidia-glcore, still spinning ten minutes later — so no bounded WAIT could have
    fixed it. And the recreate could not recover anyway: the engine holds Vulkan
    objects on that VkDevice outside the render system (the ray-query tier) and is
    never told. So `validateDevice` no longer recreates a LOST device: it logs one
    critical line and returns false (a device ELECTION on a live device still
    recreates), and `VulkanDevice::destroy` / `VulkanQueue::destroy` skip
    `vkDeviceWaitIdle` when the device is lost, as 0069 did for the staging fence. The
    HOST then has to end the process without an orderly teardown — `Engine::deviceLost()`
    plus `src/viewport/devicelossend.h` do that in Studio. Measured on the reproducer
    with 0071 deliberately removed: before, 4/4 and 6/6 FROZEN; after, 4/4 ended in
    27-55 s with one `VK_ERROR_DEVICE_LOST` line instead of 4,348, the fatal sentence
    in the session log and on stderr, exit code 3, no crash file. What it does NOT do
    is make `vkDestroyDevice` safe; a real recreate is a program.

  0073-swapchain-rebuild-retires-its-semaphore — A SWAPCHAIN REBUILD MUST NOT DESTROY
    A SEMAPHORE THAT STILL HAS A PENDING SIGNAL (lane VR-3b, 2026-09-17), SOURCE, one
    file (RenderSystems/Vulkan/src/OgreVulkanWindow.cpp — shared with 0008 and 0013,
    so expect it on the reverse-check's overlap list). `acquireNextSwapchain` hands a
    pooled semaphore to `vkAcquireNextImageKHR`, which leaves a SIGNAL pending that
    only a queue submit waiting on it can consume — `vkDeviceWaitIdle` cannot. But
    `destroySwapchain` called `notifySemaphoreUnused`, a bare `vkDestroySemaphore`, so
    every rebuild that happened between an acquire and the next frame's render freed a
    semaphore mid-operation; the driver hands the same handle back and the recreated
    swapchain's acquire gets it. Measured on the rig under VK_LAYER_KHRONOS_validation,
    twelve VR entry/exit cycles (a VR session start turns vsync off, which rebuilds the
    swapchain twice): 20 reports of VUID-vkAcquireNextImageKHR-semaphore-01779 —
    "Semaphore must not have any pending operations", the layer's duplicate limit, two
    per cycle — before, ZERO after. The patch retires the semaphore through the
    deferred `notifyWaitSemaphoreSubmitted` route `swapBuffers()` already uses, after
    submitting the wait that consumes the acquire's signal; and `setVSync` /
    `setWantsToDownload` now stall before rebuilding, which the sibling rebuild site
    (VulkanXcbWindow::windowMovedOrResized) already did. It is the standing candidate
    for the owner's WiVRn `VK_ERROR_DEVICE_LOST` with no Xid at a session's first frame
    (the rejection is synchronous, the GPU never hung) — not proof: that crash has not
    been reproduced on the simulated runtime.

  0074-cascade-continuation-composites-once — THE CASCADE CONTINUATION COMPOSITES THE
    TRANSMITTANCE ONCE, AND THE BOUNCE-INJECTION WALK CARRIES ITS CONE (lane PHOTON-M1,
    2026-09-17; the 2026-09-17 render audit's PHOTON F8 + F9), MEDIA-only, two files
    (Samples/Media/Hlms/Pbs/Any/Vct_piece_ps.any — overlaps 0021/0033/0045/0048/0066/0070
    — and Samples/Media/VCT/LightVctBounceInject_piece_cs.any). A continuation trace
    STARTS its opacity accumulator at the caller's `result.alpha` and weights every
    sample it composites by `1 - alpha` from there, so what it returns already carries
    the transmittance; the composite multiplied by `( 1.0 - result.alpha )` a second
    time and every outer cascade contributed (1-A0)^2 of its radiance. Upstream calls
    that "de-amplification" (OgreVctCascadedVoxelizer.cpp:470-486) and pairs it with
    "more bounces for coarser cascades": both are one brightness stabilisation invented
    when the opacity was DOUBLING per hop, which patch 0033 fixed at the cause. The
    compute twin of the walk still had that doubling (`result.alpha += newRes.alpha`),
    so a bounce never travelled past cascade 1, and restarted the cone's age at every
    hop (0066's defect); both are fixed here. MEASURED: the default scene's selftest
    picture moves 10,686 px (max 1/255, all brighter) for the composite and a further
    4,590 for the walk; Showroom 2 at High reads 74.4583 -> 75.0694 mean probe against
    an unchanged single-volume 91.3056; the chain-OFF picture is byte-identical
    (7407fff2ded7f6462fb2ca3f23f11d292484b6395d56931922814132a25a5799 both sides) and
    the lightless slab suite gi.chain_face is bit-identical.

  (0075 — A BOUNCE IS NOT DIVIDED BY PI — was delivered by PHOTON-M1 and HELD at merge
    (the lead, 2026-09-17, after the Fable second read): correct in the voxel's own
    units, but one of three coupled defects in the same bounce job — the injection
    stores k*rho*E (pi times the outgoing radiance, and the RGBA8 voxel saturates on
    most lit surfaces) while the pixel consumes the gather as radiance, and the
    bounce recurrence re-gathers the TOTAL each pass ((1+rho*G)^n, never contracts).
    All three land together as PHOTON-M2 (0075 + a Jacobi bounce + the injection's
    1/pi), one SOURCE rebuild, one hash move, one re-anchoring.)
  0075-lod-switch-hysteresis — A LOD SWITCH HAS A HYSTERESIS BAND, AND THE BAND
    BELONGS TO THE PASS (lane ATOM-3, the render audit's A7, 2026-09-17; AMENDED IN
    PLACE the same day by lane ATOM-3-FIX, ledger §664 finding 1), SOURCE.
    `LodStrategy::lodSet` picks a level with one `lower_bound` and writes
    `mCurrentMeshLod`: the comparison is a step in BOTH directions, so an object whose
    LOD value sits on a threshold changes level every frame the value dithers.
    Measured on the rig with one chained mesh and a camera oscillating by 2 % of its
    switch distance: 71 level changes in 600 frames (one per threshold crossing) with
    no band, 0 with a 10 % one; at a 20 % amplitude both read 71, so the band removes
    the noise and keeps every real transition. It cannot be written outside the pin:
    `mCurrentMeshLod` and `mLodMesh` are protected and MovableObject grants friendship
    to `LodStrategy::lodSet`/`lodUpdateImpl` BY NAME, which does not extend to a
    derived strategy's override — which is also why the rest of that lane's view rule
    (the `jah_world_error` strategy) needs NO patch at all.
      THE AMENDMENT, and why the first version was wrong: the band was process-wide
    (`LodStrategy::setHysteresis`) and used `mCurrentMeshLod` as its direction state,
    but ONE SCENE IS RENDERED BY MANY PASSES AND THEY DO NOT SHARE A CAMERA — a
    planar reflector's mirrored camera, a PiP inset, a probe cube face and a thumbnail
    are all `pass_scene` definitions on the same SceneManager with `mUpdateLodLists`
    true and no LOD camera of their own, and every one of them wrote that slot. The
    watched view's band was therefore measured against another camera's level:
    measured at 1,760 moved pixels (the whole level-0/level-1 difference — the view
    drew the INSET camera's level, and stayed on it after the inset was switched off),
    0 with the amendment (`engine.lod_hysteresis`). So the band is now
    `CompositorPassSceneDef::mLodHysteresis`, DEFAULT 0 = upstream to the bit, threaded
    through `SceneManager::updateAllLods` (a defaulted 5th argument — no other caller
    changes) and `UpdateLodRequest` into `lodUpdateImpl`/`lodSet` beside `lodBias`,
    with its own per-object state slot `MovableObject::mHysteresisLod` (the last level
    a BANDED pass chose; only a banded pass writes it; 0xFF = none yet, so a capture's
    first frame is upstream's answer exactly; it costs no memory — the padding beside
    `mCurrentMeshLod`). THIRTEEN FILES now: OgreLodStrategy.h,
    OgreLodStrategyPrivate.inl, OgreMovableObject.h/.cpp, OgreRenderable.h (the two
    `friend` declarations name those functions BY SIGNATURE), OgreDistanceLodStrategy.h
    /.cpp and OgrePixelCountLodStrategy.h/.cpp (the three upstream strategies pass the
    band through; a pin bump that adds a fourth fails to COMPILE rather than ignoring
    the band), OgreSceneManager.h/.cpp, OgreCompositorPassSceneDef.h and
    OgreCompositorPassScene.cpp. Of those only `OgreMain/src/OgreSceneManager.cpp` is
    shared with another patch in this stack (0016 at ~1620 and 0053 at ~1081, far from
    this patch's ~2126), so a tree carrying those two can see this patch reported as
    "unapplied" by the per-patch reverse check — the documented overlap class; the cure
    is the submodule reset recipe before `build-ogre.sh`. `chain::build` writes
    `ChainDesc::lodHysteresis` onto every `PASS_SCENE` of the view's own node (all of
    them or none — they share the view's camera) and `OgreView::chainDesc()` sets 0.10
    for a view a person watches over time (on-screen, and the VR session's stereo view)
    and 0 for every capture, so thumbnails, previews, screenshots and pixel suites keep
    taking the exact level their own value asks for. `JAHSHAKA_NO_LOD_HYSTERESIS` still
    zeroes the band run-wide; the offscreen exception is a PER-VIEW field,
    `View::setLodHysteresisOffscreen` (LOD-LATCH-1, 2026-09-18 — it replaced the
    process-wide env latch `JAHSHAKA_LOD_HYSTERESIS_OFFSCREEN`, deleted), asked for by
    the one suite that can read pixels and by nothing else. The patch's PROSE HEADER
    was amended to say so in the same lane; no hunk of its diff mentions either name,
    so no tree pays a `build-ogre.sh` rerun for it (the script's per-patch check reads
    the hunks — `git apply --reverse --check` verified after the edit).

  0076-vct-bounce-is-a-jacobi-iteration — THE VOXEL BOUNCE IS A JACOBI ITERATION, AND
    ITS DAMPENING IS 1 (lane PHOTON-M2, the physics read of ledger §659 findings F-B
    and F-C, 2026-09-17), SOURCE + MEDIA, six files (OgreVctLighting.{h,cpp}, the two
    VCT compute shells and their two pieces; it OVERLAPS 0060/0070/0074 in the
    bounce-injection piece, so the reverse check false-positives on a re-run — reset
    the submodule, that is the documented blind spot). The bounce job read the RUNNING
    TOTAL as its base and added the gather to it, so the series was
    L_n = (1 + rho*G)^n * D — binomial, never contracting, the first bounce counted
    once per pass; upstream's 1/pi dampening, its commented-out per-pass one and the
    "more bounces for coarser cascades" stabilisation PHOTON-M1 deleted were three
    fudges against that one defect. The fix is the fixed point the volume's radiance
    satisfies, L = D + rho*G(L): the DIRECT injection is kept in its own volume
    (`mLightDirect`, one mip, +8 MB at 128^3 / +1 MB at 64^3 per cascade, created and
    destroyed with `mLightBounce`), written by a SECOND UAV of the same injection
    dispatch under a new `vct_keep_direct` property (one image store per voxel — no
    copy, no second dispatch, nothing on the CPU), read by the bounce job as
    `directVoxel` at the unit after every probe array, and `iterationDampening`
    becomes 1.0 because the six-cone gather is already an estimate of E/pi. Measured
    on a closed 5 m cube of albedo 0.791 (spikes/photon-m2/m2-bounceC.js): the
    per-pass increments run 0.88/0.52/0.60 at the base (ratios 0.59, 1.15 — not
    contracting), 2.00/1.88/1.96 with the dampening alone (0.94, 1.04 — plainly
    divergent) and 3.64/1.60/0.40 with this patch (0.44, 0.25 — contracting at
    rho*f). Selftest `277ba3c2…` -> `ebe346e1…` with 0077.
  0077-injection-stores-radiance — THE LIGHT INJECTION STORES RADIANCE, and the
    specular cone's eye-tuned 1/pi retires with the error it cancelled (lane
    PHOTON-M2, finding F-A / render-audit F2, 2026-09-17), MEDIA-only, two files
    (LightInjection_piece_cs.any, Vct_piece_ps.any; OVERLAPS
    0021/0033/0045/0048/0066/0070/0074 in the pixel piece and 0076 in the injection
    piece). The injection stored `NdotL * D * rho * shadow` = pi times the surface's
    outgoing radiance (D = colour*powerScale = pi*I in this tree), while the cone
    gather is consumed as `envColourD` with no division (200.BRDFs: Rd =
    envColourD*albedo) — so cone-lit diffuse was pi times brighter than the emissive
    and the ambient in the same voxel slot, and the RGBA8 store (scaled by
    pi/D_max) SATURATED wherever rho*NdotL*D/D_max > 1/pi, desaturating the bleed of
    every lit surface. The file's own `c_invPI` was VALUED pi and unused: it is given
    its name's value and applied, which also makes the store exactly 1 at the
    brightest fully-lit surface. Upstream's hard-coded `0.31831f` on the specular
    composite ("I'm not sure why is it even needed") was this pi found by eye and is
    deleted, together with its copy in patch 0048's exported escape colour so the sky
    swap stays exact. Measured against the analytic wall/floor form factor
    (spikes/photon-m2/m2-bounceA.js, which predicts rho_w/2 = 0.5 with no units in the
    answer): 0.800/0.726/0.637 at the base, 0.521/0.475/0.412 with this patch. It also
    un-darkens the SPECULAR ambient by pi (the scene's one ambient convention now
    reaches both slots the same), which re-anchored gi.volume_edge (+2/255 on a 24/255
    band, attributed by zeroing the term in staged media), gi.chain_face (1.029 ->
    1.057) and gi.rt_reflect (the no-ray control 0.0099 -> 0.0315, the ray's own
    answer unchanged). Selftest alone: `277ba3c2…` -> `8254977e…`.
  0078-custom-projection-publishes-its-frustum-extents — A CAMERA WITH A CUSTOM
    PROJECTION MATRIX MUST REPORT ITS OWN FRUSTUM EXTENTS (lane REFLECT-VR-1,
    2026-09-18), SOURCE-only, one file (OgreMain/src/OgreFrustum.cpp, no overlap
    with any other patch). `Frustum::calcProjectionParameters`' three branches all
    compute the extents; the two AUTO ones publish them to `mLeft/mRight/mTop/
    mBottom` (:380, :396) and the `mCustomProjMatrix` one (:330) publishes nothing
    — and the constructor initialises every other member of the class but not
    those four (OgreFrustum.h:137). So `getFrustumExtents()` on a camera given a
    custom projection matrix before its first `updateFrustum` — the normal way to
    use one, and what Ogre's own VR recipe does — returns INDETERMINATE values:
    measured on an OpenXR eye (near 0.05, true tangents 0.847 x 0.985) it answered
    (-1106.711, 1102.311, 825.453, -1147.873), wrong by a factor of ~1,300 and
    stable across frames. Anything that needs a frustum's SHAPE rather than its
    matrix reads it: our ray-per-pixel reflection basis (which is how it was
    found — `vrEyeScreenshot`'s per-eye control traced a frustum 1,300 times too
    wide), the compositor's `VIEW_SPACE_CORNERS_*` auto-parameters through
    `getWorldSpaceCorners`, upstream's own shadow setups — AND FORWARD+, which
    makes this a behavioural change and not only a corrected reading:
    `ForwardClustered::collectLights` pushes the current camera's tangents onto
    the camera it slices the light grid with (OgreForwardClustered.cpp:341-343),
    so such a camera culled its lights against a garbage frustum before the
    patch and against its own after. Nothing that ships today moves (the only
    custom-projection cameras here are the VR session's, whose grid is the CULL
    camera's, and `vrEyeScreenshot`'s mono control). The tangent form divides the
    published positions by the Frustum's OWN `mNearDist`, so it is right only
    while `setNearClipDistance` matches the matrix's near — asserted where the
    session builds the projection. The fix is the four
    missing assignments plus the four missing constructor initialisers, in the
    same units and convention (`FET_PROJ_PLANE_POS`) the other branches publish.
    A camera without a custom projection takes neither hunk: the selftest hash and
    the directional-lit pixel suites are unchanged (`ebe346e1…` either way,
    verified). NOT fixed: under a portrait `OrientationMode` (compiled out here)
    `updateFrustumImpl` calls the function with swapped arguments, so every branch
    publishes under swapped names — upstream's behaviour, untouched.

  0079-final-grade-is-dithered — THE FINAL GRADE'S 8-BIT WRITE IS DITHERED (lane
    DITHER-1, 2026-09-18), MEDIA-only, two files
    (Samples/Media/2.0/scripts/materials/HDR/GLSL/FinalToneMapping_ps.glsl and
    .../HDR/HDR.material), NO overlap with any other patch. The owner's "faint
    circular rippling in the ground plane while flying" is 8-bit contour banding:
    the renderer computes in RGBA16F, every target a person sees is 8-bit UNORM,
    10-bit output is not available on this path (LATER_OPTIMISATIONS L14), and
    there was no dither anywhere in the final grade. `HDR/FinalToneMapping` is the
    ONE place a float picture becomes display codes in this engine — every target
    it writes is 8-bit (the window, the offscreen RTT, the VR eye image, SMAA's
    LDR buffer, the looks stage's first buffer, the picture-in-picture inset) —
    so the dither is unconditional there and needed nowhere else. The dither
    itself is OUR media (irisgl/engine/media/Hlms/Jahshaka/JahDither.glsl,
    reached through Ogre's own `#include` + `enable_include_header`, which
    resolves through the resource GROUP): a deterministic interleaved-gradient
    offset of at most 0.498 of a code, keyed on the INTEGER pixel coordinate
    alone — no time term, so a still frame stays byte-identical — added to the
    shader's own output value, which in this engine IS the display code. 0.498
    and not 0.500 so that a value already sitting exactly on a code rounds back
    to it, which is what makes a dithered write over an already-quantised
    picture the identity.
    IT IS ALL INTEGER ARITHMETIC AND THE SHADER QUANTISES ITSELF, because
    neither the compiler nor the driver may decide the picture: a float
    `fract(52.98 * fract(dot(p,k)))` turns a 1-ULP difference into a completely
    different offset under fused-multiply-add contraction, and a float handed
    to a UNORM attachment lets the hardware break the rounding tie. So the
    noise is 32-bit unsigned (the same IGN, its constants as Q32 fixed point)
    and the shader writes `floor(v*255 + 0.5 + n)/255`. MEASURED: NVIDIA's and
    lavapipe's dropped-pixel sets on an unshaded frame are NESTED (12,339 a
    subset of 14,425, zero violations) — the same noise field thresholded at
    two slightly different fractions, which is what an identical integer noise
    looks like through two float pipelines that disagree by 1e-4.
    Measured (default scene, plain ground, 25 m overhead, 'scene' grade): the
    longest run of one code on a cut 475 -> 76 px, the radial profile's max
    annulus-to-annulus step 0.5756 -> 0.0243 of a code. `jahDitherOff` is the
    diagnostic switch (`PostFxDesc::ditherOff` per view, or JAHSHAKA_NO_DITHER
    read once for the process), named so that its SAFE value is zero — an
    unwritten constant buffer renders the CORRECT picture. Guarded by
    `hdr.dither` (ten arms, both pictures in one process) and, for the eyes, by
    `vr.session` + `vr.session_undithered`. The selftest hashes move by design:
    pose 1 `2bc1ab3a…` -> `3d2e88e7…`, pose 2 `a42ec3d6…` -> `90d06e1f…`. The
    same binary with the dither off gives `e4f35c84…`/`89335f30…` and NOT the
    pre-lane pair: taking the rounding of ties away from the driver moves 2.02 %
    / 1.72 % of the poses' pixels by exactly 1/255 on its own (flat regions
    sharing one float value that lands on a tie), and the dither then moves
    25.08 %.

  0080-vct-total-volume-is-a-float — THE VCT TOTAL VOLUME IS AN RGBA16F FLOAT
    (lane PHOTON-M3, 2026-09-20), SOURCE + MEDIA: OgreVctLighting.{h,cpp} and
    four VCT compute pieces (LightInjection, LightVctBounceInject, both
    AnisotropicMipVctStep pieces); overlaps 0076/0077 on the reverse check. The
    volume holds the fixed point of L = D + rho*G(L), which is unbounded — an 8-bit
    sRGB store CLIPPED 3-26 % of the lit voxels in Showroom 2, Mirror Room and
    World Background and showed 82 % of a white room's bounce. The TOTAL
    (mLightVoxel[0..3] + mLightBounce) is now PFG_RGBA16_FLOAT; the DIRECT volume
    (0076's D term) stays 8-bit sRGB because D is under the ceiling by
    construction. The store encoding in the media follows the bound FORMAT
    (jahStoreLight under `uav0_orig_pf_srgb`, jahStoreDirect under
    `uav1_orig_pf_srgb`): an sRGB store keeps its encode, a float one is written
    linear; every reader is a sampler read, so no reader changes. The total is NOT
    TextureFlags::Reinterpretable: a reinterpretable 16F texture is created as its
    format FAMILY (R16G16B16A16_UINT) and the mip chain's linear blit is then
    invalid (VUID-vkCmdBlitImage-filter-02001, found by the validation suite).
    `getLightDirectTexture()` lets the host measure the direct volume
    (Scene::giVoxelStats). Measured: no frame cost (4.36 vs 4.44 ms), +36/+56 MB
    resident. Numbers: spikes/photon-m3/MEASUREMENTS.md sections 2-7. The
    selftest hashes moved (2aadbc10… / 0f084cec… after GRID-2 on top).

  0081-vct-material-cache-evicts-a-dying-datablock — VctMaterial FORGETS A DYING
    DATABLOCK (lane MATERIAL-SWAP-GI-1, 2026-09-20), SOURCE-only:
    OgreVctMaterial.{h,cpp}, OgreVctVoxelizer.h; no overlap. The conversion cache
    is keyed by the RAW datablock pointer across builds, so a datablock destroyed
    and another created at the same address would alias the dead one's slot —
    the host's only answer to any material death was a from-scratch
    re-voxelisation of every volume (a hover preview's reclaim cost the world).
    `VctMaterial::removeDatablock` erases the cache entry; `VctVoxelizer::
    getVctMaterial` lets the host reach the cache (OgreGi's noteGiDatablockDied).
    The bucket keeps the dead pointer in its membership set on purpose: slots are
    numbered by that set's size, so erasing it would hand the next datablock a
    live one's slot — one slot per death leaks until the VctMaterial is recreated
    (a mode or quality change). With Scene::setNodeMaterial's in-place datablock
    swap the hover's cascade rebuilds went [2,2,2,2] -> [0,0,0,2].


  0082-bloom-composite-takes-an-amount — THE BLOOM COMPOSITE TAKES AN AMOUNT
    (lane BLOOM-AMOUNT-1, 2026-09-20), MEDIA-only, one file
    (Samples/Media/2.0/scripts/materials/HDR/GLSL/FinalToneMapping_ps.glsl),
    OVERLAPS 0079 — it adds a second member to the parameter block the dither
    introduced and edits the line above it, so a tree carrying both resets the
    submodule before `build-ogre.sh` rather than reading the per-patch reverse
    check's complaint as an upstream change. The owner asked (review R17) for
    "a value setting next to its check box so we can change the amount of bloom
    from 0 to 2x with current bloom being 1": the renderer had no strength term
    at all — `bloomThreshold` and `bloomKnee` say WHICH pixels bloom and nothing
    said how much of the blurred result reached the picture. The multiply is on
    the composite (`vSample += fromSRGB(bloom) * (16.0 * amount)`) and NOT in
    the ladder, for two reasons that are the whole content of the patch: the
    blur targets are R10G10B10A2_UNORM, so a scale above 1 applied before them
    CLIPS (the dim edge of a halo would brighten and its core would not move),
    and this quad reads the ladder through `fromSRGB`, which is a SQUARE, so a
    factor applied upstream arrives squared (2x would be 4x). The uniform is
    `jahBloomAmountMinusOne`, following 0079's rule in the same block: the value
    an unwritten constant buffer carries — zero — must be the picture this
    engine drew before the patch, so zero means 1x; the shader clamps the sum to
    the document's own 0..2. AMOUNT 1 IS BYTE-IDENTICAL (`clamp(1+0)` is exactly
    1.0 and `16.0*1.0` is exactly 16.0), and both selftest hashes are unchanged:
    pose 1 `2aadbc10…`, pose 2 `0f084cec…`. Amount 0 makes the added term
    exactly zero, which is what the bloom-OFF chain feeds this quad (a black
    texture), so the dial reaches the bloom-off picture with the compositor's
    shape untouched. Host side: `PostFxDesc::bloomAmount` ->
    `chain::setBloomAmount`, pushed per view beside the bloom threshold and from
    `OgreView::setPostFx` for the VR session's view; staged media without the
    patch logs one line and renders every scene at 1x. Guarded by
    `hdr.bloom_amount` (six arms on one emitter fixture in one process,
    including the RATIO that either wrong place for the multiply would fail and
    the workspace generation that makes it a draggable dial).

  0083-diffuse-cone-basis-is-the-normals — THE DIFFUSE CONE BASIS IS A FUNCTION
    OF THE NORMAL, IN THE WORLD (lane CUBE-SHADE-1, 2026-09-21), MEDIA-only, one
    file (Samples/Media/Hlms/Pbs/Any/Vct_piece_ps.any), OVERLAPS 0021, 0033,
    0045, 0048, 0066 and 0070 in that file — a tree carrying those resets the
    submodule before `build-ogre.sh` rather than reading the per-patch reverse
    check's complaint as an upstream change. The owner's review R8: a hard
    DIAGONAL across one flat face of a cube, corner to corner, with a stippled
    grain over it (screenshot 11), on a flat uniformly lit single-material quad.
    `applyVoxelConeTracing` traced its six diffuse cones in whatever frame
    happened to be in scope, which for every tangented mesh in this tree is the
    MATERIAL's TBN — `normalize( inPs.tangent )`, a per-VERTEX attribute
    authored for TEXTURING and interpolated from a different vertex triple
    inside each triangle of a quad — and everywhere else `generateTbn`, whose
    tangent is `OGRE_ddx( pos )` and whose own comment in the file said it "isn't
    fully stable". A cone DISTRIBUTION is not a tangent-space quantity: nothing
    about where six cones should point depends on the UV layout. And the error
    is large, because the cone set is not azimuthally symmetric against an
    ANISOTROPIC voxel field — rotating the frame about the normal changes which
    of the six directional volumes each cone reads. Measured on a 4 m cube at
    Epic, TURNING IT 90 DEGREES ABOUT ITS OWN AXIS (same solid, same place, same
    face normal): 92.9 % of the face's pixels moved, worst 16/255, mean 4.8 —
    1/255 (the dither) after. The per-pixel grain inside one triangle fell from
    3.28 display codes of high-frequency energy to 0.49, which is what the same
    face measures with GI off.
    THE FRAME IS ANCHORED IN THE WORLD, which is the second half of the fix and
    the one the lane's first cut got wrong. Everything in this shader is in VIEW
    space (the vertex shader multiplies the normal by worldView; VctLighting's
    xform is voxel * view^-1), so a frame built from `pixelData.geomNormal` as
    it stands turns about the normal with the camera — a swim while orbiting and
    a moving picture on every VR head turn, which the material tangent frame,
    being a world-attached attribute, never had. `passBuf.invViewMatCubemap` (in
    every non-caster Pbs pass buffer) takes the normal out to a
    camera-independent space vector-first and brings the frame back matrix-first,
    the matrix being orthogonal; the frame is composed back in ONCE so the cone
    loop is untouched, at two 3x3 products per pixel. Measured with the camera
    ORBITING the point it looks at — same world point, same normal, same voxels,
    each read drained until two consecutive reads agree to a quarter of a code —
    worst move over +-30 degrees of yaw, same-pose floor in brackets: stock
    5.79 (2.00), view-anchored 5.79 (2.00), world-anchored 1.00 (1.00). The
    view-anchored arm is NOT separable from stock at that angle (the residual
    both carry is the cascade's own re-centring as the camera position moves), so
    the magnitude of the temporal artefact is not established by that table; the
    argument for anchoring is structural and the third row's stability is what it
    buys. A ZERO-YAW camera cannot see the difference at all — with no yaw the
    view x-axis IS world x, so every axis-aligned surface's view-space normal has
    x = 0 and Frisvad's tangent lands on world x either way, which is why pose 1
    of the selftest is byte-identical under this patch and the yawed pose 2 is
    not.
    `buildConeBasis( n )` is Frisvad's orthonormal basis (JGT 2012) — continuous
    over the sphere but for ONE direction, where a branch returns a fixed frame
    instead of dividing by zero — and it is built about the SPACE'S OWN AXES,
    which is a measurement rather than a default. The hairy-ball theorem says
    some direction must be special, and once the frame is world-anchored that is
    a fixed direction OF THE WORLD, so it is a real choice. Steering it onto a
    body diagonal (a fixed pre-rotation, four lines) was built and measured and
    COSTS LIGHT: the six cones of an axis-aligned surface then all point at odd
    angles, where every sample is a three-way blend of the anisotropic voxel
    volumes instead of a near-direct read of one. On `gi.field_follows`' own bar
    — the red a wall bounces onto grey ground beyond cascade 0, against a GI-off
    control of 0.0000 — stock reads 0.0174, world-anchored on the axes 0.0174,
    and world-anchored on a body diagonal 0.0105, which REDS the suite (bar
    0.012): forty per cent of the bounce, on every surface in every scene. So
    the axes stay, and the price is stated rather than hidden — the special
    direction is a world axis, so a normal within a hair of it takes the
    branch's fixed frame while its neighbours take a rapidly turning one, and
    the two properties cannot be separated ("the frame is axis-aligned for axis
    normals" and "the reference direction is an axis" are the same statement).
    Duff's branchless variant is refused for a reason that survives either
    choice: its sign flip makes the discontinuity a great CIRCLE, not a point.
    THE OTHER TRADE: a world-anchored frame is invariant under camera motion and
    not under rotating a whole scene about an axis; a view-anchored one is the
    reverse, and cameras move every frame where whole scenes are rotated never.
    The material's own TBN is untouched; `generateTbn` had exactly one call
    site, this one, and goes with it.
    BOTH SELFTEST HASHES ARE UNCHANGED — pose 1 `2aadbc10…`, pose 2 `0f084cec…`,
    zero pixels of either — which is the strongest thing this entry says. The
    default scene's ground gets the same frame from the world-anchored Frisvad
    construction as it got from its own affine UV tangent, in EVERY pose
    including the yawed one, so the patch is a no-op exactly where the old frame
    already happened to be right and a fix where it was not. (The lane's first
    cut built the frame in VIEW space and moved 63.8 % of the yawed pose's
    pixels; that is what camera dependence looks like in a hash.) Guarded by
    `scripting.e2e.cube_shade`: one flat face must read as ONE surface across its
    own diagonal (worst per-height spread over five probe columns 3.00/255
    against 8.07 on the stock frame), and turning the cube about its own axis
    must barely move it (3.07/255 against 7.07). Neither residual is the frame —
    the turned orientation's five columns read identically at every height — and
    both are handed to LATTICE-1 as the VOXELISER's own dependence on a mesh's
    triangulation.


  0084-vct-escape-reads-both-axis-halves — THE VOXEL OCCUPANCY THE AMBIENT RIDES
    IS A PROPERTY OF THE CELL, NOT OF THE CONE (lane CLIFF-32-1, 2026-09-21),
    MEDIA-only, one file (Samples/Media/Hlms/Pbs/Any/Vct_piece_ps.any), OVERLAPS
    0021, 0033, 0045, 0048, 0066, 0070 and 0083 in that file (it amends one
    sentence of 0021's own comment) — a tree carrying those resets the submodule
    before `build-ogre.sh` rather than reading the per-patch reverse check's
    complaint as an upstream change. LATTICE-1's finding 2: a surface reads 8-9
    display codes BRIGHTER for standing more than 32 m from the world origin.
    Translate a whole scene AND its camera together by an exact multiple of every
    cascade's cell — so that not one voxel and not one light moves inside any
    volume, which LATTICE-1 proved (voxelsLit 16384/18660/4548/2960 and meanLit
    to six decimals, identical at every arm) — and the cube's shaded face 60 m
    out is 8.82/255 brighter than the same face at the origin, a STEP at 32 m and
    flat to 180 m.
    `isNegative` picks which of the two composites packed into an anisotropic
    axis texture — the one accumulated towards +axis or the one towards -axis — a
    sample reads, from the SIGN of that component of the cone's direction. For
    the COLOUR composite that is right and self-healing: the sample is weighted
    by `dirLSSquared`, so as a component goes to zero the half it chose stops
    mattering. Patch 0021's ESCAPE estimate is `min( escapeX, escapeY, escapeZ )`
    and a `min` carries no weight — it gives an axis whose direction component is
    ZERO the same authority as the axis the cone travels down, and on every
    axis-aligned surface in the world two of the three components ARE zero. Zero
    in exact arithmetic; in the shader they are the last bits of a round trip
    (the normal is interpolated in VIEW space, carried out through
    `invViewMatCubemap` to build the cone frame — patch 0083 — carried back, and
    rotated into probe space), and every rotation in it is built from the view
    matrix, which is a function of where the camera stands IN THE WORLD.
    MEASURED: `isNegative.y` for the first diffuse cone reads 0.5 on 100 % of the
    cube face's pixels at the origin and flips on 50.5 % of them (11,321 of
    22,400) with the scene 60 m out; the cone's `alpha`, its step count, its lod,
    its `travelled`, its start position in every cascade and every voxel it
    samples are IDENTICAL between the two arms to the last readable bit, while
    the escape weight — which is exactly what the sky's ambient rides
    (`light.w`) — moves by 26/255.
    THE FIX IS TO STOP ASKING THE CONE: the two halves of one axis texture are
    two measurements of the same cells composited in opposite directions, and
    their mean is the direction-free reading `min3` was always after (0021
    derived it as "the cell's mean occupancy up to the compositing"). Each half
    is still a COMPOSITE, so a wall one voxel thick still reports 1 along its own
    normal at every mip and the sealed-room leak the main volume's
    mean-occupancy alpha had stays shut; and it is continuous in the direction,
    so no last bit of a matrix can move a picture again. The face's dependence on
    the world origin: 8.823 -> 0.109 of a code at 60 m, 7.589 -> 0.104 at 120 m,
    0.746 -> 0.105 at 30 m (8x8-smoothed mean over the face, max 1). Guarded by
    `scripting.e2e.lattice_placement`'s translation arms (60 and 120 m, bar
    3/255, measured 1; 15/255 without the patch). SIX alpha fetches per
    anisotropic step instead of three, on the diffuse cones and on the specular
    one (the two marches share the loop; the specular cone's `escapeAlpha` is
    carried but never read downstream, so those three buy nothing and removing
    them is a follow-up, not a rider on a fix); the frame cost is below the
    instrument — Showroom 2 at Epic, four runs of 600 scripted frames per
    process, clocks locked, read 11.45-11.61 and 10.92-11.35 ms/frame in two
    patched processes against 10.98-11.03 in the base one, and
    `perf.epic_steady_state` / `perf.drag_mirror_room` / `perf.hide_soak` pass.
    BOTH SELFTEST HASHES MOVE, by design — the escape estimate is a different number everywhere.

  0085-movable-object-lod-level-is-settable — A PASS MAY CHOOSE THE MESH LOD
    LEVEL IT DRAWS (lane SURFACE-CACHE-1b, 2026-09-21), SOURCE, one file
    (OgreMain/include/OgreMovableObject.h), one public inline setter beside
    upstream's own `resetMeshLod()`. The surface cache captures one card per
    compositor pass and the level it must raster is not a screen size: it is
    the coarsest baked level whose simplifier error is below THE CARD'S OWN
    TEXEL — the ATOM rule spent at a texel instead of at a pixel, the same
    `lodLevelForWorldError` the cascade voxeliser spends at a cell through
    0064. Upstream derives `mCurrentMeshLod` only through
    `LodStrategy::lodUpdateImpl` / `SceneManager::updateAllLods`, which answers
    a different question, and offers a caller exactly two doors: a reset to 0
    and a read-only getter. The alternatives were a derived-class reach-in at a
    protected member or an ortho camera with a hand-picked LOD bias chosen so
    that upstream's screen-size formula happens to land on the level we want —
    a workaround that lives only while the pin's internals stay put, and a lie
    about what the number means. Nothing in the pin calls it; a pass whose
    `mUpdateLodLists` is true recomputes the level and overwrites it, which is
    what restores a view's own level on the same frame a capture borrowed the
    object (the capture pass sets `mUpdateLodLists` false for the same reason).
    OVERLAPS 0075 in that file (the LOD hysteresis band's `mHysteresisLod`), so
    a tree carrying 0075 reads the usual per-patch reverse-check complaint and
    resets the submodule before `build-ogre.sh` rather than reading it as an
    upstream change.

THE STACK IS 0001-0085 WITHOUT 0023 (this list; `build-ogre.sh` globs `*.patch`, so the file
count under thirdparty/ogre-patches/ is the truth and this document tracks it).
Updating Ogre: bump the submodule pin, re-run scripts/build-ogre.sh. A patch that
no longer applies is the signal to review upstream's change and adapt. Media-only
patches (0003/0009/0011/0019/0021/0022/0029/0030/0031/0033/0034/0036/0042/0043/0045/0048/0058/0066/0074/0077/0079/0082/0083/0084) need no Ogre rebuild (0024 and 0028 are
SOURCE + media; 0025, 0026, 0027, 0032, 0038, 0039, 0040, 0041, 0044, 0046, 0047, 0049, 0050-0057, 0059, 0060, 0061, 0063, 0064, 0067, 0068, 0069, 0071, 0072, 0073, 0075, 0078, 0081 and 0085 are SOURCE-only (0062, 0065, 0076 and 0080 are SOURCE + media; 0066 is media-only), and 0020 touches the
sample framework only) — the Studio build stages the
media straight from the submodule — but the patch loop must have run in that tree,
and a tree whose media predates 0019 will THROW when chain::updateSsao pushes
`jahOrthoParams` at a shader that does not declare it (Ogre's setNamedConstant
raises on an unknown name), which is the loud failure that stale media deserves.
