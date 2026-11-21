// THE COMPUTE LAYER'S RECORDS - the keys and compute.bin's bytes (ComputeRecord.h says why).
#include "ComputeRecord.h"

#include <OgreIdString.h>
#include <Hash/MurmurHash3.h>

#include <algorithm>
#include <cstring>

namespace jahshaka::engine::detail {

namespace {

ComputeKey128 hash128(const void *data, std::size_t size) {
    ComputeKey128 k{ { 0u, 0u } };
    Ogre::MurmurHash3_x64_128(data, static_cast<int>(size), Ogre::IdString::Seed, k.data());
    return k;
}

/// A tagged, length-prefixed stream of the key's inputs: no two different input sets write the same bytes.
struct KeyBytes {
    std::string b;
    void tag(char t) { b.push_back(t); }
    void u64(std::uint64_t v) { b.append(reinterpret_cast<const char *>(&v), sizeof v); }
    void str(const std::string &s) { u64(s.size()); b.append(s); }
};

/// compute.bin's writer and reader: little-endian fields, as the machine writes them (the layer key names the build).
struct Out {
    std::vector<char> &v;
    void raw(const void *p, std::size_t n) { v.insert(v.end(), static_cast<const char *>(p), static_cast<const char *>(p) + n); }
    void u32(std::uint32_t x) { raw(&x, sizeof x); }
    void u64(std::uint64_t x) { raw(&x, sizeof x); }
    void str(const std::string &s) { u64(s.size()); raw(s.data(), s.size()); }
};
struct In {
    const char *p = nullptr, *end = nullptr;
    bool good = true;
    bool raw(void *dst, std::size_t n) {
        if (!good || std::size_t(end - p) < n) return good = false;
        std::memcpy(dst, p, n);
        p += n;
        return true;
    }
    std::uint32_t u32() { std::uint32_t x = 0; raw(&x, sizeof x); return x; }
    std::uint64_t u64() { std::uint64_t x = 0; raw(&x, sizeof x); return x; }
    std::string str() {
        const std::uint64_t n = u64();
        if (!good || std::uint64_t(end - p) < n) { good = false; return std::string(); }
        std::string s(p, p + n);
        p += n;
        return s;
    }
};

constexpr char kMagic[8] = { 'J', 'A', 'H', 'C', 'O', 'M', 'P', '1' };

}  // namespace

bool ComputeCodeRecord::operator==(const ComputeCodeRecord &o) const {
    return inputKey == o.inputKey && programKey == o.programKey && name == o.name &&
           sourceFilename == o.sourceFilename && source == o.source && rootLayout == o.rootLayout &&
           explicitApi == o.explicitApi && arrayBindings == o.arrayBindings &&
           std::equal(threadsPerGroup, threadsPerGroup + 3, o.threadsPerGroup) &&
           std::equal(numThreadGroups, numThreadGroups + 3, o.numThreadGroups) && indirect == o.indirect &&
           jobSized == o.jobSized && files == o.files;
}

ComputeKey128 computeFileHash(const std::string &bytes) { return hash128(bytes.data(), bytes.size()); }

ComputeKey128 computeRecordInputKey(const ComputeKeyInputs &in) {
    KeyBytes k;
    k.tag('T');
    k.str(in.templateText);
    k.tag('F');
    k.u64(in.pieceFiles.size());
    for (const auto &f : in.pieceFiles) { k.str(f.first); k.str(f.second); }
    // The C++ pieces are a MAP on the job (HlmsComputeJob::mPieces): their order is the container's, not an input.
    std::vector<std::pair<std::string, std::string>> cpp = in.cppPieces;
    std::sort(cpp.begin(), cpp.end());
    k.tag('C');
    k.u64(cpp.size());
    for (const auto &c : cpp) { k.str(c.first); k.str(c.second); }
    k.tag('P');
    k.u64(in.properties.size());
    for (const auto &p : in.properties) { k.u64(p.first); k.u64(std::uint32_t(p.second)); }
    return hash128(k.b.data(), k.b.size());
}

ComputeKey128 computeProgramKey(const std::string &source, const std::string &rootLayout, bool explicitApi) {
    // compileShader's own steps (fork OgreHlmsCompute.cpp): the source's hash, then - on an explicit API - that hash and
    // the root layout's hashed together.
    ComputeKey128 h = hash128(source.data(), source.size());
    if (!explicitApi) return h;
    const ComputeKey128 pair[2] = { h, hash128(rootLayout.data(), rootLayout.size()) };
    return hash128(pair, sizeof pair);
}

std::vector<char> writeComputeLayer(const std::vector<ComputeRecordPtr> &records) {
    std::vector<char> v;
    Out o{ v };
    o.raw(kMagic, sizeof kMagic);
    o.u64(records.size());
    for (const ComputeRecordPtr &p : records) {
        const ComputeCodeRecord &r = *p;
        o.u64(r.inputKey[0]); o.u64(r.inputKey[1]);
        o.u64(r.programKey[0]); o.u64(r.programKey[1]);
        o.str(r.name);
        o.str(r.sourceFilename);
        o.str(r.source);
        o.str(r.rootLayout);
        o.u32((r.explicitApi ? 1u : 0u) | (r.arrayBindings ? 2u : 0u) | (r.indirect ? 4u : 0u) | (r.jobSized ? 8u : 0u));
        for (std::uint32_t t : r.threadsPerGroup) o.u32(t);
        for (std::uint32_t g : r.numThreadGroups) o.u32(g);
        o.u64(r.files.size());
        for (const auto &f : r.files) { o.str(f.first); o.u64(f.second[0]); o.u64(f.second[1]); }
    }
    return v;
}

ComputeLayerRead readComputeLayer(const char *data, std::size_t size) {
    ComputeLayerRead out;
    In in{ data, data + size };
    char magic[sizeof kMagic];
    if (!in.raw(magic, sizeof magic) || std::memcmp(magic, kMagic, sizeof magic) != 0) return out;
    const std::uint64_t n = in.u64();
    for (std::uint64_t i = 0; in.good && i < n; ++i) {
        ComputeCodeRecord r;
        r.inputKey[0] = in.u64(); r.inputKey[1] = in.u64();
        r.programKey[0] = in.u64(); r.programKey[1] = in.u64();
        r.name = in.str();
        r.sourceFilename = in.str();
        r.source = in.str();
        r.rootLayout = in.str();
        const std::uint32_t flags = in.u32();
        r.explicitApi = flags & 1u; r.arrayBindings = flags & 2u; r.indirect = flags & 4u; r.jobSized = flags & 8u;
        for (std::uint32_t &t : r.threadsPerGroup) t = in.u32();
        for (std::uint32_t &g : r.numThreadGroups) g = in.u32();
        const std::uint64_t nf = in.u64();
        for (std::uint64_t f = 0; in.good && f < nf; ++f) {
            std::string fname = in.str();
            ComputeKey128 fh{ { in.u64(), 0u } };
            fh[1] = in.u64();
            r.files.emplace_back(std::move(fname), fh);
        }
        if (!in.good) break;
        // A RECORD PAIRS WITH A PROGRAM ONLY THROUGH ITS PROGRAM KEY: one whose stored source does not hash to it is
        // another compiler's, or damaged - never filed (the lead's condition 1).
        if (computeProgramKey(r.source, r.rootLayout, r.explicitApi) != r.programKey) { ++out.rejected; continue; }
        out.records.push_back(std::move(r));
    }
    out.ok = in.good && in.p == in.end;
    if (!out.ok) out.records.clear();
    return out;
}

unsigned pruneStaleRecords(std::vector<ComputeCodeRecord> &records,
                           const std::function<bool(const std::string &, ComputeKey128 &)> &currentHash) {
    const std::size_t before = records.size();
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [&](const ComputeCodeRecord &r) {
                                     for (const auto &f : r.files) {
                                         ComputeKey128 now{ { 0u, 0u } };
                                         if (!currentHash(f.first, now) || now != f.second) return true;
                                     }
                                     return false;
                                 }),
                  records.end());
    return unsigned(before - records.size());
}

}  // namespace jahshaka::engine::detail
