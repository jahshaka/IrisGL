// The persistent shader cache (SHADER_CACHE_SPEC.md) — three Ogre layers behind
// one container we fingerprint, checksum, cap and lock ourselves.
//
// Read the class comment in EnginePrivate.h first: it says WHY each of those four
// words is here. The short version is `OgreGpuProgramManager.cpp:368-398`, which
// reads a uint32 count out of a file and then trusts it all the way to
// vkCreateShaderModule. Nothing upstream checks that file's integrity. We do.
//
// What this file deliberately does NOT do:
//  * invent a per-shader key. Ogre content-addresses the microcode on the
//    GENERATED source (OgreVulkanProgram.cpp:169-172) and template-hashes the
//    Hlms cache (OgreHlms.cpp:418-449, which walks our Hlms/Jahshaka library
//    folder too). A second source of truth would be a second bug farm.
//  * evict. The microcode map only grows; the size cap wipes the whole
//    generation instead of maintaining an LRU we would have to get right.
//  * survive doubt. Every failure path ends in "delete the directory, run cold".
#include "EnginePrivate.h"

#include <mutex>
// JAHSHAKA_ENGINE_BUILD_ID: a hash of this library's own sources, regenerated
// on every BUILD (irisgl/cmake/EngineBuildId.cmake) rather than at configure
// time — an incremental edit to OgreEngine.cpp changes what the generated
// shaders mean and must therefore invalidate the cache (LIGHTING_FIX fix 10).
#include "jahshaka_engine_build_id.h"

#include <OgreGpuProgramManager.h>
#include <OgreHlms.h>
#include <OgreHlmsDiskCache.h>
#include <OgreHlmsManager.h>
#include <OgreRenderSystem.h>
#include <OgreRenderSystemCapabilities.h>
#include <OgreDataStream.h>
#include <OgreLog.h>
#include <Hash/MurmurHash3.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>

