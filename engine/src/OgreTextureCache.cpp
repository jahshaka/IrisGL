// The persistent TEXTURE cache (SPECS/THREADING_ADOPTION_SPEC.md P2, items 6
// and 7; decision D-D(b)). Read the class comment in EnginePrivate.h first — it
// says what the two files are and why a stale one is safe.
//
// What this file deliberately does NOT do:
//  * invent a texture key. Ogre content-addresses its metadata cache by the
//    texture's ALIAS, which loadTexture sets to the file's full path, and the
//    channel sidecar is keyed the same way on purpose: one key, one lifetime.
//  * validate what Ogre already validates. A metadata entry that no longer
//    matches the file raises upstream's OutOfDateCache and reloads
//    (OgreObjCmdBuffer.cpp:137-152). We check the CONTAINER (is this our file,
//    from this build, unmodified), never the contents.
//  * hash the GPU or the driver into the key (I-5). A resolution and a channel
//    count are properties of a FILE. Sharing the shader cache's fingerprint
//    would throw this away on every driver update for nothing.
//  * lock. The shader cache's single-writer lock covers the directory; the two
//    files here are written atomically and a torn read fails the manifest check,
//    which costs one cold run and nothing else.
#include "EnginePrivate.h"

#include <OgreLogManager.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreTextureGpuManager.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace jahshaka { namespace engine {
namespace detail {
namespace {

/// The container's own format version. Bump it and every existing texture cache
/// on every machine is discarded — the escape hatch for a change in THIS file
/// that no other key term would notice.
constexpr int kTextureCacheFormat = 1;

constexpr const char *kMetaFile     = "texture-meta.json";
constexpr const char *kChannelFile  = "texture-channels.txt";
constexpr const char *kManifestFile = "texture-manifest.txt";

/// A metadata cache with more entries than this is not a cache, it is a leak of
/// every texture path a machine has ever seen. Ogre's map only grows (entries
/// are removed only when a texture is destroyed with a stale entry), so the
/// container needs a ceiling of its own; past it we write nothing and the next
/// run starts cold, exactly like the shader cache's size cap.
constexpr size_t kMaxMetadataBytes = 4u * 1024u * 1024u;

void logLine(const std::string &s) {
    if (Ogre::LogManager::getSingletonPtr())
        Ogre::LogManager::getSingleton().logMessage("Jahshaka texture cache: " + s);
}

Ogre::TextureGpuManager *textureManager(Ogre::Root *root) {
    if (!root) return nullptr;
    Ogre::RenderSystem *rs = root->getRenderSystem();
    return rs ? rs->getTextureGpuManager() : nullptr;
}

}   // namespace

TextureCache &textureCache() {
    // Function-local static: constructed on first use, destroyed at exit, and
    // never before a translation unit that might still be talking to it.
    static TextureCache instance;
    return instance;
}

TextureCache::TextureCache() = default;
TextureCache::~TextureCache() = default;

std::string TextureCache::path(const std::string &name) const { return mDir + "/" + name; }

void TextureCache::configure(const std::string &dir, const std::string &appBuildId) {
    mDir = dir;
    mChannels.clear();
    mDirty = false;
    mEnabled = !dir.empty();
    if (!mEnabled) return;
    while (!mDir.empty() && mDir.back() == '/') mDir.pop_back();
    // THE VALIDITY KEY, and everything it deliberately leaves out (I-5).
    // format: this file's own layout. app: our build identity, because the app
    // is what decides which flags a texture is created with (sRGB preference,
    // AutomaticBatching, the mipmap filter) and those decide the pool a metadata
    // entry names. NOT the GPU, NOT the driver, NOT the Hlms media tree — none
    // of them can change what a PNG's channel count is.
    std::ostringstream k;
    k << "format=" << kTextureCacheFormat << "|app=" << appBuildId;
    mKey = cachefile::hexOf(k.str());
}

// ---------------------------------------------------------------------------
// The manifest. Three lines of header, then one line per file:
//     jahshaka-texture-cache 1
//     key <hex>
//     file <name> <bytes> <hash>
bool TextureCache::readManifest(std::vector<FileRec> &out) const {
    out.clear();
    std::vector<char> bytes;
    if (!cachefile::readWholeFile(path(kManifestFile), bytes)) return false;
    std::istringstream in(std::string(bytes.data(), bytes.size()));
    std::string tag;
    int version = 0;
    if (!(in >> tag >> version) || tag != "jahshaka-texture-cache" ||
        version != kTextureCacheFormat)
        return false;
    std::string keyTag, key;
    if (!(in >> keyTag >> key) || keyTag != "key" || key != mKey) return false;
    std::string what;
    while (in >> what) {
        if (what != "file") return false;
        FileRec r;
        if (!(in >> r.name >> r.bytes >> r.hash)) return false;
        out.push_back(r);
    }
    return true;
}

bool TextureCache::writeManifest(const std::vector<FileRec> &files) const {
    std::ostringstream s;
    s << "jahshaka-texture-cache " << kTextureCacheFormat << "\n";
    s << "key " << mKey << "\n";
    for (const FileRec &f : files) s << "file " << f.name << " " << f.bytes << " " << f.hash << "\n";
    const std::string text = s.str();
    return cachefile::writeAtomic(mDir, kManifestFile, text.data(), text.size());
}

void TextureCache::wipe() const {
    if (mDir.empty()) return;
    std::remove(path(kManifestFile).c_str());
    std::remove(path(kMetaFile).c_str());
    std::remove(path(kChannelFile).c_str());
}

// ---------------------------------------------------------------------------
void TextureCache::load(Ogre::Root *root) {
    if (!mEnabled) return;
    Ogre::TextureGpuManager *tm = textureManager(root);
    if (!tm) return;

    std::vector<FileRec> manifest;
    if (!readManifest(manifest)) {
        // No manifest, a different format, or a different app build. Not an
        // error and not worth a log line on a first-ever launch — just cold.
        wipe();
        return;
    }

    // Every file the manifest names must be present, the right size and the
    // right hash. Anything else and the whole container goes: two files that
    // disagree about which run wrote them are worse than none.
    std::map<std::string, std::vector<char>> contents;
    for (const FileRec &f : manifest) {
        std::vector<char> bytes;
        if (!cachefile::readWholeFile(path(f.name), bytes) ||
            bytes.size() != size_t(f.bytes) ||
            cachefile::hex128(bytes.data(), bytes.size()) != f.hash) {
            logLine("manifest does not match " + f.name + " — starting cold");
            wipe();
            return;
        }
        contents[f.name] = std::move(bytes);
    }

    // ---- Ogre's half. bCreateReservedPools=false, deliberately: we never call
    // reservePoolId(), so there is no pool of ours for the import to recreate,
    // and letting it reserve pools from a file would be the one way this cache
    // could change how textures are BATCHED rather than just how fast they load.
    auto meta = contents.find(kMetaFile);
    if (meta != contents.end() && !meta->second.empty()) {
        const std::string json(meta->second.data(), meta->second.size());
        try {
            tm->importTextureMetadataCache(kMetaFile, json.c_str(), false);
        } catch (const Ogre::Exception &e) {
            // A parse error is the one thing importTextureMetadataCache throws.
            logLine(std::string("metadata cache rejected: ") + e.getDescription() +
                    " — starting cold");
            wipe();
            return;
        }
    }

    // ---- Our half (D-D(b)). One record per line: "<components> <0|1> <path>",
    // path LAST so it may contain spaces. A malformed line is skipped rather
    // than fatal — the worst case is a probe we could have avoided.
    auto ch = contents.find(kChannelFile);
    if (ch != contents.end()) {
        std::istringstream in(std::string(ch->second.data(), ch->second.size()));
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            const size_t a = line.find(' ');
            if (a == std::string::npos) continue;
            const size_t b = line.find(' ', a + 1);
            if (b == std::string::npos) continue;
            const std::string p = line.substr(b + 1);
            if (p.empty()) continue;
            mChannels[p] = { unsigned(std::strtoul(line.c_str(), nullptr, 10)),
                             line[a + 1] != '0' };
        }
    }
    mDirty = false;
    logLine("loaded " + std::to_string(mChannels.size()) + " channel rows");
}

