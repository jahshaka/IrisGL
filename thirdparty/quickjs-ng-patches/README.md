# Jahshaka's patches for the vendored quickjs-ng

`irisgl/thirdparty/quickjs-ng` is upstream `quickjs-ng/quickjs`, pinned to a
**release tag** (v0.18.0, `d7215b0`): the JavaScript engine of the engine's
script host (SCRIPT-IN-ENGINE-1). It is vendored, never edited in place, and never
committed to. What we must change lives here as numbered, self-documenting patch
files, and the submodule's working tree ends up in the **applied-not-committed**
state — the same law and the same mechanism as `thirdparty/assimp-patches/` and
`thirdparty/bullet-patches/` (read assimp's README for the whole of it).

## How they get applied

`irisgl/CMakeLists.txt` runs `irisgl/cmake/ApplyVendorPatches.cmake` at
**configure time**, before `add_subdirectory(thirdparty/quickjs-ng)`, so a fresh
clone cannot build an unpatched JS engine. Idempotent (an applied patch
reverse-applies and is skipped); a patch that neither applies nor reverse-applies
is a hard configure error — the signal that upstream touched our lines. By hand:

```bash
cmake -DSRC=irisgl/thirdparty/quickjs-ng -DPATCHES=irisgl/thirdparty/quickjs-ng-patches \
      -P irisgl/cmake/ApplyVendorPatches.cmake
```

## The stack

| # | file | what | its law (tests/engine/test_script_host.cpp) |
|---|---|---|---|
| 0001 | `0001-proxy-ownkeys-sorted-key-checks.patch` | a Proxy's ownKeys list checked for duplicates and target keys through a sorted copy of its (atom, index) pairs - a heapsort, n log n in the WORST case whatever atoms a script chose (it was ~n^2/2 comparisons in C no interrupt reaches) | case 18: the own-key laws kept exactly; 4x the keys under 8x the CPU for ordinary keys AND crafted ones (one low-bit residue, adjacent residues) |
| 0002 | `0002-tocstring-free-on-failure.patch` | JS_ToCStringLen2 frees its string when the UTF-8 cannot be allocated (it leaked it for the runtime's life) | case 12d (bis): a refused conversion gives its string back |

Each patch's header says the bug, the measurement and the fix (MCP-ON-HOST-1
stage 2, 2026-11-23). ONE ACCEPTED DIFFERENCE from the pin (0001): near a full
heap, "out of memory" (its sorted copy refused, 8 bytes a key) can come where the
pin would have answered the duplicate or target-key TypeError - both FAIL the call
by name; the copy cannot be done in place without reordering the trap's list. Neither is reported upstream (we report nothing upstream);
when the pin moves, each is re-read against upstream's change and dropped if
upstream fixed it.
