// THE SHADER CACHE's LAST-USED STAMPS (SHADER-SYSTEM-1, the start preflight's F10; the prune's law as the lead ruled it after
// the final read, 2026-11-22). Once nothing but a producer drops a layer (stage 5), a user's cache would grow without end:
// the microcode map is loaded whole and saved whole, Ogre's disk cache re-saves every recipe it restored, the compute layer
// re-files every record. So each ENTRY carries the launch it was last used in - in a sidecar of ours (entry-uses.bin), keyed
// by the entry's own content key; Ogre's formats are unchanged. AGE ALONE NEVER DROPS AN ENTRY (an "unused for 30
// launches" rule wore the rig's shared warm caches - every green row is a launch - and would age a headset session's
// shaders out behind any 30 desktop launches): the stamps only ORDER an eviction, which runs only when a save's layers
// would pass the size cap - the oldest stamp first, a whole stamp at a time, never this launch's (OgreShaderCache.cpp
// runWrite). The sidecar also holds the LAUNCH COUNTER, so a launch that compiled and evicted nothing writes no layer and
// no manifest - only this file, when a stamp moved. A sidecar missing, damaged or of an older format is not read: every
// entry is stamped with this launch, that launch evicts NOTHING and never wipes, even over the cap (one line says so),
// and the table is rebuilt - the next launch that reads it intact evicts by it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace jahshaka::engine::detail {

/// An entry of one layer, by its content key: the microcode map's hash, a compute record's input key, a recipe's
/// HlmsDiskCache::jahEntryKey (one layer per Hlms type).
struct UseKey {
    std::uint8_t  layer = 0;
    std::uint64_t h0 = 0, h1 = 0;
    bool operator<(const UseKey &o) const { return std::tie(layer, h0, h1) < std::tie(o.layer, o.h0, o.h1); }
    bool operator==(const UseKey &o) const { return layer == o.layer && h0 == o.h0 && h1 == o.h1; }
};
enum : std::uint8_t { kUsesMicrocode = 1, kUsesCompute = 2, kUsesHlmsBase = 16 };   ///< + the Hlms type

/// Entry -> the launch it was last used in.
using UseTable = std::map<UseKey, std::uint32_t>;

/// THE STAMP: an entry used this launch is stamped with it; one the table does not hold is stamped with it too (a new
/// entry, or a sidecar that was lost: nothing is evictable on a missing stamp); one unused keeps its stamp.
std::uint32_t stampByUse(const UseTable &previous, const UseKey &key, bool used, std::uint32_t launch);

/// entry-uses.bin's bytes ("JAHUSES2", the launch that wrote it, the count, the entries, a trailing 128-bit checksum of
/// all before it), and its read: false for bytes that are not a whole, intact table of this format (the caller stamps
/// every entry anew and rebuilds it).
std::vector<char> writeUseTable(const UseTable &table, std::uint32_t launch);
bool readUseTable(const char *data, std::size_t size, UseTable &out, std::uint32_t &launch);

}  // namespace jahshaka::engine::detail