bool TextureCache::save(Ogre::Root *root) {
    if (!mEnabled) return false;
    Ogre::TextureGpuManager *tm = textureManager(root);
    if (!tm) return false;
    if (!cachefile::mkpath(mDir)) return false;

    Ogre::String json;
    tm->exportTextureMetadataCache(json);
    if (json.size() > kMaxMetadataBytes) {
        logLine("metadata cache is " + std::to_string(json.size()) +
                " bytes (cap " + std::to_string(kMaxMetadataBytes) + ") — not written");
        wipe();
        return false;
    }

    std::string channels;
    channels.reserve(mChannels.size() * 64);
    for (const auto &kv : mChannels) {
        // A path with a newline in it would corrupt the file; there is no
        // escaping scheme and there does not need to be one, because such a path
        // simply never gets a row (it re-probes, exactly as it does today).
        if (kv.first.find('\n') != std::string::npos) continue;
        channels += std::to_string(kv.second.first);
        channels += kv.second.second ? " 1 " : " 0 ";
        channels += kv.first;
        channels += '\n';
    }

    std::vector<FileRec> manifest;
    if (!cachefile::writeAtomic(mDir, kMetaFile, json.data(), json.size())) return false;
    manifest.push_back({ kMetaFile, json.size(), cachefile::hex128(json.data(), json.size()) });
    if (!cachefile::writeAtomic(mDir, kChannelFile, channels.data(), channels.size())) return false;
    manifest.push_back({ kChannelFile, channels.size(),
                         cachefile::hex128(channels.data(), channels.size()) });
    // The manifest LAST, always: it is the thing that makes the other two
    // readable, so a crash between the two writes leaves a container that fails
    // its own check and starts cold, never one that half-loads.
    if (!writeManifest(manifest)) return false;
    mDirty = false;
    return true;
}

