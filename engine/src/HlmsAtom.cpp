// HlmsAtom — see HlmsAtom.h. The shape is the stage-0 spike's
// (spikes/atom-stage0/src/HlmsAtomSpike.cpp), which is the shape of Ogre's own
// HlmsTerra (Samples/2.0/Tutorials/Tutorial_Terrain/src/Terra/Hlms/OgreHlmsTerra.cpp):
// the constructor names the type and reserves the slots, getDefaultPaths makes
// PBS's piece folders our LIBRARIES, notifyPropertiesMergedPreGenerationStep names
// the reserved registers, setupRootLayout widens the read-only range over them, and
// fillBuffersForV2 calls PBS's and then binds what is ours. Terra copies the whole of
// PBS's fillBuffersFor (300 lines); this does not have to.
#include "HlmsAtom.h"

#include <CommandBuffer/OgreCbShaderBuffer.h>
#include <CommandBuffer/OgreCbTexture.h>
#include <CommandBuffer/OgreCommandBuffer.h>
#include <OgreHlmsJson.h>
#include <OgreCamera.h>
#include <OgreHlmsManager.h>
#include <OgreResourceTransition.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreLogManager.h>
#include <OgreRenderQueue.h>
#include <OgreRenderSystem.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreRootLayout.h>
#include <OgreCamera.h>
#include <OgreSceneManager.h>
#include <OgreStagingTexture.h>
#include <OgreTextureBox.h>
#include <OgreTextureGpuManager.h>
#include <OgreTextureGpu.h>
#include <Vao/OgreConstBufferPacked.h>
#include <Vao/OgreIndexBufferPacked.h>
#include <Vao/OgreReadOnlyBufferPacked.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>
#include <Vao/OgreVertexArrayObject.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace jahshaka {
namespace engine {
namespace detail {

const char *const HlmsAtom::kTypeName = "Atom";

HlmsAtom::HlmsAtom(Ogre::Archive *dataFolder, Ogre::ArchiveVec *libraryFolders)
    : Ogre::HlmsPbs(dataFolder, libraryFolders) {
    // Terra's own three lines (OgreHlmsTerra.cpp:87-89).
    mType = kType;
    mTypeName = kTypeName;
    mTypeNameStr = kTypeName;
    // PBS reserves ONE tex-buffer slot, the vertex shader's worldMatBuf. The decode
    // needs four more (the instance table, the level table, the geometry rows, the
    // bucket table) and one texture (the id image). PBS's own slot accounting honours
    // both counters (OgreHlmsPbs.cpp: the registers in notifyPropertiesMergedPre-
    // GenerationStep, the binds in fillBuffersFor), so nothing of PBS's moves onto them.
    mReservedTexBufferSlots = kReservedBufSlots;
    mReservedTexSlots = 1u;
}

HlmsAtom::~HlmsAtom() {
    destroyDecodeTwins();
    if (mPointSampler && mHlmsManager) mHlmsManager->destroySamplerblock(mPointSampler);
    mPointSampler = nullptr;
    if (mEmptyBuf && mVaoManager) mVaoManager->destroyReadOnlyBuffer(mEmptyBuf);
    mEmptyBuf = nullptr;
    if (mEmptyIds && mRenderSystem && mRenderSystem->getTextureGpuManager())
        mRenderSystem->getTextureGpuManager()->destroyTexture(mEmptyIds);
    mEmptyIds = nullptr;
}

/// THE STAND-INS a draw without a source binds: a 1x1 id image holding "nothing
/// covers this pixel" and a zero table holding ONE WHOLE ENTRY of the largest table
/// (a GpuInstance, 160 bytes) — the decode clamps every index into its table and
/// reads unconditionally, so a stand-in must be a table it can read in range. Zeros
/// name no bucket (a twin's entry is its slot PLUS ONE), so the draw shades nothing.
/// Created on the first draw that needs them.
void HlmsAtom::ensureStandIns() {
    if (!mVaoManager || !mRenderSystem) return;
    if (!mEmptyBuf) {
        uint32_t zeros[40] = {};
        mEmptyBuf = mVaoManager->createReadOnlyBuffer(Ogre::PFG_R32_UINT, sizeof(zeros), Ogre::BT_IMMUTABLE,
                                                      zeros, false);
    }
    if (!mEmptyIds) {
        Ogre::TextureGpuManager *tm = mRenderSystem->getTextureGpuManager();
        // A ManualTexture goes Resident by an immediate transition and is never
        // notifyDataIsReady'd (DOCS/traps/ENGINE.md).
        mEmptyIds = tm->createTexture("jahAtomEmptyIds", Ogre::GpuPageOutStrategy::Discard,
                                      Ogre::TextureFlags::ManualTexture, Ogre::TextureTypes::Type2D);
        mEmptyIds->setResolution(1u, 1u);
        mEmptyIds->setPixelFormat(Ogre::PFG_RG32_UINT);
        mEmptyIds->setNumMipmaps(1u);
        mEmptyIds->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        Ogre::StagingTexture *st = tm->getStagingTexture(1u, 1u, 1u, 1u, Ogre::PFG_RG32_UINT);
        st->startMapRegion();
        Ogre::TextureBox box = st->mapRegion(1u, 1u, 1u, 1u, Ogre::PFG_RG32_UINT);
        auto *px = reinterpret_cast<uint32_t *>(box.at(0, 0, 0));
        px[0] = AtomId::kEmpty;
        px[1] = 0u;
        st->stopMapRegion();
        st->upload(box, mEmptyIds, 0, nullptr, nullptr, true);
        tm->removeStagingTexture(st);
    }
}

void HlmsAtom::getDefaultPaths(Ogre::String &outDataFolderPath,
                               Ogre::StringVector &outLibraryFoldersPaths) {
    // PBS's list FIRST, verbatim, so the library order — and therefore which piece
    // wins a redefinition — is PBS's; then the engine's own pieces, LAST among the
    // lighting libraries exactly as registerHlms gives them to PBS (the fog piece
    // redefines one of Pbs/Any/Atmosphere and a redefinition only works after the
    // original was collected); then ours.
    Ogre::String pbsData;
    Ogre::HlmsPbs::getDefaultPaths(pbsData, outLibraryFoldersPaths);
    outLibraryFoldersPaths.push_back("Hlms/Jahshaka");
    outLibraryFoldersPaths.push_back("Hlms/Atom/Any");
    // PBS's data folder is Hlms/Pbs/<syntax>; ours is its sibling.
    const Ogre::String::size_type slash = pbsData.find_last_of('/');
    const Ogre::String syntax =
        slash == Ogre::String::npos ? Ogre::String("GLSL") : pbsData.substr(slash + 1u);
    outDataFolderPath = "Hlms/Atom/" + syntax;
}

void HlmsAtom::setDecodeSource(const DecodeSource &src) {
    mSource = src;
    if (!mPointSampler && mHlmsManager) {
        // ACQUIRED ONCE PER MANAGER (GATHER-0's trap: getSamplerblock takes a
        // reference and HlmsSamplerblock::mRefCount is a uint16).
        Ogre::HlmsSamplerblock ref;
        ref.setFiltering(Ogre::TFO_NONE);
        ref.setAddressingMode(Ogre::TAM_CLAMP);
        mPointSampler = mHlmsManager->getSamplerblock(ref);
    }
}

uint32_t HlmsAtom::materialWordOf(const Ogre::HlmsDatablock *db) {
    if (!db || !db->getCreator() || db->getCreator()->getType() != Ogre::HLMS_PBS)
        return kNoMaterialWord;
    const auto *pbsDb = static_cast<const Ogre::HlmsPbsDatablock *>(db);
    auto *pbs = static_cast<Ogre::HlmsPbs *>(db->getCreator());
    if (!pbsDb->getAssignedPool()) return kNoMaterialWord;
    // ConstBufferPool::getPoolIndex is public and names the pool "unique per hash";
    // every PBS datablock asks for hash 0 (HlmsPbsDatablock::createBuffers), so the
    // index is unique across the whole PBS instance.
    const size_t pool =
        pbs->getPoolIndex(const_cast<Ogre::HlmsPbsDatablock *>(pbsDb));
    return (uint32_t(pool) << 16u) | (pbsDb->getAssignedSlot() & 0xFFFFu);
}

// ---------------------------------------------------------------------------
// THE DECODE TWIN
// ---------------------------------------------------------------------------
Ogre::HlmsPbsDatablock *HlmsAtom::decodeTwinFor(Ogre::HlmsPbsDatablock *pbs, std::string &err) {
    if (!pbs || !pbs->getCreator() || pbs->getCreator()->getType() != Ogre::HLMS_PBS) {
        err = "decodeTwinFor: not an HlmsPbs datablock";
        return nullptr;
    }
    if (auto it = mTwinOfPbs.find(pbs); it != mTwinOfPbs.end()) return it->second;
    for (size_t s = 0; s < Ogre::CustomPieceStage::NumCustomPieceStages; ++s) {
        if (pbs->getCustomPieceFileIdHash(Ogre::CustomPieceStage::CustomPieceStage(s))) {
            err = "decodeTwinFor: the datablock carries a per-datablock custom piece; it stays "
                  "on stock HlmsPbs (D1 section 1)";
            return nullptr;
        }
    }
    const uint32_t word = materialWordOf(pbs);
    if (word == kNoMaterialWord) {
        err = "decodeTwinFor: the datablock has no const-buffer slot";
        return nullptr;
    }

    // OGRE'S OWN SERIALISER IS THE COPY. HlmsJson writes one datablock under its
    // Hlms's type name; the same text under OUR type name loads through the loader
    // PBS's JSON half registers (HlmsAtom inherits HlmsPbs::_loadJson), into this
    // Hlms. Every permutation-relevant field travels — the textures (retrieved by
    // name, never reloaded), their samplers and uv sets, workflow, BRDF, transparency,
    // the maps — without a field list of ours to fall out of date.
    const Ogre::String *pbsName = pbs->getNameStr();
    if (!pbsName) {
        err = "decodeTwinFor: the datablock has no name to serialise";
        return nullptr;
    }
    const Ogre::String twinName = "jahAtomTwin/" + std::to_string(++mTwinSerial) + "/" + *pbsName;
    Ogre::String json;
    Ogre::HlmsJson hj(mHlmsManager, nullptr);
    hj.saveMaterial(pbs, json, "");
    const Ogre::String typeKey = "\"" + pbs->getCreator()->getTypeNameStr() + "\" : ";
    const Ogre::String::size_type at = json.find(typeKey);
    const Ogre::String nameKey = "\"" + *pbsName + "\" :";
    const Ogre::String::size_type nameAt =
        at == Ogre::String::npos ? Ogre::String::npos : json.find(nameKey, at + typeKey.size());
    if (at == Ogre::String::npos || nameAt == Ogre::String::npos) {
        err = "decodeTwinFor: unexpected JSON shape from HlmsJson::saveMaterial";
        return nullptr;
    }
    json.replace(nameAt, nameKey.size(), "\"" + twinName + "\" :");
    json.replace(at, typeKey.size(), "\"" + Ogre::String(kTypeName) + "\" : ");
    try {
        hj.loadMaterials("jahAtomTwin", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                         json.c_str(), "");
    } catch (Ogre::Exception &e) {
        err = "decodeTwinFor: " + e.getFullDescription();
        return nullptr;
    }
    auto *twin = static_cast<Ogre::HlmsPbsDatablock *>(getDatablock(Ogre::IdString(twinName)));
    if (!twin) {
        err = "decodeTwinFor: the twin did not load";
        return nullptr;
    }
    // THE DECODE'S MACROBLOCK: a full-screen triangle neither tests nor writes the
    // depth the id pass owns, and its winding is not the scene's (CULL_NONE — with
    // Ogre's default the decode draws NOTHING and a mean test reads it as "most
    // pixels differ", FINDINGS §2.4 (3)).
    Ogre::HlmsMacroblock macro;
    macro.mDepthCheck = false;
    macro.mDepthWrite = false;
    macro.mCullMode = Ogre::CULL_NONE;
    twin->setMacroblock(macro);
    // The decode never casts: its shadow-caster permutation is never requested.

    Twin t;
    t.pbs = pbs;
    t.twin = twin;
    t.pbsWord = word;
    mTwins[twin] = t;
    mTwinOfPbs[pbs] = twin;
    mBucketDirty = true;
    return twin;
}

void HlmsAtom::forgetDecodeTwinOf(const Ogre::HlmsDatablock *pbs) {
    auto it = mTwinOfPbs.find(pbs);
    if (it == mTwinOfPbs.end()) return;
    Ogre::HlmsPbsDatablock *twin = it->second;
    mTwinOfPbs.erase(it);
    mTwins.erase(twin);
    // A decode draw may still carry the twin; the owner detaches its renderables
    // before it destroys materials (the grid's arm does), and Ogre asserts on a
    // datablock with linked renderables.
    if (twin && twin->getNameStr()) destroyDatablock(twin->getName());
    mBucketDirty = true;
}

void forgetDecodeTwinOf(const Ogre::HlmsDatablock *pbs) {
    if (!pbs || !pbs->getCreator() || pbs->getCreator()->getType() != Ogre::HLMS_PBS) return;
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
    if (auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr)
        atom->forgetDecodeTwinOf(pbs);
}

void HlmsAtom::destroyDecodeTwins() {
    for (auto &kv : mTwins) {
        if (kv.second.twin && kv.second.twin->getNameStr())
            destroyDatablock(kv.second.twin->getName());
    }
    mTwins.clear();
    mTwinOfPbs.clear();
    if (mBucketBuf && mVaoManager) mVaoManager->destroyReadOnlyBuffer(mBucketBuf);
    mBucketBuf = nullptr;
    mBucketMirror.clear();
    mBucketDirty = true;
}

/// THE BUCKET TABLE: one uint per PBS (pool, slot) — 1 + the twin's slot in THIS
/// Hlms's pool, 0 where no twin serves that material. Rewritten only when the twin
/// set changes.
void HlmsAtom::uploadBucketTable() {
    if (!mBucketDirty || !mVaoManager) return;
    mBucketDirty = false;
    uint32_t pools = 1u;
    for (const auto &kv : mTwins) pools = std::max(pools, (kv.second.pbsWord >> 16u) + 1u);
    const uint32_t perPool = mSlotsPerPool;
    std::vector<uint32_t> table(size_t(pools) * perPool, 0u);
    for (const auto &kv : mTwins) {
        const uint32_t pool = kv.second.pbsWord >> 16u, slot = kv.second.pbsWord & 0xFFFFu;
        if (slot < perPool) table[size_t(pool) * perPool + slot] = kv.second.twin->getAssignedSlot() + 1u;
    }
    // A read-only buffer's ELEMENT is one byte (VaoManager::createReadOnlyBuffer
    // hands the Vulkan implementation bytesPerElement = 1), so sizes and upload
    // ranges below are in bytes.
    const size_t bytes = table.size() * sizeof(uint32_t);
    if (mBucketBuf && mBucketBuf->getNumElements() < bytes) {
        mVaoManager->destroyReadOnlyBuffer(mBucketBuf);
        mBucketBuf = nullptr;
    }
    if (!mBucketBuf)
        mBucketBuf = mVaoManager->createReadOnlyBuffer(Ogre::PFG_R32_UINT, bytes, Ogre::BT_DEFAULT,
                                                       nullptr, false);
    mBucketBuf->upload(table.data(), 0u, bytes);
    mBucketMirror.swap(table);
}

// ---------------------------------------------------------------------------
// THE PASS: the first read of a frame asks PBS what it holds (tellEveryHlms, once per frame).
// ---------------------------------------------------------------------------
void HlmsAtom::analyzeBarriers(Ogre::BarrierSolver &barrierSolver,
                               Ogre::ResourceTransitionArray &resourceTransitions,
                               Ogre::Camera *renderingCamera, const bool bCasterPass) {
    // NOTHING TO DECODE, NOTHING TO DO: every registered Hlms is asked this for
    // every scene pass of the frame, and this host draws only through its twins.
    if (mTwins.empty()) return;
    // THE FIRST THING A PASS ASKS OF US READS THE VOXEL AND FIELD TEXTURES — so the
    // relay and THIS PASS'S SCENE'S arms (bindSceneGi) come BEFORE, never after: an
    // arm from the previous pass's scene is the wrong one, and may be deleted.
    tellEveryHlms(mHlmsManager);
    bindSceneGi(this, renderingCamera ? renderingCamera->getSceneManager() : nullptr);
    Ogre::HlmsPbs::analyzeBarriers(barrierSolver, resourceTransitions, renderingCamera, bCasterPass);
    if (bCasterPass) return;
    // WHAT THE DECODE READS, declared to Ogre's solver like every other pass input:
    // the id image as a texture, the GPU scene's tables as read-only buffers in the
    // pixel stage (their writes are the frame's staging copies — without this the
    // read is an unordered hazard against them).
    if (mSource.ids)
        barrierSolver.resolveTransition(resourceTransitions, mSource.ids, Ogre::ResourceLayout::Texture,
                                        Ogre::ResourceAccess::Read, 1u << Ogre::PixelShader);
    for (Ogre::UavBufferPacked *b : { mSource.instances, mSource.levels, mSource.geomRows })
        if (b)
            barrierSolver.resolveTransition(resourceTransitions, b, Ogre::ResourceAccess::Read,
                                            1u << Ogre::PixelShader);
}

Ogre::HlmsCache HlmsAtom::preparePassHash(const Ogre::CompositorShadowNode *shadowNode,
                                          bool casterPass, bool dualParaboloid,
                                          Ogre::SceneManager *sceneManager) {
    // RenderQueue::renderPassPrepare asks EVERY registered Hlms for its pass hash on
    // EVERY scene pass (OgreRenderQueue.cpp), whether or not anything of its draws.
    // PBS's preparePassHash is the expensive half of a pass's CPU prepare (the pass
    // buffer, the lights, the shadow maps), and this host draws only through its
    // decode twins — so while there are none it costs the frame nothing: the cache
    // it returns is never looked up, because no renderable of this type is queued.
    if (mTwins.empty()) {
        mPassSkipped = true;
        return Ogre::HlmsCache();
    }
    mPassSkipped = false;
    tellEveryHlms(mHlmsManager);
    bindSceneGi(this, sceneManager);
    uploadBucketTable();
    return Ogre::HlmsPbs::preparePassHash(shadowNode, casterPass, dualParaboloid, sceneManager);
}

void HlmsAtom::postCommandBufferExecution(Ogre::CommandBuffer *commandBuffer) {
    if (mPassSkipped) {
        Ogre::HlmsBufferManager::postCommandBufferExecution(commandBuffer);
        return;
    }
    Ogre::HlmsPbs::postCommandBufferExecution(commandBuffer);
}

Ogre::Hlms::PropertiesMergeStatus HlmsAtom::notifyPropertiesMergedPreGenerationStep(
    size_t tid, Ogre::PiecesMap *inOutPieces) {
    const PropertiesMergeStatus status =
        Ogre::HlmsPbs::notifyPropertiesMergedPreGenerationStep(tid, inOutPieces);
    if (status == PropertiesMergeStatusError) return status;
    // THE RESERVED REGISTERS, named exactly where Terra names its own
    // (OgreHlmsTerra.cpp:429-438), and derived only from properties already in the
    // merged set, so the shader cache still keys on the same thing.
    setProperty(tid, "atomInstanceBuf", kInstanceBufSlot);
    setProperty(tid, "atomLevelBuf", kLevelBufSlot);
    setProperty(tid, "atomGeomRowBuf", kGeomRowBufSlot);
    setProperty(tid, "atomBucketBuf", kBucketBufSlot);
    setProperty(tid, "atomSlotsPerPool", Ogre::int32(mSlotsPerPool));
    Ogre::int32 texSlotsStart = kReservedBufSlots;
    if (getProperty(tid, Ogre::HlmsBaseProp::ForwardPlus))
        texSlotsStart = getProperty(tid, "f3dGrid") + 1;
    if (!getProperty(tid, Ogre::HlmsBaseProp::ShadowCaster))
        setTextureReg(tid, Ogre::PixelShader, "atomIdTex", texSlotsStart);
    return status;
}

void HlmsAtom::setupRootLayout(Ogre::RootLayout &rootLayout, size_t tid) {
    Ogre::HlmsPbs::setupRootLayout(rootLayout, tid);
    // PBS's range covers [0, worldMatBuf + 1) and, with Forward Clustered on,
    // [0, f3dLightList + 1) — which already contains our reserved slots, because the
    // reservation pushed f3dLightList above them. Without Forward Clustered it would
    // not, so the range is widened explicitly: a slot inside the root layout but
    // never bound is an undefined descriptor.
    Ogre::DescBindingRange *ranges = rootLayout.mDescBindingRanges[0];
    if (ranges[Ogre::DescBindingTypes::ReadOnlyBuffer].end < kReservedBufSlots) {
        ranges[Ogre::DescBindingTypes::ReadOnlyBuffer].start = 0u;
        ranges[Ogre::DescBindingTypes::ReadOnlyBuffer].end = kReservedBufSlots;
        if (ranges[Ogre::DescBindingTypes::Texture].start < kReservedBufSlots)
            ranges[Ogre::DescBindingTypes::Texture].start = kReservedBufSlots;
        if (ranges[Ogre::DescBindingTypes::Sampler].start < ranges[Ogre::DescBindingTypes::Texture].start)
            ranges[Ogre::DescBindingTypes::Sampler].start = ranges[Ogre::DescBindingTypes::Texture].start;
    }
}

Ogre::uint32 HlmsAtom::fillBuffersForV2(const Ogre::HlmsCache *cache,
                                        const Ogre::QueuedRenderable &queuedRenderable,
                                        bool casterPass, Ogre::uint32 lastCacheHash,
                                        Ogre::CommandBuffer *commandBuffer) {
    const Ogre::uint32 ret = Ogre::HlmsPbs::fillBuffersForV2(cache, queuedRenderable, casterPass,
                                                             lastCacheHash, commandBuffer);
    if (casterPass) return ret;

    // THE PASS RESOURCES, on the Hlms-type switch exactly like PBS's own
    // (OgreHlmsPbs.cpp fillBuffersFor), in the slots reserved for them. The GPU
    // scene's tables are bound as READ-ONLY VIEWS of their UAV buffers, fetched here
    // and never cached: a table is re-created when it grows.
    //
    // EVERY SLOT IS BOUND, ALWAYS — a real source or the host's empty stand-in. A slot
    // this draw did not bind still holds whatever the previous pass left there
    // (measured on the RTX: an RGBA8 texture read as the id image, garbage ids, a GPU
    // hang), and a wrong device address in a shader is never a validation error.
    if (OGRE_EXTRACT_HLMS_TYPE_FROM_CACHE_HASH(lastCacheHash) != mType) {
        const bool complete = mSource.ids && mSource.instances && mSource.levels && mSource.geomRows;
        if (!complete) {
            ensureStandIns();
            if (mSourcelessDraws++ == 0u) {
                const Ogre::Camera *cam =
                    queuedRenderable.movableObject && queuedRenderable.movableObject->_getManager()
                        ? queuedRenderable.movableObject->_getManager()->getCamerasInProgress().renderingCamera
                        : nullptr;
                Ogre::LogManager::getSingleton().logMessage(
                    "HlmsAtom: a decode draw was recorded with no decode source (camera '" +
                        (cam ? cam->getName() : Ogre::String("?")) +
                        "'); it binds the empty stand-ins and shades nothing. Logged once.",
                    Ogre::LML_CRITICAL);
            }
        }
        auto roView = [this](Ogre::UavBufferPacked *b) -> Ogre::ReadOnlyBufferPacked * {
            return b ? b->getAsReadOnlyBufferView() : mEmptyBuf;
        };
        const bool whole = complete;
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kInstanceBufSlot, whole ? roView(mSource.instances) : mEmptyBuf, 0, 0);
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kLevelBufSlot, whole ? roView(mSource.levels) : mEmptyBuf, 0, 0);
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kGeomRowBufSlot, whole ? roView(mSource.geomRows) : mEmptyBuf, 0, 0);
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kBucketBufSlot, (whole && mBucketBuf) ? mBucketBuf : mEmptyBuf, 0, 0);
        Ogre::uint16 texSlot = kReservedBufSlots;
        if (mGridBuffer) texSlot = Ogre::uint16(texSlot + 2u);
        *commandBuffer->addCommand<Ogre::CbTexture>() =
            Ogre::CbTexture(texSlot, whole ? mSource.ids : mEmptyIds, mPointSampler);
    }

    // THE MATERIAL BUFFER IS PBS's POOL, NOT OURS. PBS just bound the twin's own pool
    // at const-buffer slot 1 (`materialArray`); the id buffer's material word names
    // the PBS datablock's slot in the PBS pool, so that pool replaces it — per draw,
    // because each twin may serve a different pool. The twin's own constants are never
    // read. mLastBoundPool is forgotten so the next draw's PBS half rebinds whatever
    // it needs rather than trusting a slot we overwrote.
    const Ogre::HlmsDatablock *db = queuedRenderable.renderable->getDatablock();
    if (auto it = mTwins.find(db); it != mTwins.end() && it->second.pbs &&
                                   it->second.pbs->getAssignedPool()) {
        const Ogre::ConstBufferPool::BufferPool *pool = it->second.pbs->getAssignedPool();
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, 1, pool->materialBuffer, 0,
            Ogre::uint32(pool->materialBuffer->getTotalSizeBytes()));
        mLastBoundPool = nullptr;
    }
    return ret;
}

