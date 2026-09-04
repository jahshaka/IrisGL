// Decals — projected-texture stickers (DECALS_SPEC.md).
//
// An Ogre::Decal is a MovableObject that is NOT a Renderable: it draws nothing
// itself. Forward+ clustered culling collects it into the per-cell object list
// and the PBS pixel shader (ForwardPlus_DecalsCubemaps_piece_ps.any) overwrites
// base colour / roughness / F0 for every pixel inside the decal's oriented box.
//
// Three backend facts shape everything below.
//
// 1. THE BOX IS THE NODE'S SCALE. Decal's constructor pins its local AABB to a
//    unit box and ForwardClustered::collectLightForSlice deliberately IGNORES
//    mLocalAabb/mWorldAabb, rebuilding the OBB from the node's derived
//    position, orientation and scale*0.5 (OgreForwardClustered.cpp:186-213).
//    So the decal rides an internal child node whose scale IS
//    (width, depth, height) — and the document node's own scale multiplies it.
//
// 2. DECALS STAY IN RENDER QUEUE 0, ALWAYS. ForwardClustered::collectObjs
//    counts them with `if( MinDecalRq >= rqId && rqId <= MaxDecalRq )`
//    (OgreForwardClustered.cpp:877) — upstream's typo for `rqId >= MinDecalRq`.
//    With MinDecalRq == 0 that is true ONLY for rqId 0, so decals in RQ 1..4 are
//    NOT counted while fillGlobalLightListBuffer still WRITES them
//    (OgreForwardPlusBase.cpp:254-284): more than 16 of them overruns the global
//    list buffer. Decal's constructor defaults to RQ 0, which is counted
//    correctly, so the fix is simply never to move them. There is an assert
//    below that pins it.
//
// 3. DECAL IMAGES LIVE IN A DEDICATED, FIXED-GEOMETRY TEXTURE POOL. A decal does
//    not sample "a texture": it samples one Type2DArray per channel and carries
//    a slice index into it (Decal::setDiffuseTexture reads
//    tex->getInternalSliceStart()). TextureGpuManager matches pools EXACTLY on
//    width + height + format + mip count + poolId, never grows one
//    (TODO_grow_pool, OgreTextureGpuManager.cpp:2133) and silently creates a
//    SECOND pool when the first is full — at which point the new decal keeps a
//    slice index valid for the OLD pool and samples another decal's image, with
//    no assert anywhere. Hence: we reserve our own pools, we resample every
//    image into their exact geometry, and we REFUSE LOUDLY when one is full.
//    Studio's ordinary loadTexture() is unusable here twice over: it uses
//    poolId 0 (shared with every PBR map) and its grayscale branch produces a
//    NON-batched texture, which trips Decal::setDiffuseTexture's assert.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine {
namespace detail {

namespace {

// Pool geometry (DECALS_SPEC D1). 512x512 with a full mip chain:
//   diffuse/emissive  RGBA8 sRGB  -> 1.33 MiB/slice -> 32 slices ~= 43 MiB
//   normal            RG8  SNORM  -> 0.67 MiB/slice -> 32 slices ~= 21 MiB
// Each pool is reserved LAZILY, on the first image of that kind, because
// reservePoolId transitions the master texture Resident immediately (the
// loadLtcMatrix precedent). A project with only diffuse decals never pays for
// the normal or emissive atlas.
constexpr Ogre::uint32 kDecalAtlasSize   = 512u;
constexpr Ogre::uint32 kDecalAtlasSlices = 32u;
// Pool ids must not collide with 0, which is what every ordinary
// createOrRetrieveTexture (i.e. OgreScene::loadTexture) uses.
constexpr Ogre::uint32 kPoolIdDiffuse  = 0x4A414801u;   // 'JAH' + 1
constexpr Ogre::uint32 kPoolIdNormal   = 0x4A414802u;
constexpr Ogre::uint32 kPoolIdEmissive = 0x4A414803u;

Ogre::PixelFormatGpu atlasFormat(DecalMap kind)
{
    // The shader reads decalsNormalsTex.xy and ADDS it to the tangent-space
    // normal, so the normal atlas must be SIGNED; RG8_SNORM is the smallest
    // format Image2::generateMipmaps has a downsampler for (OgreImage2.cpp:1023).
    if (kind == DecalMap::Normal) return Ogre::PFG_RG8_SNORM;
    return Ogre::PFG_RGBA8_UNORM_SRGB;
}

Ogre::uint32 atlasPoolId(DecalMap kind)
{
    switch (kind) {
    case DecalMap::Normal:   return kPoolIdNormal;
    case DecalMap::Emissive: return kPoolIdEmissive;
    default:                 return kPoolIdDiffuse;
    }
}

const char *atlasName(DecalMap kind)
{
    switch (kind) {
    case DecalMap::Normal:   return "normal";
    case DecalMap::Emissive: return "emissive";
    default:                 return "diffuse";
    }
}

}  // namespace

// The atlases are PROCESS-WIDE, like the pools they wrap: TextureGpuManager
// belongs to Ogre::Root, and Root is a singleton. Every scene (editor, player,
// thumbnails, material preview) points its SceneManager at the same masters, so
// one image loaded twice costs one slice, not two.
struct DecalAtlas {
    Ogre::TextureGpu *master = nullptr;          ///< the reserved pool owner
    unsigned          used   = 0;                ///< slices handed out
    std::map<std::string, Ogre::TextureGpu *> byPath;   ///< path -> slice texture
    std::map<Ogre::TextureGpu *, unsigned>    refs;     ///< slice texture -> live users
};

DecalAtlas &decalAtlas(DecalMap kind)
{
    static DecalAtlas sAtlases[3];
    return sAtlases[static_cast<int>(kind)];
}

void resetDecalAtlases()
{
    for (int i = 0; i < 3; ++i) {
        DecalAtlas &a = decalAtlas(static_cast<DecalMap>(i));
        a.master = nullptr;
        a.used = 0;
        a.byPath.clear();
        a.refs.clear();
    }
}

unsigned decalAtlasCapacity()
{
    return kDecalAtlasSlices;
}

/// One user of a pooled decal image went away. Frees the slice when the last
/// one does. Returns true when the texture was actually destroyed.
bool releaseDecalTexture(Ogre::TextureGpuManager *tm, DecalMap kind, Ogre::TextureGpu *tex)
{
    if (!tex) return false;
    DecalAtlas &atlas = decalAtlas(kind);
    auto it = atlas.refs.find(tex);
    if (it == atlas.refs.end()) return false;
    if (--it->second > 0) return false;
    atlas.refs.erase(it);
    for (auto pit = atlas.byPath.begin(); pit != atlas.byPath.end(); ++pit) {
        if (pit->second == tex) { atlas.byPath.erase(pit); break; }
    }
    if (atlas.used) --atlas.used;
    tm->destroyTexture(tex);
    return true;
}

}  // namespace detail

