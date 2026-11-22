// THE CUT-OUTS' MASKS — ONE MAKER FOR THE WHOLE ENGINE (REFLECT-MOVERS-2's maker, moved out of the ray tier by
// ENGINE-POLISH-ATOM-1 stage 3 and shipped by ATOM-3 stage 2 with its PYRAMID; OgreAlphaMasks.cpp).
//
// A cut-out's material decides, texel by texel, where its surface exists: its albedo texture's alpha through the
// datablock's own alpha test. Three consumers need that answer on the device, and all three read it the same way —
// ONE BIT A TEXEL, through a buffer's device address (jah_alpha_mask.glsl):
//   * the visibility buffer's id pass, which DISCARDS a masked cluster's fragment where the bit is clear
//     (atom_id.frag JAH_MASKED: the ids, the depth and so the depth pyramid are holed by construction);
//   * the caster cut, which discards the same way in its masked ranges (atom_caster_masked.frag);
//   * the ray queries, which confirm a candidate triangle only where the bit is set (jah_rq_alpha.glsl).
// THE PYRAMID (ATOM-3 stage 2): a 1-bit mask sampled at one level is the point sample of level 0 — a 1-texel
// checker minified 2x draws 50 % through it and SOLID through the stock alpha test of a filtered mip (whose alpha
// is a uniform 0.5), and neither is the other's coverage at every minification. So a mask is a pyramid of 1-bit
// levels built on the device by a COVERAGE-PRESERVING downsample (JahAlphaMaskPyramid_cs: each level's set
// fraction equals level 0's within one texel of the level, row-major error diffusion in closed form), and the two
// Atom passes sample the level their fragment's UV footprint asks for (jah_alpha_mask.glsl, the derivatives). The
// rays sample level 0 (a ray carries no footprint). Bytes: 4/3 of level 0 — a 1024-square mask 170.7 KiB.
// The ray tier used to make masks for its own traced cut-outs only, so a tier with the id pass and no ray tier had
// none (the deep read's STOP 3). The maker is the engine's now, the masks are shared by every scene, and every
// scene's GPU SCENE TABLE (GpuScene::maskTable*, written by OgreScene::updateMaskTable) names, per alpha-tested slot
// and per submesh, its mask and its base-map UV transform — the table the id pass, the caster cut and the ray tier
// all read.
//
// A MASK is made on the device by two Ogre compute jobs (Jahshaka/AlphaMask, JahAlphaMask_cs.glsl: level 0;
// Jahshaka/AlphaMaskPyramid, JahAlphaMaskPyramid_cs.glsl: every coarser level) the first frame its texture is
// resident: level 0 is the texture's first level no larger than kMaxSide on a side, one bit a texel, into an Ogre
// UAV buffer that holds the levels one after the other (levelOffsetWords). Its identity is the texture (pointer, name, size), the test (compare, threshold) AND the texture's content
// (its upload generation: new texels make a new mask; the old one is served, "stale", until it is remade). At most
// kPerFrame are made a frame; a mask no table names for kIdleFrames frames is forgotten.
#ifndef JAHSHAKA_ENGINE_ALPHAMASKS_H
#define JAHSHAKA_ENGINE_ALPHAMASKS_H

#include <OgreIdString.h>
#include <OgreCommon.h>

#include <cstdint>
#include <vector>

namespace Ogre {
class RenderSystem;
class TextureGpu;
class UavBufferPacked;
class VaoManager;
class HlmsDatablock;
}

namespace jahshaka {
namespace engine {
namespace detail {

class AlphaMaskStore {
public:
    /// The mask's largest side, texels: the texture's first level no larger is the one thresholded (a 1024 x 1024
    /// level 0 is 128 KiB, its pyramid 170.7 KiB).
    static constexpr uint32_t kMaxSide = 1024u;
    /// Masks made per frame at most (a scene opened with a forest of foliage materials makes them over a few frames;
    /// its cut-outs wait on the stock route meanwhile, and a ray candidate is refused).
    static constexpr unsigned kPerFrame = 8u;
    /// A mask no table names, and no table asked for in this many frames, is forgotten.
    static constexpr uint32_t kIdleFrames = 600u;

    struct Mask {
        const Ogre::TextureGpu *tex = nullptr;
        Ogre::IdString name;
        uint32_t texW = 0, texH = 0, cmp = 0, thresholdBits = 0;
        /// The texture's upload generation the mask was made from (ALPHA-MASK-IDENTITY-1).
        uint32_t generation = 0;
        /// Level 0's size and the texture level it was thresholded from; the pyramid's level count (levelCount).
        uint32_t w = 0, h = 0, level = 0, levels = 1;
        Ogre::UavBufferPacked *bits = nullptr;
        uint64_t address = 0;
        uint32_t lastUsed = 0;
        /// A stable id, and the scenes' tables that name this mask now (a mask a table names never expires).
        unsigned long long id = 0ull;
        uint32_t users = 0u;
    };

