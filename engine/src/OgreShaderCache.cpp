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
//  * drop by age. An entry leaves only when a save would pass the size cap,
//    the least recently used first (EntryUses.h; runWrite).
//  * survive doubt. Every failure path ends in "delete the directory, run cold".
#include "EnginePrivate.h"
#include "ShaderCacheKeys.h"

#include <OgreHlmsAsyncCompiler.h>
#include <mutex>
// JAHSHAKA_SHADER_CACHE_CODE_ID: a hash of THIS file (the container that writes the
// layers' bytes), regenerated on every BUILD (irisgl/cmake/EngineBuildId.cmake, which
// says why the engine's other sources are not a term: an Hlms entry is keyed by its
// merged properties and pieces after every C++ hook - TEST-STREAMLINE-1 1a).
#include "jahshaka_engine_build_id.h"

#include <OgreGpuProgramManager.h>
#include <OgreHlms.h>
#include <OgreHlmsDiskCache.h>
#include <limits>
#include <cstdio>
#include <OgreHlmsManager.h>
#include <OgreRenderSystem.h>
#include <OgreRenderSystemCapabilities.h>
#include <OgreDataStream.h>
#include <OgreLog.h>
#include <Hash/MurmurHash3.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
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
#include <execinfo.h>
#include <algorithm>
#include <map>
#include <set>
#include <OgreResourceGroupManager.h>

#include "core/threadbudget.h"   // the boot apply's width (THREADING-1)
#include "core/files.h"          // the caches' file I/O (NOQT-CORE-1)