using namespace detail;

// ---------------------------------------------------------------------------
// Image normalization: any source image -> the atlas geometry.
//
// DECALS_SPEC D2 (a): resample on bind, aspect preserved, letterboxed into
// TRANSPARENT padding. The alpha channel is already the decal's mask, so
// padding is invisible; squashing non-square art instead would silently distort
// every logo and poster.
namespace {

struct Rgba { float r, g, b, a; };

/// Bilinear sample of `src` in normalized [0,1] coordinates, with the source's
/// single-channel (grayscale) images expanded to neutral RGB — the same defect
/// OgreScene::loadTexture works around: a 1-channel file decodes to R8 and
/// would otherwise sample red-only.
Rgba sampleSrc(const Ogre::Image2 &src, bool grayscale, float u, float v)
{
    const float fw = float(src.getWidth()), fh = float(src.getHeight());
    const float x = std::min(std::max(u * fw - 0.5f, 0.0f), fw - 1.0f);
    const float y = std::min(std::max(v * fh - 0.5f, 0.0f), fh - 1.0f);
    const Ogre::uint32 x0 = Ogre::uint32(x), y0 = Ogre::uint32(y);
    const Ogre::uint32 x1 = std::min(x0 + 1u, src.getWidth() - 1u);
    const Ogre::uint32 y1 = std::min(y0 + 1u, src.getHeight() - 1u);
    const float fx = x - float(x0), fy = y - float(y0);
    Ogre::ColourValue c00 = src.getColourAt(x0, y0, 0);
    Ogre::ColourValue c10 = src.getColourAt(x1, y0, 0);
    Ogre::ColourValue c01 = src.getColourAt(x0, y1, 0);
    Ogre::ColourValue c11 = src.getColourAt(x1, y1, 0);
    if (grayscale) {
        c00.g = c00.b = c00.r; c00.a = 1.0f;
        c10.g = c10.b = c10.r; c10.a = 1.0f;
        c01.g = c01.b = c01.r; c01.a = 1.0f;
        c11.g = c11.b = c11.r; c11.a = 1.0f;
    }
    const Ogre::ColourValue top = c00 * (1.0f - fx) + c10 * fx;
    const Ogre::ColourValue bot = c01 * (1.0f - fx) + c11 * fx;
    const Ogre::ColourValue c = top * (1.0f - fy) + bot * fy;
    return Rgba{ c.r, c.g, c.b, c.a };
}

}  // namespace