    /// WHAT A CUT-OUT'S MASK IS NOW (a lookup, never a make: the GPU scene's compose asks it from its worker lanes,
    /// so it reads and writes nothing else). Ready: a mask of this content or an older one (served until remade);
    /// Pending: none yet (the texture loading, the frame's budget spent); Refused: this texture can never have one.
    enum class State : uint8_t { Ready, Pending, Refused };
    State stateOf(const Ogre::TextureGpu *tex, uint32_t cmp, float threshold) const;

    /// The mask for (tex, cmp, threshold): its index, or -1 while it cannot be made yet (the texture not resident,
    /// this frame's budget spent, no compute job). `stale` = the index is an OLDER content's mask, served because
    /// this frame's budget is spent (the caller re-asks next frame); `refused` = this texture can never have one.
    /// Dispatches the compute job (outside any render pass: the frame's GPU-scene step).
    int maskFor(Ogre::TextureGpu *tex, uint32_t cmp, float threshold, unsigned &budget, bool &stale, bool &refused);
    const Mask &mask(int i) const { return mMasks[size_t(i)]; }
    /// The masks held now (a test walks them: atom.masked_coverage).
    size_t count() const { return mMasks.size(); }
    /// Moves whenever a mask is made or dropped: every table that may name it is re-written.
    unsigned long long epoch() const { return mEpoch; }
    /// Masks made, ever (RayQueryStatus::alphaMasksMade).
    unsigned long long made() const { return mMade; }
    /// A table that named `was` names `now` (sorted ids): the users follow.
    void retarget(const std::vector<unsigned long long> &was, const std::vector<unsigned long long> &now);
    /// THE EDGE every address reader needs after this frame's makes (compute, vertex and fragment stages read the
    /// bits through an address the barrier solver cannot see), and the idle sweep (one per kIdleFrames frames).
    void endFrame(Ogre::RenderSystem *rs);
    /// Before Root: every mask's buffer.
    void release();
    /// A TEST'S READ (atom.masked_coverage): mask `i`'s whole pyramid, synchronously (a measurement, never a frame's).
    bool readBits(int i, std::vector<uint32_t> &out) const;

    /// THE PYRAMID'S SHAPE (JahAlphaMaskPyramid_cs and jah_alpha_mask.glsl compute the same): level k is
    /// max(1, w >> k) x max(1, h >> k), the levels run until both sides are 1, level k's bits start at word
    /// levelOffsetWords(w, h, k).
    static uint32_t levelCount(uint32_t w, uint32_t h);
    static uint32_t levelOffsetWords(uint32_t w, uint32_t h, uint32_t k);
private:
    int findIndex(const Ogre::TextureGpu *tex, uint32_t cmp, uint32_t thresholdBits, bool &stale) const;
    std::vector<Mask> mMasks;
    /// Textures refused for good (not 2D, not a 2D array): pointer + name.
    std::vector<std::pair<const Ogre::TextureGpu *, Ogre::IdString>> mRefused;
    unsigned long long mNextId = 1ull, mMade = 0ull, mEpoch = 0ull;
    uint32_t mSweptAt = 0u;
    bool mFailed = false;     ///< the compute job is missing: no mask is ever made
    bool mDispatched = false; ///< a make this frame: endFrame records the edge
    Ogre::VaoManager *mVao = nullptr;
};

/// The engine's one store (a process singleton, like Root: the masks are device objects of the one render system).
AlphaMaskStore &alphaMaskStore();
/// The store if one exists, else null (a scene torn down after releaseAlphaMasks must not make a new one).
AlphaMaskStore *alphaMaskStoreIfAny();
/// Before Root (~OgreEngine).
void releaseAlphaMasks();
/// Ogre's alpha test, the raster's way round (800.PixelShader: discard when `threshold CMP alpha`): is a texel of
/// this alpha KEPT? (A cut-out with no albedo map: the colour's alpha decides for every texel.)
bool alphaTestKeeps(Ogre::CompareFunction cmp, float threshold, float alpha);

/// THE TABLE'S LAYOUT (jah_alpha_mask.glsl reads it; one table per scene, GpuScene::maskTable*):
///   u[0] = N, the slots the per-slot index covers; u[1] = entries;
///   u[2] = the highest pyramid level a fragment may sample (kMaskAllLevels: the footprint's; 0 = level 0 only —
///          the arm atom.maskPyramid 0, the level-0 reference of the silhouette rows); u[3] = 0
///   u[4 + s] = the word where slot s's entry starts, kMaskNone none (s < N)
///   an entry: (submeshes, 0, 0, 0), then kMaskRecordWords per SUBMESH, its own datablock's:
///     (mask address lo, hi, width | height << 16, flags),
///     the UV transform's (scale x, scale y, bias x, bias y), float bits,
///     its rotation, row-major (m00, m01, m10, m11), float bits.
///   flags: kMaskConstant = no mask, the constant answer is kMaskConstantSolid; kMaskPending = the mask is still
///   being made.
constexpr uint32_t kMaskNone = 0xFFFFFFFFu;
constexpr uint32_t kMaskConstant = 1u, kMaskConstantSolid = 2u, kMaskPending = 4u;
constexpr uint32_t kMaskRecordWords = 12u;
constexpr uint32_t kMaskAllLevels = 0xFFFFu;
constexpr uint32_t kMaskHeaderWords = 4u;

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_ALPHAMASKS_H