namespace jahshaka { namespace engine {
namespace detail {

// The shared on-disk-cache primitives are DEFINED at the bottom of this file
// (`namespace cachefile`) and declared in EnginePrivate.h; pulled in here so the
// rest of the file reads exactly as it did when they were file statics.
using cachefile::hex128;
using cachefile::hexOf;
using cachefile::mkpath;
using cachefile::readWholeFile;
using cachefile::writeAtomic;

namespace {

/// Our container's own format version. Bump it and every existing cache on
/// every machine is discarded — the escape hatch for a change in THIS file that
/// none of the other fingerprint terms would notice.
constexpr int kCacheFormat = 1;

/// Directory size cap (§4.3 rule 6). On exceed we wipe and re-warm rather than
/// evict: the microcode map has no eviction upstream, and an LRU we maintain
/// ourselves is a correctness risk for a few megabytes of derived data.
constexpr unsigned long long kMaxCacheBytes = 256ull * 1024ull * 1024ull;

constexpr const char *kManifest = "cache-manifest.txt";
constexpr const char *kLockFile = "cache.lock";

/// How long a save waits for a write that is still in flight (FSYNC-1). Thirty
/// seconds is not a latency budget — no caller is expected to wait at all — it
/// is the point at which "the filesystem is wedged" beats "the disk is busy",
/// and a save that gives up costs nothing: the layers stay dirty and the next
/// one writes them.
constexpr unsigned kWriteWaitMs = 30000u;

long long nowUnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/// Hash of the staged Hlms template tree. Belt-and-braces: Ogre's own
/// getTemplateChecksum already covers layer 1 correctly, but layer 2's file has
/// no version field of any kind. 83 files / 624 KB on this tree — ~5 ms.
/// Names are hashed alongside contents so a RENAME counts as a change.
std::string hashTree(const std::string &dir) {
    std::vector<std::string> names;
    // Iterative walk; no <filesystem> because the engine still targets C++17 on
    // toolchains where <filesystem> needs an extra link library on some hosts.
    std::vector<std::string> pending{dir};
    while (!pending.empty()) {
        const std::string cur = pending.back();
        pending.pop_back();
        DIR *d = opendir(cur.c_str());
        if (!d) continue;
        while (dirent *e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = cur + "/" + name;
            struct stat st {};
            if (::stat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) pending.push_back(full);
            else                     names.push_back(full);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());   // readdir order is not stable
    std::string blob;
    for (const std::string &n : names) {
        blob += n.substr(dir.size());
        std::vector<char> bytes;
        if (readWholeFile(n, bytes)) blob.append(bytes.data(), bytes.size());
    }
    return hexOf(blob);
}

unsigned long long dirBytes(const std::string &dir, unsigned *fileCount) {
    unsigned long long total = 0;
    unsigned files = 0;
    DIR *d = opendir(dir.c_str());
    if (!d) { if (fileCount) *fileCount = 0; return 0; }
    while (dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        struct stat st {};
        if (::stat((dir + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            total += static_cast<unsigned long long>(st.st_size);
            ++files;
        }
    }
    closedir(d);
    if (fileCount) *fileCount = files;
    return total;
}

void logLine(const std::string &s) {
    if (Ogre::LogManager::getSingletonPtr())
        Ogre::LogManager::getSingleton().logMessage("Jahshaka shader cache: " + s);
}

/// A WRITE-ONLY DataStream THAT GROWS (FSYNC-1). Ogre ships two shapes and
/// neither fits a serializer whose length is unknown until it finishes:
/// MemoryDataStream is a fixed buffer, FileStreamDataStream is a file. Every
/// caller here — HlmsDiskCache::saveTo, GpuProgramManager::saveMicrocodeCache,
/// RenderSystem::savePipelineCache — writes sequentially and reads nothing
/// back (verified against the pin), so this is the whole of what they need.
/// `seek` past the end is still honoured (zero-filled) rather than refused: a
/// stream that silently loses a write is the bug this class must not have.
class GrowingMemoryStream final : public Ogre::DataStream {
public:
    explicit GrowingMemoryStream(const Ogre::String &name)
        : Ogre::DataStream(name, WRITE) {}

    size_t read(void *, size_t) override { return 0; }   // write-only, by design

    size_t write(const void *buf, size_t count) override {
        if (!count) return 0;
        // HlmsDiskCache writes FOUR BYTES AT A TIME, a few hundred thousand
        // times per save, so this function's own cost is the serialization's
        // cost. Grow in large steps and copy: measured 26-28 ms for a 1.9 MB
        // generation, against 37-39 for a vector::insert per call and 486 for
        // the scratch FILE this replaced when the disk was busy.
        const size_t need = mPos + count;
        if (need > mBytes.size()) {
            if (need > mBytes.capacity())
                mBytes.reserve(need > 2u * mBytes.capacity() ? need + kGrowStep
                                                             : 2u * mBytes.capacity());
            mBytes.resize(need);
        }
        std::memcpy(mBytes.data() + mPos, buf, count);
        mPos += count;
        mSize = mBytes.size();
        return count;
    }

    void skip(long count) override {
        const long target = static_cast<long>(mPos) + count;
        seek(target < 0 ? 0u : static_cast<size_t>(target));
    }
    void seek(size_t pos) override {
        if (pos > mBytes.size()) mBytes.resize(pos, 0);
        mPos = pos;
        mSize = mBytes.size();
    }
    size_t tell() const override { return mPos; }
    bool   eof() const override { return mPos >= mBytes.size(); }
    void   close() override {}

    /// The bytes, moved out. The stream is empty afterwards.
    std::vector<char> take() { mPos = 0; mSize = 0; return std::move(mBytes); }

private:
    /// The first allocation and the floor for every growth: a generation is
    /// 1-2 MB and the first writer through here is the biggest of the three.
    static constexpr size_t kGrowStep = 1024u * 1024u;
    std::vector<char> mBytes;
    size_t            mPos = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// The shared on-disk-cache primitives (EnginePrivate.h `namespace cachefile`).
// They were file statics here until the texture cache became the second caller
// (THREADING_ADOPTION_SPEC.md P2); nothing about them changed but the linkage.
namespace cachefile {

std::string hex128(const void *data, size_t len) {
    Ogre::uint64 out[2] = {};
    Ogre::MurmurHash3_x64_128(data, static_cast<int>(len), 0x9E3779B9u, out);
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(out[0]), static_cast<unsigned long long>(out[1]));
    return std::string(buf);
}

bool readWholeFile(const std::string &p, std::vector<char> &out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    out.resize(static_cast<size_t>(n));
    f.seekg(0);
    if (n && !f.read(out.data(), n)) return false;
    return true;
}

bool mkpath(const std::string &dir) {
    if (dir.empty()) return false;
    std::string acc;
    size_t i = 0;
    if (dir[0] == '/') { acc = "/"; i = 1; }
    while (i <= dir.size()) {
        const size_t slash = dir.find('/', i);
        const std::string part = dir.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (!part.empty()) {
            acc += part;
            if (::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) return false;
            acc += "/";
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return true;
}

bool writeAtomic(const std::string &dir, const std::string &name,
                 const void *data, size_t len) {
    const std::string tmp = dir + "/" + name + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        if (len && !f.write(static_cast<const char *>(data), static_cast<std::streamsize>(len))) return false;
        f.flush();
        if (!f) return false;
    }
    const int fd = ::open(tmp.c_str(), O_RDONLY);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
    if (::rename(tmp.c_str(), (dir + "/" + name).c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    return true;
}

}   // namespace cachefile

// ---------------------------------------------------------------------------
// The compile counters.
//
// Ogre exposes no callback for "a shader was compiled" and no counter for "a
// shader came out of the microcode cache" — but it logs both, on every backend,
// with two fixed sentences (OgreVulkanProgram.cpp:343 and the compile path's
// "compiled successfully"). A LogListener is therefore the only zero-patch hook,
// and it is the one that counts ALL shaders rather than only the cacheable
// subset (a microcode-map delta would miss every low-level material script).
//
// Thread safety: with OGRE_SHADER_COMPILATION_THREADING_MODE at its default our
// compiles are single-threaded, but the counters are atomics anyway — mode 2 is
// a live question (SHADER_CACHE_SPEC row F) and this must not be what breaks.
class ShaderCache::Counter final : public Ogre::LogListener {
public:
    std::atomic<unsigned> compiled{0};
    std::atomic<unsigned> fromCache{0};
    /// THE MONITOR'S FEED (see ShaderCache::recordCompileNames). Written from
    /// whatever thread compiled (mode 2 = the scene's worker pool), drained on
    /// the UI thread. Bounded: a compile burst must never grow this without
    /// limit, and the count above is the honest total either way.
    std::atomic<bool>        recordNames{false};
    std::mutex               namesMutex;
    std::vector<std::string> names;
    static constexpr size_t  kMaxNames = 256u;

    /// THE PIPELINE BLOB'S ACTUAL FATE (audit F7). `loadPipelineCache` is void:
    /// the driver's verdict on the blob we hand it exists ONLY as a log line, so
    /// the same listener that counts shaders reads it. Three sentinels, all from
    /// OgreVulkanRenderSystem::loadPipelineCache:
    ///   ":613  Vulkan: Pipeline cache outdated, not loaded."  (header mismatch)
    ///   ":628  Vulkan: Pipeline cache loaded, N bytes."       (accepted)
    ///   ":635  Vulkan: Pipeline cache loading failed. ..."    (vkCreate failed)
    /// A FOURTH case says nothing at all: OGRE_VK_WORKAROUND_BROKEN_VKPIPELINECACHE
    /// makes load and save silent no-ops on PowerVR (OgreVulkanDevice.cpp:813).
    /// Defaulting the verdict to "not accepted" and only ever raising it on the
    /// "loaded" sentence covers that path for free — silence is a rejection.
    enum class PipelineVerdict { Silent, Accepted, Outdated, Rejected };
    std::atomic<PipelineVerdict> pipeline{PipelineVerdict::Silent};

    void messageLogged(const Ogre::String &message, Ogre::LogMessageLevel,
                       bool, const Ogre::String &, bool &) override {
        // Both shader sentences begin "Shader ". Bail on the first character for
        // the thousands of unrelated messages a startup logs.
        if (message.size() >= 8 && message.compare(0, 7, "Shader ") == 0) {
            if (message.find(" compiled successfully") != Ogre::String::npos) {
                ++compiled;
                if (recordNames.load(std::memory_order_relaxed)) {
                    std::lock_guard<std::mutex> lock(namesMutex);
                    if (names.size() < kMaxNames) names.push_back(message);
                }
            }
            else if (message.find(" was in microcode cache") != Ogre::String::npos) ++fromCache;
            return;
        }
        // The pipeline-cache verdicts. Same shape: one fixed prefix, then the
        // discriminating word. Cheap enough to sit beside the hot path.
        if (message.compare(0, 30, "Vulkan: Pipeline cache loaded,") == 0)
            pipeline.store(PipelineVerdict::Accepted);
        else if (message.compare(0, 29, "Vulkan: Pipeline cache outdat") == 0)
            pipeline.store(PipelineVerdict::Outdated);
        else if (message.compare(0, 29, "Vulkan: Pipeline cache loadin") == 0)
            pipeline.store(PipelineVerdict::Rejected);
    }
};

// Out of line, both of them: Counter is an incomplete type at every other
// translation unit that holds a ShaderCache by value (OgreEngine).
ShaderCache::ShaderCache() = default;
ShaderCache::~ShaderCache() {
    // THE LAST BYTES OF THE SESSION. The engine's destructor saves before it
    // tears anything down, and that save is now a hand-off — so the writer is
    // joined here, which is the point that runs after every caller has had its
    // turn and before the process can exit. A save in flight is waited for; a
    // wedged filesystem costs the quit kWriteWaitMs and no more.
    flushWrites(kWriteWaitMs);
    stopWriter();
    releaseLock();
}

// ---------------------------------------------------------------------------
void ShaderCache::configure(const std::string &dir, const std::string &appBuildId,
                            const std::string &mediaDir) {
    mDir = dir;
    mAppBuildId = appBuildId;
    mMediaDir = mediaDir;
    mEnabled = !dir.empty();
    if (!mEnabled) return;
    while (!mDir.empty() && mDir.back() == '/') mDir.pop_back();

    // The composite key (§4.2). Terms Ogre cannot see for itself come first;
    // the ones that mirror Ogre's own reject conditions are there so we fail at
    // the DIRECTORY level instead of three files in.
    std::ostringstream k;
    k << "format=" << kCacheFormat
      // The app's build identity. Our C++ decides which Hlms properties get set
      // and which datablocks exist; no hash inside Ogre can see that.
      << "|app=" << mAppBuildId
      // The engine library's own build identity, and the Ogre patch series that
      // produced the .so. Patches 0009/0010/0011 change SHADER BEHAVIOUR while
      // leaving every Ogre-side hash untouched — without this term, re-running
      // build-ogre.sh with a new patch leaves a cache Ogre considers perfect.
      << "|engine=" << JAHSHAKA_ENGINE_BUILD_ID
      << "|patches=" << JAHSHAKA_OGRE_PATCH_SERIES
      // Belt and braces over the staged templates (see hashTree).
      << "|media=" << hashTree(mMediaDir + "Hlms")
      // Ogre rejects a cache across these three anyway; failing here is faster
      // and, for the microcode file (which has NO version field at all), it is
      // the only check that exists.
#ifdef OGRE_DEBUG_STR_SIZE
      << "|dbgstr=" << OGRE_DEBUG_STR_SIZE
#else
      // Undefined in a Release-built Ogre: IdString then carries no readable
      // string and HlmsDiskCache writes 0 for it. The Debug/Release asymmetry
      // OgreHlmsDiskCache.h:65-70 warns about lives exactly here — a Release
      // cache cannot load into Debug — so the term must appear either way.
      << "|dbgstr=0"
#endif
      << "|hashbits=" << OGRE_HASH_BITS
#if OGRE_DEBUG_MODE
      << "|build=debug"
#else
      << "|build=release"
#endif
        ;
    // The GPU terms are appended by load(): mDeviceProperties does not exist
    // until the render system has a device, and configure() runs earlier.
    mFingerprint = k.str();
}

void ShaderCache::attachCounters() {
    if (mCounter) return;
    mCounter.reset(new Counter);
    if (Ogre::LogManager::getSingletonPtr() && Ogre::LogManager::getSingleton().getDefaultLog())
        Ogre::LogManager::getSingleton().getDefaultLog()->addListener(mCounter.get());
}

void ShaderCache::detachCounters() {
    if (!mCounter) return;
    if (Ogre::LogManager::getSingletonPtr() && Ogre::LogManager::getSingleton().getDefaultLog())
        Ogre::LogManager::getSingleton().getDefaultLog()->removeListener(mCounter.get());
    mCounter.reset();
}

void ShaderCache::recordCompileNames(bool on) {
    if (!mCounter) return;
    // SEED THE WINDOW HERE, under the same lock the drain takes. The monitor is
    // FORWARD-ONLY: a capture reports what compiled DURING it, and nothing else.
    // Without this seed `mCompileNamesAt` was still 0 (or the previous
    // capture's mark), so the first frame of every capture reported every
    // shader compiled since process start — thousands on a cold cache — and a
    // second capture re-reported everything that happened between the two.
    std::lock_guard<std::mutex> lock(mCounter->namesMutex);
    mCounter->recordNames.store(on, std::memory_order_relaxed);
    mCompileNamesAt = mCounter->compiled.load();
    mCounter->names.clear();
}

unsigned ShaderCache::drainCompileNames(std::vector<std::string> &out) {
    if (!mCounter) return 0u;
    // THE COUNT AND THE NAMES MUST BE TAKEN TOGETHER. The compiles run on the
    // scene's worker pool (OGRE_SHADER_COMPILATION_THREADING_MODE=2), so a
    // compile that lands between an unlocked read of the counter and the lock
    // would have its NAME drained here and its COUNT attributed to the next
    // window. One lock, one consistent window.
    std::lock_guard<std::mutex> lock(mCounter->namesMutex);
    const unsigned total = mCounter->compiled.load();
    const unsigned since = total - mCompileNamesAt;
    mCompileNamesAt = total;
    for (std::string &n : mCounter->names) out.push_back(std::move(n));
    mCounter->names.clear();
    return since;
}

void ShaderCache::progress(unsigned &compiled, unsigned &fromCache, unsigned &expected) const {
    compiled  = mCounter ? mCounter->compiled.load()  : 0u;
    fromCache = mCounter ? mCounter->fromCache.load() : 0u;
    expected  = mExpectedShaders;
}

std::string ShaderCache::path(const std::string &name) const { return mDir + "/" + name; }

// ---------------------------------------------------------------------------
// The single-writer lock (§4.3 rule 4). Two Jahshaka processes are routine — the
// editor plus a scripted run, and the whole gate runs many at once. Losing the
// lock must NEVER fail a run: the loser reads the cache and declines to write.
bool ShaderCache::acquireLock() {
    if (mLockFd >= 0) return mWriter;
    mLockFd = ::open(path(kLockFile).c_str(), O_RDWR | O_CREAT, 0644);
    if (mLockFd < 0) return false;
    struct flock fl {};
    fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
    mWriter = (::fcntl(mLockFd, F_SETLK, &fl) == 0);
    if (!mWriter) logLine("another process holds the writer lock — read-only for this run");
    return mWriter;
}

void ShaderCache::releaseLock() {
    if (mLockFd >= 0) { ::close(mLockFd); mLockFd = -1; }
    mWriter = false;
}

// ---------------------------------------------------------------------------
// The manifest. A strict line-oriented text file, NOT JSON: the engine has no
// JSON reader, and a hand-rolled one parsing a file whose whole job is to be
// hostile-input-safe is exactly the wrong trade. Unknown lines are ignored;
// anything missing or malformed rejects the whole directory.
//
//   jahshaka-shader-cache <format>
//   fingerprint <key>
//   saved <unix-ms>
//   shaders <count>
//   file <name> <bytes> <hash128>
bool ShaderCache::readManifest(std::vector<Entry> &filesOut) const {
    std::ifstream f(path(kManifest));
    if (!f) return false;
    std::string line, storedFingerprint;
    bool header = false;
    long long saved = 0;
    unsigned shaders = 0;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "jahshaka-shader-cache") { int v = -1; ls >> v; header = (v == kCacheFormat); }
        else if (tag == "fingerprint") { std::getline(ls >> std::ws, storedFingerprint); }
        else if (tag == "saved")    ls >> saved;
        else if (tag == "shaders")  ls >> shaders;
        else if (tag == "file") {
            Entry e{};
            ls >> e.name >> e.bytes >> e.hash;
            if (e.name.empty() || e.hash.size() != 32) return false;
            // A name that could escape the directory is a corrupt manifest, not
            // a file to open.
            if (e.name.find('/') != std::string::npos || e.name.find("..") != std::string::npos)
                return false;
            filesOut.push_back(e);
        }
    }
    if (!header) return false;
    if (storedFingerprint != mFingerprint) {
        logLine("fingerprint changed — discarding the cache");
        return false;
    }
    const_cast<ShaderCache *>(this)->mLastSavedUnixMs = saved;
    const_cast<ShaderCache *>(this)->mExpectedShaders = shaders;
    return true;
}

bool ShaderCache::writeManifest(const std::vector<Entry> &files, unsigned shaders) const {
    std::ostringstream o;
    o << "jahshaka-shader-cache " << kCacheFormat << "\n"
      << "fingerprint " << mFingerprint << "\n"
      << "saved " << nowUnixMs() << "\n"
      << "shaders " << shaders << "\n";
    for (const Entry &e : files) o << "file " << e.name << " " << e.bytes << " " << e.hash << "\n";
    const std::string s = o.str();
    return writeAtomic(mDir, kManifest, s.data(), s.size());
}

bool ShaderCache::readVerified(const Entry &e, std::vector<char> &out) const {
    if (!readWholeFile(path(e.name), out)) return false;
    if (out.size() != e.bytes) {
        logLine(e.name + ": length " + std::to_string(out.size()) + " != manifest " +
                std::to_string(e.bytes));
        return false;
    }
    if (hex128(out.data(), out.size()) != e.hash) {
        logLine(e.name + ": checksum mismatch");
        return false;
    }
    return true;
}

void ShaderCache::wipe() const {
    DIR *d = opendir(mDir.c_str());
    if (!d) return;
    while (dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == ".." || name == kLockFile) continue;
        // A recorded warm-up SET (*.set — the host parks warmup.set here) is
        // user intent, not fingerprint-derived data: the permutations it names
        // are exactly what a fresh cache should be warmed WITH. Wiping it
        // alongside the cache meant the one scenario the startup warm-up gate
        // exists for — replaying the set against a cold cache after an engine
        // update — could never occur (threading-P2 lane finding, 2026-09-07:
        // every post-update launch silently lost its content warm-up).
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".set") == 0) continue;
        ::unlink(path(name).c_str());
    }
    closedir(d);
}

bool ShaderCache::clear() {
    if (!mEnabled) return true;
    // A write in flight would otherwise land IN the directory we are about to
    // empty, leaving a manifest naming files this wipe deleted.
    flushWrites(kWriteWaitMs);
    wipe();
    mExpectedShaders = 0;
    mLastSavedUnixMs = 0;
    mPipelineLoaded = mMicrocodeLoaded = false;
    mPipelineReason = "absent";
    mHlmsLoaded = 0;
    mMicrocodeAtLoad = 0;
    // The next save must WRITE, even though nothing has compiled since the last
    // one — the files it would have skipped as "already on disk" are gone.
    //
    // What that save can and cannot recover is worth being precise about,
    // because it is the honest answer to "rebuild all cached data" mid-session:
    // the pipeline blob is always re-serializable, the Hlms caches are while
    // their dirty flag holds, but GpuProgramManager::saveMicrocodeCache
    // early-returns on a clean cache (OgreGpuProgramManager.cpp:333) — so on a
    // session that LOADED its microcode rather than compiling it, that layer
    // cannot be written back and is rebuilt on the next launch instead. Which
    // is exactly what "rebuild" means; it just costs one cold start.
    mForceSave = true;
    return true;
}

// ---------------------------------------------------------------------------
void ShaderCache::load(Ogre::Root *root) {
    if (!mEnabled || !root) return;
    Ogre::RenderSystem *rs = root->getRenderSystem();
    if (!rs) return;

    // The GPU half of the fingerprint (§4.2). Ogre re-checks vendor/device/
    // driver/UUID inside the pipeline blob itself, so this is redundancy ON
    // PURPOSE: it lets a GPU or driver swap reject the WHOLE directory instead
    // of silently loading two stale files and one rejected one. The values come
    // from the capabilities the render system already published — no new Ogre
    // type crosses the boundary.
    if (const Ogre::RenderSystemCapabilities *caps = rs->getCapabilities()) {
        std::ostringstream g;
        g << "|rs=" << rs->getName()
          << "|vendor=" << static_cast<int>(caps->getVendor())
          << "|device=" << caps->getDeviceName()
          << "|driver=" << caps->getDriverVersion().toString();
        mFingerprint += g.str();
    }

    if (!mkpath(mDir)) { logLine("cannot create " + mDir + " — cache disabled"); mEnabled = false; return; }
    acquireLock();

    // MICROCODE SAVING MUST BE ENABLED BEFORE THE LOAD. GpuProgramManager's own
    // header says so, and the canonical wiring does it in this order
    // (GraphicsSystem.cpp:648-658). It is also the flag that makes the SAVE side
    // record anything at all, so it goes on even when there is nothing to read.
    Ogre::GpuProgramManager::getSingleton().setSaveMicrocodesToCache(true);

    std::vector<Entry> files;
    if (!readManifest(files)) { wipe(); return; }

    // Read and VERIFY everything before handing a single byte to Ogre. If any
    // file is short, corrupt or missing, the whole generation goes: a cache that
    // is half-valid is exactly the state the microcode loader cannot survive.
    std::vector<char> pipeline, microcode;
    std::vector<std::pair<int, std::vector<char>>> hlms;
    for (const Entry &e : files) {
        std::vector<char> bytes;
        if (!readVerified(e, bytes)) { wipe(); logLine("verification failed — starting cold"); return; }
        if      (e.name == "pipeline.cache")  pipeline.swap(bytes);
        else if (e.name == "microcode.cache") microcode.swap(bytes);
        else if (e.name.compare(0, 5, "hlms.") == 0) {
            const int type = std::atoi(e.name.c_str() + 5);
            hlms.emplace_back(type, std::move(bytes));
        }
    }

    // Order is upstream's (OgreHlmsDiskCache.h:74-77 + GraphicsSystem.cpp).
    try {
        if (!pipeline.empty()) {
            // NAMED streams, all three of them (audit F11). Ogre logs the stream
            // NAME when it reads one — "Loading HlmsDiskCache from " with an
            // empty tail is the log line that started the caching audit — and a
            // MemoryDataStream built from the anonymous ctor has none. The full
            // path is the useful name: it says which cache directory a session
            // read, which is exactly the question a support log has to answer.
            Ogre::DataStreamPtr s(OGRE_NEW Ogre::MemoryDataStream(path("pipeline.cache"),
                                                                  pipeline.data(), pipeline.size(),
                                                                  false, true));
            // The verdict comes from the LOG, not from the call (F7): the driver
            // may reject the blob outright (wrong device, wrong driver version,
            // bad hash) and loadPipelineCache would still return void. Clear the
            // listener's verdict first so a second load in the same process
            // cannot inherit the first one's answer.
            if (mCounter) mCounter->pipeline.store(Counter::PipelineVerdict::Silent);
            mPipelineReason = "silent";
            rs->loadPipelineCache(s);
            if (mCounter) {
                switch (mCounter->pipeline.load()) {
                case Counter::PipelineVerdict::Accepted:
                    mPipelineLoaded = true;  mPipelineReason = "accepted"; break;
                case Counter::PipelineVerdict::Outdated:
                    mPipelineLoaded = false; mPipelineReason = "outdated"; break;
                case Counter::PipelineVerdict::Rejected:
                    mPipelineLoaded = false; mPipelineReason = "rejected"; break;
                case Counter::PipelineVerdict::Silent:
                    mPipelineLoaded = false; mPipelineReason = "silent";   break;
                }
            }
        }
        if (!microcode.empty()) {
            Ogre::DataStreamPtr s(OGRE_NEW Ogre::MemoryDataStream(path("microcode.cache"),
                                                                  microcode.data(), microcode.size(),
                                                                  false, true));
            Ogre::GpuProgramManager::getSingleton().loadMicrocodeCache(s);
            mMicrocodeLoaded = true;
            // Ogre exposes no count for the live map, but the file's first
            // uint32 IS the entry count (OgreGpuProgramManager.cpp:344) and
            // loadMicrocodeCache clears the map first, so after a successful
            // load the two are the same number. We verified this file's
            // checksum before reading a byte of it, which is the only reason
            // trusting that uint32 is defensible at all: upstream trusts it
            // with no verification whatsoever and allocates from it directly.
            if (microcode.size() >= sizeof(Ogre::uint32)) {
                Ogre::uint32 n = 0;
                std::memcpy(&n, microcode.data(), sizeof(n));
                mMicrocodeAtLoad = n;
            }
        }
        if (!hlms.empty()) {
            Ogre::HlmsManager *hm = root->getHlmsManager();
            Ogre::HlmsDiskCache disk(hm);
            for (auto &entry : hlms) {
                Ogre::Hlms *h = hm->getHlms(static_cast<Ogre::HlmsTypes>(entry.first));
                if (!h) continue;
                Ogre::DataStreamPtr s(OGRE_NEW Ogre::MemoryDataStream(
                    path("hlms." + std::to_string(entry.first) + ".bin"),
                    entry.second.data(), entry.second.size(), false, true));
                disk.loadFrom(s);
                // THREADED CACHE APPLY (THREADING_ADOPTION_SPEC.md P1). This
                // used to pass a hardcoded 1 with a comment explaining that
                // anything else was a lie: mode 1 in a SHARED build leaves
                // supportsMultithreadedShaderCompilation() false, and applyTo
                // takes its serial branch whatever it is given
                // (OgreHlmsDiskCache.cpp:396). Since the engine is built with
                // OGRE_SHADER_COMPILATION_THREADING_MODE=2 the number is real,
                // and this is the single largest win of the whole flip: every
                // shader in the disk cache is re-created here, on the critical
                // path of the first createView.
                //
                // WHY THIS COUNT. Nothing better is available at this point in
                // the boot — the cache loads inside the first createView, and
                // no Scene (and so no worker pool) exists yet. Studio's
                // measured Primary tier is clamp(idealThreadCount, 2, 8)
                // (src/bridge/sceneworkerthreads.h); the engine cannot include
                // that header (IrisGL and Studio never link each other), so the
                // policy is reproduced here rather than shared. 8 is the same
                // measured ceiling, and the floor of 1 keeps a
                // hardware_concurrency() of 0 (allowed to fail) honest.
                unsigned threads = std::thread::hardware_concurrency();
                if (threads == 0u) threads = 1u;
                if (threads > 8u) threads = 8u;
                disk.applyTo(h, threads);
                ++mHlmsLoaded;
            }
        }
    } catch (const Ogre::Exception &e) {
        // Rule 7: never fatal. Ogre throws typed exceptions here and the
        // canonical wiring catches them exactly like this.
        logLine(std::string("load failed (") + e.getDescription() + ") — starting cold");
        wipe();
        mPipelineLoaded = mMicrocodeLoaded = false;
        mPipelineReason = "rejected";
        mHlmsLoaded = 0;
        return;
    }
    logLine("loaded: pipeline=" + std::string(mPipelineLoaded ? "yes" : "no") +
            " microcode=" + std::to_string(mMicrocodeAtLoad) + " entries" +
            " hlms=" + std::to_string(mHlmsLoaded));
}

// ---------------------------------------------------------------------------
bool ShaderCache::dirty(Ogre::Root *root) const {
    if (!mEnabled || !mWriter || !root) return false;
    // Nothing has compiled since the last successful write. Ogre's own dirty
    // flags do NOT settle after a save (saveMicrocodeCache early-returns on a
    // clean cache but never clears the flag), so without this the clean-quit
    // path would serialize and rewrite every layer twice — once from
    // EngineHost::shutdown and again from ~OgreEngine.
    if (mCounter && mSavedAtCompileCount == mCounter->compiled.load() + mCounter->fromCache.load()
        && mLastSavedUnixMs != 0)
        return false;
    if (Ogre::GpuProgramManager::getSingletonPtr() &&
        Ogre::GpuProgramManager::getSingleton().isCacheDirty())
        return true;
    Ogre::HlmsManager *hm = root->getHlmsManager();
    if (!hm) return false;
    for (int i = Ogre::HLMS_LOW_LEVEL + 1; i < Ogre::HLMS_MAX; ++i)
        if (Ogre::Hlms *h = hm->getHlms(static_cast<Ogre::HlmsTypes>(i)))
            if (h->isShaderCodeCacheDirty()) return true;
    // The pipeline blob has no dirty flag; a run that compiled anything at all
    // has almost certainly created PSOs too.
    return mCounter && mCounter->compiled.load() > 0u;
}

bool ShaderCache::save(Ogre::Root *root) {
    if (!mEnabled || !root) return false;
    // NOT RE-ENTRANT, AND IT IS CHEAP TO SAY SO — but read the second paragraph
    // before believing it fixed anything.
    //
    // A save serializes about a megabyte in memory, walks Ogre's Hlms caches
    // through HlmsDiskCache::copyFrom, and calls vkGetPipelineCacheData. It is
    // reachable from three places — the host's watchdog QTimer, the
    // `app.saveShaderCache()` verb (scripts and MCP), and the clean-quit save —
    // and two of them running at once would mean two of them handing the writer
    // a job. One bool closes that. It is deliberately not a lock: every caller
    // is the main thread, and a save arriving from anywhere else is a bug this
    // would hide rather than fix.
    //
    // IT DID NOT CONTRIBUTE TO THE 2026-09-14 CRASHES, and the first version of
    // this comment implied it did (round-2 review item 4). The reasoning was
    // that the app's waits pump the event loop with ExcludeUserInputEvents,
    // which does not exclude timers — true, but irrelevant HERE: nothing in
    // this function's body pumps an event loop, so the window a nested pump
    // would need is a window save() never opens. The crash was Ogre indexing
    // its own vectors with unvalidated indices and ogre-patch 0035 is the guard
    // for it. This bool is hygiene, kept on its own merits.
    if (mSaving) { logLine("save already in progress — skipped the re-entrant call"); return false; }
    struct Reentry {
        bool &flag;
        explicit Reentry(bool &f) : flag(f) { flag = true; }
        ~Reentry() { flag = false; }
    } reentry(mSaving);
    // ONE WRITER, ONE JOB (FSYNC-1). Everything below this line serializes a
    // fresh copy of the layers, so a write still in flight means THIS SAVE IS
    // SKIPPED — never waited for. The layers stay dirty and the next save
    // writes them, so skipping costs nothing; waiting would cost the calling
    // thread (the UI thread, for the host's timer) the rest of a write that is
    // slow for exactly the reason this writer thread exists — a disk behind a
    // stream of dirty pages — which is the block this lane removed. The callers
    // that MEAN to wait (the clean quit, the destructor, a wipe) call
    // flushWrites() themselves. (Lead's merge read, 2026-09-15: the first cut
    // waited here with a 30 s bound.)
    if (writeInFlight()) {
        logLine("the previous write is still in flight — skipping this save");
        return false;
    }
    if (!mWriter && !acquireLock()) return false;   // read-only run: not an error
    Ogre::RenderSystem *rs = root->getRenderSystem();
    if (!rs || !Ogre::GpuProgramManager::getSingletonPtr()) return false;
    if (!mForceSave && !dirty(root)) return true;    // nothing new — a no-op, not a failure
    mForceSave = false;
    // The DIRECTORY may not exist: app.clearShaderCache() removes it whole (the
    // host's half deletes recursively), and the clean-quit save that follows
    // has to be able to write the session's shaders back into it. Found by
    // shadercache.app, whose run 2 was silently cold because every write in
    // run 1's final save failed against a missing directory.
    if (!mkpath(mDir)) { logLine("cannot recreate " + mDir + " — nothing saved"); return false; }

    const auto serializeStart = std::chrono::steady_clock::now();
    Ogre::HlmsManager *hm = root->getHlmsManager();
    // THE JOB, filled here and written on the writer thread. Serializing is
    // Ogre's half and belongs to this thread; from the moment a layer is a
    // `std::vector<char>` nothing about it is Ogre's any more.
    auto job = std::unique_ptr<PendingWrite>(new PendingWrite());

    // Each layer becomes a blob in the job. The bytes are checksummed and
    // written by the writer thread — this side never touches the disk.
    auto emit = [&](const std::string &name, std::vector<char> &bytes) {
        if (bytes.empty()) return;
        job->names.push_back(name);
        job->blobs.push_back(std::move(bytes));
    };
    // INTO MEMORY, NOT THROUGH A SCRATCH FILE (FSYNC-1). Ogre's three writers
    // want a DataStreamPtr and none of them can be asked how many bytes they
    // are about to produce, which is why this used to write a `*.building` file
    // and read it back: MemoryDataStream is fixed-size. A stream that GROWS
    // removes the file — and with it 1.5 MB of buffered write plus the read on
    // this thread, which the same dirty-page queue that owns the fsync also
    // throttles (486 ms measured on the serialization alone, with a build's
    // writeback in front of it, against 17 ms quiet). The bytes were always
    // copied into a vector at the end of this lambda; now they start there.
    auto serialize = [&](const std::string &name,
                         const std::function<void(Ogre::DataStreamPtr &)> &writer,
                         std::vector<char> &out) -> bool {
        // Named for the file these bytes END UP IN (F11): a log line from
        // inside Ogre's writer names something that exists.
        GrowingMemoryStream *mem = OGRE_NEW GrowingMemoryStream(mDir + "/" + name);
        Ogre::DataStreamPtr s(mem);
        writer(s);
        s->close();
        out = mem->take();
        return true;
    };

    try {
        if (hm) {
            Ogre::HlmsDiskCache disk(hm);
            for (int i = Ogre::HLMS_LOW_LEVEL + 1; i < Ogre::HLMS_MAX; ++i) {
                Ogre::Hlms *h = hm->getHlms(static_cast<Ogre::HlmsTypes>(i));
                if (!h || !h->isShaderCodeCacheDirty()) continue;
                disk.copyFrom(h);
                std::vector<char> bytes;
                if (serialize("hlms." + std::to_string(i) + ".bin",
                              [&](Ogre::DataStreamPtr &s) { disk.saveTo(s); }, bytes))
                    emit("hlms." + std::to_string(i) + ".bin", bytes);
            }
        }
        if (Ogre::GpuProgramManager::getSingleton().isCacheDirty()) {
            std::vector<char> bytes;
            if (serialize("microcode.cache",
                          [](Ogre::DataStreamPtr &s) {
                              Ogre::GpuProgramManager::getSingleton().saveMicrocodeCache(s);
                          }, bytes))
                emit("microcode.cache", bytes);
        }
        {
            // No dirty flag exists for the pipeline blob and it is cheap; always
            // rewrite it while we are here. vkGetPipelineCacheData is documented
            // as fragile if called too close to PSO creation, which is why this
            // only ever runs on a settled burst or on shutdown, never on a timer
            // during a compile storm (§4.4).
            std::vector<char> bytes;
            if (serialize("pipeline.cache",
                          [rs](Ogre::DataStreamPtr &s) { rs->savePipelineCache(s); }, bytes))
                emit("pipeline.cache", bytes);
        }
    } catch (const Ogre::Exception &e) {
        logLine(std::string("save failed (") + e.getDescription() + ")");
        return false;
    }

    // THE SPLASH DENOMINATOR, and it is LAST-RUN, not all-time (audit F6).
    // shaderbuildgate.h:26-29 promises "the run that wrote the cache recorded
    // how many shaders it needed", and std::max broke that promise in one
    // direction only: one heavy world (or one run with the cache disabled, or
    // one that opened five projects) pinned the number forever and every launch
    // afterwards showed "61/76" and stopped. A session total that only ever
    // grows is not a denominator, it is a high-water mark.
    //
    // The last save of a session is the clean-quit save (EngineHost::shutdown),
    // so the value that survives IS the session total — which is what the next
    // launch should expect to build or serve. It is read HERE, on the counting
    // thread, and travels with the job: the manifest the writer produces must
    // name the session this save measured, not one that moved under it.
    if (mCounter) {
        mExpectedShaders = mCounter->compiled.load() + mCounter->fromCache.load();
        job->compileCount = mExpectedShaders;
    }
    logLine("serialized " + std::to_string(job->names.size()) + " file(s) in " +
            std::to_string(int(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - serializeStart).count())) +
            " ms — handing the write off");
    if (!dispatchWrite(std::move(job))) {
        // Only reachable if a write started between the flush above and here,
        // which no caller can do: every one of them is this thread.
        logLine("a write was already in flight — nothing dispatched");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// THE WRITER THREAD (FSYNC-1).
//
// Everything below runs off the caller's thread and touches NO Ogre object:
// blobs the caller serialized, the manifest, and the size cap. The reason it
// exists is `fsync`, which is how this cache earns its atomic-rename contract
// and which waits behind whatever else the filesystem's journal is holding —
// 17 ms on an idle disk against 403 ms measured with a stream of dirty pages in
// front of it, on the UI thread, in the middle of an archive the user was
// watching. The contract does not move: bytes to a sibling `.tmp`, flushed,
// renamed over the target, manifest last, so a crash at any instant leaves the
// previous generation or the new one. Only the thread that waits moves.
bool ShaderCache::runWrite(const PendingWrite &job) {
    std::vector<Entry> files;
    for (size_t i = 0; i < job.names.size(); ++i) {
        const std::vector<char> &bytes = job.blobs[i];
        if (bytes.empty()) continue;
        if (!writeAtomic(mDir, job.names[i], bytes.data(), bytes.size())) {
            logLine("could not write " + job.names[i]);
            continue;
        }
        files.push_back({job.names[i], bytes.size(), hex128(bytes.data(), bytes.size())});
    }

    // Files we did not rewrite this time are still valid: carry their manifest
    // entries forward, or the next run would reject a perfectly good file.
    std::vector<Entry> previous;
    readManifest(previous);
    for (const Entry &p : previous) {
        const bool rewritten = std::any_of(files.begin(), files.end(),
                                           [&](const Entry &e) { return e.name == p.name; });
        struct stat st {};
        if (!rewritten && ::stat(path(p.name).c_str(), &st) == 0) files.push_back(p);
    }

    // THE MANIFEST IS THE PUBLICATION. Every file above is already in place and
    // already durable; until this line names them, a reader still sees the
    // previous generation. Failing here therefore leaves the cache exactly as
    // it was, which is why nothing below the write is allowed to run on a
    // false.
    if (!writeManifest(files, job.compileCount)) {
        logLine("the manifest could not be written — the previous cache stands");
        return false;
    }
    mLastSavedUnixMs.store(nowUnixMs());
    mSavedAtCompileCount.store(job.compileCount);

    // Size cap: wipe the generation rather than evict (§4.3 rule 6).
    unsigned n = 0;
    if (dirBytes(mDir, &n) > kMaxCacheBytes) {
        logLine("cache exceeded the size cap — wiped; the next launch re-warms it");
        wipe();
        return true;
    }
    logLine("saved " + std::to_string(files.size()) + " files");
    return true;
}

void ShaderCache::writerLoop() {
    for (;;) {
        std::unique_ptr<PendingWrite> job;
        {
            std::unique_lock<std::mutex> lock(mWriteMutex);
            mWriteCv.wait(lock, [this]() { return mWriteStop || mWriteJob != nullptr; });
            if (!mWriteJob) return;           // stopping, and nothing left to write
            job = std::move(mWriteJob);
            mWriteBusy = true;
        }
        const auto began = std::chrono::steady_clock::now();
        const bool ok = runWrite(*job);
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began).count();
        logLine(std::string(ok ? "write finished in " : "write FAILED after ") +
                std::to_string(ms) + " ms (off the calling thread)");
        {
            std::lock_guard<std::mutex> lock(mWriteMutex);
            mWriteBusy = false;
        }
        mWriteDoneCv.notify_all();
    }
}

bool ShaderCache::dispatchWrite(std::unique_ptr<PendingWrite> job) {
    std::unique_lock<std::mutex> lock(mWriteMutex);
    if (mWriteJob || mWriteBusy) return false;
    mWriteJob = std::move(job);
    if (!mWriteThread.joinable()) {
        mWriteStop = false;
        mWriteThread = std::thread([this]() { writerLoop(); });
    }
    lock.unlock();
    mWriteCv.notify_one();
    return true;
}

bool ShaderCache::writeInFlight() const {
    std::lock_guard<std::mutex> lock(mWriteMutex);
    return mWriteJob || mWriteBusy;
}

bool ShaderCache::flushWrites(unsigned budgetMs) {
    std::unique_lock<std::mutex> lock(mWriteMutex);
    if (!mWriteJob && !mWriteBusy) return true;
    return mWriteDoneCv.wait_for(lock, std::chrono::milliseconds(budgetMs),
                                 [this]() { return !mWriteJob && !mWriteBusy; });
}

void ShaderCache::stopWriter() {
    {
        std::lock_guard<std::mutex> lock(mWriteMutex);
        if (!mWriteThread.joinable()) return;
        mWriteStop = true;
    }
    mWriteCv.notify_all();
    mWriteThread.join();
}

// ---------------------------------------------------------------------------
ShaderCacheStats ShaderCache::stats(Ogre::Root *root) const {
    ShaderCacheStats s;
    s.enabled = mEnabled;
    s.dir = mDir;
    s.fingerprint = mEnabled ? hexOf(mFingerprint) : std::string();
    if (mEnabled) s.sizeBytes = dirBytes(mDir, &s.files);
    s.pipelineCacheLoaded = mPipelineLoaded;
    s.pipelineCacheReason = mPipelineReason;
    s.microcodeLoaded = mMicrocodeLoaded;
    s.hlmsCachesLoaded = mHlmsLoaded;
    s.microcodeEntries = static_cast<unsigned>(mMicrocodeAtLoad);
    s.compiledThisRun = mCounter ? mCounter->compiled.load() : 0u;
    s.loadedThisRun   = mCounter ? mCounter->fromCache.load() : 0u;
    s.expectedShaders = mExpectedShaders;
    s.lastSavedUnixMs = mLastSavedUnixMs;

    // THE SHADER HASH'S TWO INDEX SPACES, LIVE (HLMSBITS-1). Ogre packs every
    // shader lookup as [type:3][renderable:16][pass:13] and grows both caches
    // for the life of the process with no eviction anywhere; the pass side ran
    // out of its EIGHT bits on 2026-09-14 and corrupted the renderable index,
    // which is the crash ogre-patch 0046 rebalanced the fields for. The worst
    // Hlms is reported because one exhausted cache is the fault whichever Hlms
    // owns it, and the capacities travel with the counts so a reading is
    // self-describing ("115 of 8192") on any future split.
    if (root) {
        if (Ogre::HlmsManager *hlmsManager = root->getHlmsManager()) {
            for (int i = 0; i < Ogre::HLMS_MAX; ++i) {
                Ogre::Hlms *hlms = hlmsManager->getHlms(static_cast<Ogre::HlmsTypes>(i));
                if (!hlms) continue;
                s.passCacheEntries =
                    std::max(s.passCacheEntries, unsigned(hlms->getPassCacheSize()));
                s.renderableCacheEntries =
                    std::max(s.renderableCacheEntries, unsigned(hlms->getRenderableCacheSize()));
            }
            s.passCacheCapacity = unsigned(Ogre::Hlms::getMaxPassCacheEntries());
            s.renderableCacheCapacity = unsigned(Ogre::Hlms::getMaxRenderableCacheEntries());
        }
    }
    return s;
}

}  // namespace detail
}}  // namespace jahshaka::engine
