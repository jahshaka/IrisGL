#!/usr/bin/env bash
# prune-ogre-install.sh <install prefix> <install_manifest.txt>
#
# Deletes every file under <prefix>/{include,lib,bin} that the manifest does not list —
# a header the fork deleted, a library or tool no longer built — then the empty
# directories. Called by build-ogre.sh after `cmake --install` (D6-FORK-TOOLING).
#
# THE PATHS MUST BE THE MANIFEST'S OWN SPELLING. CMake writes the manifest with the
# install prefix as CMAKE_INSTALL_PREFIX holds it (a trailing slash stripped, symlinks
# NOT resolved), so the prefix is taken from the build's CMakeCache when the caller can
# (build-ogre.sh does) and normalised here the way CMake normalises it. And the prune
# REFUSES when not one found file matches the manifest: a spelling mismatch would
# otherwise read the whole install as orphans (the Fable read's case: OGRE_PREFIX=/x/y/).
set -u
PREFIX="${1:?usage: prune-ogre-install.sh <prefix> <manifest>}"
MANIFEST="${2:?usage: prune-ogre-install.sh <prefix> <manifest>}"
while [ "${#PREFIX}" -gt 1 ] && [ "${PREFIX%/}" != "$PREFIX" ]; do PREFIX="${PREFIX%/}"; done
[ -f "$MANIFEST" ] || { echo "prune-ogre-install: no manifest $MANIFEST — nothing pruned"; exit 0; }
found=(); kept=0
while IFS= read -r -d '' f; do
    found+=("$f")
    grep -qxF "$f" "$MANIFEST" && kept=$((kept + 1))
done < <(find "$PREFIX/include" "$PREFIX/lib" "$PREFIX/bin" \( -type f -o -type l \) -print0 2>/dev/null)
if [ "${#found[@]}" -gt 0 ] && [ "$kept" = "0" ]; then
    echo "prune-ogre-install: REFUSING — none of the ${#found[@]} files under $PREFIX is in $MANIFEST" >&2
    echo "(the prefix is not spelled the way the manifest spells it); nothing pruned." >&2
    exit 1
fi
orphans=0
for f in "${found[@]}"; do
    grep -qxF "$f" "$MANIFEST" || { rm -f "$f"; orphans=$((orphans + 1)); }
done
find "$PREFIX/include" "$PREFIX/lib" "$PREFIX/bin" -type d -empty -delete 2>/dev/null || true
[ "$orphans" = "0" ] || echo "Pruned $orphans orphaned file(s) from $PREFIX (not in this build's install manifest)."
exit 0