// ---------------------------------------------------------------------------
// THE FULL-SCREEN TRIANGLE
// ---------------------------------------------------------------------------
namespace {
struct FsVertex {
    float px = 0, py = 0, pz = 0;              // POSITION: clip-space xy
    float nx = 0, ny = 0, nz = 0;              // NORMAL
    float tx = 0, ty = 0, tz = 0, tw = 0;      // TANGENT4
    float u = 0, v = 0;                        // UV0
};
// Clip space, z = 0, w = 1: the decode neither tests nor writes depth (the twin's
// macroblock says so); the id image alone decides coverage.
const FsVertex kFullScreenTri[3] = {
    { -1.0f, -1.0f, 0.0f, 0, 0, 1, 1, 0, 0, 1, 0, 0 },
    { 3.0f, -1.0f, 0.0f, 0, 0, 1, 1, 0, 0, 1, 0, 0 },
    { -1.0f, 3.0f, 0.0f, 0, 0, 1, 1, 0, 0, 1, 0, 0 },
};
}  // namespace

AtomDecodeRenderable::AtomDecodeRenderable(Ogre::IdType id,
                                           Ogre::ObjectMemoryManager *objectMemoryManager,
                                           Ogre::SceneManager *manager, Ogre::uint8 renderQueueId)
    : Ogre::MovableObject(id, objectMemoryManager, manager, renderQueueId), Ogre::Renderable() {
    // An infinite AABB: a full-screen triangle must never be frustum-culled.
    const Ogre::Aabb aabb(Ogre::Aabb::BOX_INFINITE);
    mObjectData.mLocalAabb->setFromAabb(aabb, mObjectData.mIndex);
    mObjectData.mWorldAabb->setFromAabb(aabb, mObjectData.mIndex);
    mObjectData.mLocalRadius[mObjectData.mIndex] = std::numeric_limits<Ogre::Real>::max();
    mObjectData.mWorldRadius[mObjectData.mIndex] = std::numeric_limits<Ogre::Real>::max();

    Ogre::VaoManager *vaoManager = manager->getDestinationRenderSystem()->getVaoManager();
    auto *indices = reinterpret_cast<Ogre::uint16 *>(
        OGRE_MALLOC_SIMD(sizeof(Ogre::uint16) * 3u, Ogre::MEMCATEGORY_GEOMETRY));
    indices[0] = 0;
    indices[1] = 1;
    indices[2] = 2;
    Ogre::IndexBufferPacked *indexBuffer = vaoManager->createIndexBuffer(
        Ogre::IndexBufferPacked::IT_16BIT, 3u, Ogre::BT_IMMUTABLE, indices, true);
    Ogre::VertexElement2Vec elements;
    elements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION));
    elements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_NORMAL));
    elements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT4, Ogre::VES_TANGENT));
    elements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES));
    auto *verts = reinterpret_cast<FsVertex *>(
        OGRE_MALLOC_SIMD(sizeof(kFullScreenTri), Ogre::MEMCATEGORY_GEOMETRY));
    std::memcpy(verts, kFullScreenTri, sizeof(kFullScreenTri));
    Ogre::VertexBufferPacked *vertexBuffer =
        vaoManager->createVertexBuffer(elements, 3u, Ogre::BT_IMMUTABLE, verts, true);
    Ogre::VertexBufferPackedVec vertexBuffers;
    vertexBuffers.push_back(vertexBuffer);
    mVao = vaoManager->createVertexArrayObject(vertexBuffers, indexBuffer, Ogre::OT_TRIANGLE_LIST);
    mVaoPerLod[Ogre::VpNormal].push_back(mVao);
    mVaoPerLod[Ogre::VpShadow].push_back(mVao);
    mRenderables.push_back(this);
    setCastShadows(false);
}

