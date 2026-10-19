# Jahshaka's patches for the vendored Bullet

`irisgl/thirdparty/bullet3` is upstream `bulletphysics/bullet3`, pinned to a
**release tag** (3.25, `2c204c49e`). It is vendored, never edited in place, and
never committed to. Everything we need to change lives here as a numbered,
self-documenting patch file, and the submodule's working tree ends up in the
**applied-not-committed** state — the same law, and the same mechanism, as
`thirdparty/assimp-patches/` (read its README for the whole of it).

Why a stack and not a fork: Bullet is a release tag we track, compiled by our
own build every time, and we change a handful of its lines. Ogre-Next, with a
whole engine's worth of our own work, is a fork instead (`irisgl/docs/OGRE_BUILD.md`).

## How they get applied

`irisgl/CMakeLists.txt` runs `irisgl/cmake/ApplyVendorPatches.cmake` at
**configure time**, before `add_subdirectory(thirdparty/bullet3)`, so a fresh
clone cannot build unpatched sources. The configure log says
`vendor patch applied: 0001-…` on a fresh tree and
`vendor patch already applied: 0001-…` on every configure after. By hand:

```bash
cmake -DSRC=irisgl/thirdparty/bullet3 -DPATCHES=irisgl/thirdparty/bullet-patches \
      -P irisgl/cmake/ApplyVendorPatches.cmake
```

Idempotent (an already-applied patch reverse-applies and is skipped); a patch
that neither applies nor reverse-applies is a **hard configure error** — the
signal that upstream touched our lines. The applier is chosen per source tree:
`git apply` for the submodule checkout, GNU `patch -p1 --fuzz=0 -N` (with the
flag pairing the applier's header documents: `--force` on the reverse probe,
`--batch` on the forward probe and the apply) for a tree that is not its own
git toplevel.

The patch files and the script are configure inputs (CONFIGURE_DEPENDS): a new
or edited patch re-runs CMake. An EDITED patch in a configured tree neither
applies nor reverse-applies — `git -C irisgl/thirdparty/bullet3 checkout .`,
then reconfigure.

**The applied stack is not dirt.** `git -C irisgl status` shows
` m thirdparty/bullet3` in every configured tree (modified CONTENT, the
gitlink unchanged). The build stamp (`cmake/BuiltFrom.cmake`) and the lead's
tools (`scripts/lead/lib/util.py` `worktree_dirt`, `records.py` `tree_tip`)
read irisgl with `--ignore-submodules=dirty`, which hides a submodule's
modified content and still shows a changed gitlink — so the applied stack is
not dirt and an uncommitted Bullet pin change still is (measured,
BULLET-BATCHING-1 stage 1). No path exclusion is needed or wanted: an
`:(exclude)thirdparty/bullet3` would also hide that pin change.

## The law

- Never edit `thirdparty/bullet3` in place; never commit to it.
- One numbered patch per change, its rationale in its own header (the bug, the
  fix, upstream's status). A genuine upstream defect is also an upstream PR.
- Bumping the pin: check out the new release tag, reconfigure; a patch that no
  longer applies stops the build — check whether upstream fixed it and delete
  the patch if so; re-run the physics rows (`physics.*`, `document.physics_*`).

## Current patches

| # | Patch | Upstream status |
|---|-------|-----------------|
| 0001 | `0001-batched-constraints-grid-cell-epsilon.patch` — `setupSpatialGridBatchesMt`'s grid cell is `consExtent * (1 + 1e-3)` instead of exactly the largest constraint extent: two bodies one extent apart could land two cells apart through the cell coordinate's rounding, breaking `assignConstraintsToGridBatches`'s `coordMax == coordMin + 1` (an abort in Debug; two batches of one phase sharing a body — a data race — in Release). BULLET-BATCHING-1 | **Open**: present at the pinned 3.25; an upstream PR is owed. Drop at the pin bump that fixes it. |