namespace jahshaka { namespace engine {
namespace detail {

// The caches' content hash is DEFINED at the bottom of this file (`namespace cachefile`) and declared in
// EnginePrivate.h; pulled in here so the rest of the file reads as it did when it was a file static.
using cachefile::hex128;
using cachefile::hexOf;

namespace {
/// Milliseconds with three decimals (the copy steps are sub-millisecond: an int would print 0).
std::string fmtMs(double ms) { char b[32]; std::snprintf(b, sizeof b, "%.3f", ms); return b; }
/// A layer key as the log prints it: its first 16 hex digits (64 bits - enough to tell two keys apart by eye).
std::string shortKey(const std::string &hex) { return hex.substr(0, 16); }

/// Our container's own format version. Bump it and every existing cache on
/// every machine is discarded — the escape hatch for a change in THIS file that
/// none of the other fingerprint terms would notice. 2 = one key PER LAYER
/// (SPEED-CPU, perf audit CS-1): the manifest names each file's layer key.
constexpr int kCacheFormat = 2;

/// The four layers, by file name: each is kept or dropped on its own key.
enum class Layer { Pipeline, Microcode, Hlms, Compute, Unknown };
Layer layerOf(const std::string &name) {
    if (name == "pipeline.cache") return Layer::Pipeline;
    if (name == "microcode.cache") return Layer::Microcode;
    if (name == "compute.bin") return Layer::Compute;
    if (name.compare(0, 5, "hlms.") == 0) return Layer::Hlms;
    return Layer::Unknown;
}

/// The SPIR-V compiler the fork's Vulkan render system links (statically, from
/// the system's glslang at build-ogre.sh time). A compiler upgrade under an
/// unchanged fork commit is a different microcode producer.
#if __has_include(<glslang/build_info.h>)
#include <glslang/build_info.h>
#define JAH_GLSLANG_VERSION_TERM \
    (std::to_string(GLSLANG_VERSION_MAJOR) + "." + std::to_string(GLSLANG_VERSION_MINOR) + "." + \
     std::to_string(GLSLANG_VERSION_PATCH) + GLSLANG_VERSION_FLAVOR)
#else
#define JAH_GLSLANG_VERSION_TERM std::string("unknown")
#endif

// The size cap (§4.3 rule 6): EngineConfig::shaderCacheMaxBytes, held in mMaxBytes, against the bytes of the layers a
// save's manifest names. Over it the save EVICTS whole entries, least recently used first (runWrite); a wipe only backs
// that up.

constexpr const char *kManifest = "cache-manifest.txt";
/// The entries' last-used stamps and the launch counter (EntryUses.h): a file of ours beside the layers, self-checked,
/// never in the manifest (a launch that only moved a stamp writes this file alone).
constexpr const char *kEntryUses = "entry-uses.bin";
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
// The caches' content hash (EnginePrivate.h `namespace cachefile`); their file I/O is iris::files.
namespace cachefile {

std::string hex128(const void *data, size_t len) {
    Ogre::uint64 out[2] = {};
    Ogre::MurmurHash3_x64_128(data, static_cast<int>(len), 0x9E3779B9u, out);
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(out[0]), static_cast<unsigned long long>(out[1]));
    return std::string(buf);
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
    /// Of `compiled`, those built on the background compiler's threads (ASYNC-SHADERS-1):
    /// no frame waited for them.
    std::atomic<unsigned> background{0};
    /// ...and those built on any OTHER thread (a frame that waited). Its own counter, not
    /// compiled - background: two loads of two counters race a service-thread compile that
    /// lands between them (read compiled, then a compile bumps both, then read background) and
    /// the difference dips by one and comes back — a phantom "UI-thread compile" the host's
    /// live-compile sentry counted (shader.sample_opens_quiet, red under load, ASYNC-SHADERS-1).
    std::atomic<unsigned> foreground{0};
    /// THE MONITOR'S FEED (see ShaderCache::recordCompileNames). Written from
    /// whatever thread compiled (mode 2 = the scene's worker pool), drained on
    /// the UI thread. Bounded: a compile burst must never grow this without
    /// limit, and the count above is the honest total either way.
    std::atomic<bool>        recordNames{false};
    /// Engine::setCompileObserver (SHADER-WARM-2): called after a compile made
    /// on `observerThread` only. Set and cleared on that thread, between
    /// compiles; read here on whatever thread compiled.
    std::function<void()>    observer;
    std::atomic<std::thread::id> observerThread{};
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
                if (Ogre::HlmsAsyncCompiler::isServiceThread()) ++background;
                else ++foreground;
                // THE OFFENDER, BY ITS STACK (ASYNC-SHADERS-1): a shader compiled on any thread
                // but the background compiler's while a view compiles in the background is a
                // frame waiting for glslang; its call stack names the path (stderr, so a row's
                // driver prints it with the app's output).
                if (!Ogre::HlmsAsyncCompiler::isServiceThread() && detail::asyncViewCount() > 0) {
                    std::fprintf(stderr, "Jahshaka shader cache: OFF-SERVICE COMPILE while a view "
                                         "compiles in the background: %s\n", message.c_str());
                    void *frames[48];
                    const int n = ::backtrace(frames, 48);
                    ::backtrace_symbols_fd(frames, n, 2);
                }
                if (observerThread.load(std::memory_order_relaxed) == std::this_thread::get_id() &&
                    observer)
                    observer();
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
    // turn and before the process can exit — normally a no-op, because the
    // engine's destructor already called finishWrites() before deleting Root.
    // A save in flight is waited for and the join is UNBOUNDED: a wedged
    // filesystem holds the quit for as long as the write takes, because the
    // bytes are never abandoned (kWriteWaitMs only decides when we stop
    // waiting politely and join).
    finishWrites();
    releaseLock();
}

// ---------------------------------------------------------------------------
void ShaderCache::configure(const std::string &dir, const std::string &seedDir, unsigned long long maxBytes) {
    mMaxBytes = maxBytes;
    mDir = dir;
    mEnabled = !dir.empty();
    // THE SEED RIDES THE CACHE (rule 8): a cache that is off reads nothing, the seed included.
    mSeedDir = mEnabled ? seedDir : std::string();
    while (mSeedDir.size() > 1 && mSeedDir.back() == '/') mSeedDir.pop_back();
    mSeedState = "off";
    mSeedEntries = 0;
    if (!mEnabled) return;
    while (!mDir.empty() && mDir.back() == '/') mDir.pop_back();

    // ONE KEY PER LAYER (SPEED-CPU, perf audit CS-1), EACH NAMING ONLY ITS PRODUCER (SHADER-SYSTEM-1 stage 5: the terms
    // and their reasons are composeLayerKeys', below). The fork's commit and the staged media trees are no term of any
    // layer any more: a fork commit or a media edit that changes no producer drops nothing - an entry is named by the
    // content it was made from (SPIR-V by its source, a recipe by its properties and pieces under Ogre's template
    // checksum, a compute record by its input key).
    mKeyInputs = LayerKeyInputs();
    mKeyInputs.format = kCacheFormat;
    mKeyInputs.code = JAHSHAKA_SHADER_CACHE_CODE_ID;
    mKeyInputs.microcodePath = JAHSHAKA_COMPILE_PATH_MICROCODE_ID;
    mKeyInputs.preprocessorPath = JAHSHAKA_COMPILE_PATH_PREPROCESSOR_ID;
    mKeyInputs.computePath = JAHSHAKA_COMPILE_PATH_COMPUTE_ID;
    mKeyInputs.glslang = JAH_GLSLANG_VERSION_TERM;
#if OGRE_DEBUG_MODE
    mKeyInputs.debugBuild = true;
#endif
#ifdef OGRE_DEBUG_STR_SIZE
    mKeyInputs.dbgstr = std::to_string(OGRE_DEBUG_STR_SIZE);
#else
    // Undefined in a Release-built Ogre: IdString then carries no readable string and HlmsDiskCache writes 0 for it. The
    // Debug/Release asymmetry OgreHlmsDiskCache.h:65-70 warns about lives exactly here - a Release cache cannot load into
    // Debug - so the term must appear either way.
    mKeyInputs.dbgstr = "0";
#endif
    mKeyInputs.hashBits = OGRE_HASH_BITS;
    applyLayerKeys();
    // The GPU terms are added by load(): the render system has no device until then, and configure() runs earlier.
}

void ShaderCache::applyLayerKeys() {
    const LayerKeys k = composeLayerKeys(mKeyInputs);
    mLayerKey[0] = k.pipeline;
    mLayerKey[1] = k.microcode;
    mLayerKey[2] = k.hlms;
    mLayerKey[3] = k.compute;
    composeFingerprint();
}

// THE LAST-USED STAMPS (SHADER-SYSTEM-1 F10; EntryUses.h says why).
std::uint32_t stampByUse(const UseTable &previous, const UseKey &key, bool used, std::uint32_t launch) {
    if (used) return launch;
    const auto it = previous.find(key);
    return it != previous.end() && it->second <= launch ? it->second : launch;
}

namespace {
/// "JAHUSES2": the launch counter rides the table (the final read's ruling). A "JAHUSES1" table is not read.
constexpr char kUsesMagic[8] = { 'J', 'A', 'H', 'U', 'S', 'E', 'S', '2' };
}  // namespace

std::vector<char> writeUseTable(const UseTable &table, std::uint32_t launch) {
    std::vector<char> v(kUsesMagic, kUsesMagic + sizeof kUsesMagic);
    auto raw = [&v](const void *p, size_t n) { v.insert(v.end(), (const char *)p, (const char *)p + n); };
    raw(&launch, sizeof launch);
    const std::uint64_t count = table.size();
    raw(&count, sizeof count);
    for (const auto &e : table) {
        raw(&e.first.layer, sizeof e.first.layer);
        raw(&e.first.h0, sizeof e.first.h0);
        raw(&e.first.h1, sizeof e.first.h1);
        raw(&e.second, sizeof e.second);
    }
    std::uint64_t sum[2] = { 0u, 0u };
    Ogre::MurmurHash3_x64_128(v.data(), int(v.size()), Ogre::IdString::Seed, sum);
    raw(sum, sizeof sum);
    return v;
}

bool readUseTable(const char *data, std::size_t size, UseTable &out, std::uint32_t &launch) {
    out.clear();
    launch = 0u;
    constexpr std::size_t kEntry = 1 + 8 + 8 + 4, kHead = sizeof kUsesMagic + 4 + 8, kSum = 16;
    if (size < kHead + kSum || std::memcmp(data, kUsesMagic, sizeof kUsesMagic) != 0) return false;
    std::uint64_t count = 0;
    std::uint32_t written = 0;
    std::memcpy(&written, data + sizeof kUsesMagic, sizeof written);
    std::memcpy(&count, data + sizeof kUsesMagic + 4, sizeof count);
    if (count > (size - kHead - kSum) / kEntry || kHead + count * kEntry + kSum != size) return false;
    std::uint64_t sum[2] = { 0u, 0u }, stored[2] = { 0u, 0u };
    Ogre::MurmurHash3_x64_128(data, int(size - kSum), Ogre::IdString::Seed, sum);
    std::memcpy(stored, data + size - kSum, kSum);
    if (sum[0] != stored[0] || sum[1] != stored[1]) return false;
    const char *p = data + kHead;
    for (std::uint64_t i = 0; i < count; ++i, p += kEntry) {
        UseKey k;
        std::uint32_t stamp = 0;
        std::memcpy(&k.layer, p, 1);
        std::memcpy(&k.h0, p + 1, 8);
        std::memcpy(&k.h1, p + 9, 8);
        std::memcpy(&stamp, p + 17, 4);
        out[k] = stamp;
    }
    launch = written;
    return true;
}

// THE FOUR KEYS (SHADER-SYSTEM-1 stage 5; ShaderCacheKeys.h). Per layer, what can make one of its bytes WRONG:
//   * pipeline.cache is validated by the driver and by Ogre (its header names vendor, device, driver and UUID): the
//     container format and the GPU terms. ITS TEXT IS BYTE-IDENTICAL TO THE KEY IT REPLACED (F5: a changed text would
//     drop the 56-MB blob the driver validates; engine.shader_cache_keys holds the golden case).
//   * microcode.cache is CONTENT-ADDRESSED (an entry's key is a hash of the generated source + preamble, fork
//     OgreVulkanProgram.cpp): what makes an entry wrong is its PRODUCER - the compile path's microcode group (ruling 1:
//     the content of the fork's files that call glslang and frame the microcode, irisgl/cmake/ShaderCompilePath.txt),
//     the SPIR-V compiler, the build type and the render system.
//   * hlms.N.bin holds the recipes (properties + pieces after every C++ hook) and their processed source: this
//     container's own code (ruling 3), the IdString/hash layout terms and the GPU (its caps are properties). A changed
//     TEMPLATE or a changed PREPROCESSOR is not a term: Ogre's template checksum (salted with the preprocessor group,
//     ruling 2: Hlms::setJahTemplateSalt) makes it RE-PARSE the recipes, never drop them.
//   * compute.bin holds the records (each its own content key, ComputeRecord.h): this container's code, the compile
//     path's compute group and preprocessor group (F3: a record's hit skips the parse, so a change to the parse's
//     property set-up, the preprocessor or the root layout's bytes must drop them), glslang, the build and the RS.
LayerKeys composeLayerKeys(const LayerKeyInputs &in) {
    LayerKeys k;
    const std::string format = "format=" + std::to_string(in.format);
    const std::string build = in.debugBuild ? "|build=debug" : "|build=release";
    std::string gpu, rs;
    if (in.haveGpu) {
        gpu = "|rs=" + in.rs + "|vendor=" + in.vendor + "|device=" + in.device + "|driver=" + in.driver;
        rs = "|rs=" + in.rs;
    }
    k.pipeline = format + gpu;
    k.microcode = format + "|microcode=" + in.microcodePath + "|glslang=" + in.glslang + build + rs;
    k.hlms = format + "|code=" + in.code + "|dbgstr=" + in.dbgstr + "|hashbits=" + std::to_string(in.hashBits) + build +
             gpu;
    k.compute = format + "|code=" + in.code + "|compute=" + in.computePath + "|preprocessor=" + in.preprocessorPath +
                "|glslang=" + in.glslang + build + rs;
    return k;
}

void ShaderCache::composeFingerprint() {
    mFingerprint = "pipeline{" + mLayerKey[0] + "}microcode{" + mLayerKey[1] + "}hlms{" +
                   mLayerKey[2] + "}compute{" + mLayerKey[3] + "}";
}

std::string ShaderCache::layerKeyFor(const std::string &name) const {
    switch (layerOf(name)) {
    case Layer::Pipeline:  return hexOf(mLayerKey[0]);
    case Layer::Microcode: return hexOf(mLayerKey[1]);
    case Layer::Hlms:      return hexOf(mLayerKey[2]);
    case Layer::Compute:   return hexOf(mLayerKey[3]);
    case Layer::Unknown:   break;
    }
    return std::string();
}

void ShaderCache::attachCounters() {
    if (mCounter) return;
    mCounter.reset(new Counter);
    if (Ogre::LogManager::getSingletonPtr() && Ogre::LogManager::getSingleton().getDefaultLog())
        Ogre::LogManager::getSingleton().getDefaultLog()->addListener(mCounter.get());
}

void ShaderCache::setCompileObserver(std::function<void()> observer) {
    if (!mCounter) return;
    mCounter->observerThread.store(std::thread::id(), std::memory_order_relaxed);
    mCounter->observer = std::move(observer);
    if (mCounter->observer)
        mCounter->observerThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
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

void ShaderCache::backgroundCompiles(unsigned &background, unsigned &foreground) const {
    background = mCounter ? mCounter->background.load() : 0u;
    foreground = mCounter ? mCounter->foreground.load() : 0u;
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
//   saved <unix-ms>
//   shaders <count>
//   file <name> <bytes> <hash128> <layer key>
//
// Each file carries the hex of its LAYER's key (see configure): readManifest
// hands every entry back and the caller keeps the ones whose key is current.
bool ShaderCache::readManifest(std::vector<Entry> &filesOut, bool adopt) const {
    long long saved = 0;
    unsigned shaders = 0;
    if (!readManifestAt(mDir, filesOut, &saved, &shaders)) return false;
    if (adopt) {
        const_cast<ShaderCache *>(this)->mLastSavedUnixMs = saved;
        const_cast<ShaderCache *>(this)->mExpectedShaders = shaders;
    }
    return true;
}

bool ShaderCache::readManifestAt(const std::string &dir, std::vector<Entry> &filesOut, long long *savedOut,
                                 unsigned *shadersOut) {
    std::ifstream f(dir + "/" + kManifest);
    if (!f) return false;
    std::string line;
    bool header = false;
    long long saved = 0;
    unsigned shaders = 0;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "jahshaka-shader-cache") { int v = -1; ls >> v; header = (v == kCacheFormat); }
        else if (tag == "saved")    ls >> saved;
        else if (tag == "shaders")  ls >> shaders;
        else if (tag == "file") {
            Entry e{};
            ls >> e.name >> e.bytes >> e.hash >> e.key;
            if (e.name.empty() || e.hash.size() != 32 || e.key.empty()) return false;
            // A name that could escape the directory is a corrupt manifest, not
            // a file to open.
            if (e.name.find('/') != std::string::npos || e.name.find("..") != std::string::npos)
                return false;
            filesOut.push_back(e);
        }
    }
    if (!header) return false;
    if (savedOut) *savedOut = saved;
    if (shadersOut) *shadersOut = shaders;
    return true;
}

bool ShaderCache::writeManifest(const std::vector<Entry> &files, unsigned shaders) const {
    std::ostringstream o;
    o << "jahshaka-shader-cache " << kCacheFormat << "\n"
      << "saved " << nowUnixMs() << "\n"
      << "shaders " << shaders << "\n";
    for (const Entry &e : files)
        o << "file " << e.name << " " << e.bytes << " " << e.hash << " " << e.key << "\n";
    const std::string s = o.str();
    return iris::files::writeAtomic(iris::files::path(path(kManifest)), s);
}

bool ShaderCache::readVerified(const Entry &e, std::string &out) const { return readVerifiedAt(mDir, e, out); }

// ONE COPY OF THE BLOB, the read itself (the owner's fast start: a pipeline blob is tens of MB): readAll's string is
// MOVED into `out`, verified in place, swapped into its layer by load() and wrapped by a MemoryDataStream that owns nothing.
bool ShaderCache::readVerifiedAt(const std::string &dir, const Entry &e, std::string &out) {
    std::optional<std::string> bytes = iris::files::readAll(iris::files::path(dir + "/" + e.name));
    if (!bytes) return false;
    out = std::move(*bytes);
    if (out.size() != e.bytes) {
        logLine(dir + "/" + e.name + ": length " + std::to_string(out.size()) + " != manifest " +
                std::to_string(e.bytes));
        return false;
    }
    if (hex128(out.data(), out.size()) != e.hash) {
        logLine(dir + "/" + e.name + ": checksum mismatch");
        return false;
    }
    return true;
}

/// THE WIPE IS THE WRITER'S ALONE (SHADERCACHE-LOCK-1, found by VR-JERK-1's
/// rig, reproduced 2026-09-18): every "this cache is no good to me" path in
/// `load()` used to delete the whole directory whether or not this process had
/// the writer lock — so a SECOND instance on the same data root deleted the
/// FIRST one's live cache and left it to recompile from cold. Measured before
/// the guard: with a long-lived instance holding the lock, a second launch that
/// read a manifest it could not use logged "another process holds the writer
/// lock — read-only for this run" and then took the manifest, both Hlms caches,
/// the microcode cache and the pipeline blob with it (10 files to 5).
///
/// A read-only run that cannot use the cache needs nothing from the directory:
/// it simply runs cold itself, which is what it would have done anyway. So the
/// three call sites in `load()` ask this first, and the directory outlives them.
/// (The size-cap wipe in `save()` is the writer's by construction, and
/// `clear()` is a host asking for it outright.)
bool ShaderCache::mayWipe() const {
    if (mWriter) return true;
    logLine("this run does not hold the writer lock — leaving the cache for whoever does");
    return false;
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
    // A COPY IN PROGRESS IS ABANDONED, not completed (the read's item 4): its files are about to be deleted, and
    // completing it would be one unbounded message plus a wait on this thread. The copies stay valid — the next
    // save continues them incrementally.
    mCopyInProgress = false;
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
    mComputeRecordsLoaded = mComputeRecordsRejected = mComputeRecordsStale = 0;
    mUses.clear();   // (F10) the stamps went with the files: the next write rebuilds them, evicting nothing
    mUsesSignalAt = kNoUsesJob;
    mStampsTrusted = false;
    mForceCompute = true;   // the store still holds its records: the next save writes them back
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
// THE SEED (rule 8, EnginePrivate.h; COLD-BOOT-1 S1). A build carries the microcode layer a warm run of it saved
// (scripts/shadercache/make-seed.sh) — the layer whose key names only the PRODUCER (the fork, glslang, the build
// type, the render system) and nothing of the device, so it is valid on any GPU (upstream's own answer for a first
// boot: OgreGpuProgramManager.h, "Vulkan: Yes (even created on PC and bundled into Android)"). A launch whose
// directory holds no microcode layer valid at this build's key reads it here, in the microcode slot of upstream's
// order, before any shader compiles. It is the directory's own format — a manifest whose `file microcode.cache …`
// line is the one read (any other line is ignored) — so the same parser, the same key rule and the same checksum
// stand guard over it. READ-ONLY BY CONSTRUCTION: nothing here opens it for writing, and the wipe, the lock and the
// saves all name mDir alone. The session's map begins as the seed, so the directory's first saved microcode layer
// holds the seed's entries and every later launch reads that one instead.
bool ShaderCache::readSeed(std::string &out) {
    out.clear();
    mSeedEntries = 0;
    if (mSeedDir.empty()) { mSeedState = "off"; return false; }
    struct stat st {};
    if (::stat((mSeedDir + "/" + kManifest).c_str(), &st) != 0) { mSeedState = "absent"; return false; }
    std::vector<Entry> files;
    if (!readManifestAt(mSeedDir, files, nullptr, nullptr)) {
        mSeedState = "corrupt";
        logLine("seed " + mSeedDir + ": its manifest is malformed or of another cache format — not read");
        return false;
    }
    const Entry *micro = nullptr;
    for (const Entry &e : files)
        if (layerOf(e.name) == Layer::Microcode) micro = &e;
    if (!micro) {
        mSeedState = "corrupt";
        logLine("seed " + mSeedDir + ": its manifest names no microcode.cache — not read");
        return false;
    }
    // A KEY MISMATCH IS A MISS: the seed was made by another producer, so not one of its blobs is read.
    if (micro->key != layerKeyFor(micro->name)) {
        mSeedState = "stale";
        logLine("seed " + mSeedDir + ": made at another microcode key (another compile path, glslang, build type or "
                "render system) — not read; every shader this launch needs compiles");
        return false;
    }
    if (!readVerifiedAt(mSeedDir, *micro, out)) {
        out.clear();
        mSeedState = "corrupt";
        logLine("seed " + mSeedDir + ": its microcode.cache failed verification — not read");
        return false;
    }
    return true;
}

void ShaderCache::loadSeed() {
    std::string bytes;
    if (!readSeed(bytes)) return;
    try {
        Ogre::DataStreamPtr s(OGRE_NEW Ogre::MemoryDataStream(mSeedDir + "/microcode.cache", bytes.data(),
                                                              bytes.size(), false, true));
        Ogre::GpuProgramManager::getSingleton().loadMicrocodeCache(s);
    } catch (const Ogre::Exception &e) {
        // Whatever the map took before the throw is verified bytes under content keys: a partial hit, never wrong.
        mSeedState = "rejected";
        logLine("seed " + mSeedDir + ": the microcode load failed (" + e.getDescription() + ")");
        return;
    }
    // The count is the file's first uint32 (load()'s note below): trusted only because the checksum passed.
    Ogre::uint32 n = 0;
    if (bytes.size() >= sizeof(n)) std::memcpy(&n, bytes.data(), sizeof(n));
    mSeedEntries = n;
    mSeedState = "loaded";
    mMicrocodeLoaded = true;
    mMicrocodeAtLoad = n;
    logLine("seed " + mSeedDir + ": " + std::to_string(n) +
            " microcode entries loaded (the directory had no microcode layer at this build's key)");
}

// ---------------------------------------------------------------------------
void ShaderCache::load(Ogre::Root *root) {
    // THE TEMPLATE CHECKSUMS, HASHED AT BOOT (FRAME-MONITOR-DOOR-1 stage 4): Hlms::getTemplateChecksum reads and hashes
    // every template and piece file (0.18-0.75 ms per Hlms measured) and the fork caches it per code-cache generation;
    // asked here, on every exit of the load, the first save of the session never pays it inside a frame.
    struct PrimeChecksums {
        Ogre::Root *root = nullptr;
        ~PrimeChecksums() {
            Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
            if (!hm) return;
            try {
                for (int i = Ogre::HLMS_LOW_LEVEL + 1; i < Ogre::HLMS_MAX; ++i)
                    if (Ogre::Hlms *h = hm->getHlms(static_cast<Ogre::HlmsTypes>(i))) {
                        Ogre::uint64 sum[2];
                        h->getTemplateChecksum(sum);
                    }
            } catch (...) {}
        }
    } primeChecksums{ root };
    // THE PREPROCESSOR's SALT (SHADER-SYSTEM-1 stage 5, the lead's ruling 2): every Hlms' template checksum is seeded with
    // the compile path's preprocessor group (irisgl/cmake/ShaderCompilePath.txt), before anything asks for it - so a
    // changed preprocessor makes Ogre RE-PARSE the saved recipes; the hlms layer's key holds no fork commit to drop them.
    if (Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr) {
        const std::string id = JAHSHAKA_COMPILE_PATH_PREPROCESSOR_ID;
        const Ogre::uint64 salt[2] = { std::stoull(id.substr(0, 16), nullptr, 16),
                                       std::stoull(id.substr(16, 16), nullptr, 16) };
        for (int i = Ogre::HLMS_LOW_LEVEL + 1; i < Ogre::HLMS_MAX; ++i)
            if (Ogre::Hlms *h = hm->getHlms(static_cast<Ogre::HlmsTypes>(i))) h->setJahTemplateSalt(salt);
    }
    if (!mEnabled || !root) return;
    Ogre::RenderSystem *rs = root->getRenderSystem();
    if (!rs) return;

    // The GPU half of the fingerprint (§4.2). Ogre re-checks vendor/device/
    // driver/UUID inside the pipeline blob itself, so this is redundancy ON
    // PURPOSE: it lets a GPU or driver swap reject the WHOLE directory instead
    // of silently loading two stale files and one rejected one. The values come
    // from the capabilities the render system already published — no new Ogre
    // type crosses the boundary.
    // Per layer: the pipeline blob and the Hlms layer take all four (a GPU's
    // caps are Hlms properties); the microcode only the render system's name,
    // which is what Ogre folds into its own microcode keys — SPIR-V is not a
    // property of the device.
    if (const Ogre::RenderSystemCapabilities *caps = rs->getCapabilities()) {
        mKeyInputs.haveGpu = true;
        mKeyInputs.rs = rs->getName();
        mKeyInputs.vendor = std::to_string(static_cast<int>(caps->getVendor()));
        mKeyInputs.device = caps->getDeviceName();
        mKeyInputs.driver = caps->getDriverVersion().toString();
        applyLayerKeys();
    }

    if (!iris::files::makeDirs(iris::files::path(mDir))) {
        logLine("cannot create " + mDir + " — cache disabled");
        mEnabled = false;
        return;
    }
    acquireLock();

    // A KILLED WRITE's TEMP IS SWEPT HERE, BY THE WRITER (NOQT-CORE-1): every write into this directory - the shader
    // cache's and the texture cache's, which shares it - stages "<name>.tmp-<pid>-<serial>" (iris::files::stagingTempPath)
    // and a write killed before its rename leaves it for ever, outside the manifest and the size cap. One is removed
    // when its writer cannot be alive: its pid names no process (kill: ESRCH), or it was last written before this boot.
    // A live pid keeps its temp whatever its age - a slow write in flight in another process (the texture cache writes
    // without the lock) - and a reused pid only delays the sweep to a later open. Nothing else here is touched.
    if (mWriter) {
        timespec now {}, up {};
        ::clock_gettime(CLOCK_REALTIME, &now);
        ::clock_gettime(CLOCK_BOOTTIME, &up);
        const time_t bootStart = now.tv_sec - up.tv_sec;
        unsigned swept = 0;
        if (DIR *d = opendir(mDir.c_str())) {
            while (dirent *e = readdir(d)) {
                const std::string name = e->d_name;
                const size_t at = name.rfind(".tmp-");
                if (at == std::string::npos || at == 0) continue;
                const char *digits = name.c_str() + at + 5;
                char *dash = nullptr;
                const long pid = std::isdigit(static_cast<unsigned char>(*digits)) ? std::strtol(digits, &dash, 10) : 0;
                if (pid <= 0 || *dash != '-' || !dash[1] || std::strspn(dash + 1, "0123456789") != std::strlen(dash + 1))
                    continue;
                struct stat st {};
                if (::stat(path(name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
                const bool dead = ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
                if ((dead || st.st_mtime < bootStart) && ::unlink(path(name).c_str()) == 0) ++swept;
            }
            closedir(d);
        }
        if (swept) logLine("swept " + std::to_string(swept) + " temp(s) a killed write left");
    }

    // THE LAST-USED STAMPS AND THE LAUNCH COUNTER (EntryUses.h): read whole and checked, or none at all - a missing,
    // damaged or older table stamps every entry with this launch (nothing is evictable) and is rebuilt by its first write.
    {
        mUses.clear();
        mUsesSignalAt = kNoUsesJob;
        std::uint32_t written = 0u;
        const std::optional<std::string> bytes = iris::files::readAll(iris::files::path(path(kEntryUses)));
        if (!bytes) mUsesState = "absent";
        else if (readUseTable(bytes->data(), bytes->size(), mUses, written)) mUsesState = "loaded";
        else {
            mUses.clear();
            mUsesState = "damaged";
            logLine(std::string(kEntryUses) + ": damaged or of an older format - not read; every entry counts as used "
                    "this launch and the table is rebuilt");
        }
        mLaunch = written + 1u;
        mStampsTrusted = mUsesState == "loaded";
    }

    // WHICH KEYS THIS RUN READS AT (URGENT #126, HARNESS-4): one line per boot naming the directory and this build's
    // three layer keys, so every app, lab and fixture log says which shaders it measured; a dropped layer's line below
    // names the cache's key beside this build's. (#126's "stale cache" was refuted: each layer is dropped on its own key;
    // the line makes that visible in the log a measurement is read from.)
    logLine(mDir + ": this build's layer keys pipeline=" + shortKey(layerKeyFor("pipeline.cache")) +
            " microcode=" + shortKey(layerKeyFor("microcode.cache")) + " hlms=" + shortKey(layerKeyFor("hlms.1.bin")) +
            " compute=" + shortKey(layerKeyFor("compute.bin")));

    // MICROCODE SAVING MUST BE ENABLED BEFORE THE LOAD. GpuProgramManager's own
    // header says so, and the canonical wiring does it in this order
    // (GraphicsSystem.cpp:648-658). It is also the flag that makes the SAVE side
    // record anything at all, so it goes on even when there is nothing to read.
    Ogre::GpuProgramManager::getSingleton().setSaveMicrocodesToCache(true);

    std::vector<Entry> files;
    if (!readManifest(files)) { if (mayWipe()) wipe(); loadSeed(); return; }

    // A LAYER WHOSE KEY MOVED IS DROPPED ALONE (CS-1): its file is not read and
    // — by the writer, the only process that may delete — removed now; the
    // other layers load. The next save writes the dropped layer afresh.
    {
        std::vector<Entry> current;
        for (const Entry &e : files) {
            if (e.key == layerKeyFor(e.name)) { current.push_back(e); continue; }
            logLine(e.name + ": its layer's key changed (the cache's " + shortKey(e.key) + ", this build's " +
                    shortKey(layerKeyFor(e.name)) + ") — dropped, the other layers stand");
            if (mWriter) ::unlink(path(e.name).c_str());
        }
        files.swap(current);
    }

    // Read and VERIFY everything before handing a single byte to Ogre. If any
    // file is short, corrupt or missing, the whole generation goes: a cache that
    // is half-valid is exactly the state the microcode loader cannot survive.
    std::string pipeline, microcode, compute;
    std::vector<std::pair<int, std::string>> hlms;
    for (const Entry &e : files) {
        std::string bytes;
        if (!readVerified(e, bytes)) {
            if (mayWipe()) wipe();
            logLine("verification failed — starting cold");
            loadSeed();   // the seed is not the directory's: its own checks decide it
            return;
        }
        if      (e.name == "pipeline.cache")  pipeline.swap(bytes);
        else if (e.name == "microcode.cache") microcode.swap(bytes);
        else if (e.name == "compute.bin")     compute.swap(bytes);
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
            mSeedState = mSeedDir.empty() ? "off" : "unused";
        } else {
            // No microcode layer at this key in the directory: the seed's, in the same slot (rule 8).
            loadSeed();
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
                // WHY THIS COUNT. The cache loads inside the first createView, before
                // any Scene (and so any worker pool) exists and before a frame plays:
                // the boot's apply borrows the render tier's width from the ONE core
                // budget (irisgl/core/threadbudget.h, THREADING-1) — the primary
                // scene's measured pool, clamp(hardware, 2, 8) — the same number
                // Studio's Tier::Primary reads, never a copy of its policy.
                const unsigned threads = unsigned(iris::threadBudget().ogrePool);
                // THE APPLY's COST, NAMED (SHADER-SYSTEM-1 stage 5): with the fork's commit out of the key, a changed
                // template or preprocessor makes Ogre RE-PARSE the recipes here ("the templates have changed") instead
                // of the layer being dropped - this line is what that costs, every boot.
                const auto applyStart = std::chrono::steady_clock::now();
                disk.applyTo(h, threads);
                logLine("hlms." + std::to_string(entry.first) + ".bin: " + std::to_string(h->getShaderCodeCache().size()) +
                        " recipes applied in " +
                        fmtMs(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - applyStart)
                                  .count()) +
                        " ms");
                ++mHlmsLoaded;
            }
        }
    } catch (const Ogre::Exception &e) {
        // Rule 7: never fatal. Ogre throws typed exceptions here and the
        // canonical wiring catches them exactly like this.
        logLine(std::string("load failed (") + e.getDescription() + ") — starting cold");
        if (mayWipe()) wipe();
        // THE SEED STILL STANDS: the wipe took the directory, not the seed, and a seed already in the map stays.
        const bool seeded = mSeedState == "loaded";
        mPipelineLoaded = false;
        mMicrocodeLoaded = seeded;
        mPipelineReason = "rejected";
        mHlmsLoaded = 0;
        if (!seeded) loadSeed();
        return;
    }
    if (!compute.empty()) loadCompute(compute);
    logLine("loaded: pipeline=" + std::string(mPipelineLoaded ? "yes" : "no") +
            " microcode=" + std::to_string(mMicrocodeAtLoad) + " entries" +
            " hlms=" + std::to_string(mHlmsLoaded) + " compute=" + std::to_string(mComputeRecordsLoaded) + " records" +
            " seed=" + mSeedState + " launch=" + std::to_string(mLaunch) + " uses=" + mUsesState + " (" +
            std::to_string(mUses.size()) + " stamps)");
}

// THE COMPUTE LAYER's READ (COLD-BOOT-1 stage 3). The file's checksum was verified with the others; here its records
// are parsed (a record whose source does not hash to its program key is rejected: readComputeLayer) and kept for
// prewarmCompute, which drops the STALE ones. A file that is no compute layer is dropped alone, the other layers stand.
void ShaderCache::loadCompute(const std::string &bytes) {
    ComputeLayerRead read = readComputeLayer(bytes.data(), bytes.size());
    if (!read.ok) {
        logLine("compute.bin: not a compute layer - dropped, the other layers stand");
        if (mWriter) ::unlink(path("compute.bin").c_str());
        return;
    }
    mComputeRecordsRejected = read.rejected;
    mComputeRecordsLoaded = unsigned(read.records.size());
    mComputeLoaded = std::move(read.records);
    if (mComputeRecordsRejected)
        logLine("compute.bin: " + std::to_string(mComputeRecordsRejected) + " record(s) rejected (source and program "
                "key disagree)");
}

// THE STALE PRUNE RUNS HERE, NOT AT LOAD (SHADER-SYSTEM-1 stage 3F, the start preflight's F1): a record is stale when
// one of its files (its template, a piece) the staged media no longer holds with the same content (pruneStaleRecords:
// "a changed source drops exactly its entries"), and the files are found through the resource groups - which hold the
// compute media only from registerCommonMaterials on, after the load (OgreEngine.cpp). Pruned at load, every record
// was stale. The engine calls this after the folders are registered and the async compiler runs.
void ShaderCache::prewarmCompute() {
    if (mComputeLoaded.empty()) return;
    // Each file hashed once, from where the engine reads it (its resource groups: the compute jobs' own lookup).
    std::map<std::string, std::pair<bool, ComputeKey128>> memo;
    auto current = [&memo](const std::string &name, ComputeKey128 &out) {
        auto it = memo.find(name);
        if (it == memo.end()) {
            std::pair<bool, ComputeKey128> v{ false, ComputeKey128{ { 0u, 0u } } };
            try {
                // HlmsCompute's own lookup (OgreHlmsCompute.cpp compileShader / requestAsync): the default group,
                // searching the others. A file found nowhere is asked first, never thrown for (an Ogre exception is
                // an engine error line): the record that read it is stale.
                Ogre::ResourceGroupManager &rgm = Ogre::ResourceGroupManager::getSingleton();
                if (rgm.resourceExistsInAnyGroup(name))
                    if (Ogre::DataStreamPtr s = rgm.openResource(name)) v = { true, computeFileHash(s->getAsString()) };
            } catch (const Ogre::Exception &) {}
            it = memo.emplace(name, v).first;
        }
        out = it->second.second;
        return it->second.first;
    };
    mComputeRecordsStale = pruneStaleRecords(mComputeLoaded, current);
    mComputeRecordsLoaded = unsigned(mComputeLoaded.size());
    logLine("compute.bin: " + std::to_string(mComputeRecordsLoaded) + " record(s) kept, " +
            std::to_string(mComputeRecordsStale) + " stale (a file they read changed)");
    if (!mComputeStore) return;
    mComputeStore->prewarm(std::move(mComputeLoaded));
    mComputeLoaded.clear();
    // What the store now holds came from the disk (less the stale): no dirt of its own until a new record is filed.
    mSavedAtComputeGeneration.store(mComputeStore->generation());
}

// ---------------------------------------------------------------------------
std::atomic<unsigned>  ShaderCache::sEnginePipelines { 0 };
std::atomic<long long> ShaderCache::sEnginePipelineUs { 0 };

namespace {
std::mutex gEnginePipelineNamesMutex;
std::vector<std::string> gEnginePipelineNames;
}   // namespace

void ShaderCache::noteEnginePipeline(double ms, const char *name) {
    sEnginePipelines.fetch_add(1);
    sEnginePipelineUs.fetch_add(static_cast<long long>(ms * 1000.0));
    std::lock_guard<std::mutex> lock(gEnginePipelineNamesMutex);
    gEnginePipelineNames.emplace_back(name ? name : "");
}

bool ShaderCache::dirty(Ogre::Root *root) const {
    if (!mEnabled || !mWriter || !root) return false;
    // (The entries' stamps are no layer's dirt: saveStep writes them by their own signal, the sidecar alone.)
    // Nothing has compiled since the last successful write. Ogre's own dirty
    // flags do NOT settle after a save (saveMicrocodeCache early-returns on a
    // clean cache but never clears the flag), so without this the clean-quit
    // path would serialize and rewrite every layer twice — once from
    // EngineHost::shutdown and again from ~OgreEngine.
    // An engine-built compute pipeline since the last save is dirt of its own
    // (PIPELINE-CACHE-1 fix round): no Hlms flag sees it, and a warm-Hlms run
    // that first builds one (a cut-out mask, a velocity job) compiles nothing.
    const bool newEnginePipelines = sEnginePipelines.load() != mSavedAtEnginePipelines.load();
    // A COMPUTE RECORD FILED since the last write is dirt of its own (COLD-BOOT-1 stage 3; the lead's ruling P6): a
    // record whose code came from the code cache compiled nothing, so no counter above sees it.
    const bool newCompute = mComputeStore && mComputeStore->generation() != mSavedAtComputeGeneration.load();
    if (mCounter && mSavedAtCompileCount == mCounter->compiled.load() + mCounter->fromCache.load()
        && mLastSavedUnixMs != 0 && !newEnginePipelines && !newCompute)
        return false;
    if (newEnginePipelines || newCompute) return true;
    if (Ogre::GpuProgramManager::getSingletonPtr() &&
        Ogre::GpuProgramManager::getSingleton().isCacheDirty())
        return true;
    Ogre::HlmsManager *hm = root->getHlmsManager();
    if (!hm) return false;
    // (HLMS_COMPUTE, 9, lies past HLMS_MAX, 8: none of this file's Hlms loops reach it - the compute layer above is its.)
    for (int i = Ogre::HLMS_LOW_LEVEL + 1; i < Ogre::HLMS_MAX; ++i)
        if (Ogre::Hlms *h = hm->getHlms(static_cast<Ogre::HlmsTypes>(i)))
            if (h->isShaderCodeCacheDirty()) return true;
    // The pipeline blob has no dirty flag; a run that compiled anything at all
    // has almost certainly created PSOs too.
    return mCounter && mCounter->compiled.load() > 0u;
}

bool ShaderCache::save(Ogre::Root *root) { return saveStep(root, kCopyChunkEntries); }

bool ShaderCache::saveWhole(Ogre::Root *root) { return saveStep(root, std::numeric_limits<size_t>::max()); }

// THE SAVE, ONE STEP (FRAME-MONITOR-DOOR-1 stage 4). Since ENGINE-UPGRADE-U3 the host's save is a MESSAGE, so this
// runs inside a render frame (its drain), and every millisecond here is the frame's: the old body deep-copied every
// shader's source of every dirty Hlms AND serialized the copies, the microcode and the pipeline blob — 12.5 ms warm,
// 22.7 ms cold measured at the drag row's shape, a frame overrun each time a compile burst settled. What stays here is
// only what reads the render thread's objects: an INCREMENTAL copy (HlmsDiskCache::appendFrom, the fork) of at most
// `entryBudget` new shader entries — the copies live on in mCopy, so only what is new since the last save is copied —
// and, when caught up, the microcode map's snapshot. Everything that turns a copy into bytes runs on the writer.
// A copy that is not caught up returns and is continued once a frame by the render loop (OgreEngine::runFrame).
bool ShaderCache::saveStep(Ogre::Root *root, size_t entryBudget) {
    if (!mEnabled || !root) return false;
    // NOT RE-ENTRANT, AND IT IS CHEAP TO SAY SO. Reachable from the host's watchdog, the `app.saveShaderCache()`
    // verb and the clean-quit save — all as engine MESSAGES now, so every caller is the render thread's drain (and
    // the loop's continuation, and a teardown save); two at once would mean two jobs. One bool closes that; it is
    // deliberately not a lock — a save arriving from another thread is a bug this would hide rather than fix.
    if (mSaving) { logLine("save already in progress — skipped the re-entrant call"); return false; }
    struct Reentry {
        bool &flag;
        explicit Reentry(bool &f) : flag(f) { flag = true; }
        ~Reentry() { flag = false; }
    } reentry(mSaving);
    // THE MESSAGE'S WHOLE SPAN (the read's item 3): from the entry — the write check, the lock's open and fcntl,
    // dirty(), the directory — to the dispatch.
    const auto stepStart = std::chrono::steady_clock::now();
    mStepMark = true;
    Ogre::RenderSystem *rs = root->getRenderSystem();
    if (!rs || !Ogre::GpuProgramManager::getSingletonPtr()) return false;
    Ogre::HlmsManager *hm = root->getHlmsManager();
    if (!mCopyInProgress) {
        // ONE WRITER, ONE JOB (FSYNC-1) — AND THE LAW THAT MAKES THE COPIES SHAREABLE (stage 4): a write in flight
        // means THIS SAVE IS SKIPPED, never waited for. The writer reads mCopy while it serializes; the render
        // thread appends to mCopy only from here on, so a copy never starts while a write could be reading it, and
        // a copy in progress cannot be overtaken by a write — only the step that catches up dispatches one. The
        // layers stay dirty and the next save writes them. (The callers that MEAN to wait — the clean quit, the
        // destructor, a wipe — call flushWrites(), which completes a copy in progress first.)
        if (writeInFlight()) {
            logLine("the previous write is still in flight — skipping this save");
            return false;
        }
        if (!mWriter && !acquireLock()) return false;   // read-only run: not an error
        // THE STAMPS' OWN SIGNAL (the final read's ruling: they are no layer's dirt): what can mark an entry used - a
        // shader compiled or served from the microcode map, a pipeline created (a found recipe's draw, a compute job's
        // first dispatch), a compute record filed. A job runs when a layer is dirty, at a launch's first save, or when
        // the signal moved since the last job; a job with no dirty layer writes the sidecar alone, and only when a stamp
        // moved (runWrite).
        const Ogre::RenderSystem::JahCompileCosts costs = Ogre::RenderSystem::getJahCompileCosts();
        const std::uint64_t usesSignal = (mCounter ? std::uint64_t(mCounter->compiled.load()) + mCounter->fromCache.load()
                                                   : 0u) +
                                         costs.graphicsPsos + costs.computePsos + sEnginePipelines.load() +
                                         (mComputeStore ? mComputeStore->generation() : 0u);
        mCopyLayersDirty = mForceSave || dirty(root);
        if (!mCopyLayersDirty && usesSignal == mUsesSignalAt) return true;   // nothing new — a no-op, not a failure
        mUsesSignalAt = usesSignal;
        mForceSave = false;
        // The DIRECTORY may not exist: app.clearShaderCache() removes it whole (the host's half deletes
        // recursively), and the clean-quit save that follows has to be able to write the session's shaders back
        // into it. Found by shadercache.app, whose run 2 was silently cold because every write in run 1's final
        // save failed against a missing directory.
        if (!iris::files::makeDirs(iris::files::path(mDir))) {
            logLine("cannot recreate " + mDir + " — nothing saved");
            return false;
        }
        mCopyMask = 0u;
        mCopyDirtyMask = 0u;
        // (F10) Every job copies EVERY Hlms (incrementally: only what is new since the last copy, and the use flags):
        // its recipes are stamped and may be evicted; the writer rewrites a layer only if it is dirty or loses a recipe.
        if (hm)
            for (int i = Ogre::HLMS_LOW_LEVEL + 1; i < Ogre::HLMS_MAX; ++i)
                if (Ogre::Hlms *h = hm->getHlms(static_cast<Ogre::HlmsTypes>(i))) {
                    if (mCopyLayersDirty && h->isShaderCodeCacheDirty()) mCopyDirtyMask |= 1u << unsigned(i);
                    mCopyMask |= 1u << unsigned(i);
                }
        mCopyInProgress = true;
        mCopyRoot = root;
        mCopyMessages = 0u;
        mCopyWorstMs = 0.0;
        mCopyRearmLogged = false;
    }

    size_t left = entryBudget, entries = 0u;
    bool caughtUp = true;
    try {
        for (int i = Ogre::HLMS_LOW_LEVEL + 1; i < Ogre::HLMS_MAX; ++i) {
            if (!(mCopyMask & (1u << unsigned(i)))) continue;
            Ogre::Hlms *h = hm ? hm->getHlms(static_cast<Ogre::HlmsTypes>(i)) : nullptr;
            if (!h) { mCopyMask &= ~(1u << unsigned(i)); continue; }
            if (!mCopy[i]) mCopy[i].reset(new Ogre::HlmsDiskCache(hm));
            size_t copied = 0u;
            const size_t remaining = mCopy[i]->appendFrom(h, left, &copied);
            entries += copied;
            left -= std::min(copied, left);
            if (remaining) caughtUp = false;
        }
    } catch (const Ogre::Exception &e) {
        logLine(std::string("save failed (") + e.getDescription() + ")");
        mCopyInProgress = false;
        return false;
    }
    const double stepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stepStart).count();
    ++mCopyMessages;
    mCopyWorstMs = std::max(mCopyWorstMs, stepMs);
    ++mCopyMessagesTotal;
    // THE CHUNK'S COUNT (the row's guard): the most entries one CHUNKED message took — the bound is K by construction.
    if (entryBudget == kCopyChunkEntries) mChunkMaxEntries = std::max(mChunkMaxEntries, unsigned(entries));
    if (!caughtUp) {
        // (No log line per chunk — an Ogre Log write and flush under the Log mutex the writer shares; one line a save.)
        mCopyChunkWorstEverMs = std::max(mCopyChunkWorstEverMs, stepMs);
        mCopyWorstEverMs = std::max(mCopyWorstEverMs, stepMs);
        return true;
    }
    // THE WRITER MAY BE BUSY WITH A TASK (the read's F2: dispatchTask — the texture cache's write — can hold it while a
    // copy spans frames): the catch-up RE-ARMS — the copy stays in progress and the next frame's step dispatches. Never
    // a dropped save the watchdog believes saved. (A write JOB cannot be in flight: only this step dispatches one.)
    if (writeInFlight()) {
        if (!mCopyRearmLogged) { logLine("caught up while the writer is busy — the next frame dispatches"); mCopyRearmLogged = true; }
        mCopyWorstEverMs = std::max(mCopyWorstEverMs, stepMs);
        return true;
    }

    // CAUGHT UP: the job, with the copies by reference, the microcode's snapshot and the render system.
    const auto tailStart = std::chrono::steady_clock::now();
    auto job = std::unique_ptr<PendingWrite>(new PendingWrite());
    for (int i = Ogre::HLMS_LOW_LEVEL + 1; i < Ogre::HLMS_MAX; ++i)
        if ((mCopyMask & (1u << unsigned(i))) && mCopy[i]) {
            job->hlms.emplace_back("hlms." + std::to_string(i) + ".bin", mCopy[i].get());
            job->hlmsDirty.push_back((mCopyDirtyMask & (1u << unsigned(i))) != 0u);
        }
    // THE MICROCODE MAP, every job (F10): a snapshot of shared pointers and its used keys, in one lock; written when
    // dirty or when the writer evicts an entry. (A job with no dirty layer - the stamps' own - rewrites no layer.)
    Ogre::GpuProgramManager &gpm = Ogre::GpuProgramManager::getSingleton();
    gpm.copyMicrocodeCache(job->microcode, job->microcodeUsed);
    job->microcodeDirty = mCopyLayersDirty && gpm.isCacheDirty();
    job->launch = mLaunch;
    job->uses = mUses;   // the table the last write left (the writer updates it; no write is in flight here)
    job->stampsTrusted = mStampsTrusted;
    // A LOST DEVICE IS NOT READ (the read's item 5): after the engine latched a loss, the writer serializes the CPU
    // copies only — no vkGetPipelineCacheData against a dead device. (Its destruction is not reachable while the
    // writer runs: the fork never recreates a lost device and no forced election exists — validateDevice(true) has
    // no caller — and the teardown joins the writer before the render system goes; finishWrites, OgreEngine.cpp.)
    job->rs = mDeviceGone || !mCopyLayersDirty ? nullptr : rs;
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
    job->enginePipelines = sEnginePipelines.load();
    // THE COMPUTE RECORDS, when the store filed any since the last write: pointers to immutable records, so the render
    // thread copies no source here; the writer serializes them.
    if (mComputeStore) {
        const std::uint64_t gen = mComputeStore->generation();
        // (F10) every write: the records and their used keys (pointers; the writer rewrites compute.bin when dirty
        // or when it drops a record).
        mComputeStore->snapshot(job->compute);
        mComputeStore->usedKeys(job->computeUsed);
        job->computeHave = true;
        job->computeGeneration = gen;
        if (gen != mSavedAtComputeGeneration.load() || mForceCompute) {
            job->computeDirty = true;
            mForceCompute = false;
        }
    }
    const double tailMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tailStart).count();
    mCopyWorstMs = std::max(mCopyWorstMs, stepMs + tailMs);
    mCopyWorstEverMs = std::max(mCopyWorstEverMs, stepMs + tailMs);
    const size_t layers = job->hlms.size() + (job->microcodeDirty ? 1u : 0u) + (job->computeDirty ? 1u : 0u) + 1u;
    mCopyInProgress = false;
    logLine("copied " + std::to_string(layers) + " layer(s) on the render thread in " + std::to_string(mCopyMessages) +
            " message(s), the longest " + fmtMs(mCopyWorstMs) + " ms (this one " + fmtMs(stepMs + tailMs) +
            " ms, " + std::to_string(entries) + " entries) — serializing and writing off-thread");
    if (!dispatchWrite(std::move(job))) {
        // Only reachable if the writer took a TASK between the check above and here (dispatchTask runs on other
        // threads): re-armed like the busy case — the copy stays in progress and the next frame dispatches.
        mCopyInProgress = true;
        logLine("the writer took a task first — re-armed, the next frame dispatches");
        return true;
    }
    ++mSavesCopied;
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
// watching. The contract does not move: bytes to a sibling temp, flushed,
// renamed over the target, manifest last, so a crash at any instant leaves the
// previous generation or the new one. Only the thread that waits moves.
bool ShaderCache::runWrite(PendingWrite &job) {
    // THE SERIALIZATION, HERE (FRAME-MONITOR-DOOR-1 stage 4): the copies become bytes on this thread, into memory —
    // a stream that GROWS (FSYNC-1: Ogre's three writers want a DataStreamPtr and cannot say how many bytes they
    // are about to produce). The render thread is not touching the copies (the skip law) and the pipeline cache
    // needs no external synchronisation (vk.xml); the microcode is a snapshot of immutable blobs.
    const auto serializeStart = std::chrono::steady_clock::now();
    // The manifest this write replaces: its files are carried forward, and an unchanged pipeline blob is not rewritten.
    std::vector<Entry> previous;
    readManifest(previous, /*adopt*/ false);   // the writer thread: never publish
    std::map<std::string, std::vector<char>> out;   // the files this write rewrites, by name
    auto serialize = [&](const std::string &name, const std::function<void(Ogre::DataStreamPtr &)> &writer) {
        // Named for the file these bytes END UP IN (F11): a log line from inside Ogre's writer names something
        // that exists.
        GrowingMemoryStream *mem = OGRE_NEW GrowingMemoryStream(mDir + "/" + name);
        Ogre::DataStreamPtr s(mem);
        writer(s);
        s->close();
        std::vector<char> bytes = mem->take();
        if (!bytes.empty()) out[name] = std::move(bytes);
    };
    // THE STAMPS (EntryUses.h): every entry of every layer the job carries, used this launch or kept at its last use,
    // into `next` (the table written); `held` names each (layer, stamp) that holds an entry - the eviction's index.
    UseTable next;
    std::set<std::pair<std::uint8_t, std::uint32_t>> held;
    auto stamp = [&](const UseKey &k, bool used) {
        const std::uint32_t s = stampByUse(job.uses, k, used, job.launch);
        next[k] = s;
        held.insert({ k.layer, s });
    };
    auto hlmsLayer = [&](size_t i) { return std::uint8_t(kUsesHlmsBase + std::atoi(job.hlms[i].first.c_str() + 5)); };
    for (size_t i = 0; i < job.hlms.size(); ++i) {
        const std::uint8_t layer = hlmsLayer(i);
        job.hlms[i].second->jahEntries(
            [&](const Ogre::uint64 key[2], bool used) { stamp(UseKey{ layer, key[0], key[1] }, used); });
    }
    for (const auto &e : job.microcode)
        stamp(UseKey{ kUsesMicrocode, e.first.hashVal[0], e.first.hashVal[1] }, job.microcodeUsed.count(e.first) != 0);
    std::sort(job.computeUsed.begin(), job.computeUsed.end());
    for (const ComputeRecordPtr &r : job.compute)
        stamp(UseKey{ kUsesCompute, r->inputKey[0], r->inputKey[1] },
              std::binary_search(job.computeUsed.begin(), job.computeUsed.end(), r->inputKey));

    // A LAYER's BYTES, keeping the entries last used AFTER launch `below` (0 keeps every one: a stamp is >= 1).
    auto kept = [&](const UseKey &k, std::uint32_t below) {
        const auto it = next.find(k);
        return it == next.end() || it->second > below;
    };
    auto writeHlms = [&](size_t i, std::uint32_t below) {
        const std::uint8_t layer = hlmsLayer(i);
        serialize(job.hlms[i].first, [&](Ogre::DataStreamPtr &s) {
            job.hlms[i].second->saveTo(
                s, [&](const Ogre::uint64 key[2], bool) { return kept(UseKey{ layer, key[0], key[1] }, below); });
        });
    };
    auto writeMicrocode = [&](std::uint32_t below) {
        Ogre::GpuProgramManager::MicrocodeMap map;
        for (const auto &e : job.microcode)
            if (kept(UseKey{ kUsesMicrocode, e.first.hashVal[0], e.first.hashVal[1] }, below)) map.insert(e);
        serialize("microcode.cache", [&](Ogre::DataStreamPtr &s) { Ogre::GpuProgramManager::saveMicrocodeCache(s, map); });
    };
    auto writeCompute = [&](std::uint32_t below) {
        std::vector<ComputeRecordPtr> records;
        for (const ComputeRecordPtr &r : job.compute)
            if (kept(UseKey{ kUsesCompute, r->inputKey[0], r->inputKey[1] }, below)) records.push_back(r);
        out["compute.bin"] = writeComputeLayer(records);
    };
    // A file of the manifest this write carries forward: not rewritten, its layer's key current, still on disk.
    auto carried = [&](const Entry &p) {
        struct stat st {};
        return !out.count(p.name) && p.key == layerKeyFor(p.name) && ::stat(path(p.name).c_str(), &st) == 0;
    };
    // THE TOTAL THE SIZE CAP IS HELD AGAINST: the bytes of the layers the manifest will name, rewritten or carried.
    auto total = [&]() {
        unsigned long long t = 0;
        for (const auto &o : out) t += o.second.size();
        for (const Entry &p : previous)
            if (carried(p)) t += p.bytes;
        return t;
    };
    std::uint32_t below = 0u;
    try {
        for (size_t i = 0; i < job.hlms.size(); ++i)
            if (job.hlmsDirty[i]) writeHlms(i, 0u);
        if (job.microcodeDirty) writeMicrocode(0u);
        // The pipeline blob has no dirty flag: a job with a dirty layer serializes it, and it is rewritten only when its
        // bytes moved (a warm launch's pipelines are the driver cache's own hits: the same bytes, measured at
        // FE/ctl c -> c2). vkGetPipelineCacheData is documented as fragile if called too close to PSO creation, which is
        // why a save only ever runs on a settled burst or on shutdown, never on a timer during a compile storm (§4.4).
        if (job.rs) {
            serialize("pipeline.cache", [&](Ogre::DataStreamPtr &s) { job.rs->savePipelineCache(s); });
            const auto it = out.find("pipeline.cache");
            if (it != out.end())
                for (const Entry &p : previous)
                    if (p.name == it->first && p.key == layerKeyFor(p.name) && p.bytes == it->second.size() &&
                        p.hash == hex128(it->second.data(), it->second.size())) {
                        out.erase(it);
                        break;
                    }
        }
        if (job.computeDirty) writeCompute(0u);
        // THE SIZE CAP (the final read's ruling): AGE ALONE NEVER DROPS AN ENTRY. Only when the layers would pass
        // mMaxBytes are whole stamps evicted, the oldest first and never this launch's, each layer that held the stamp
        // re-serialized, until the total is under the cap. pipeline.cache is not entry-evictable: it counts, it stays.
        // WITH NO TRUSTWORTHY STAMPS (the sidecar was missing, damaged or of an older format at this launch's load) the
        // cap evicts NOTHING and never wipes - a damaged 1-KB file is no reason to throw the cache away (the lead): every
        // entry counts as used this launch, the table is rebuilt from this launch's uses, and the eviction runs at the
        // first save of a launch that reads it intact.
        if (total() > mMaxBytes && !job.stampsTrusted)
            logLine("OVER ITS BUDGET WITH NO STAMPS: the layers hold " + std::to_string(total()) + " bytes, " +
                    std::to_string(total() - mMaxBytes) + " over the cap of " + std::to_string(mMaxBytes) +
                    ", and this launch read no intact entry-uses.bin - nothing is evicted; the next launch evicts by the "
                    "stamps this one writes");
        else if (total() > mMaxBytes) {
            std::set<std::uint32_t> older;
            for (const auto &h : held)
                if (h.second < job.launch) older.insert(h.second);
            for (const std::uint32_t s : older) {
                below = s;
                for (size_t i = 0; i < job.hlms.size(); ++i)
                    if (held.count({ hlmsLayer(i), s })) writeHlms(i, below);
                if (held.count({ kUsesMicrocode, s })) writeMicrocode(below);
                if (held.count({ kUsesCompute, s })) writeCompute(below);
                if (total() <= mMaxBytes) break;
            }
            // THE BACKSTOP: only this launch's entries are left and the layers still pass the cap - wiped, the next
            // launch re-warms. At the app's 256 MiB it needs pipeline.cache past ~218 MiB: measured at the feature end's
            // control (FE/ctl/c-cache.txt) the layers are 55.6 MiB, of which pipeline.cache 18.3 MB and the others
            // 40.1 MB - all of which one launch can use - so 268,435,456 - 40,056,641 = 228.4 MB, ~12.5x today's blob.
            if (total() > mMaxBytes) {
                std::string per;
                for (const auto &o : out) per += " " + o.first + "=" + std::to_string(o.second.size());
                for (const Entry &p : previous)
                    if (carried(p)) per += " " + p.name + "=" + std::to_string(p.bytes);
                logLine("WIPED BY THE SIZE CAP's BACKSTOP: this launch's own entries alone hold " + std::to_string(total()) +
                        " bytes of layers, over the cap of " + std::to_string(mMaxBytes) + " (launch " +
                        std::to_string(job.launch) + "; per layer:" + per + ") - the next launch re-warms it");
                wipe();
                mUses.clear();
                return true;
            }
        }
    } catch (const Ogre::Exception &e) {
        logLine(std::string("serializing failed (") + e.getDescription() + ") — nothing written");
        return false;
    }
    unsigned evicted = 0;
    for (auto it = next.begin(); it != next.end();) {
        if (it->second <= below) { it = next.erase(it); ++evicted; }
        else ++it;
    }
    logLine("serialized " + std::to_string(out.size()) + " file(s) in " +
            fmtMs(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - serializeStart).count()) +
            " ms (off the render thread)");
    std::vector<Entry> files;
    for (const auto &o : out) {
        const std::string_view bytes(o.second.data(), o.second.size());
        if (!iris::files::writeAtomic(iris::files::path(path(o.first)), bytes)) {
            logLine("could not write " + o.first);
            continue;
        }
        files.push_back({ o.first, o.second.size(), hex128(o.second.data(), o.second.size()), layerKeyFor(o.first) });
    }

