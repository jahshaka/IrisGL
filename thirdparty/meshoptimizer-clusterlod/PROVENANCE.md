# `clusterlod.h` — a vendored COPY, and why it is not the submodule

**What this is.** `clusterlod.h` from the meshoptimizer repository, copied verbatim.

| | |
|---|---|
| Upstream | https://github.com/zeux/meshoptimizer — file `demo/clusterlod.h` |
| Commit | `9d9890c73011d75920af614485296d1e03e95448` (release tag **v1.2**) |
| Copied | 2026-09-15 (lane ATOM-1) |
| Licence | MIT, © 2016-2026 Arseny Kapoulkine — `LICENSE.md` beside this file |
| Size | 809 lines, header-only; needs `CLUSTERLOD_IMPLEMENTATION` in exactly one TU **and a full meshoptimizer build** |

**Why a copy and not the submodule's own file.** It is not part of the library: it lives under
`demo/` and describes itself as *"a small 'library'/example … intended to either be used as is, or
as a reference"*. Its API has already moved once between the revisions this program studied
(NANITE_SPEC §2c records the diff: cluster bounds error stopped being monotonic across the DAG and
the render test moved to the GROUP's `simplified` bounds). A submodule bump must therefore be
allowed to move the library without silently moving this file under our feet — so the file is
OURS, pinned by this document, and a bump re-copies it deliberately.

**What ATOM stage 1 uses from it.** Only the projection formula stated in its own comment at lines
94-97:

```
screen error (0..1, multiply by screen height for pixels) =
    bounds.error / max(distance(bounds.center, camera_position) - bounds.radius, camera_znear)
                 * (camera_proj * 0.5f)                 // camera_proj = projection[1][1] = cot(fovy/2)
```

`irisgl/engine/src/OgreMesh.cpp` (`OgreScene::applyLodValues` + `Types.h::lodSwitchDistance`) inverts that formula to turn a baked
per-level error into the distance at which the level becomes acceptable, and cites this file. The
header itself is **NOT COMPILED** by stage 1 — no TU defines `CLUSTERLOD_IMPLEMENTATION` yet. It is
here because stage 2 (the cluster DAG) calls `clodBuild`/`clodLocalIndices` and must call the
*pinned* version, and because the formula our code inverts has to be readable next to its source.

**Vendored, never edit** — the rule that covers `thirdparty/{assimp,bullet3,zip,ogre-next,meshoptimizer}`
covers this copy too. A change we need becomes a patch beside it, never an edit in place.
