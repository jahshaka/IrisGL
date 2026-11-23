# xxHash, vendored (NOQT-DOCUMENT-1 stage 5, the bake's payload checksum)

- Upstream: https://github.com/Cyan4973/xxHash (Yann Collet), BSD 2-Clause (LICENSE, kept beside the header).
- Release: v0.8.4 (published 2026-09-19; the latest release at least two weeks old on 2026-11-23).
- Archive: https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz
  sha256 5738270935e7c3d38a79b3adf7c9692566ce7895a25f67de43ad52ab504acd32
- Kept, unmodified, ONLY:
  - xxhash.h  sha256 3dc8d161e867a62d3417f7885b55fdaded8b7b497e2f165d05c94f4fe24f2ca4
  - LICENSE   sha256 6ffedbc0f7878612d2b23589f1ff2ab15633e1df7963a5d9fc750ec5500c7e7a
  Never `xxhsum` and never the `cli/` folder (GPL); nothing else of the archive.
- Fetched 2026-11-23 from a neutral scratch directory (no project data sent); nothing of the archive was run.
- Used by ONE translation unit: irisgl/import/meshbake.cpp, with `XXH_INLINE_ALL` (every xxHash function static
  inline in that unit: no exported symbol, so no clash with another copy of xxHash anywhere in the process).
- A vendored tree is never edited (CODING_RULES § 2): a change is a new release vendored the same way.