bool TextureCache::clear() {
    mChannels.clear();
    mDirty = false;
    if (!mEnabled) return true;
    wipe();
    return true;
}

bool TextureCache::channels(const std::string &p, unsigned &components, bool &compressed) const {
    auto it = mChannels.find(p);
    if (it == mChannels.end()) return false;
    components = it->second.first;
    compressed = it->second.second;
    return true;
}

void TextureCache::note(const std::string &p, unsigned components, bool compressed) {
    if (!mEnabled || p.empty()) return;
    auto &slot = mChannels[p];
    if (slot.first == components && slot.second == compressed) return;
    slot = { components, compressed };
    mDirty = true;
}

unsigned TextureCache::channelEntries() const { return unsigned(mChannels.size()); }

unsigned TextureCache::metadataEntries(Ogre::Root *root) const {
    Ogre::TextureGpuManager *tm = textureManager(root);
    if (!tm) return 0;
    // Ogre keeps mMetadataCache private with no size() — the export is the only
    // way to ask. Counting `"texture_type"` is exact: exportTextureMetadataCache
    // writes it once per TEXTURE entry and never in the reserved_pool_ids block,
    // which carries only poolId/resolution/mipmaps/format
    // (OgreTextureGpuManager.cpp:1096-1160).
    Ogre::String json;
    tm->exportTextureMetadataCache(json);
    unsigned n = 0;
    const std::string needle = "\"texture_type\"";
    for (size_t at = json.find(needle); at != std::string::npos;
         at = json.find(needle, at + needle.size()))
        ++n;
    return n;
}

}   // namespace detail
}}  // namespace jahshaka::engine
