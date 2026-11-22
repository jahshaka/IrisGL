// iris::files - THE ONE ATOMIC FILE WRITE, AND THE FILE PRIMITIVES BELOW THE EDITOR (NOQT-CORE-1 stage 1; V2 plan 0.12).
// It replaces the two writers that each had one law (Studio's FileWrite, the engine's cachefile::writeAtomic).
//
// A file written in place LIES about its content between the truncate and the last byte: fatal for a content-addressed
// object (its name is a sha256 claim nobody re-hashes on read), a corrupt artifact that survives for ever for a sidecar,
// a store.json, a baked map or a cache entry. So a write is always: stage into a SIBLING temp (the same directory, so the
// same filesystem, so the rename is atomic), flush, then ONE rename that REPLACES the target. A crash leaves the old file
// or the new one, never half of either. The directory entry is not fsynced: losing the rename loses the write, and a
// missing artifact is a re-write, not a corruption.
//
// THE FLUSH IS A POLICY (FSYNC-2, 2026-09-17: fsync waits for the whole filesystem's journal, measured 220-1153 ms a call
// while anything else wrote to the device), answered per writer by what a LOST or TORN copy costs:
//   Durability::Flush    the bytes ARE the artifact and nothing re-derives them (a CAS object, store.json, a baked map,
//                        a cache entry): fsync before the rename, always.
//   Durability::Derived  a PROJECTION of durable state, re-written by a function call (the CAS sidecar): the atomic
//                        rename, no wait - a torn JSON does not parse and every reader skips what does not parse.
//
// PATHS ARE UTF-8 TEXT AT THIS DOOR: path() goes through std::filesystem::u8path (a narrow std::string reaches a path in
// the platform's NARROW encoding, which on Windows is not UTF-8) and utf8() back. The temp's name is
// "<target>.tmp-<pid>-<serial>" (a process-wide serial: concurrent writers and the import worker never collide); the
// library GC and the library reset READ that shape (src/services/assetgc.cpp, libraryreset.cpp): it does not change.
#pragma once

#include "core/export.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace iris {
namespace files {

/// What a lost or torn copy of the file costs (the header comment).
enum class Durability
{
    Flush,     ///< the bytes are the artifact: fsync before the rename
    Derived    ///< a projection of durable state: atomic rename, no wait
};

/// UTF-8 text <-> a path, the same on every platform.
IRIS_CORE_API std::filesystem::path path(std::string_view utf8);
IRIS_CORE_API std::string utf8(const std::filesystem::path &p);

/// The whole file's bytes, or nothing when it does not exist or cannot be read.
IRIS_CORE_API std::optional<std::string> readAll(const std::filesystem::path &p);
/// mkdir -p. False only when a component could not be created.
IRIS_CORE_API bool makeDirs(const std::filesystem::path &dir);

/// The staged write's three steps, for a writer that streams into its temp itself (the CAS copy, an archive's or a video's
/// partial): a unique sibling temp path for `target`; the flush of a written file to the device (no-op off POSIX: the
/// rename's ordering only); the atomic replace of `to` by `from` - on failure `from` is removed and `error` says why.
IRIS_CORE_API std::filesystem::path stagingTempPath(const std::filesystem::path &target);
IRIS_CORE_API void fsyncPath(const std::filesystem::path &p);
IRIS_CORE_API bool atomicRename(const std::filesystem::path &from, const std::filesystem::path &to, std::string *error = nullptr);

/// `bytes` written to `target` through a staged temp (its directory made first): the old file stays whole until the
/// rename replaces it. False with `error` said when anything failed - the target is then untouched and no temp is left.
IRIS_CORE_API bool writeAtomic(const std::filesystem::path &target, std::string_view bytes, Durability durability = Durability::Flush,
                 std::string *error = nullptr);

}   // namespace files
}   // namespace iris