AtomDecodeRenderable::~AtomDecodeRenderable() {
    // THE OWNER DESTROYS THIS WHILE Root — and so the VaoManager — IS ALIVE (the
    // startup/teardown trap: a geometry object outliving Root throws in VaoManager).
    mVaoPerLod[Ogre::VpNormal].clear();
    mVaoPerLod[Ogre::VpShadow].clear();
    if (mVao && mManager) {
        Ogre::VaoManager *vaoManager = mManager->getDestinationRenderSystem()->getVaoManager();
        Ogre::IndexBufferPacked *ib = mVao->getIndexBuffer();
        const Ogre::VertexBufferPackedVec vbs = mVao->getVertexBuffers();
        vaoManager->destroyVertexArrayObject(mVao);
        if (ib) vaoManager->destroyIndexBuffer(ib);
        for (Ogre::VertexBufferPacked *vb : vbs) vaoManager->destroyVertexBuffer(vb);
    }
    mVao = nullptr;
}

const Ogre::String &AtomDecodeRenderable::getMovableType() const { return Ogre::BLANKSTRING; }
const Ogre::LightList &AtomDecodeRenderable::getLights() const { return this->queryLights(); }
void AtomDecodeRenderable::getRenderOperation(Ogre::v1::RenderOperation &, bool) {
    OGRE_EXCEPT(Ogre::Exception::ERR_NOT_IMPLEMENTED, "v2 only", "AtomDecodeRenderable");
}
void AtomDecodeRenderable::getWorldTransforms(Ogre::Matrix4 *) const {
    OGRE_EXCEPT(Ogre::Exception::ERR_NOT_IMPLEMENTED, "v2 only", "AtomDecodeRenderable");
}
bool AtomDecodeRenderable::getCastsShadows() const { return false; }

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
