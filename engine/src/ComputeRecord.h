// THE COMPUTE LAYER'S RECORDS (COLD-BOOT-1 stage 3; the lead's rulings P2, P3, P6).
//
// WHY. No compute permutation survived a process: the shader cache's Hlms layers are saved by loops that end at HLMS_MAX
// (8), and HLMS_COMPUTE is 9 (fork OgreHlmsCommon.h:199-201) - so every warm launch re-parsed and re-built ~197 compute
// permutations inside frames (spikes/cold-boot-1/S1b: 108 parses + 89 PSO-only, in every arm). A RECORD is what a built
// permutation's code needs to come back without its template parse: the GENERATED source, the root layout it was hashed
// and built with, and the parse's outputs a PSO needs (threads per group, group counts, indirect, job-sized).
//
// TWO KEYS, KEPT APART (P3):
//   * the INPUT KEY - the content of what the parse READS: the template's bytes, every piece file's bytes, the job's C++
//     pieces and its properties before the parse. The fork's compileShader computes it BEFORE the parse (through
//     HlmsCompute's key hook, which calls computeRecordInputKey below: ONE home of the key) and a hit skips the parse.
//     A changed template or piece moves exactly the keys of the records that read it ("a changed source drops exactly
//     its entries"); nothing of a VIEW (its size, its eye) is an input - source.compute_size_keys' clause (2) holds it.
//   * the PROGRAM KEY - the fork's own code-cache key (HlmsCompute::mCompiledShaderCache): 128-bit MurmurHash3 of the
//     generated source, combined with the root layout's bytes on an explicit API (OgreHlmsCompute.cpp compileShader).
//     A record PAIRS with a program only through it; a record whose stored source does not hash to its stored program key
//     came from another compiler or was damaged, and is REJECTED at read (the lead's condition 1).
// The LAYER's key (the file compute.bin) is the producer only - format, this code, the fork, glslang, the build, the
// render system (OgreShaderCache.cpp) - never the media: the records carry their content.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jahshaka::engine::detail {

/// A 128-bit key, as Ogre's MurmurHash3_x64_128 writes it.
using ComputeKey128 = std::array<std::uint64_t, 2>;

/// One built compute permutation's code, as the compute layer stores it.
struct ComputeCodeRecord {
    ComputeKey128 inputKey{ { 0u, 0u } };     ///< computeRecordInputKey of what the parse read
    ComputeKey128 programKey{ { 0u, 0u } };   ///< computeProgramKey(source, rootLayout, explicitApi)
    std::string   name;                       ///< the job's name, for logs only (never a key)
    std::string   sourceFilename;             ///< the template's name (+ extension), the program's name stem
    std::string   source;                     ///< the GENERATED source (after the parse)
    std::string   rootLayout;                 ///< the root layout's bytes, as the program was built with
    bool          explicitApi = true;         ///< the root layout is part of the program key (Vulkan)
    bool          arrayBindings = false;      ///< uses_array_bindings: the program reflects its array bindings
    std::uint32_t threadsPerGroup[3] = { 0u, 0u, 0u };
    std::uint32_t numThreadGroups[3] = { 0u, 0u, 0u };
    bool          indirect = false;           ///< the job is dispatched indirectly (no CPU group count)
    bool          jobSized = false;           ///< the job carries its own group counts to the dispatch (stage 2)
    /// The FILES the parse read - the template and every piece file - with each one's content hash (computeFileHash):
    /// a launch whose staged file differs drops exactly the records that read it (pruneStaleRecords).
    std::vector<std::pair<std::string, ComputeKey128>> files;

    bool operator==(const ComputeCodeRecord &o) const;
};

/// What a compute template's parse READS - and nothing else (P3; the lead's condition 3: no field of a view).
struct ComputeKeyInputs {
    std::string templateText;                                         ///< the template file's bytes
    std::vector<std::pair<std::string, std::string>> pieceFiles;      ///< (file name, bytes), in the job's order
    std::vector<std::pair<std::string, std::string>> cppPieces;       ///< (piece name, text): the job's C++ pieces
    std::vector<std::pair<std::uint32_t, std::int32_t>> properties;   ///< (key hash, value), before the parse
};

/// THE INPUT KEY: a pure function of its inputs (the C++ pieces' order does not matter; everything else is as given).
ComputeKey128 computeRecordInputKey(const ComputeKeyInputs &in);

/// A file's content hash, as a record stores it.
ComputeKey128 computeFileHash(const std::string &bytes);

/// THE PROGRAM KEY: the fork's code-cache key of a generated compute source (OgreHlmsCompute.cpp compileShader).
ComputeKey128 computeProgramKey(const std::string &source, const std::string &rootLayout, bool explicitApi);

/// A filed record is immutable: the store, a save's job and the writer share it (the save copies pointers, never sources).
using ComputeRecordPtr = std::shared_ptr<const ComputeCodeRecord>;

/// compute.bin's bytes: a header ("JAHCOMP1", the record count) and the records, each length-checked.
std::vector<char> writeComputeLayer(const std::vector<ComputeRecordPtr> &records);

/// The read: ok = false for bytes that are not a compute layer (bad magic, a short or overlong record); otherwise every
/// record whose program key its source re-hashes to, and the count of those REJECTED because it did not.
struct ComputeLayerRead {
    bool ok = false;
    std::vector<ComputeCodeRecord> records;
    unsigned rejected = 0;
};
ComputeLayerRead readComputeLayer(const char *data, std::size_t size);

/// "A CHANGED SOURCE DROPS EXACTLY ITS ENTRIES" (P3): every record one of whose files `currentHash` no longer finds, or
/// finds with other content, is removed; the count removed is returned. `currentHash(name, out)` answers the staged
/// file's content hash (false = no such file now).
unsigned pruneStaleRecords(std::vector<ComputeCodeRecord> &records,
                           const std::function<bool(const std::string &, ComputeKey128 &)> &currentHash);

/// What the shader cache reads from, and hands back to, the fork's compute layer (P6). The fork's HlmsCompute holds the
/// records (recorded at a permutation's build, on the main thread) and builds a record's code off the frame (its
/// AsyncCodeJob); the engine adapts it to this interface. Every call is on the render thread.
class ComputeCodeStore {
public:
    virtual ~ComputeCodeStore() = default;
    /// Bumps on every record the store files (ShaderCache::dirty compares it with the generation it last saved).
    virtual std::uint64_t generation() const = 0;
    /// Every record the store holds, in a stable order (pointers: nothing is copied on the render thread).
    virtual void snapshot(std::vector<ComputeRecordPtr> &out) const = 0;
    /// The records a launch read: filed, and their code compiled on the async compiler (code only: no job, no PSO).
    virtual void prewarm(std::vector<ComputeCodeRecord> &&records) = 0;
};

}  // namespace jahshaka::engine::detail