// ---------------------------------------------------------------------------
TextureId OgreScene::loadDecalTexture(const std::string &path, DecalMap kind)
{
    DecalAtlas &atlas = decalAtlas(kind);

    // Already in this scene's table? (Same path, same kind = same slice.)
    // One hash lookup, not a scan of every texture the scene holds — the index
    // namespaces decal slices by kind exactly so this stays correct.
    {
        auto hit = mTextureIndex.find(textureKey(path, true, kind));
        if (hit != mTextureIndex.end()) return hit->second;
    }

    JAH_TRY {
        Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();

        // Another scene already loaded it: share the slice, take a reference.
        auto shared = atlas.byPath.find(path);
        if (shared != atlas.byPath.end()) {
            ++atlas.refs[shared->second];
            TextureRec rec;
            rec.texture = shared->second;
            rec.path = path;
            rec.decal = true;
            rec.decalKind = kind;
            return trackTexture(rec);
        }

        // Budget check BEFORE anything is created. Overflow is the one failure
        // mode with no symptom: Ogre would quietly open a second pool and the
        // decal would sample a different decal's image forever.
        if (atlas.used >= kDecalAtlasSlices) {
            mError = std::string("loadDecalTexture: the ") + atlasName(kind) +
                     " decal atlas is full (" + std::to_string(kDecalAtlasSlices) +
                     " distinct images). Remove a decal image before adding another.";
            return 0;
        }

        const size_t slash = path.find_last_of("/\\");
        const std::string dir  = slash == std::string::npos ? "." : path.substr(0, slash);
        const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
        Ogre::ResourceGroupManager &rgm = Ogre::ResourceGroupManager::getSingleton();
        static const char *kGroup = "Jahshaka";
        if (!rgm.resourceGroupExists(kGroup)) rgm.createResourceGroup(kGroup, false);
        if (!mTextureDirs.count(dir)) {
            rgm.addResourceLocation(dir, "FileSystem", kGroup, false);
            mTextureDirs.insert(dir);
        }
        if (!rgm.resourceExists(kGroup, file)) {
            mError = "loadDecalTexture: file not found: " + path;
            return 0;
        }

        // load2, not load(): the extension is validated against the magic bytes
        // and content-sniffed on mismatch (mislabeled GLB-embedded textures).
        Ogre::Image2 src;
        {
            Ogre::DataStreamPtr stream = rgm.openResource(file, kGroup);
            src.load2(stream, file);
        }
        const Ogre::PixelFormatGpu srcFmt = src.getPixelFormat();
        if (Ogre::PixelFormatGpuUtils::isCompressed(srcFmt)) {
            // getColourAt cannot read block-compressed data, and resampling is
            // the whole point of this path. Refuse rather than bind garbage.
            mError = "loadDecalTexture: block-compressed images (DDS/KTX) are not "
                     "supported as decal images: " + path;
            return 0;
        }
        const bool grayscale =
            Ogre::PixelFormatGpuUtils::getNumberOfComponents(srcFmt) == 1;

        // Aspect-preserving fit into the atlas square; the remainder is
        // transparent padding (alpha is the mask, so it is invisible).
        const float srcW = float(src.getWidth()), srcH = float(src.getHeight());
        if (srcW < 1.0f || srcH < 1.0f) { mError = "loadDecalTexture: empty image: " + path; return 0; }
        const float scale = std::min(float(kDecalAtlasSize) / srcW, float(kDecalAtlasSize) / srcH);
        const Ogre::uint32 fitW = std::max(1u, Ogre::uint32(srcW * scale + 0.5f));
        const Ogre::uint32 fitH = std::max(1u, Ogre::uint32(srcH * scale + 0.5f));
        const Ogre::uint32 offX = (kDecalAtlasSize - std::min(fitW, kDecalAtlasSize)) / 2u;
        const Ogre::uint32 offY = (kDecalAtlasSize - std::min(fitH, kDecalAtlasSize)) / 2u;

        const Ogre::PixelFormatGpu fmt = atlasFormat(kind);
        const Ogre::uint8 mips =
            Ogre::PixelFormatGpuUtils::getMaxMipmapCount(kDecalAtlasSize, kDecalAtlasSize);

        // Build the padded, resampled image. Written through the raw TextureBox
        // rather than setColourAt so the SNORM normal atlas is exact.
        Ogre::Image2 *dst = new Ogre::Image2();
        dst->createEmptyImage(kDecalAtlasSize, kDecalAtlasSize, 1u,
                              Ogre::TextureTypes::Type2D, fmt, 1u);
        {
            Ogre::TextureBox box = dst->getData(0);
            const size_t bpp = Ogre::PixelFormatGpuUtils::getBytesPerPixel(fmt);
            for (Ogre::uint32 y = 0; y < kDecalAtlasSize; ++y) {
                auto *row = static_cast<Ogre::uint8 *>(box.at(0, y, 0));
                for (Ogre::uint32 x = 0; x < kDecalAtlasSize; ++x) {
                    Ogre::uint8 *px = row + size_t(x) * bpp;
                    const bool inside = x >= offX && x < offX + fitW &&
                                        y >= offY && y < offY + fitH;
                    Rgba c{ 0, 0, 0, 0 };
                    if (inside) {
                        const float u = (float(x - offX) + 0.5f) / float(fitW);
                        const float v = (float(y - offY) + 0.5f) / float(fitH);
                        c = sampleSrc(src, grayscale, u, v);
                    }
                    if (kind == DecalMap::Normal) {
                        // Tangent-space normal map: RGB in [0,1] -> XY in [-1,1].
                        // Padding (c == 0) must decode to a FLAT normal, i.e. 0,
                        // not -1: outside the image the decal adds nothing.
                        const float nx = inside ? (c.r * 2.0f - 1.0f) : 0.0f;
                        const float ny = inside ? (c.g * 2.0f - 1.0f) : 0.0f;
                        px[0] = Ogre::uint8(Ogre::int8(std::lround(
                            std::min(std::max(nx, -1.0f), 1.0f) * 127.0f)));
                        px[1] = Ogre::uint8(Ogre::int8(std::lround(
                            std::min(std::max(ny, -1.0f), 1.0f) * 127.0f)));
                    } else {
                        px[0] = Ogre::uint8(std::lround(std::min(std::max(c.r, 0.0f), 1.0f) * 255.0f));
                        px[1] = Ogre::uint8(std::lround(std::min(std::max(c.g, 0.0f), 1.0f) * 255.0f));
                        px[2] = Ogre::uint8(std::lround(std::min(std::max(c.b, 0.0f), 1.0f) * 255.0f));
                        px[3] = Ogre::uint8(std::lround(std::min(std::max(c.a, 0.0f), 1.0f) * 255.0f));
                    }
                }
            }
        }
        // Mip count must match the pool's EXACTLY or the texture lands in a new
        // pool (see the header comment); generateMipmaps grows the chain to the
        // full count, which is what the pool is reserved with.
        if (!dst->generateMipmaps(kind != DecalMap::Normal, Ogre::Image2::FILTER_BILINEAR)) {
            delete dst;
            mError = std::string("loadDecalTexture: could not build mipmaps for the ") +
                     atlasName(kind) + " atlas format";
            return 0;
        }

        // Reserve the pool on first use — reservePoolId allocates the whole
        // master texture immediately.
        if (!atlas.master) {
            atlas.master = tm->reservePoolId(atlasPoolId(kind), kDecalAtlasSize, kDecalAtlasSize,
                                             kDecalAtlasSlices, mips, fmt);
            if (atlas.master) {
                // UPSTREAM GAP: reservePoolId calls _transitionTo(Resident) +
                // notifyDataIsReady but never _setNextResidencyStatus, so the
                // master reads Resident / next = OnStorage. Anything that then
                // calls scheduleTransitionTo(Resident) on it (Ogre does, once
                // the pool is bound as a shader texture) queues a FILE load for
                // a texture that has no file — the streaming worker ends up
                // memcpy'ing from a null mip pointer and the process dies in
                // TextureGpuManager::processQueuedImage. One line, no patch.
                atlas.master->_setNextResidencyStatus(Ogre::GpuResidency::Resident);
            }
            if (!atlas.master) {
                delete dst;
                mError = std::string("loadDecalTexture: could not reserve the ") +
                         atlasName(kind) + " decal atlas";
                return 0;
            }
        }

        Ogre::TextureGpu *tex = tm->createTexture(
            processUniqueName("decal"), Ogre::GpuPageOutStrategy::Discard,
            Ogre::TextureFlags::AutomaticBatching, Ogre::TextureTypes::Type2D);
        tex->setResolution(kDecalAtlasSize, kDecalAtlasSize);
        tex->setNumMipmaps(mips);
        tex->setPixelFormat(fmt);
        tex->setTexturePoolId(atlasPoolId(kind));
        // SYNCHRONOUS upload, the area-light sample's route — NOT
        // scheduleTransitionTo(Resident, image, true).
        //
        // The async route is for textures that come from FILES. Handing it an
        // in-memory Image2 for an AutomaticBatching texture segfaults the
        // streaming worker on this pin: processLoadRequest re-derives the
        // texture's metadata from the image and the queued image ends up with a
        // null mip-0 pointer, which processQueuedImage memcpy's from
        // (OgreTextureGpuManager.cpp:2607; Ogre's asserts are compiled out in
        // our RelWithDebInfo Ogre, so it is a crash and not a message).
        //
        // _transitionTo(Resident) reserves the pool slot (OgreTextureGpu.cpp:471);
        // _setNextResidencyStatus keeps getNextResidencyStatus() in step, which
        // matters because Decal::setDiffuseTexture calls scheduleTransitionTo
        // (Resident) and would otherwise queue a file load for a texture that has
        // no file. notifyDataIsReady is ours to call here — the automatic call in
        // _transitionTo is ManualTexture-only, and calling it twice underflows
        // mDataPreparationsPending (see OgreScene::createTexture).
        tex->_setNextResidencyStatus(Ogre::GpuResidency::Resident);
        tex->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        dst->uploadTo(tex, 0, static_cast<Ogre::uint8>(mips - 1u));
        tex->notifyDataIsReady();
        delete dst;

        atlas.byPath[path] = tex;
        atlas.refs[tex] = 1;
        ++atlas.used;

        TextureRec rec;
        rec.texture = tex;
        rec.path = path;
        rec.decal = true;
        rec.decalKind = kind;
        return trackTexture(rec);
    } JAH_CATCH(mError, 0);
}

