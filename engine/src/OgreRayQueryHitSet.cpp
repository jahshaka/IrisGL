// THE HIT SET (PHOTON-7 stage 10, 10-2a): the ONE layout, write and order of every input the hit read takes, for every
// ray job that reads its hits (ScreenProbeGather.h, HitSetSources says what the set is and who binds it). Before it the
// reflection and the gather each wrote the same thirty-three bindings inline, and the cards' gather would have been a
// third copy (the lead's ruling: never a fourth).
#ifdef JAH_RAY_QUERY

#include "ScreenProbeGather.h"

#include <algorithm>
#include <cstring>

#include <OgreDescriptorSetTexture.h>
#include <OgreGpuProgram.h>
#include <OgreDescriptorSetUav.h>
#include <OgreResourceTransition.h>
#include <Vao/OgreUavBufferPacked.h>

#include "OgreVulkanTextureGpu.h"
#include "Vao/OgreVulkanBufferInterface.h"

namespace jahshaka {
namespace engine {

void hitSetLayout(const HitSetSlots &s, VkDescriptorSetLayoutBinding *b) {
    const auto put = [b](uint32_t binding, VkDescriptorType type, uint32_t count) {
        b[binding].binding = binding;
        b[binding].descriptorType = type;
        b[binding].descriptorCount = count;
        b[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    };
    put(s.tlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u);
    for (unsigned k = 0; k < kGatherVoxelKinds; ++k)
        put(s.voxel[k], VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kGatherMaxCascades);
    put(s.sky, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u);
    put(s.card[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u);
    put(s.card[1], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u);
    put(s.card[2], VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u);
    put(s.card[3], VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u);
    for (unsigned i = 0; i < SurfaceCache::kViewLayers; ++i)
        put(s.cardView + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u);
    put(s.geom[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u);
    put(s.geom[1], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u);
    put(s.hit[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u);
    put(s.hit[1], VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u);
    put(s.hit[2], VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u);
    put(s.hit[3], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u);
    put(s.pool, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u);
    put(s.cloud, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u);
}

bool HitSetWrite::writeVolumes(GatherHost &host, VkDescriptorSet set, const uint32_t (&binding)[kGatherVoxelKinds],
                               const HitSetSources &src, VkWriteDescriptorSet *w, std::string &err) {
    VkImageView dummyCube = VK_NULL_HANDLE, dummyVolume = VK_NULL_HANDLE, dummyFlat = VK_NULL_HANDLE;
    VkBuffer dummyStorage = VK_NULL_HANDLE;
    if (!host.gatherDummies(dummyCube, dummyVolume, dummyFlat, dummyStorage, err)) return false;
    const VkSampler linear = host.gatherLinearSampler();
    // every kind for every slot the shaders index: past the chain's length the last cascade's (a slot with no chain at
    // all the black stand-in, never read: the cascade count is 0). Views uncached and retired, as `write`'s.
    for (unsigned k = 0; k < kGatherVoxelKinds; ++k) {
        if (binding[k] == kNoBinding) continue;
        for (unsigned c = 0; c < kGatherMaxCascades; ++c) {
            const unsigned from = c < src.cascadeCount ? c : (src.cascadeCount ? src.cascadeCount - 1u : 0u);
            Ogre::TextureGpu *t = src.cascadeCount ? src.voxel[from][k] : nullptr;
            VkImageView view = dummyVolume;
            if (t) {
                Ogre::DescriptorSetTexture2::TextureSlot slot = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
                slot.texture = t;
                view = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
                host.gatherRetireView(view);
            }
            mVolumes[k][c].sampler = linear;
            mVolumes[k][c].imageView = view;
            mVolumes[k][c].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (!mVolumes[k][c].imageView) { err = "a voxel view is null"; return false; }
        }
        VkWriteDescriptorSet &e = w[binding[k]];
        e = VkWriteDescriptorSet{};
        e.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        e.dstSet = set;
        e.dstBinding = binding[k];
        e.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        e.descriptorCount = kGatherMaxCascades;
        e.pImageInfo = mVolumes[k];
    }
    return true;
}

bool HitSetWrite::write(GatherHost &host, VkDescriptorSet set, const HitSetSlots &s, const HitSetSources &src,
                        VkBuffer rowOfSlot, VkWriteDescriptorSet *w, std::string &err) {
    // IMAGE VIEWS, UNCACHED AND RETIRED (the reflection's measured trap, OgreRayQuery.cpp recordReflect: Ogre's view
    // cache is keyed by the TextureGpu POINTER, and a texture recreated at the same address answers with the view of
    // the destroyed image): a fresh view a frame, retired into the tier's window.
    const auto sampledView = [&host](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView view = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        host.gatherRetireView(view);
        return view;
    };
    const auto bufferInfo = [](Ogre::UavBufferPacked *b) {
        VkDescriptorBufferInfo info{};
        auto *bi = static_cast<Ogre::VulkanBufferInterface *>(b->getBufferInterface());
        info.buffer = bi->getVboName();
        info.offset = VkDeviceSize(b->_getFinalBufferStart()) * b->getBytesPerElement();
        info.range = b->getTotalSizeBytes();
        return info;
    };
    VkImageView dummyCube = VK_NULL_HANDLE, dummyVolume = VK_NULL_HANDLE, dummyFlat = VK_NULL_HANDLE;
    VkBuffer dummyStorage = VK_NULL_HANDLE;
    if (!host.gatherDummies(dummyCube, dummyVolume, dummyFlat, dummyStorage, err)) return false;
    const auto entry = [&](uint32_t binding, VkDescriptorType type, uint32_t count) -> VkWriteDescriptorSet & {
        VkWriteDescriptorSet &e = w[binding];
        e = VkWriteDescriptorSet{};
        e.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        e.dstSet = set;
        e.dstBinding = binding;
        e.descriptorType = type;
        e.descriptorCount = count;
        return e;
    };

    // THE TLAS
    if (!src.tlas) { err = "the hit set has no TLAS"; return false; }
    mAs = VkWriteDescriptorSetAccelerationStructureKHR{};
    mAs.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    mAs.accelerationStructureCount = 1;
    mAs.pAccelerationStructures = &src.tlas;
    entry(s.tlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u).pNext = &mAs;

    // THE VOLUMES, every kind for every slot the shaders index (writeVolumes)
    const VkSampler linear = host.gatherLinearSampler(), point = host.gatherPointSampler(),
                    wrap = host.gatherWrapSampler();
    if (!writeVolumes(host, set, s.voxel, src, w, err)) return false;
    // THE SKY
    mSky.sampler = linear;
    mSky.imageView = src.sky ? sampledView(src.sky) : dummyCube;
    mSky.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!mSky.imageView) { err = "the sky view is null"; return false; }
    entry(s.sky, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u).pImageInfo = &mSky;

    // THE CARD READ'S FOUR AND ITS VIEW TERM: the cache's own or the stand-ins (zero slots: never indexed).
    const bool cards = src.cardsBound();
    VkDescriptorBufferInfo stand{};
    stand.buffer = dummyStorage;
    stand.range = VK_WHOLE_SIZE;
    mCardBufs[0] = cards ? bufferInfo(src.cardTable) : stand;
    mCardBufs[1] = cards ? bufferInfo(src.cardInstances) : stand;
    Ogre::TextureGpu *const layers[2] = { src.cardDepth, src.cardRadiance };
    for (int i = 0; i < 2; ++i) {
        mCardImgs[i].sampler = point;
        mCardImgs[i].imageView = cards ? sampledView(layers[i]) : dummyFlat;
        mCardImgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (!mCardImgs[i].imageView || !mCardBufs[i].buffer) { err = "a card-read binding is null"; return false; }
        entry(s.card[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u).pBufferInfo = &mCardBufs[i];
        entry(s.card[2 + i], VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u).pImageInfo = &mCardImgs[i];
    }
    for (unsigned i = 0; i < SurfaceCache::kViewLayers; ++i) {
        mCardView[i].sampler = point;
        // (the SunVis layer, the seventh, is RGBA32_UINT: an unsigned stand-in for its usampler)
        mCardView[i].imageView = cards ? sampledView(src.cardView[i])
                                       : i == SurfaceCache::kViewSunVis ? host.gatherDummyUint() : dummyFlat;
        mCardView[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (!mCardView[i].imageView) { err = "a card view-term binding is null"; return false; }
        entry(s.cardView + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u).pImageInfo = &mCardView[i];
    }

    // THE GEOMETRIC NORMAL'S TWO: the job's copy of the per-slot rows and the GPU scene's rows, or the stand-ins.
    const bool geom = rowOfSlot && src.geomRows;
    mGeomBufs[0] = stand;
    mGeomBufs[1] = stand;
    if (geom) {
        mGeomBufs[0].buffer = rowOfSlot;
        mGeomBufs[0].offset = 0u;
        mGeomBufs[0].range = VK_WHOLE_SIZE;
        mGeomBufs[1] = bufferInfo(src.geomRows);
    }
    for (int i = 0; i < 2; ++i)
        entry(s.geom[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u).pBufferInfo = &mGeomBufs[i];

    // THE HIT RECORD'S FOUR: the job's list or the tier's stand-ins - every view real, whatever `on` says (a null view
    // in a set is undefined).
    const HitListBinding &hit = src.hit;
    mHitBufs[0].buffer = hit.instances ? hit.instances : dummyStorage;
    mHitBufs[0].offset = hit.instances ? hit.instancesOffset : 0u;
    mHitBufs[0].range = hit.instances ? hit.instancesRange : VK_WHOLE_SIZE;
    mHitBufs[1].buffer = hit.buf ? hit.buf : dummyStorage;
    mHitBufs[1].offset = hit.buf ? hit.bufOffset : 0u;
    mHitBufs[1].range = hit.buf ? hit.bufRange : VK_WHOLE_SIZE;
    const VkImageView hitViews[2] = { hit.ids, hit.dest };
    for (int i = 0; i < 2; ++i) {
        if (!hitViews[i]) { err = "a hit-list view is null"; return false; }
        mHitImgs[i] = VkDescriptorImageInfo{};
        mHitImgs[i].imageView = hitViews[i];
        mHitImgs[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        entry(s.hit[1 + i], VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u).pImageInfo = &mHitImgs[i];
    }
    entry(s.hit[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u).pBufferInfo = &mHitBufs[0];
    entry(s.hit[3], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u).pBufferInfo = &mHitBufs[1];

    // THE HIT'S OWN SURFACE'S TWO (FAR-SUN-2): the texture pool and the cloud field, or the stand-ins (never read).
    mPool.sampler = wrap;
    mPool.imageView = src.hitPool ? sampledView(src.hitPool) : host.gatherDummyFlatArray();
    mPool.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    mCloud.sampler = wrap;
    mCloud.imageView = src.cloudField ? sampledView(src.cloudField) : dummyFlat;
    mCloud.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!mPool.imageView || !mCloud.imageView || !mPool.sampler) { err = "a hit-surface binding is null"; return false; }
    entry(s.pool, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u).pImageInfo = &mPool;
    entry(s.cloud, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u).pImageInfo = &mCloud;
    return true;
}

float gHitMirrorContOffArm = 0.0f;   // MEASURING ARM (11-2 (7), reverted)
void hitSetParams(const HitSetSources &src, unsigned geomSlots, HitSetParams &out, HitReader reader) {
    out = HitSetParams();
    out.counts[3] = gHitMirrorContOffArm;   // MEASURING ARM (reverted): 1 = no near-mirror continuation in any reader
    out.cards[2] = 1.0f;   // (the readers' demand stamps on: a reader past its demand span turns them off - cards.z)
    // THE CASCADES, every slot a shader indexes: past the chain's length the last cascade's (none: the unit box)
    const unsigned n = src.cascadeCount;
    for (unsigned c = 0; c < kGatherMaxCascades; ++c) {
        const unsigned from = c < n ? c : (n ? n - 1u : 0u);
        for (int k = 0; k < 3; ++k) {
            out.voxelOrigin[c][k] = n ? src.voxelOrigin[from][k] : 0.0f;
            const float size = n ? src.voxelSize[from][k] : 1.0f;
            out.voxelInvSize[c][k] = size > 0.0f ? 1.0f / size : 0.0f;
        }
        out.voxelInvSize[c][3] = n ? src.voxelCell[from] : 1.0f;
    }
    out.counts[0] = float(n);
    for (int k = 0; k < 3; ++k) {
        out.worldMin[k] = src.worldMin[k];
        out.worldMax[k] = src.worldMax[k];
    }
    out.worldMin[3] = src.worldOn ? 1.0f : 0.0f;
    // THE LEVEL RULE (HitReader): a view's by the GI camera (10-2e: the camera and each cascade's radius and band, past
    // the chain's length the last's - never read, the outermost reads by its box); a cache's by its ray's footprint
    // (each cascade's K), whatever the camera
    for (int k = 0; k < 3; ++k) out.levelCam[k] = src.levelCam[k];
    out.levelCam[3] = !n ? 0.0f : reader == HitReader::Cache ? 2.0f : src.levelOn ? 1.0f : 0.0f;
    for (unsigned c = 0; c < kGatherMaxCascades; ++c) {
        const unsigned from = c < n ? c : (n ? n - 1u : 0u);
        out.level[c][0] = n ? src.levelRadius[from] : 0.0f;
        out.level[c][1] = n ? src.levelBand[from] : 0.0f;
        out.level[c][2] = n ? src.levelMaxLod[from] : 0.0f;
    }
    out.counts[1] = src.sky ? 1.0f : 0.0f;
    out.counts[2] = float(geomSlots);
    // the flat environment with no cube (jah_rq_hit.glsl's JAH_SKY_COLOUR contract)
    for (int i = 0; i < 3; ++i) out.skyColour[i] = std::max(0.0f, src.skyColour[i]);
    out.cards[0] = src.cardsBound() ? float(src.cardSlots) : 0.0f;
    out.cards[1] = src.cardsBound() ? float(src.cardRecords) : 0.0f;
    // THE HIT RECORD (PHOTON-HIT-SHADE-1): the job's list and the sun ray of its hits
    out.hitList[0] = float(src.hit.capacity);
    out.hitList[1] = float(src.hit.width);
    out.hitList[2] = float(src.hit.instanceEntries);
    out.hitList[3] = src.hit.on ? 1.0f : 0.0f;
    for (int i = 0; i < 3; ++i) out.hitSun[i] = src.hit.toSun[i];
    out.hitSun[3] = src.hit.sun ? 1.0f : 0.0f;
    out.hitSun2[0] = float(src.hit.sunMask);
    out.hitSun2[1] = 0.0f;   // unused: a near hit's surface term is 0 (the one origin law)
    out.hitSun2[2] = src.hit.sunRange;
    out.hitSun2[3] = src.hit.farLift;
    // the two tables' device addresses, bit-copied
    const uint32_t a[4] = { uint32_t(src.alphaTable & 0xFFFFFFFFu), uint32_t(src.alphaTable >> 32u),
                            uint32_t(src.hitTable & 0xFFFFFFFFu), uint32_t(src.hitTable >> 32u) };
    std::memcpy(out.tables, a, sizeof(a));
    out.surface[0] = src.hitPool ? 1.0f : 0.0f;
    out.surface[1] = src.cloudField ? 1.0f : 0.0f;
    const uint32_t c[2] = { uint32_t(src.tierCounters & 0xFFFFFFFFu), uint32_t(src.tierCounters >> 32u) };
    std::memcpy(&out.surface[2], c, sizeof(c));
    std::memcpy(out.cloudMap, src.cloudMap, sizeof(out.cloudMap));
    std::memcpy(out.cloudSun, src.cloudSun, sizeof(out.cloudSun));
}

void hitSetTransitions(Ogre::BarrierSolver &solver, Ogre::ResourceTransitionArray &trans, const HitSetSources &src) {
    const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
    const auto read = [&](Ogre::TextureGpu *t) {
        if (t)
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                     computeStage);
    };
    for (unsigned c = 0; c < src.cascadeCount; ++c)
        for (unsigned k = 0; k < kGatherVoxelKinds; ++k) read(src.voxel[c][k]);
    read(src.sky);
    read(src.hitPool);
    read(src.cloudField);
    // THE CARD READ'S INPUTS: the Radiance layer the CardLight job wrote (a UAV) and the Depth layer the capture copied
    // into, read as textures; the view term's seven, read as textures; the card table read, the instance table READ
    // AND WRITTEN (the hits' demand stamps, PHOTON-7 stage 5, 5-2).
    if (src.cardsBound()) {
        read(src.cardDepth);
        read(src.cardRadiance);
        for (Ogre::TextureGpu *t : src.cardView) read(t);
        solver.resolveTransition(trans, src.cardTable, Ogre::ResourceAccess::Read, computeStage);
        solver.resolveTransition(trans, src.cardInstances, Ogre::ResourceAccess::ReadWrite, computeStage);
    }
    if (src.geomRows) solver.resolveTransition(trans, src.geomRows, Ogre::ResourceAccess::Read, computeStage);
}

}   // namespace engine
}   // namespace jahshaka

#endif   // JAH_RAY_QUERY
