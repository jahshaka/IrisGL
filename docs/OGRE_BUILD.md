# Ogre-Next — how IrisGL builds it

The engine is the pinned `thirdparty/ogre-next` submodule, and that submodule is **our fork**:
`github.com/jahshaka/ogre-next`, branch **`jahshaka`** = upstream's 52d1a7aaf (master, 4.0.0-unstable) plus our
commits. `scripts/build-ogre.sh` is the whole build — it checks that the checkout really is the fork, configures,
builds (RelWithDebInfo, shader-compile threading mode 2, an explicitly pinned Component set) and installs into
THIS tree's `thirdparty/ogre-next-install`. Every tree and worktree owns its install. Never configure the
submodule by hand, and never build upstream's `master`: it compiles and runs, and renders a different picture.

A stock `git submodule update --init` is now enough — there is nothing to apply, and the old "reset the submodule
first" recipe is gone with the patch stack (2026-09-22; `SPECS/OGRE_FORK_DESIGN.md`). A submodule whose working
tree is dirty is a real edit somebody made, not the stack.

## Dependencies (Linux; install ALL of it before the first configure — CMake caches not-found results)
```bash
sudo apt-get install -y libxrandr-dev libxaw7-dev rapidjson-dev libzzip-dev \
     libsdl2-dev glslang-tools spirv-tools vulkan-tools libshaderc-dev \
     libfreeimage-dev libxcb-randr0-dev libx11-xcb-dev libxcb1-dev \
     libxcb-keysyms1-dev libx11-dev libxt-dev libgl1-mesa-dev \
     libglu1-mesa-dev libfreetype-dev zlib1g-dev libvulkan-dev
```
- `libshaderc-dev` missing makes Ogre configure "successfully" with NO Vulkan RenderSystem; the script checks
  and fails loudly.
- Vulkan's XCB windowing needs `libxcb-randr0-dev` + `libx11-xcb-dev`.
- macOS: Xcode, the LunarG Vulkan SDK (MoltenVK), rapidjson headers; the script switches on `uname` (no X11,
  GL3Plus off, plain dylibs, the STBI codec, baked rpaths). Windows is not validated.

## What is built
The Vulkan RenderSystem with `OGRE_VULKAN_WINDOW_NULL=ON` (the surfaceless `windowType=null` window beside XCB,
selected at run time — every offscreen test uses it), GL3Plus on Linux, the NULL RenderSystem (headless),
ParticleFX + ParticleFX2, and the Components HlmsPbs, HlmsUnlit, SceneFormat, PlanarReflections, Atmosphere,
MeshLodGenerator, Property, Overlay. The install is self-contained: `INSTALL_RPATH=$ORIGIN:$ORIGIN/..`, stale
libraries pruned, and a self-check that every installed `.so` resolves with no `LD_LIBRARY_PATH`.

## Runtime data
The Hlms templates are required at run time. The Studio build stages Ogre's media from the submodule's SOURCE
tree beside `engine/media`, so a media change on the fork reaches a tree through the pin bump + a rebuild
(nothing recompiles, but the tree must be at the new pin).

## The fork — where our engine changes live
- **Remotes.** `origin` = `github.com/jahshaka/ogre-next` (the `.gitmodules` url is the public https form, which
  clones without a key; the lead pushes over the `github-ogre` ssh alias). Add upstream once per clone:
  `git -C thirdparty/ogre-next remote add upstream https://github.com/OGRECave/ogre-next.git`.
- **Branches.** `master` is upstream's, never written by us. `jahshaka` carries our work. The tag
  `jahshaka-stack-v1` marks the commit the 88-patch stack became — proven byte-identical to the applied stack
  (one permitted difference: 0003's retirement). `build-ogre.sh` refuses to build anything that is not a
  descendant of it.
- **A SOURCE or MEDIA change = a commit on `jahshaka`, grouped by FILE FAMILY** (never a commit that mixes
  SOURCE with MEDIA: a media edit must not cost every tree an engine rebuild), with the rationale — the
  measurement, the suite that fences it, the "no picture moves" claim — in the commit message. Then bump the
  submodule pin in irisgl. PUSH ORDER: the fork, then irisgl, then Studio; each pins the one before.
- **An upstream bump = `git fetch upstream && git merge upstream/master` on `jahshaka`**, then one
  `build-ogre.sh` and the gate. Git resolves what a patch replay used to fail at; a conflict is a file upstream
  genuinely moved under us, and the commit that carries our side of it is right there in `git log`.
- **The numbers ended at 0089.** New work is named by its commit, not by a number. The 352 comments across the
  tree that cite `ogre-patches NNNN` are NOT rewritten — the index below is what they resolve against.

- **The LOG of every change we carry is `DOCS/OGRE_NEXT_CHANGES.md` in the workspace repo** (one entry per fork
  commit, newest first, with its upstream status; an upstream bump gets a section naming what conflicted and
  what was dropped because upstream took it). No commit on `jahshaka` is merged without its entry there. This
  file keeps the build facts and the number index below.

## The NUMBER -> commit index (what the 88 patches became)
The stack was 0001-0089 without 0023 (0023 was withdrawn 2026-09-17 and never reused). It became 26 commits,
one per file family — 17 SOURCE then 9 MEDIA — each the cumulative diff of its own files, generated from the
fully applied tree. Every commit message carries its members' rationale verbatim, so `git log` (or
`git show <sha>`) is the registry this file used to be. Three patches were retired in the conversion.

