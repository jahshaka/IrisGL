// THE CUT-OUTS' MASKS (AlphaMasks.h says what and why; ENGINE-POLISH-ATOM-1 stage 3 moved the maker here from the
// ray tier, REFLECT-MOVERS-2's) and EVERY SCENE'S MASK TABLE (OgreScene::updateMaskTable).
#include "AlphaMasks.h"
#include "EnginePrivate.h"
#include "GpuScene.h"
#if JAH_RAY_QUERY
#include "AtomVulkan.h"
#include "OgreVulkanDevice.h"
#include "OgreVulkanQueue.h"
#endif

#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreItem.h>
#include <OgreLogManager.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreSubItem.h>
#include <OgreTextureGpu.h>
#include <Vao/OgreAsyncTicket.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <cstring>

namespace jahshaka {
namespace engine {
namespace detail {

namespace {
AlphaMaskStore *gStore = nullptr;

Ogre::HlmsComputeJob *computeJob(const char *name) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
    Ogre::HlmsCompute *hc = hm ? hm->getComputeHlms() : nullptr;
    return hc ? hc->findComputeJobNoThrow(name) : nullptr;
}
}  // namespace

uint32_t AlphaMaskStore::levelCount(uint32_t w, uint32_t h) {
    uint32_t n = 1u;
    for (uint32_t m = std::max(std::max(w, h), 1u); m > 1u; m >>= 1u) ++n;
    return n;
}

uint32_t AlphaMaskStore::levelOffsetWords(uint32_t w, uint32_t h, uint32_t k) {
    uint32_t o = 0u;
    for (uint32_t j = 0u; j < k; ++j) {
        const uint32_t lw = std::max(w >> j, 1u), lh = std::max(h >> j, 1u);
        o += (lw * lh + 31u) / 32u;
    }
    return o;
}

AlphaMaskStore &alphaMaskStore() {
    if (!gStore) gStore = new AlphaMaskStore();
    return *gStore;
}

/// The store if one exists (a scene's teardown after the engine's releaseAlphaMasks must not make a new one).
AlphaMaskStore *alphaMaskStoreIfAny() { return gStore; }

void releaseAlphaMasks() {
    if (!gStore) return;
    gStore->release();
    delete gStore;
    gStore = nullptr;
}

bool alphaTestKeeps(Ogre::CompareFunction cmp, float threshold, float alpha) {
    switch (cmp) {
    case Ogre::CMPF_LESS: return !(threshold < alpha);
    case Ogre::CMPF_LESS_EQUAL: return !(threshold <= alpha);
    case Ogre::CMPF_EQUAL: return !(threshold == alpha);
    case Ogre::CMPF_NOT_EQUAL: return !(threshold != alpha);
    case Ogre::CMPF_GREATER_EQUAL: return !(threshold >= alpha);
    case Ogre::CMPF_GREATER: return !(threshold > alpha);
    case Ogre::CMPF_ALWAYS_FAIL: return false;
    default: return true;
    }
}

int AlphaMaskStore::findIndex(const Ogre::TextureGpu *tex, uint32_t cmp, uint32_t thresholdBits, bool &stale) const {
    // THE IDENTITY: the pointer AND the name and size (a destroyed texture's address can come back as another
    // texture) — and the CONTENT's: the texture's upload generation (ALPHA-MASK-IDENTITY-1), which is the only thing
    // that moves when new texels are written into the same texture.
    stale = false;
    const uint32_t generation = textureUploadGeneration(tex);
    int older = -1;
    for (size_t i = 0; i < mMasks.size(); ++i) {
        const Mask &m = mMasks[i];
        if (m.tex == tex && m.name == tex->getName() && m.texW == tex->getWidth() && m.texH == tex->getHeight() &&
            m.cmp == cmp && m.thresholdBits == thresholdBits) {
            if (m.generation == generation) return int(i);
            older = int(i);
        }
    }
    stale = older >= 0;
    return older;
}

AlphaMaskStore::State AlphaMaskStore::stateOf(const Ogre::TextureGpu *tex, uint32_t cmp, float threshold) const {
    if (!tex) return State::Ready;   // no map: the colour's alpha is the constant answer
    uint32_t bits = 0;
    std::memcpy(&bits, &threshold, sizeof(bits));
    bool stale = false;
    if (findIndex(tex, cmp, bits, stale) >= 0) return State::Ready;
    for (const auto &r : mRefused)
        if (r.first == tex && r.second == tex->getName()) return State::Refused;
    return mFailed ? State::Refused : State::Pending;
}

int AlphaMaskStore::maskFor(Ogre::TextureGpu *tex, uint32_t cmp, float threshold, unsigned &budget, bool &staleOut,
                            bool &refused) {
    staleOut = false;
    refused = false;
    if (!tex) return -1;
    uint32_t thresholdBits = 0;
    std::memcpy(&thresholdBits, &threshold, sizeof(thresholdBits));
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    const uint32_t now = vao ? vao->getFrameCount() : 0u;
    bool stale = false;
    const int found = findIndex(tex, cmp, thresholdBits, stale);
    if (found >= 0) mMasks[size_t(found)].lastUsed = now;
    if (found >= 0 && !stale) return found;
    for (const auto &r : mRefused)
        if (r.first == tex && r.second == tex->getName()) { refused = true; return found; }
    // THE OLD HOLES until a mask can be made - said so: a stale index is not a made mask.
    if (mFailed || !vao) { refused = true; staleOut = stale; return found; }
    if (!budget) { staleOut = stale; return found; }
    if (tex->getResidencyStatus() != Ogre::GpuResidency::Resident || !tex->isDataReady()) {
        staleOut = stale;
        return found;
    }
    if (tex->getInternalTextureType() != Ogre::TextureTypes::Type2D &&
        tex->getInternalTextureType() != Ogre::TextureTypes::Type2DArray) {
        mRefused.push_back({ tex, tex->getName() });   // never a mask: the stock route keeps it
        refused = true;
        return found;
    }
    Ogre::HlmsComputeJob *job = computeJob("Jahshaka/AlphaMask");
    Ogre::HlmsComputeJob *pyramidJob = computeJob("Jahshaka/AlphaMaskPyramid");
    if (!job || !pyramidJob) {
        mFailed = true;
        refused = true;
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: the cut-out masks' compute jobs (Jahshaka/AlphaMask, Jahshaka/AlphaMaskPyramid) are missing - "
            "cut-outs stay on the stock route and out of the rays' reach");
        return found;
    }
    mVao = vao;
    // THE STALE MASK GOES: its slot is re-made below (every table naming it is re-written: the epoch).
    if (stale && found >= 0) {
        if (mMasks[size_t(found)].bits) vao->destroyUavBuffer(mMasks[size_t(found)].bits);
        mMasks.erase(mMasks.begin() + found);
        ++mEpoch;
    }
    Mask m;
    m.tex = tex;
    m.name = tex->getName();
    m.texW = tex->getWidth();
    m.texH = tex->getHeight();
    m.cmp = cmp;
    m.thresholdBits = thresholdBits;
    m.generation = textureUploadGeneration(tex);
    while (m.level + 1u < tex->getNumMipmaps() && std::max(m.texW >> m.level, m.texH >> m.level) > kMaxSide)
        ++m.level;
    m.w = std::max(m.texW >> m.level, 1u);
    m.h = std::max(m.texH >> m.level, 1u);
    m.levels = levelCount(m.w, m.h);
    const uint32_t words = (m.w * m.h + 31u) / 32u;
    // THE PYRAMID'S WORDS after level 0's (levelOffsetWords; 4/3 of level 0).
    const uint32_t allWords = levelOffsetWords(m.w, m.h, m.levels);
    try {
        m.bits = vao->createUavBuffer(allWords, sizeof(uint32_t), 0, nullptr, false);
    } catch (Ogre::Exception &e) {
        Ogre::LogManager::getSingleton().logMessage(std::string("Jahshaka: a cut-out mask's buffer: ") +
                                                    e.getDescription());
        return -1;
    }
    m.address = vao->getBufferDeviceAddress(m.bits);
    if (!m.address) {
        vao->destroyUavBuffer(m.bits);
        mFailed = true;
        refused = true;
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: no buffer device addresses - cut-outs stay on the stock route and out of the rays' reach");
        return -1;
    }
    // THE JOB: the texture (Ogre's solver moves it to a compute read), the buffer as its UAV, the level and the test.
    Ogre::DescriptorSetTexture2::TextureSlot ts = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
    ts.texture = tex;
    job->setTexture(0u, ts);
    Ogre::DescriptorSetUav::BufferSlot us = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    us.buffer = m.bits;
    us.offset = 0;
    us.sizeBytes = 0;
    us.access = Ogre::ResourceAccess::Write;
    job->_setUavBuffer(0u, us);
    Ogre::ShaderParams &sp = job->getShaderParams("default");
    if (Ogre::ShaderParams::Param *a = sp.findParameter("alphaMaskA")) {
        const Ogre::uint32 v[4] = { m.w, m.h, m.level, cmp };
        a->setManualValue(v, 4u);
    }
    if (Ogre::ShaderParams::Param *b = sp.findParameter("alphaMaskB")) {
        const Ogre::uint32 v[4] = { thresholdBits, words, Ogre::uint32(tex->getInternalSliceStart()), 0u };
        b->setManualValue(v, 4u);
    }
    sp.setDirty();
    job->setNumThreadGroups((words + 63u) / 64u, 1u, 1u);
    {
        Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
        job->analyzeBarriers(rt);
        rs->executeResourceTransition(rt);
    }
    root->getHlmsManager()->getComputeHlms()->dispatch(job, nullptr, nullptr);
    job->setTexture(0u, Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
    job->_setUavBuffer(0u, Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
    // THE PYRAMID (ATOM-3 stage 2): one workgroup a coarser level, reading level 0 the job above wrote (Ogre's
    // solver orders the buffer's write before this read-write).
    if (m.levels > 1u) {
        Ogre::DescriptorSetUav::BufferSlot ps = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
        ps.buffer = m.bits;
        ps.offset = 0;
        ps.sizeBytes = 0;
        ps.access = Ogre::ResourceAccess::ReadWrite;
        pyramidJob->_setUavBuffer(0u, ps);
        Ogre::ShaderParams &pp = pyramidJob->getShaderParams("default");
        if (Ogre::ShaderParams::Param *a = pp.findParameter("alphaPyramidA")) {
            const Ogre::uint32 v[4] = { m.w, m.h, m.levels, 0u };
            a->setManualValue(v, 4u);
        }
        pp.setDirty();
        pyramidJob->setNumThreadGroups(m.levels - 1u, 1u, 1u);
        {
            Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
            pyramidJob->analyzeBarriers(rt);
            rs->executeResourceTransition(rt);
        }
        root->getHlmsManager()->getComputeHlms()->dispatch(pyramidJob, nullptr, nullptr);
        pyramidJob->_setUavBuffer(0u, Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
    }
    mDispatched = true;
    --budget;
    ++mMade;
    m.lastUsed = now;
    m.id = mNextId++;
    ++mEpoch;
    mMasks.push_back(std::move(m));
    return int(mMasks.size() - 1u);
}

void AlphaMaskStore::retarget(const std::vector<unsigned long long> &was, const std::vector<unsigned long long> &now) {
    for (Mask &m : mMasks) {
        if (std::binary_search(was.begin(), was.end(), m.id) && m.users) --m.users;
        if (std::binary_search(now.begin(), now.end(), m.id)) ++m.users;
    }
}

void AlphaMaskStore::endFrame(Ogre::RenderSystem *rs) {
    // THE EDGE: the bits were written by a compute job through Ogre's UAV binding, and are READ through their device
    // address — by the ray queries (compute) and the id pass and the caster cut (vertex, fragment) — which the barrier
    // solver cannot see. One memory barrier after the frame's makes, before any of them records.
    if (mDispatched) {
        mDispatched = false;
#if JAH_RAY_QUERY
        Ogre::VulkanRenderSystem *vk = atomVulkanOf(rs);
        Ogre::VulkanDevice *dev = vk ? vk->getVulkanDevice() : nullptr;
        if (dev) {
            dev->_jahRequireGraphics("the cut-out masks' edge");
            dev->_jahRecordQueue().endAllEncoders();
            VkCommandBuffer cmd = dev->_jahRecordQueue().getCurrentCmdBuffer();
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 1, &mb, 0, nullptr, 0, nullptr);
        }
#else
        (void)rs;
#endif
    }
    // FORGET THE IDLE MASKS (no table names one and none asked for one in kIdleFrames) - ONE SWEEP PER kIdleFrames
    // FRAMES on its own clock (decision 15.4 of U5: it walked every mask every frame).
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    if (!vao) return;
    const uint32_t now = vao->getFrameCount();
    if (mSweptAt && uint32_t(now - mSweptAt) < kIdleFrames) return;
    mSweptAt = now ? now : 1u;
    for (size_t i = 0; i < mMasks.size();) {
        if (!mMasks[i].users && uint32_t(now - mMasks[i].lastUsed) > kIdleFrames) {
            if (mMasks[i].bits) vao->destroyUavBuffer(mMasks[i].bits);
            mMasks.erase(mMasks.begin() + ptrdiff_t(i));
            ++mEpoch;
        } else {
            ++i;
        }
    }
}

bool AlphaMaskStore::readBits(int i, std::vector<uint32_t> &out) const {
    out.clear();
    if (i < 0 || size_t(i) >= mMasks.size() || !mMasks[size_t(i)].bits) return false;
    const Mask &m = mMasks[size_t(i)];
    Ogre::UavBufferPacked *b = m.bits;
    const size_t words = b->getNumElements();
    Ogre::AsyncTicketPtr t = b->readRequest(0u, words);
    const auto *src = static_cast<const uint32_t *>(t->map());
    if (src) out.assign(src, src + words);
    t->unmap();
    return !out.empty();
}

void AlphaMaskStore::release() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : mVao;
    for (Mask &m : mMasks)
        if (m.bits && vao) vao->destroyUavBuffer(m.bits);
    mMasks.clear();
    mRefused.clear();
    ++mEpoch;
}

// ---------------------------------------------------------------------------
// THE SCENE'S MASK TABLE (AlphaMasks.h has the layout). Re-written only when its inputs moved: the alpha-tested set
// (the flags word), a material edit (the writer's serial), a mask made or dropped (the store's epoch), a texture's
// texels (the upload serial), or a mask still pending last time. Every SHOWN alpha-tested slot has an entry — the
// Atom route reads the masks of the cut-outs it draws, the ray tier those it traces.
// ---------------------------------------------------------------------------
void OgreScene::MaskFeed::gpuSlotChanged(uint32_t slot, const detail::GpuInstance *now) {
    uint32_t f = 0u;
    if (now) std::memcpy(&f, &now->boundsMax[3], sizeof(f));
    if ((f & kGpuAlphaTested) && (f & kGpuVisible)) {
        if (alpha.insert(slot).second) dirty = true;
        const Node *nd = scene && slot < scene->mItemNodes.size() ? scene->mItemNodes[slot] : nullptr;
        const void *db = nd && nd->item && nd->item->getNumSubItems() ? nd->item->getSubItem(0)->getDatablock() : nullptr;
        if (slot >= dbOf.size()) dbOf.resize(size_t(slot) + 1u, nullptr);
        if (dbOf[slot] != db) { dbOf[slot] = db; dirty = true; }
    } else if (alpha.erase(slot)) {
        dirty = true;
    }
}

void OgreScene::MaskFeed::gpuSceneReset() {
    alpha.clear();
    dbOf.clear();
    dirty = true;
    // THE TABLE IS GONE: the masks it named lose a user (the store keeps them for other scenes until idle).
    if (AlphaMaskStore *st = alphaMaskStoreIfAny()) st->retarget(ids, {});
    ids.clear();
}

OgreScene::MaskFeed::~MaskFeed() {
    if (table) table->removeObserver(this);
}

void OgreScene::updateMaskTable() {
    mMaskStaleServed = 0ull;
    if (!mGpuScene.live()) return;
    // THE FEED JOINS THE TABLE ON ITS FIRST BUILD (primed with every live slot: GpuScene::addObserver).
    if (!mMaskFeed.table) {
        mMaskFeed.scene = this;
        mMaskFeed.table = &mGpuScene;
        mGpuScene.addObserver(&mMaskFeed);
    }
    AlphaMaskStore &store = alphaMaskStore();
    // THE LEVELS A FRAGMENT MAY SAMPLE (the arm atom.maskPyramid: the footprint's, or level 0 alone - the
    // silhouette rows' reference).
    const uint32_t levelCap =
        (!mEngine || mEngine->armRegistry().on(ArmId::AtomMaskPyramid)) ? kMaskAllLevels : 0u;
    const unsigned long long matSerial = mPhoton.materialEditSerial();
    const unsigned long long uploads = textureUploadSerial();
    if (!mMaskFeed.dirty && !mMaskPending && matSerial == mMaskMaterialSerial && store.epoch() == mMaskEpoch &&
        uploads == mMaskUploadSerial && mGpuScene.maskTableSerial() == mMaskTableSerial && levelCap == mMaskLevelCap)
        return;
    mMaskFeed.dirty = false;
    mMaskLevelCap = levelCap;
    // THE SET: the shown alpha-tested slots (the feed's, ascending).
    mMaskSet.assign(mMaskFeed.alpha.begin(), mMaskFeed.alpha.end());
    const std::vector<uint32_t> &set = mMaskSet;
    mMaskMaterialSerial = matSerial;
    mMaskUploadSerial = uploads;
    mMaskPending = false;
    std::vector<unsigned long long> ids;
    std::vector<uint32_t> w;
    std::vector<uint32_t> ready;   // the slots whose every submesh's answer is known now
    unsigned budget = AlphaMaskStore::kPerFrame;
    if (!set.empty()) {
        const uint32_t n = set.back() + 1u;
        w.assign(kMaskHeaderWords + ((n + 3u) & ~3u), kMaskNone);
        w[0] = n;
        w[1] = 0u;
        w[2] = levelCap;
        w[3] = 0u;
        for (const uint32_t slot : set) {
            const Node *nd = slot < mItemNodes.size() ? mItemNodes[slot] : nullptr;
            if (!nd || !nd->item || !nd->item->getNumSubItems()) continue;
            const uint32_t subs =
                uint32_t(std::min<size_t>(nd->item->getNumSubItems(), detail::GpuScene::kSubmeshesPerMesh));
            w[kMaskHeaderWords + slot] = uint32_t(w.size());
            ++w[1];
            const uint32_t head[4] = { subs, 0u, 0u, 0u };
            w.insert(w.end(), head, head + 4);
            bool slotPending = false;
            for (uint32_t g = 0; g < subs; ++g) {
                const Ogre::HlmsDatablock *db = nd->item->getSubItem(g)->getDatablock();
                uint32_t e[kMaskRecordWords] = {};
                uint32_t flags = kMaskConstant | kMaskConstantSolid;
                float uv0[4] = { 1.0f, 1.0f, 0.0f, 0.0f }, uv1[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
                if (db && db->getCreator() && db->getCreator()->getType() == Ogre::HLMS_PBS &&
                    db->getAlphaTest() != Ogre::CMPF_ALWAYS_PASS) {
                    const auto *pbs = static_cast<const Ogre::HlmsPbsDatablock *>(db);
                    const Ogre::CompareFunction cmp = db->getAlphaTest();
                    const float thr = db->getAlphaTestThreshold();
                    Ogre::TextureGpu *tex = pbs->getTexture(Ogre::PBSM_DIFFUSE);
                    if (!tex) {
                        // the raster then tests the background diffuse's alpha (800.PixelShader)
                        flags = kMaskConstant |
                                (alphaTestKeeps(cmp, thr, pbs->getBackgroundDiffuse().a) ? kMaskConstantSolid : 0u);
                    } else {
                        bool stale = false, refused = false;
                        const int mi = store.maskFor(tex, uint32_t(cmp), thr, budget, stale, refused);
                        // RE-WRITTEN NEXT FRAME until it is made - a stale mask served past the budget too; a
                        // refusal waits for the next change.
                        if (stale) { mMaskPending = true; ++mMaskStaleServed; }
                        if (mi < 0) {
                            flags = kMaskPending;
                            if (!refused) mMaskPending = true;
                            slotPending = true;
                        } else {
                            const AlphaMaskStore::Mask &m = store.mask(mi);
                            ids.push_back(m.id);
                            e[0] = uint32_t(m.address & 0xFFFFFFFFu);
                            e[1] = uint32_t(m.address >> 32u);
                            e[2] = (m.w & 0xFFFFu) | ((m.h & 0xFFFFu) << 16u);
                            flags = 0u;
                        }
                    }
                    // THE BASE-MAP UV TRANSFORM (OgreMaterials.cpp: user values 0 and 1)
                    const Ogre::Vector4 a = pbs->getUserValue(0), b = pbs->getUserValue(1);
                    uv0[0] = float(a.x); uv0[1] = float(a.y); uv0[2] = float(a.z); uv0[3] = float(a.w);
                    uv1[0] = float(b.x); uv1[1] = float(b.y); uv1[2] = float(b.z); uv1[3] = float(b.w);
                    // an untouched datablock's user values are zero: the identity then
                    if (uv0[0] == 0.0f && uv0[1] == 0.0f && uv1[0] == 0.0f && uv1[3] == 0.0f) {
                        uv0[0] = uv0[1] = 1.0f; uv0[2] = uv0[3] = 0.0f;
                        uv1[0] = uv1[3] = 1.0f; uv1[1] = uv1[2] = 0.0f;
                    }
                }
                e[3] = flags;
                std::memcpy(e + 4, uv0, sizeof(uv0));
                std::memcpy(e + 8, uv1, sizeof(uv1));
                w.insert(w.end(), e, e + kMaskRecordWords);
            }
            if (!slotPending) ready.push_back(slot);
        }
    }
    // THE MASKS THE TABLE NAMES, counted (after the build: maskFor may have made or dropped some).
    mMaskEpoch = store.epoch();
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    store.retarget(mMaskFeed.ids, ids);
    mMaskFeed.ids.swap(ids);
    mGpuScene.setMaskTable(w);
    mMaskTableSerial = mGpuScene.maskTableSerial();
    // A CUT-OUT WHOSE MASK IS NEWLY KNOWN re-routes (AtomRoute::Pending -> Atom): its slot was composed this frame
    // BEFORE the table asked for the mask, so every slot ready now and not ready at the last build is re-composed
    // next frame (a slot's first build included: the compose may have seen it pending).
    std::sort(ready.begin(), ready.end());
    for (const uint32_t slot : ready)
        if (!std::binary_search(mMaskReady.begin(), mMaskReady.end(), slot) && slot < mItemNodes.size() &&
            mItemNodes[slot])
            markGpuSlotDirty(*mItemNodes[slot]);
    mMaskReady.swap(ready);
}

// ---------------------------------------------------------------------------
// GpuScene's half: the table's buffer (re-created when its words moved; the old one Ogre destroys late).
// ---------------------------------------------------------------------------
bool GpuScene::setMaskTable(const std::vector<uint32_t> &words) {
    if (words == mMaskWords && (mMaskTable || words.empty())) return false;
    if (mMaskTable && mVao) mVao->destroyUavBuffer(mMaskTable);
    mMaskTable = nullptr;
    mMaskAddress = 0u;
    mMaskWords = words;
    ++mMaskSerial;
    if (words.empty() || !mVao) return true;
    try {
        mMaskTable = mVao->createUavBuffer(words.size(), sizeof(uint32_t), 0,
                                           const_cast<uint32_t *>(words.data()), false);
        mMaskAddress = mVao->getBufferDeviceAddress(mMaskTable);
    } catch (Ogre::Exception &e) {
        Ogre::LogManager::getSingleton().logMessage(std::string("Jahshaka: the cut-out mask table: ") +
                                                    e.getDescription());
        mMaskTable = nullptr;
        mMaskAddress = 0u;
    }
    return true;
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
