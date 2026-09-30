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
header is **COMPILED since ATOM stage 2** (lane ATOM-CLUSTER-1, 2026-09-23): exactly one TU defines
`CLUSTERLOD_IMPLEMENTATION` — `irisgl/import/clusterlod.cpp` — with zero warnings under GCC 15.2 (also clean
at `-Wall -Wextra`), and the mesh bake (`import/meshbake.cpp`, `clusterdag::build`) calls `clodBuild` and
`clodLocalIndices` to write every mesh's cluster DAG. The header is in the bake's producer hash
(`irisgl/CMakeLists.txt`), so re-copying it re-bakes every library.

**Vendored, never edit** — the rule that covers `thirdparty/{assimp,bullet3,zip,ogre-next,meshoptimizer}`
covers this copy too. A change we need becomes a patch beside it, never an edit in place.

**Our patch stack** (since CLUSTER-LOCK-3, 2026-09-29): `thirdparty/meshoptimizer-clusterlod-patches/*.patch`,
applied in order at every configure by `irisgl/CMakeLists.txt` to a COPY of this header in the build tree
(`<build>/irisgl/vendor-patched/meshoptimizer-clusterlod/clusterlod.h`, which is what the bake's two TUs include);
a patch that stops applying is a configure error. Each patch is in the bake's producer hash. A re-copy of this
file re-applies the stack or refuses loudly.

| Patch | What |
|---|---|
| `0002-terminal-group-border-stays-locked.patch` | a terminal (stuck) group's border stays locked at every later level — an upstream defect in v1.2 (`lockBoundary` sees only the current level's groups; 16 open edges on round-bar-40m), an upstream PR candidate |
| `0003-per-group-verify-hook.patch` | `clodMesh::verify_group` — the caller verifies every simplified group and may re-simplify THAT group with added locks, never through the sloppy fallback; a re-simplification stuck under its locks makes the group terminal (the bake's displacement lock is per group; one `vertex_lock` bound every level up to the root) |

(0001 was CLUSTER-LOCK-2's per-level lock prototype, never landed; the number is retired, never reused.)