| was | name | fork commit(s) |
|---|---|---|
| 0001 | vulkan-cmake-debian-unbundled-glslang | `1a81f866a` M01 SOURCE: build + the Apple Metal window |
| 0002 | vulkan-device-clear-static-extension-arrays | `d014b064f` M02 SOURCE: vulkan device — creation, features, external device, device loss |
| 0003 | sky-material-silence-sliceidx-parse-error | `c290052de` M26 MEDIA: sky + SMAA material — **RETIRED (0009 made the uniform used; tested 2026-09-22)** |
| 0004 | cmake-freeimage-codec-not-forced-when-disabled | `1a81f866a` M01 SOURCE: build + the Apple Metal window |
| 0005 | vulkan-utils-stdmin-vkdevicesize-darwin | `1a81f866a` M01 SOURCE: build + the Apple Metal window |
| 0006 | vulkan-portability-moltenvk | `d014b064f` M02 SOURCE: vulkan device — creation, features, external device, device loss |
| 0007 | vulkan-metal-window-osx | `1a81f866a` M01 SOURCE: build + the Apple Metal window + `d014b064f` M02 SOURCE: vulkan device — creation, features, external device, device loss + `1bccc3f93` M03 SOURCE: vulkan render system |
| 0008 | vulkan-swapchain-honour-currentextent | `1a64cd1d8` M04 SOURCE: vulkan window / swapchain |
| 0009 | sky-equirect-glsl-sample-sliceidx | `c290052de` M26 MEDIA: sky + SMAA material |
| 0010 | pbs-refractions-max3 | `16d8e29d4` M18 MEDIA: PBS pixel shader + structs + refractions |
| 0011 | ssao-reject-far-plane-sky | `3f1ad1110` M25 MEDIA: SSAO |
| 0012 | smaa-edgedetection-viewportsize-per-delegate | `c290052de` M26 MEDIA: sky + SMAA material |
| 0013 | vulkan-present-mode-fifo-latest-ready | `d014b064f` M02 SOURCE: vulkan device — creation, features, external device, device loss + `1a64cd1d8` M04 SOURCE: vulkan window / swapchain |
| 0014 | overlay-textarea-load-font-before-first-geometry | `6130df9d1` M11 SOURCE: OgreMain correctness singles |
| 0015 | id-generator-atomic-counter | `6130df9d1` M11 SOURCE: OgreMain correctness singles |
| 0016 | warmup-collect-lights-forward-plus | `8282f6d70` M10 SOURCE: scene manager |
| 0017 | pcc-hybrid-probe-weighting | `4d5fbef16` M19 MEDIA: PCC probe loop |
| 0018 | pass-light-range-fade-parity | `36162ff37` M13 SOURCE: HlmsPbs C++ + `16d8e29d4` M18 MEDIA: PBS pixel shader + structs + refractions |
| 0019 | ssao-orthographic-position-reconstruction | `3f1ad1110` M25 MEDIA: SSAO |
| 0020 | samples-env-suppress-config-dialog | `ef462c42d` M17 SOURCE: sample framework config override |
| 0021 | vct-anisotropic-escape-fraction | `8f09c0cd4` M21 MEDIA: the VCT pixel piece |
| 0022 | geometric-specular-antialiasing | `16d8e29d4` M18 MEDIA: PBS pixel shader + structs + refractions |
| 0024 | pbs-ortho-view-dir | `36162ff37` M13 SOURCE: HlmsPbs C++ + `16d8e29d4` M18 MEDIA: PBS pixel shader + structs + refractions |
| 0025 | shadow-node-fixed-light-invalidates-cached-build | `6130df9d1` M11 SOURCE: OgreMain correctness singles |
| 0026 | hlms-disk-cache-skip-entries-without-pso | `d6348decd` M08 SOURCE: hlms cache + shader hash |
| 0027 | vulkan-gpu-timestamp-samples | `1a81f866a` M01 SOURCE: build + the Apple Metal window + `1bccc3f93` M03 SOURCE: vulkan render system |
| 0028 | pbs-probe-gate-on-material-reflectance | `36162ff37` M13 SOURCE: HlmsPbs C++ + `16d8e29d4` M18 MEDIA: PBS pixel shader + structs + refractions |
| 0029 | pcc-probe-visibility-from-captured-depth | `4d5fbef16` M19 MEDIA: PCC probe loop |
| 0030 | pcc-depth-compressor-matrix-order | `508b74369` M20 MEDIA: PCC depth compressor |
| 0031 | pbs-decals-f0-scalar-swizzle | `4d5fbef16` M19 MEDIA: PCC probe loop |
| 0032 | compute-indirect-dispatch | `1bccc3f93` M03 SOURCE: vulkan render system + `a98e2b0af` M07 SOURCE: indirect compute dispatch + buffer creation serial |
| 0033 | vct-cascade-escape-opacity-composite | `8f09c0cd4` M21 MEDIA: the VCT pixel piece |
| 0034 | hdr-nan-must-not-latch-adapted-luminance | `feab041c6` M24 MEDIA: HDR + final grade — **RETIRED (0042 superseded both NaN tests)** |
| 0035 | hlms-disk-cache-bounds-check-shader-hash-indices | `d6348decd` M08 SOURCE: hlms cache + shader hash |
| 0036 | pbs-ssr-replaces-the-environment-term | `16d8e29d4` M18 MEDIA: PBS pixel shader + structs + refractions |
| 0037 | vctlighting-setvoxelizer-and-three-lifetime-defects | `ae2ed529f` M14 SOURCE: VctLighting + `822d538f5` M16 SOURCE: IrradianceField C++ |
| 0038 | vulkan-ray-query-device-enablement | `d014b064f` M02 SOURCE: vulkan device — creation, features, external device, device loss |
| 0039 | vulkan-vbo-pools-as-blas-build-input | `b028638c1` M05 SOURCE: vulkan Vao + staging lifetime |
| 0040 | vulkan-export-onvulkanfailure | `1a81f866a` M01 SOURCE: build + the Apple Metal window |
| 0041 | descriptor-cache-buffer-creation-serial | `1bccc3f93` M03 SOURCE: vulkan render system + `a98e2b0af` M07 SOURCE: indirect compute dispatch + buffer creation serial |
| 0042 | hdr-luminance-meter-must-not-be-poisoned | `feab041c6` M24 MEDIA: HDR + final grade |
| 0043 | prepass-hands-back-the-roughness-it-wrote | `16d8e29d4` M18 MEDIA: PBS pixel shader + structs + refractions |
| 0044 | irradiance-field-movable-volume-and-lighting-rebind | `822d538f5` M16 SOURCE: IrradianceField C++ |
| 0045 | vct-cone-diffuse-blends-with-a-partial-irradiance-field | `8f09c0cd4` M21 MEDIA: the VCT pixel piece |
| 0046 | hlms-shader-hash-field-sizes | `d6348decd` M08 SOURCE: hlms cache + shader hash |
| 0047 | pcc-placement-keeps-the-depth-it-measured | `618d95cca` M12 SOURCE: component accessors — PCC, planar reflections, atmosphere |
| 0048 | where-no-probe-is-the-sky-is | `4d5fbef16` M19 MEDIA: PCC probe loop + `8f09c0cd4` M21 MEDIA: the VCT pixel piece |
| 0049 | cubemapprobe-set-is-idempotent | `618d95cca` M12 SOURCE: component accessors — PCC, planar reflections, atmosphere |
| 0050 | irradiance-field-const-buffer-size-and-vct-zero-multiplier | `ae2ed529f` M14 SOURCE: VctLighting + `822d538f5` M16 SOURCE: IrradianceField C++ |
| 0051 | reserved-texture-pool-is-resident-next-too | `6130df9d1` M11 SOURCE: OgreMain correctness singles |
| 0052 | planar-reflections-active-actor-slots | `618d95cca` M12 SOURCE: component accessors — PCC, planar reflections, atmosphere |
| 0053 | setsky-records-the-method-it-was-given | `8282f6d70` M10 SOURCE: scene manager |
| 0054 | atmospherenpr-names-its-sky-quad | `618d95cca` M12 SOURCE: component accessors — PCC, planar reflections, atmosphere |
| 0055 | rectangle2d-is-born-with-geometry | `6130df9d1` M11 SOURCE: OgreMain correctness singles |
| 0056 | groupless-load-must-not-call-a-null-listener | `6130df9d1` M11 SOURCE: OgreMain correctness singles |
| 0057 | a-cascade-added-after-bounces-were-enabled | `ae2ed529f` M14 SOURCE: VctLighting |
| 0058 | irradiance-field-piece-hooks | `b6c409c1f` M23 MEDIA: the irradiance-field piece |
| 0059 | mesh2-set-lod-values | `5230c9390` M09 SOURCE: LOD + mesh v2 |
| 0060 | bounce-bindings-per-dispatch | `ae2ed529f` M14 SOURCE: VctLighting |
| 0061 | voxelizer-octants-follow-the-region | `ad452604a` M15 SOURCE: VctVoxelizer + VctMaterial |
| 0062 | voxel-merge-and-dispatch-order | `ad452604a` M15 SOURCE: VctVoxelizer + VctMaterial + `155a56bf8` M22 MEDIA: the VCT compute media — **RETIRED into its group (0065 superseded it; only the bucket key survives)** |
| 0063 | microcode-cache-for-reflected-array-bindings | `5bfe24cd9` M06 SOURCE: microcode cache for reflected array bindings |
| 0064 | voxelizer-lod-level | `ad452604a` M15 SOURCE: VctVoxelizer + VctMaterial |
| 0065 | order-independent-voxel-merge | `ad452604a` M15 SOURCE: VctVoxelizer + VctMaterial + `155a56bf8` M22 MEDIA: the VCT compute media |
| 0066 | vct-cascade-march-carries-the-cone | `8f09c0cd4` M21 MEDIA: the VCT pixel piece |
| 0067 | delayed-blocks-wait-for-their-frame | `b028638c1` M05 SOURCE: vulkan Vao + staging lifetime |
| 0068 | external-device-renders-and-is-created-honestly | `d014b064f` M02 SOURCE: vulkan device — creation, features, external device, device loss + `1bccc3f93` M03 SOURCE: vulkan render system |
| 0069 | device-loss-must-not-abort-from-a-destructor | `b028638c1` M05 SOURCE: vulkan Vao + staging lifetime |
| 0070 | vct-cone-start-bias-is-one-cell | `8f09c0cd4` M21 MEDIA: the VCT pixel piece + `155a56bf8` M22 MEDIA: the VCT compute media |
| 0071 | the-merge-accumulator-stays-resident | `ad452604a` M15 SOURCE: VctVoxelizer + VctMaterial |
| 0072 | a-lost-device-ends-the-session | `d014b064f` M02 SOURCE: vulkan device — creation, features, external device, device loss + `1bccc3f93` M03 SOURCE: vulkan render system |
| 0073 | swapchain-rebuild-retires-its-semaphore | `1a64cd1d8` M04 SOURCE: vulkan window / swapchain |
| 0074 | cascade-continuation-composites-once | `8f09c0cd4` M21 MEDIA: the VCT pixel piece + `155a56bf8` M22 MEDIA: the VCT compute media |
| 0075 | lod-switch-hysteresis | `5230c9390` M09 SOURCE: LOD + mesh v2 + `8282f6d70` M10 SOURCE: scene manager |
| 0076 | vct-bounce-is-a-jacobi-iteration | `ae2ed529f` M14 SOURCE: VctLighting + `155a56bf8` M22 MEDIA: the VCT compute media |
| 0077 | injection-stores-radiance | `8f09c0cd4` M21 MEDIA: the VCT pixel piece + `155a56bf8` M22 MEDIA: the VCT compute media |
| 0078 | custom-projection-publishes-its-frustum-extents | `6130df9d1` M11 SOURCE: OgreMain correctness singles |
| 0079 | final-grade-is-dithered | `feab041c6` M24 MEDIA: HDR + final grade |
| 0080 | vct-total-volume-is-a-float | `ae2ed529f` M14 SOURCE: VctLighting + `155a56bf8` M22 MEDIA: the VCT compute media |
| 0081 | vct-material-cache-evicts-a-dying-datablock | `ad452604a` M15 SOURCE: VctVoxelizer + VctMaterial |
| 0082 | bloom-composite-takes-an-amount | `feab041c6` M24 MEDIA: HDR + final grade |
| 0083 | diffuse-cone-basis-is-the-normals | `8f09c0cd4` M21 MEDIA: the VCT pixel piece |
| 0084 | vct-escape-reads-both-axis-halves | `8f09c0cd4` M21 MEDIA: the VCT pixel piece |
| 0085 | movable-object-lod-level-is-settable | `5230c9390` M09 SOURCE: LOD + mesh v2 |
| 0086 | irradiance-field-cage-can-be-declined | `b6c409c1f` M23 MEDIA: the irradiance-field piece |
| 0087 | voxel-emissive-is-a-float | `ad452604a` M15 SOURCE: VctVoxelizer + VctMaterial + `155a56bf8` M22 MEDIA: the VCT compute media |
| 0088 | mixed-shadow-vao-list-is-legal | `5230c9390` M09 SOURCE: LOD + mesh v2 |
| 0089 | voxelizer-reports-queued-index-count | `ad452604a` M15 SOURCE: VctVoxelizer + VctMaterial |
