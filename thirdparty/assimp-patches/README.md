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