    // Files we did not rewrite this time are still valid IF THEIR LAYER'S KEY IS
    // CURRENT: carry those entries forward, or the next run would reject a
    // perfectly good file. One whose key moved is gone from the manifest and
    // from the disk (load() already removed it when this process could).
    // A WRITE THAT REWROTE NOTHING AND CARRIES EVERY FILE WRITES NO MANIFEST (the final read's ruling): a launch that
    // compiled and evicted nothing leaves the cache's generation as it found it (fresh_home.sh promotes by it).
    bool published = !files.empty();
    for (const Entry &p : previous) {
        const bool rewritten = std::any_of(files.begin(), files.end(),
                                           [&](const Entry &e) { return e.name == p.name; });
        if (rewritten) continue;
        if (p.key != layerKeyFor(p.name)) { ::unlink(path(p.name).c_str()); published = true; continue; }
        struct stat st {};
        if (::stat(path(p.name).c_str(), &st) == 0) files.push_back(p);
        else published = true;
    }

    // THE MANIFEST IS THE PUBLICATION. Every file above is already in place and
    // already durable; until this line names them, a reader still sees the
    // previous generation. Failing here therefore leaves the cache exactly as
    // it was, which is why nothing below the write is allowed to run on a
    // false.
    if (published && !writeManifest(files, job.compileCount)) {
        logLine("the manifest could not be written — the previous cache stands");
        return false;
    }
    // THE STAMPS, after the manifest (a crash between them leaves the previous table: an entry it lacks is stamped
    // anew, never evicted), and only when one moved: the sidecar alone is a launch's whole write when it compiled and
    // evicted nothing. Self-checked, outside the manifest, the launch counter in it.
    if (next != job.uses) {
        const std::vector<char> table = writeUseTable(next, job.launch);
        if (iris::files::writeAtomic(iris::files::path(path(kEntryUses)), std::string_view(table.data(), table.size())))
            mUses = next;
        else logLine(std::string("could not write ") + kEntryUses);
    }
    if (evicted)
        logLine("the layers would pass the size cap of " + std::to_string(mMaxBytes) + " bytes: evicted " +
                std::to_string(evicted) + " entr" + (evicted == 1 ? "y" : "ies") + " last used at or before launch " +
                std::to_string(below) + " (this launch " + std::to_string(job.launch) + "), the layers now " +
                std::to_string(total()) + " bytes");
    if (published) mLastSavedUnixMs.store(nowUnixMs());
    mSavedAtCompileCount.store(job.compileCount);
    mSavedAtEnginePipelines.store(job.enginePipelines);
    if (job.computeDirty) mSavedAtComputeGeneration.store(job.computeGeneration);
    if (!published) {
        logLine("no layer changed - no layer and no manifest written");
        return true;
    }
    logLine("saved " + std::to_string(files.size()) + " files");
    mSavesWritten.fetch_add(1u);
    return true;
}

void ShaderCache::writerLoop() {
    for (;;) {
        std::unique_ptr<PendingWrite> job;
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mWriteMutex);
            mWriteCv.wait(lock, [this]() {
                return mWriteStop || mWriteJob != nullptr || !mWriteTasks.empty(); });
            if (mWriteJob) {
                job = std::move(mWriteJob);
            } else if (!mWriteTasks.empty()) {
                task = std::move(mWriteTasks.front());
                mWriteTasks.pop_front();
            } else {
                return;                       // stopping, and nothing left to write
            }
            mWriteBusy = true;
        }
        if (task) {
            task();
            {
                std::lock_guard<std::mutex> lock(mWriteMutex);
                mWriteBusy = false;
            }
            mWriteDoneCv.notify_all();
            continue;
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
    return mWriteJob || mWriteBusy || !mWriteTasks.empty();
}

void ShaderCache::dispatchTask(std::function<void()> task) {
    std::unique_lock<std::mutex> lock(mWriteMutex);
    mWriteTasks.push_back(std::move(task));
    if (!mWriteThread.joinable()) {
        mWriteStop = false;
        mWriteThread = std::thread([this]() { writerLoop(); });
    }
    lock.unlock();
    mWriteCv.notify_one();
}

void ShaderCache::finishWrites() {
    flushWrites(kWriteWaitMs);
    stopWriter();
    // THE COPIES DIE BEFORE ROOT (stage 4): they hold the Hlms' PSO descriptions (shader programs by shared pointer);
    // the engine's teardown calls this before it deletes Root.
    for (auto &c : mCopy) c.reset();
    mCopyInProgress = false;
    mCopyRoot = nullptr;
}

bool ShaderCache::flushWrites(unsigned budgetMs) {
    // A CHUNKED COPY IN PROGRESS IS COMPLETED FIRST (stage 4): the caller means "everything saved is on disk" — the
    // clean quit (flushShaderCache -> saveShaderCache -> flushShaderCache), a wipe, the teardown — so the rest of the
    // copy is taken now, in one step, and its write is the one waited for below.
    if (mCopyInProgress && mCopyRoot && !mSaving) {
        saveStep(mCopyRoot, std::numeric_limits<size_t>::max());
        if (mCopyInProgress) {   // re-armed behind a task: wait the writer out, then dispatch
            {
                std::unique_lock<std::mutex> lock(mWriteMutex);
                mWriteDoneCv.wait_for(lock, std::chrono::milliseconds(budgetMs),
                                      [this]() { return !mWriteJob && !mWriteBusy && mWriteTasks.empty(); });
            }
            saveStep(mCopyRoot, std::numeric_limits<size_t>::max());
        }
    }
    std::unique_lock<std::mutex> lock(mWriteMutex);
    auto idle = [this]() { return !mWriteJob && !mWriteBusy && mWriteTasks.empty(); };
    if (idle()) return true;
    return mWriteDoneCv.wait_for(lock, std::chrono::milliseconds(budgetMs), idle);
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
    s.maxBytes = mMaxBytes;
    s.pipelineCacheLoaded = mPipelineLoaded;
    s.pipelineCacheReason = mPipelineReason;
    s.microcodeLoaded = mMicrocodeLoaded;
    s.hlmsCachesLoaded = mHlmsLoaded;
    s.microcodeEntries = static_cast<unsigned>(mMicrocodeAtLoad);
    s.seedDir = mSeedDir;
    s.seedState = mSeedState;
    s.seedEntries = mSeedEntries;
    s.computeRecordsLoaded = mComputeRecordsLoaded;
    s.computeRecordsRejected = mComputeRecordsRejected;
    s.computeRecordsStale = mComputeRecordsStale;
    s.compiledThisRun = mCounter ? mCounter->compiled.load() : 0u;
    s.loadedThisRun   = mCounter ? mCounter->fromCache.load() : 0u;
    s.expectedShaders = mExpectedShaders;
    s.lastSavedUnixMs = mLastSavedUnixMs;
    s.savesCopied = mSavesCopied;
    s.copyMessages = mCopyMessagesTotal;
    s.copyWorstMs = mCopyWorstEverMs;
    s.copyChunkWorstMs = mCopyChunkWorstEverMs;
    s.copyChunkMaxEntries = mChunkMaxEntries;
    s.copyChunkEntries = unsigned(kCopyChunkEntries);
    s.copyFrames = mStepFrames;
    s.copyFrameOverruns = mStepFrameOverruns;
    s.savesWritten = mSavesWritten.load();
    s.enginePipelinesThisRun = sEnginePipelines.load();
    s.enginePipelineMs = double(sEnginePipelineUs.load()) / 1000.0;
    {
        std::lock_guard<std::mutex> lock(gEnginePipelineNamesMutex);
        s.enginePipelineNames = gEnginePipelineNames;
    }

    // THE SHADER HASH'S TWO INDEX SPACES, LIVE (HLMSBITS-1). Ogre packs every
    // shader lookup as [type:3][renderable:16][pass:13] and grows both caches
    // for the life of the process with no eviction anywhere; the pass side ran
    // out of its EIGHT bits on 2026-09-14 and corrupted the renderable index,
    // which is the crash fork d6348decd (was 0046) rebalanced the fields for. The worst
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
