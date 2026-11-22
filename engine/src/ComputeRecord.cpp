// THE COMPUTE LAYER'S RECORDS - the keys and compute.bin's bytes (ComputeRecord.h says why).
#include "ComputeRecord.h"

#include <OgreHlmsCompute.h>
#include <OgreIdString.h>
#include <Hash/MurmurHash3.h>

#include <algorithm>
#include <map>
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
    std::vector<std::pair<std::uint32_t, std::string>> cpp = in.cppPieces;
    std::sort(cpp.begin(), cpp.end());
    k.tag('C');
    k.u64(cpp.size());
    for (const auto &c : cpp) { k.u64(c.first); k.str(c.second); }
    k.tag('P');
    k.u64(in.properties.size());
    for (const auto &p : in.properties) { k.u64(p.first); k.u64(std::uint32_t(p.second)); }
    k.tag('L');
    k.str(in.rootLayout);
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

namespace {

using ForkRecord = Ogre::HlmsCompute::JahCodeRecord;

void forkKeyHook(const Ogre::HlmsCompute::JahKeyInputs &in, Ogre::uint64 out[2]) {
    ComputeKeyInputs k;
    k.templateText = *in.templateText;
    for (const auto &f : *in.pieceFiles) k.pieceFiles.emplace_back(f.first, *f.second);
    for (const auto &c : *in.cppPieces) k.cppPieces.emplace_back(c.first.mHash, c.second);
    for (const auto &p : *in.properties) k.properties.emplace_back(p.keyName.mHash, p.value);
    k.rootLayout = *in.rootLayout;
    const ComputeKey128 key = computeRecordInputKey(k);
    out[0] = key[0];
    out[1] = key[1];
}

ComputeRecordPtr fromFork(const ForkRecord &f) {
    auto r = std::make_shared<ComputeCodeRecord>();
    r->inputKey = { { f.inputKey[0], f.inputKey[1] } };
    r->programKey = { { f.programKey[0], f.programKey[1] } };
    r->name = f.name;
    r->sourceFilename = f.sourceFilename;
    r->source = f.source;
    r->rootLayout = f.rootLayout;
    r->explicitApi = f.explicitApi;
    r->arrayBindings = f.arrayBindings;
    r->indirect = f.indirect;
    r->jobSized = f.jobSized;
    for (int i = 0; i < 3; ++i) {
        r->threadsPerGroup[i] = f.threadsPerGroup[i];
        r->numThreadGroups[i] = f.numThreadGroups[i];
    }
    for (const auto &file : f.files) r->files.emplace_back(file.name, ComputeKey128{ { file.hash[0], file.hash[1] } });
    return r;
}

ForkRecord toFork(ComputeCodeRecord &&r) {
    ForkRecord f;
    f.inputKey[0] = r.inputKey[0]; f.inputKey[1] = r.inputKey[1];
    f.programKey[0] = r.programKey[0]; f.programKey[1] = r.programKey[1];
    f.name = std::move(r.name);
    f.sourceFilename = std::move(r.sourceFilename);
    f.source = std::move(r.source);
    f.rootLayout = std::move(r.rootLayout);
    f.explicitApi = r.explicitApi;
    f.arrayBindings = r.arrayBindings;
    f.indirect = r.indirect;
    f.jobSized = r.jobSized;
    for (int i = 0; i < 3; ++i) {
        f.threadsPerGroup[i] = r.threadsPerGroup[i];
        f.numThreadGroups[i] = r.numThreadGroups[i];
    }
    for (auto &file : r.files) {
        Ogre::HlmsCompute::JahFileHash h;
        h.name = std::move(file.first);
        h.hash[0] = file.second[0];
        h.hash[1] = file.second[1];
        f.files.push_back(std::move(h));
    }
    return f;
}

class ForkComputeStore final : public ComputeCodeStore {
public:
    explicit ForkComputeStore(Ogre::HlmsCompute *compute) : mCompute(compute) {}
    std::uint64_t generation() const override { return mCompute->getJahRecordGeneration(); }
    void snapshot(std::vector<ComputeRecordPtr> &out) const override {
        // Each fork record is converted ONCE (the save runs on the render thread: a source is copied when its record
        // is new, never again), and its conversion held while the fork still files that record.
        Ogre::vector<Ogre::HlmsCompute::JahCodeRecordPtr>::type records;
        mCompute->getJahRecords(records);
        std::map<const ForkRecord *, std::pair<Ogre::HlmsCompute::JahCodeRecordPtr, ComputeRecordPtr>> kept;
        out.clear();
        out.reserve(records.size());
        for (const Ogre::HlmsCompute::JahCodeRecordPtr &f : records) {
            auto it = mConverted.find(f.get());
            ComputeRecordPtr r = it != mConverted.end() ? it->second.second : fromFork(*f);
            kept.emplace(f.get(), std::make_pair(f, r));
            out.push_back(std::move(r));
        }
        mConverted.swap(kept);
    }
    void usedKeys(std::vector<ComputeKey128> &out) const override {
        Ogre::vector<std::pair<Ogre::uint64, Ogre::uint64>>::type keys;
        mCompute->getJahUsedRecordKeys(keys);
        out.clear();
        for (const auto &k : keys) out.push_back(ComputeKey128{ { k.first, k.second } });
    }
    void prewarm(std::vector<ComputeCodeRecord> &&records) override {
        Ogre::vector<ForkRecord>::type in;
        in.reserve(records.size());
        for (ComputeCodeRecord &r : records) in.push_back(toFork(std::move(r)));
        records.clear();
        mCompute->jahPrewarmCode(in);
    }

private:
    Ogre::HlmsCompute *mCompute;
    mutable std::map<const ForkRecord *, std::pair<Ogre::HlmsCompute::JahCodeRecordPtr, ComputeRecordPtr>> mConverted;
};

}  // namespace

std::unique_ptr<ComputeCodeStore> makeForkComputeStore(Ogre::HlmsCompute *compute) {
    return std::unique_ptr<ComputeCodeStore>(new ForkComputeStore(compute));
}

void setForkComputeKeyHook(bool on) { Ogre::HlmsCompute::msJahComputeKeyHook = on ? &forkKeyHook : nullptr; }

}  // namespace jahshaka::engine::detail
