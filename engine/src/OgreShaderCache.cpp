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

#include <OgreGpuProgramManager.h>
#include <OgreHlmsDiskCache.h>
#include <OgreRenderSystem.h>
#include <OgreRenderSystemCapabilities.h>
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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>

namespace jahshaka { namespace engine {
namespace detail {
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

std::string hex128(const void *data, size_t len) {
    Ogre::uint64 out[2] = {};
    Ogre::MurmurHash3_x64_128(data, static_cast<int>(len), 0x9E3779B9u, out);
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(out[0]), static_cast<unsigned long long>(out[1]));
    return std::string(buf);
}

std::string hexOf(const std::string &s) { return hex128(s.data(), s.size()); }

long long nowUnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
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

void logLine(const std::string &s) {
    if (Ogre::LogManager::getSingletonPtr())
        Ogre::LogManager::getSingleton().logMessage("Jahshaka shader cache: " + s);
}

/// Atomic write: <name>.tmp in the SAME directory, flushed to the platform, then
/// renamed over the target. A crash mid-write leaves the previous good file or
/// no file — never half of one. (Ogre's Archive::create gives neither property.)
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

}  // namespace

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
            if (message.find(" compiled successfully") != Ogre::String::npos)      ++compiled;
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
ShaderCache::~ShaderCache() { releaseLock(); }

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

bool ShaderCache::writeManifest(const std::vector<Entry> &files) const {
    std::ostringstream o;
    o << "jahshaka-shader-cache " << kCacheFormat << "\n"
      << "fingerprint " << mFingerprint << "\n"
      << "saved " << nowUnixMs() << "\n"
      << "shaders " << mExpectedShaders << "\n";
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
        ::unlink(path(name).c_str());
    }
    closedir(d);
}

bool ShaderCache::clear() {
    if (!mEnabled) return true;
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
                // numThreads is INERT for us: supportsMultithreadedShaderCompilation()
                // is false in this install (OgreBuildSettings.h has
                // OGRE_SHADER_THREADING_BACKWARDS_COMPATIBLE_API and not
                // OGRE_SHADER_THREADING_USE_TLS), so applyTo runs the serial
                // branch whatever we pass. Passing 1 says so honestly.
                disk.applyTo(h, 1u);
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

    Ogre::HlmsManager *hm = root->getHlmsManager();
    std::vector<Entry> files;

    // Serialize each layer into memory first, then write atomically. Ogre's
    // save APIs want a DataStreamPtr; a MemoryDataStream we own gives us the
    // bytes to checksum before they ever reach the disk.
    auto emit = [&](const std::string &name, const std::vector<char> &bytes) {
        if (bytes.empty()) return;
        if (!writeAtomic(mDir, name, bytes.data(), bytes.size())) {
            logLine("could not write " + name);
            return;
        }
        files.push_back({name, bytes.size(), hex128(bytes.data(), bytes.size())});
    };
    // Ogre writes into the stream and we need the written length, which
    // MemoryDataStream cannot grow — so serialize through a temp file and read
    // it back. It stays atomic: the temp file IS the *.tmp writeAtomic renames.
    auto serialize = [&](const std::string &name,
                         const std::function<void(Ogre::DataStreamPtr &)> &writer,
                         std::vector<char> &out) -> bool {
        const std::string scratch = mDir + "/" + name + ".building";
        { std::ofstream probe(scratch, std::ios::binary | std::ios::trunc); if (!probe) return false; }
        {
            // freeOnClose=true is why this MUST be OGRE_NEW_T/MEMCATEGORY_GENERAL:
            // FileStreamDataStream::close() frees it with OGRE_DELETE_T.
            std::fstream *fs = OGRE_NEW_T(std::fstream, Ogre::MEMCATEGORY_GENERAL)(
                scratch.c_str(), std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
            // Named for the file the bytes are ACTUALLY in (F11). It used to be
            // named "hlms.1.bin" while writing hlms.1.bin.building, so a log
            // line naming the stream named a file that did not exist yet.
            Ogre::DataStreamPtr s(OGRE_NEW Ogre::FileStreamDataStream(scratch, fs, 0, true));
            writer(s);
            s->close();
        }
        const bool ok = readWholeFile(scratch, out);
        ::unlink(scratch.c_str());
        return ok;
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
    // launch should expect to build or serve.
    if (mCounter)
        mExpectedShaders = mCounter->compiled.load() + mCounter->fromCache.load();
    if (!writeManifest(files)) return false;
    mLastSavedUnixMs = nowUnixMs();
    if (mCounter) mSavedAtCompileCount = mCounter->compiled.load() + mCounter->fromCache.load();

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
    (void)root;
    s.microcodeEntries = static_cast<unsigned>(mMicrocodeAtLoad);
    s.compiledThisRun = mCounter ? mCounter->compiled.load() : 0u;
    s.loadedThisRun   = mCounter ? mCounter->fromCache.load() : 0u;
    s.expectedShaders = mExpectedShaders;
    s.lastSavedUnixMs = mLastSavedUnixMs;
    return s;
}

}  // namespace detail
}}  // namespace jahshaka::engine