unsigned OgreScene::decalAtlasCapacity(DecalMap) const
{
    return detail::decalAtlasCapacity();
}

unsigned OgreScene::decalAtlasUsed(DecalMap kind) const
{
    return decalAtlas(kind).used;
}

// ---------------------------------------------------------------------------
bool OgreScene::setDecal(NodeId id, const DecalDesc &d)
{
    auto it = mNodes.find(id);
    if (it == mNodes.end()) { mError = "setDecal: unknown node"; return false; }
    if (!d.diffuse) {
        mError = "setDecal: a decal needs a diffuse image (loadDecalTexture); "
                 "use removeDecal to clear one";
        return false;
    }
    auto resolve = [&](TextureId tid, DecalMap kind, Ogre::TextureGpu *&out) -> bool {
        out = nullptr;
        if (!tid) return true;
        auto tit = mTextures.find(tid);
        if (tit == mTextures.end()) { mError = "setDecal: unknown texture id"; return false; }
        if (!tit->second.decal || tit->second.decalKind != kind) {
            // The load-bearing guard: an ordinary loadTexture() id is either
            // non-batched (Decal::setDiffuseTexture asserts) or in pool 0, where
            // its slice index means nothing to the decal atlas.
            mError = "setDecal: textures must come from loadDecalTexture() for the "
                     "matching map kind";
            return false;
        }
        out = tit->second.texture;
        return true;
    };
    Ogre::TextureGpu *diffuse = nullptr, *normal = nullptr, *emissive = nullptr;
    if (!resolve(d.diffuse,  DecalMap::Diffuse,  diffuse))  return false;
    if (!resolve(d.normal,   DecalMap::Normal,   normal))   return false;
    if (!resolve(d.emissive, DecalMap::Emissive, emissive)) return false;

    JAH_TRY {
        Node &n = it->second;
        if (!n.decal) {
            n.decalNode = n.node->createChildSceneNode();
            n.decal = mSceneMgr->createDecal();
            n.decalNode->attachObject(n.decal);
            ++mDecalCount;
        }
        // NEVER call Decal::setRenderQueueGroup. The constructor puts decals in
        // RQ 0 and that is the only queue ForwardClustered::collectObjs counts
        // correctly (see the file header): decals in RQ 1..4 are written to the
        // global light list but not counted, and past 16 of them the buffer
        // overruns. This assert exists so a future edit that moves them trips
        // here rather than corrupting GPU memory.
        assert(n.decal->getRenderQueueGroup() == 0u &&
               "decals must stay in render queue 0 — upstream miscounts RQ 1..4");

        // The projector box: the culler reads the node's DERIVED scale as the
        // box's full extents (halved internally). Local X = width, local Y =
        // depth (the projection axis), local Z = height, matching the shader's
        // decalUV = localPos.xz.
        n.decalNode->setScale(std::max(d.width,  0.001f),
                              std::max(d.depth,  0.001f),
                              std::max(d.height, 0.001f));

        // The LISTENING setters (not the ...Raw ones): a pooled texture's slice
        // can move, and only these re-read it on PoolTextureSlotChanged.
        n.decal->setDiffuseTexture(diffuse);
        n.decal->setNormalTexture(normal);
        n.decal->setEmissiveTexture(emissive);
        n.decal->setMetalness(std::min(std::max(d.metalness, 0.0f), 1.0f));
        n.decal->setRoughness(std::min(std::max(d.roughness, 0.0f), 1.0f));
        n.decal->setIgnoreAlphaDiffuse(d.ignoreAlphaDiffuse);

        refreshDecalBindings();
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::removeDecal(NodeId id)
{
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.decal) return false;
    JAH_TRY {
        releaseDecal(it->second);
        refreshDecalBindings();
        return true;
    } JAH_CATCH(mError, false);
}

void OgreScene::releaseDecal(Node &n)
{
    if (n.decal) {
        n.decal->detachFromParent();
        mSceneMgr->destroyDecal(n.decal);
        n.decal = nullptr;
        if (mDecalCount) --mDecalCount;
    }
    if (n.decalNode) { mSceneMgr->destroySceneNode(n.decalNode); n.decalNode = nullptr; }
}

void OgreScene::refreshDecalBindings()
{
    if (!mSceneMgr) return;
    // The shader permutation is gated on a NON-NULL SceneManager decal texture
    // (OgreForwardClustered.cpp:1095-1120), so a scene with no decals must clear
    // the bindings — that is what drops the decal code out of every PBS shader
    // again. And per Ogre's own docs, binding a normal/emissive atlas that no
    // decal actually uses is pure wasted shader work, so each channel is bound
    // only while some decal in THIS scene carries that map.
    bool anyDiffuse = false, anyNormal = false, anyEmissive = false;
    for (auto &kv : mNodes) {
        const Ogre::Decal *dec = kv.second.decal;
        if (!dec) continue;
        if (dec->getDiffuseTexture())  anyDiffuse = true;
        if (dec->getNormalTexture())   anyNormal = true;
        if (dec->getEmissiveTexture()) anyEmissive = true;
    }
    mSceneMgr->setDecalsDiffuse(anyDiffuse ? decalAtlas(DecalMap::Diffuse).master : nullptr);
    mSceneMgr->setDecalsNormals(anyNormal ? decalAtlas(DecalMap::Normal).master : nullptr);
    mSceneMgr->setDecalsEmissive(anyEmissive ? decalAtlas(DecalMap::Emissive).master : nullptr);
}

}}  // namespace jahshaka::engine
