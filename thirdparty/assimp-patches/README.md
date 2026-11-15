# Jahshaka's patches for the vendored assimp

`irisgl/thirdparty/assimp` is upstream `assimp/assimp`, pinned to a **release
tag**. It is vendored, never edited in place, and never committed to — exactly
the Ogre-Next law (`irisgl/thirdparty/ogre-patches/`). Everything we need to
change lives here as a numbered, self-documenting patch file, and the submodule
working tree ends up in the **applied-not-committed** state.

## How they get applied

`irisgl/CMakeLists.txt` runs `irisgl/cmake/ApplyVendorPatches.cmake` at
**configure time**, before `add_subdirectory(thirdparty/assimp)`. Nobody has to
remember a step: a fresh clone configures, gets patched, and builds.

To apply them by hand (after re-syncing the submodule, say), run the same
script — it is platform-neutral CMake, no shell required:

```bash
cmake -DSRC=irisgl/thirdparty/assimp -DPATCHES=irisgl/thirdparty/assimp-patches \
      -P irisgl/cmake/ApplyVendorPatches.cmake
```

It is idempotent: an already-applied patch is detected (`git apply --reverse
--check`) and skipped. A patch that neither applies nor reverse-applies is a
**hard configure error** — that failure is the signal upstream touched our
lines. Read their change, adapt or drop the patch. Never edit the vendored
source in place.

Ogre-Next uses the same law with a different hook: `irisgl/scripts/build-ogre.sh`
applies `ogre-patches/`, because Ogre is an out-of-tree prerequisite build that
this project's configure step does not drive. assimp is compiled by our own
build every time, so our own configure is the natural hook — and a missed manual
step there would produce a *crashing importer*, not a missing library.

## Bumping the pin

1. `git -C irisgl/thirdparty/assimp fetch --tags && git -C … checkout vX.Y.Z`
   (a release tag, not master tip).
2. Re-configure. Any patch that no longer applies stops the build; check
   whether upstream fixed the thing the patch existed for and **delete the
   patch** if so.
3. Re-run the importer suites and re-measure the canonical model numbers —
   assimp post-processing is not bit-stable across releases.
4. Commit the gitlink change in the **irisgl** repo.

An empty patch directory is a fine and expected state. The mechanism stands
whether or not we currently carry a patch.

## Current patches

| # | Patch | Upstream status |
|---|-------|-----------------|
| 0001 | `0001-fbx-meshgeometry-guard-oob-vertex-mapping.patch` — `FBX::MeshGeometry::ToOutputVertexIndex` must not bind a reference to the one-past-the-end mapping entry (aborts in hardened libstdc++ / silent OOB read otherwise; four of eight Mixamo sample exports hit it) | **Open upstream**: assimp issue [#4312](https://github.com/assimp/assimp/issues/4312), filed 2021-12-29, unfixed on `master` and on the pinned tag as of 2026-09-03. Drop this patch when upstream lands its own. |
| 0002 | `0002-cve-2025-70067-materialsystem-unbounded-key-strcpy.patch` — **CVE-2025-70067**: `aiMaterial::AddBinaryProperty` strcpy'd a file-supplied property key into a fixed 1024-byte buffer | Backport of upstream `531f7359` (PR #6628). Drop at the pin bump that contains it. |
| 0003 | `0003-cve-2025-70072-fbx-material-index-vs-face-count.patch` — **CVE-2025-70072**: FBX `ConvertMeshMultiMaterial` walked the face-index-count array using the material-index count | Backport of upstream `e8651c35` (PR #6667). Must sort before 0006 (same function). |
| 0004 | `0004-cve-2025-70070-fbx-empty-layer-token-list.patch` — **CVE-2025-70070**: `FBX::MeshGeometry` read `tokens[0]` of an empty Layer token list | Backport of upstream `2ffbe3c1` (PR #6665). |
| 0005 | `0005-cve-2025-70071-fbx-binary-data-array-length.patch` — **CVE-2025-70071**: FBX binary data arrays sized a buffer with a 32-bit `stride * count` that wraps on file-supplied counts | Backport of upstream `29fdb3e7` (PR #6666). |
| 0006 | `0006-cve-2025-70069-fbx-multimaterial-face-index-bounds.patch` — **CVE-2025-70069**: FBX `ConvertMeshMultiMaterial` trusted per-face index counts that can sum past the vertex array | Backport of upstream `b7353270` (PR #6664). |
| 0007 | `0007-obj-word-buffer-stack-overflow.patch` — OBJ `copyNextWord` copied tokens into a fixed 4096-byte member and walked past the buffer end on a trailing `\` | Backport of upstream `0404c875` (PR #6714), source hunks only (see the patch header). |
| 0008 | `0008-gltf2-embedded-texture-mimetype-null-deref.patch` — glTF2 `ImportEmbeddedTextures` computed `strchr(...) + 1` before its null check | Backport of upstream `24bd7ee6` (PR #6733). |
| 0009 | `0009-parsingutils-tokenmatchi-past-terminator.patch` — `TokenMatchI` stepped one past the terminator for a token at end of buffer | Backport of upstream `23fac9e7` (PR #6737), source hunk only. |

0002–0006 are the five post-pin CVE fixes the 2026-09 deep audit found missing
(all in importers we compile: FBX, glTF2, OBJ, Collada, PostProcessing);
0007–0009 are the same defect family without CVE numbers. Every one of them was
fetched from upstream, adapted where noted in its own header, and verified to
apply and reverse-apply cleanly on the v6.0.5 pin. **They all disappear at the
next pin bump that contains them** — step 2 of "Bumping the pin" above is
exactly how they get retired.

Related, and not a patch: `irisgl/CMakeLists.txt` defines `ASSIMP_BUILD_DEBUG`
for assimp's own objects in Debug builds. Without it every `ai_assert` in the
library — ~1,900 bounds and invariant checks on parsed file content — expands to
nothing, which is how CVE-2025-70067's "asserted" `strcpy` was live end to end.
