// THE SHADER CACHE's LAST-USED STAMPS (SHADER-SYSTEM-1, the start preflight's F10; the lead's ruling (B), 2026-11-22). Once
// nothing but a producer drops a layer (stage 5), a user's cache would grow without end: the microcode map is loaded
// whole and saved whole, Ogre's disk cache re-saves every recipe it restored, the compute layer re-files every record.
// So each ENTRY carries the launch it was last used in - in a sidecar of ours (entry-uses.bin), keyed by the entry's
// own content key; Ogre's formats are unchanged - and an entry no launch used for kUnusedLaunches launches is dropped
// at a save. The manifest counts the launches. A sidecar missing or damaged prunes NOTHING: every entry is stamped
// with this launch and the sidecar is rebuilt (never a wipe). The definitions are OgreShaderCache.cpp's.
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
};
enum : std::uint8_t { kUsesMicrocode = 1, kUsesCompute = 2, kUsesHlmsBase = 16 };   ///< + the Hlms type

/// Entry -> the launch it was last used in.
using UseTable = std::map<UseKey, std::uint32_t>;

/// An entry is dropped when this many launches passed since its last use (used at launch 1, gone at launch 31).
constexpr std::uint32_t kUnusedLaunches = 30u;

/// THE ONE RULE. An entry used this launch is stamped with it; one the table does not hold is stamped with it too (a
/// new entry, or a sidecar that was lost: nothing is pruned on a missing stamp); one unused keeps its stamp. Kept
/// unless kUnusedLaunches have passed since that stamp; a kept entry's stamp goes into `next` (the sidecar written).
bool keepByUse(const UseTable &previous, UseTable &next, const UseKey &key, bool used, std::uint32_t launch);

/// entry-uses.bin's bytes ("JAHUSES1", the count, the entries, a trailing 128-bit checksum of all before it), and its
/// read: false for bytes that are not a whole, intact table (the caller prunes nothing and rebuilds it).
std::vector<char> writeUseTable(const UseTable &table);
bool readUseTable(const char *data, std::size_t size, UseTable &out);

}  // namespace jahshaka::engine::detail
