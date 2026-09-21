// SURFACE-CACHE-0 — THE PHASE-0 DESIGN SPIKE (2026-09-21).
//
// WHAT THIS FILE IS. One mesh instance's six axis-aligned cards — Lumen's
// fallback card shape, the assessment's S3 prototype used as S2's phase 0 —
// captured by hand through a `JahshakaPcc`-shaped compositor workspace into
// resident `Type2DArray` atlases, so that a suite can measure the three numbers
// `SPECS/SURFACE_CACHE_ASSESSMENT.md` §8 records as unverified:
//
//   1. what a capture costs the CPU and the GPU on THIS pin (arm A of §4 turns
//      on that number),
//   2. what reading a card instead of a voxel costs a reflection ray, and
//   3. what the swap does to the picture.
//
// WHAT IT IS NOT: a feature. Nothing here runs unless `Scene::surfaceCardSpike`
// is called, which only `tests/gi/test_gi_surface_cache_spike.cpp` does. No
// residency, no page table, no feedback, no BC encode, no invalidation, no tier
// row, no panel — those are phases 2 and 5 and this lane does not build them.
//
// THE CAPTURE IS A PREPASS, and that is the finding this file carries. Ogre's
// `PrePassCreate` mode already writes two of the four channels a card wants,
// exactly:
//
//     location 0  outNormals          the shading normal, in the CAPTURE
//                                     camera's view space
//     location 1  outShadowRoughness  x = the SUN'S SHADOW TERM sampled from
//                                     the shadow atlas, y = the GGX alpha
//
// ...and computes no lighting at all, which is what makes a capture cheap. The
// other three — albedo, emissive and the card's own depth — are added by
// `JahCardCapture_piece_ps.any` through `custom_ps_output_types` and
// `custom_ps_posExecution`, two hook pieces upstream's own template inserts,
// under a pass property this file's listener sets. NO PATCH TO THE PIN IS
// NEEDED for a five-target capture from a PBS material; the header of that
// media file states the mechanism.
//
// THE SCRATCH SCENE IS THE PIN'S OWN TRICK (`OgreVoxelizedMeshCache.cpp:195`):
// a second `SceneManager` holding ONE `Item` built from the mesh with the
// source item's datablocks copied onto it. It bounds the capture pass's cull to
// one object — which is what a shipped capture would do too, and without it the
// measured CPU cost would be "how big is the scene", not "what does a capture
// cost".
#include "EnginePrivate.h"
#include "SurfaceCardSpike.h"

#include <OgreCamera.h>
#include <OgreItem.h>
#include <OgreMesh2.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSubItem.h>
#include <OgreSubMesh2.h>
#include <OgreTextureGpuManager.h>
#include <OgreAsyncTextureTicket.h>
#include <OgreImage2.h>
#include <OgrePixelFormatGpuUtils.h>
#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jahshaka {
namespace engine {
namespace detail {

namespace {

/// THE CAPTURE PASS'S PROPERTY, and the whole of the "is a capture running"
/// state. Render-thread only, exactly like `FogHlmsListener`'s own maps: the
/// capture workspace is updated inline from `captureRound()`, so the flag is
/// set and cleared around one `_update()` on the one thread that renders.
bool gCapturing = false;

/// The six card axes, in the order the atlases' slices hold them.
const Ogre::Vector3 kAxis[SurfaceCardSpike::kCards] = {
    Ogre::Vector3(1, 0, 0),  Ogre::Vector3(-1, 0, 0), Ogre::Vector3(0, 1, 0),
    Ogre::Vector3(0, -1, 0), Ogre::Vector3(0, 0, 1),  Ogre::Vector3(0, 0, -1)
};

unsigned long long bytesOf(const Ogre::TextureGpu *t) {
    if (!t) return 0ull;
    return static_cast<unsigned long long>(t->getWidth()) * t->getHeight() *
           t->getNumSlices() *
           Ogre::PixelFormatGpuUtils::getBytesPerPixel(t->getPixelFormat());
}

}   // namespace

bool surfaceCardsCapturing() { return gCapturing; }

Ogre::CompositorWorkspace *cardSpikeWorkspace(SurfaceCardSpike *spike) {
    return spike ? spike->workspace() : nullptr;
}

SurfaceCardSpike::~SurfaceCardSpike() { destroyAll(); }

unsigned long long SurfaceCardSpike::vramBytes() const {
    return bytesOf(mNormal) + bytesOf(mShadowRough) + bytesOf(mAlbedo) + bytesOf(mEmissive) +
           bytesOf(mDepth) + bytesOf(mDepthBuffer);
}

void SurfaceCardSpike::destroyAll() {
    mReady = false;
    mEnabled = false;
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root) return;
    Ogre::CompositorManager2 *cm = root->getCompositorManager2();
    if (mWs) { cm->removeWorkspace(mWs); mWs = nullptr; }
    if (!mWsDef.empty() && cm->hasWorkspaceDefinition(mWsDef)) cm->removeWorkspaceDefinition(mWsDef);
    if (!mNodeDef.empty() && cm->hasNodeDefinition(mNodeDef)) cm->removeNodeDefinition(mNodeDef);
    mWsDef.clear(); mNodeDef.clear();
    Ogre::TextureGpuManager *tm = root->getRenderSystem()->getTextureGpuManager();
    for (Ogre::TextureGpu **t : { &mNormal, &mShadowRough, &mAlbedo, &mEmissive, &mDepth,
                                  &mDepthBuffer }) {
        if (*t) { tm->destroyTexture(*t); *t = nullptr; }
    }
    if (mScratch) {
        // The Item dies with its SceneManager; the cameras too.
        root->destroySceneManager(mScratch);
        mScratch = nullptr;
        mItem = nullptr;
        for (unsigned i = 0; i < kCards; ++i) mCam[i] = nullptr;
    }
}

bool SurfaceCardSpike::makeTextures(std::string &err) {
    Ogre::TextureGpuManager *tm =
        Ogre::Root::getSingleton().getRenderSystem()->getTextureGpuManager();
    struct Spec { Ogre::TextureGpu **dst; const char *name; Ogre::PixelFormatGpu fmt; };
    // THE CARD SET'S FIVE LAYERS. The first two are upstream's prepass G-buffer
    // in the formats our own SSR chain uses for it; the last three are ours.
    const Spec specs[] = {
        { &mNormal,      "cardNormal",   Ogre::PFG_RGBA8_UNORM      },
        { &mShadowRough, "cardShadowRgh", Ogre::PFG_RG16_UNORM      },
        { &mAlbedo,      "cardAlbedo",   Ogre::PFG_RGBA8_UNORM      },
        { &mEmissive,    "cardEmissive", Ogre::PFG_RGBA16_FLOAT     },
        { &mDepth,       "cardDepth",    Ogre::PFG_R16_FLOAT        },
    };
    for (const Spec &s : specs) {
        Ogre::TextureGpu *t = tm->createTexture(processUniqueName(s.name),
                                                Ogre::GpuPageOutStrategy::Discard,
                                                Ogre::TextureFlags::RenderToTexture,
                                                Ogre::TextureTypes::Type2DArray);
        t->setResolution(mCardSize, mCardSize, kCards);
        t->setPixelFormat(s.fmt);
        t->setNumMipmaps(1u);
        // RESIDENT, AND RESIDENT FOR GOOD (the 0071 lesson): a render target
        // created and destroyed around each capture is the residency churn that
        // cost this engine an Xid.
        t->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        *s.dst = t;
    }
    // The raster's own depth buffer — a plain 2D, shared by all six passes and
    // cleared by each, exactly as JahshakaPcc's `depthTexture` is.
    mDepthBuffer = tm->createTexture(processUniqueName("cardDepthBuf"),
                                     Ogre::GpuPageOutStrategy::Discard,
                                     Ogre::TextureFlags::RenderToTexture,
                                     Ogre::TextureTypes::Type2D);
    mDepthBuffer->setResolution(mCardSize, mCardSize, 1u);
    mDepthBuffer->setPixelFormat(Ogre::PFG_D32_FLOAT);
    mDepthBuffer->setNumMipmaps(1u);
    mDepthBuffer->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    (void)err;
    return true;
}

bool SurfaceCardSpike::makeWorkspace(std::string &err) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::CompositorManager2 *cm = root->getCompositorManager2();
    mNodeDef = processUniqueName("JahCardCaptureNode");
    mWsDef = processUniqueName("JahCardCaptureWs");
    Ogre::CompositorNodeDef *n = cm->addNodeDefinition(mNodeDef);
    // FIVE COLOUR CHANNELS IN, plus the depth buffer: the node owns nothing and
    // every target is handed to it, so the textures outlive a workspace rebuild
    // and the atlases are allocated once.
    const char *chan[6] = { "cardNormal", "cardShadowRough", "cardAlbedo",
                            "cardEmissive", "cardDepth", "cardDepthBuf" };
    for (unsigned i = 0; i < 6u; ++i)
        n->addTextureSourceName(chan[i], i, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
    // ONE RTV PER SLICE. `RenderTargetViewEntry::slice` is what names the card;
    // the six of them are six target passes of the same node, which is what
    // makes a whole card set ONE workspace update and one pass set-up per card
    // rather than six workspaces.
    n->setNumTargetPass(kCards);
    for (unsigned c = 0; c < kCards; ++c) {
        const std::string rtvName = "cardRtv" + std::to_string(c);
        Ogre::RenderTargetViewDef *rtv = n->addRenderTextureView(rtvName);
        for (unsigned i = 0; i < 5u; ++i) {
            Ogre::RenderTargetViewEntry e;
            e.textureName = chan[i];
            e.slice = c;
            rtv->colourAttachments.push_back(e);
        }
        rtv->depthAttachment.textureName = chan[5];
        rtv->stencilAttachment.textureName = chan[5];
        rtv->preferDepthTexture = true;
        Ogre::CompositorTargetDef *t = n->addTargetPass(rtvName);
        t->setNumPasses(1u);
        auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
        // THE PREPASS IS THE CAPTURE (see the file header): no lighting is
        // computed and the shadow term arrives for free.
        p->mPrePassMode = Ogre::PrePassCreate;
        p->mCameraName = Ogre::IdString(mCam[c]->getName());
        // THE SHADOW ATLAS. Named so the capture's `fShadow` is a real shadow
        // term rather than 1 everywhere — the brief's "direct light from the
        // sun through the shadow atlas". `recalculate` for the same reason
        // LocalCubemaps gives: Ogre cannot tell that this camera looks at the
        // world from somewhere new.
        if (cm->hasShadowNodeDefinition(OgreView::kProbeShadowNodeName))
            p->mShadowNode = Ogre::IdString(OgreView::kProbeShadowNodeName);
        p->setAllClearColours(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 0.0f));
        p->setAllLoadActions(Ogre::LoadAction::Clear);
        for (unsigned i = 0; i < 5u; ++i) p->mStoreActionColour[i] = Ogre::StoreAction::Store;
        p->mStoreActionDepth = Ogre::StoreAction::DontCare;
        p->mStoreActionStencil = Ogre::StoreAction::DontCare;
        p->mFirstRQ = 0u;
        p->mLastRQ = 200u;
        p->mIncludeOverlays = false;
        p->mProfilingId = "Jahshaka card capture";
    }
    Ogre::CompositorWorkspaceDef *wd = cm->addWorkspaceDefinition(mWsDef);
    for (unsigned i = 0; i < 6u; ++i) wd->connectExternal(i, mNodeDef, i);
    Ogre::CompositorChannelVec externals;
    externals.push_back(mNormal);
    externals.push_back(mShadowRough);
    externals.push_back(mAlbedo);
    externals.push_back(mEmissive);
    externals.push_back(mDepth);
    externals.push_back(mDepthBuffer);
    // DISABLED: this workspace is never part of a frame. It is updated inline
    // by captureRound(), which is what makes its CPU cost measurable as the
    // cost of one call.
    mWs = cm->addWorkspace(mScratch, externals, mCam[0], mWsDef, false);
    if (!mWs) { err = "card capture: addWorkspace failed"; return false; }
    mWs->addListener(this);
    return true;
}

bool SurfaceCardSpike::build(NodeId node, unsigned cardSize, std::string &err) {
    destroyAll();
    mNode = node;
    mCardSize = std::max(8u, std::min(1024u, cardSize));
    OgreScene::Node *rec = mScene->record(node);
    if (!rec || !rec->item) { err = "card capture: that node carries no Item"; return false; }
    Ogre::Item *src = rec->item;
    mItemSlot = rec->itemSlot == size_t(-1) ? 0u : unsigned(rec->itemSlot);
    const Ogre::MeshPtr &mesh = src->getMesh();
    if (!mesh) { err = "card capture: that Item has no mesh"; return false; }

    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    // ONE OBJECT, ONE SCENE (the pin's VoxelizedMeshCache trick). ST_GENERIC and
    // a single worker: the capture culls one Item and nothing here wants eight
    // threads to decide that.
    mScratch = root->createSceneManager(Ogre::ST_GENERIC, 1u, processUniqueName("cardScratch"));
    mScratch->setAmbientLight(Ogre::ColourValue::Black, Ogre::ColourValue::Black,
                              Ogre::Vector3::UNIT_Y);
    mItem = mScratch->createItem(mesh, Ogre::SCENE_DYNAMIC);
    const size_t subs = std::min(mItem->getNumSubItems(), src->getNumSubItems());
    for (size_t i = 0; i < subs; ++i)
        mItem->getSubItem(i)->setDatablock(src->getSubItem(i)->getDatablock());
    Ogre::SceneNode *sn = mScratch->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
    // THE WORLD TRANSFORM IS BAKED IN. A card set is per INSTANCE in Lumen too;
    // capturing in world space is what lets the read project a world hit into a
    // card with three dot products and no per-instance matrix.
    const Ogre::Node *srcNode = src->getParentNode();
    if (srcNode) {
        sn->setPosition(srcNode->_getDerivedPosition());
        sn->setOrientation(srcNode->_getDerivedOrientation());
        sn->setScale(srcNode->_getDerivedScale());
    }
    sn->attachObject(mItem);
    mItem->getWorldAabbUpdated();
    mTriangles = 0u;
    for (size_t i = 0, e = mesh->getNumSubMeshes(); i < e; ++i) {
        const Ogre::SubMesh *sub = mesh->getSubMesh(i);
        const Ogre::VertexArrayObjectArray &vaos = sub->mVao[Ogre::VpNormal];
        if (vaos.empty()) continue;
        mTriangles += unsigned(vaos[0]->getPrimitiveCount() / 3u);
    }

    const Ogre::Aabb aabb = mItem->getWorldAabb();
    const Ogre::Vector3 centre = aabb.mCenter;
    const Ogre::Vector3 half = aabb.mHalfSize;
    // A MARGIN so a card's near plane is never inside the surface it captures.
    const float margin = 0.02f * std::max(std::max(half.x, half.y), half.z) + 0.01f;

    for (unsigned c = 0; c < kCards; ++c) {
        const Ogre::Vector3 z = kAxis[c];
        const Ogre::Vector3 upHint =
            std::fabs(z.y) > 0.9f ? Ogre::Vector3::UNIT_Z : Ogre::Vector3::UNIT_Y;
        Ogre::Vector3 x = upHint.crossProduct(z);
        x.normalise();
        const Ogre::Vector3 y = z.crossProduct(x);
        const float alongHalf = std::fabs(z.x) * half.x + std::fabs(z.y) * half.y +
                                std::fabs(z.z) * half.z;
        const float wHalf = std::fabs(x.x) * half.x + std::fabs(x.y) * half.y +
                            std::fabs(x.z) * half.z;
        const float hHalf = std::fabs(y.x) * half.x + std::fabs(y.y) * half.y +
                            std::fabs(y.z) * half.z;
        const float orthoW = std::max(2.0f * wHalf, 1e-4f);
        const float orthoH = std::max(2.0f * hHalf, 1e-4f);
        const Ogre::Vector3 pos = centre + z * (alongHalf + margin);
        Ogre::Camera *cam = mScratch->createCamera(processUniqueName("cardcam"), true, true);
        cam->setProjectionType(Ogre::PT_ORTHOGRAPHIC);
        cam->setOrthoWindow(orthoW, orthoH);
        cam->setNearClipDistance(0.001f);
        cam->setFarClipDistance(2.0f * alongHalf + 4.0f * margin + 0.01f);
        cam->setFixedYawAxis(false);
        cam->setPosition(pos);
        Ogre::Quaternion q;
        q.FromAxes(x, y, z);       // Ogre looks down -Z, so +Z is the card's outward axis
        cam->setOrientation(q);
        cam->setAutoAspectRatio(false);
        mCam[c] = cam;

        CardXform &xf = mXform[c];
        xf.axis[0] = z.x; xf.axis[1] = z.y; xf.axis[2] = z.z; xf.axis[3] = 0.0f;
        // u = x_view/orthoW + 0.5, with x_view = dot(P - pos, x)
        xf.rowU[0] = x.x / orthoW; xf.rowU[1] = x.y / orthoW; xf.rowU[2] = x.z / orthoW;
        xf.rowU[3] = 0.5f - pos.dotProduct(x) / orthoW;
        // v runs DOWN the image while view y runs up.
        xf.rowV[0] = -y.x / orthoH; xf.rowV[1] = -y.y / orthoH; xf.rowV[2] = -y.z / orthoH;
        xf.rowV[3] = 0.5f + pos.dotProduct(y) / orthoH;
        // depth = -z_view = dot(pos - P, z), exactly what the capture piece
        // writes as `-inPs.pos.z`.
        xf.rowD[0] = -z.x; xf.rowD[1] = -z.y; xf.rowD[2] = -z.z;
        xf.rowD[3] = pos.dotProduct(z);
    }
    // THE CARD'S TEXEL, in world units — the number "10-20x finer than a voxel"
    // is measured against. The widest card face decides it.
    mTexelWorld = 2.0f * std::max(std::max(half.x, half.y), half.z) / float(mCardSize);

    if (!makeTextures(err)) return false;
    if (!makeWorkspace(err)) return false;
    mReady = true;
    return true;
}

// THE CARDS AS PICTURES, for the sheet the owner reads. A TEST AND TOOL path:
// it flushes the render system's commands before the ticket reads, because an
// AsyncTextureTicket taken over a target the open command buffer has only
// RECORDED into reads recycled VRAM (CLAUDE.md, the sky/IBL facts).
bool SurfaceCardSpike::dump(const std::string &prefix, std::string &err) {
    if (!mReady) { err = "no card set"; return false; }
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    rs->flushCommands();
    Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
    struct Layer { Ogre::TextureGpu *t; const char *name; float scale; };
    // `scale` turns a layer into something an eye can read: the depth is metres
    // and the emissive is radiance, so both are divided by a stated number
    // rather than clipped silently.
    const Layer layers[] = { { mAlbedo, "albedo", 1.0f },     { mNormal, "normal", 1.0f },
                             { mEmissive, "emissive", 0.25f }, { mDepth, "depth", 0.2f },
                             { mShadowRough, "shadowrough", 1.0f } };
    std::vector<unsigned char> rgba(size_t(mCardSize) * mCardSize * 4u);
    for (const Layer &L : layers) {
        if (!L.t) continue;
        for (unsigned c = 0; c < kCards; ++c) {
            Ogre::AsyncTextureTicket *tk = tm->createAsyncTextureTicket(
                mCardSize, mCardSize, 1u, Ogre::TextureTypes::Type2D, L.t->getPixelFormat());
            Ogre::TextureBox src = L.t->getEmptyBox(0);
            src.sliceStart = c;
            src.numSlices = 1u;
            tk->download(L.t, 0, true, &src, true);
            const Ogre::TextureBox box = tk->map(0);
            for (unsigned y = 0; y < mCardSize; ++y)
                for (unsigned x = 0; x < mCardSize; ++x) {
                    Ogre::ColourValue col = box.getColourAt(x, y, 0, L.t->getPixelFormat());
                    const float v[4] = { col.r * L.scale, col.g * L.scale, col.b * L.scale,
                                         col.a };
                    unsigned char *o = &rgba[(size_t(y) * mCardSize + x) * 4u];
                    for (int k = 0; k < 4; ++k)
                        o[k] = (unsigned char)(std::min(std::max(v[k], 0.0f), 1.0f) * 255.0f + 0.5f);
                    o[3] = 255u;
                }
            tk->unmap();
            tm->destroyAsyncTextureTicket(tk);
            Ogre::Image2 img;
            img.loadDynamicImage(rgba.data(), mCardSize, mCardSize, 1u,
                                 Ogre::TextureTypes::Type2D, Ogre::PFG_RGBA8_UNORM, false, 1u);
            JAH_TRY { img.save(prefix + "-" + L.name + std::to_string(c) + ".png", 0, 1u); }
            catch (const Ogre::Exception &) {}
        }
    }
    (void)err;
    return true;
}

void SurfaceCardSpike::setArmed(bool on) {
    mArmed = on;
    if (mWs) mWs->setEnabled(on);
}

void SurfaceCardSpike::workspacePreUpdate(Ogre::CompositorWorkspace *) { gCapturing = true; }
void SurfaceCardSpike::workspacePosUpdate(Ogre::CompositorWorkspace *) { gCapturing = false; }

float SurfaceCardSpike::captureRound(float *graphMsOut) {
    if (!mWs) return -1.0f;
    const auto tg = std::chrono::steady_clock::now();
    gCapturing = true;
    // THE SCRATCH SCENE'S GRAPH IS UPDATED BY NOBODY ELSE. `Root::renderOneFrame`
    // walks the scene managers it knows are drawing and calls this; a workspace
    // driven by hand is outside that walk, and without it the one Item's world
    // transform and AABB are never derived — the capture culls an object that is
    // still at the origin with a zero box and every card comes back EMPTY, with
    // nothing in any log. (Measured on this lane before it was here.)
    mScratch->updateSceneGraph();
    // ...AND IT IS TIMED APART FROM THE CAPTURE, because a shipped capture pays
    // it ONCE A FRAME for every card set it captures (Root::renderOneFrame walks
    // the scene managers before any workspace runs), while this spike drives one
    // workspace by hand. Charging it to every round would price a capture at the
    // cost of a scene-graph walk it does not really own.
    const auto t0 = std::chrono::steady_clock::now();
    if (graphMsOut)
        *graphMsOut = float(std::chrono::duration<double, std::milli>(t0 - tg).count());
    mWs->_validateFinalTarget();
    mWs->_beginUpdate(false);
    mWs->_update();
    mWs->_endUpdate(false);
    gCapturing = false;
    return float(std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - t0).count());
}

// ---------------------------------------------------------------------------
bool OgreScene::surfaceCardSpike(const SurfaceCardSpikeDesc &desc, SurfaceCardSpikeResult &out) {
    out = SurfaceCardSpikeResult();
    JAH_TRY {
        switch (desc.action) {
        case SurfaceCardSpikeAction::Destroy:
            delete mCardSpike;
            mCardSpike = nullptr;
            out.ok = true;
            return true;
        case SurfaceCardSpikeAction::ReadOn:
        case SurfaceCardSpikeAction::ReadOff:
            if (!mCardSpike || !mCardSpike->ready()) {
                out.error = "no card set built";
                return false;
            }
            mCardSpike->setCardsEnabled(desc.action == SurfaceCardSpikeAction::ReadOn);
            out.ok = true;
            break;
        case SurfaceCardSpikeAction::Build: {
            delete mCardSpike;
            mCardSpike = new SurfaceCardSpike(this);
            std::string err;
            if (!mCardSpike->build(desc.node, desc.cardSize, err)) {
                out.error = err;
                delete mCardSpike;
                mCardSpike = nullptr;
                return false;
            }
            // The FIRST round is a warm-up, never the measurement: it compiles
            // the capture permutation and allocates every pass's buffers.
            mCardSpike->captureRound();
            out.ok = true;
            break;
        }
        case SurfaceCardSpikeAction::ArmInFrame:
        case SurfaceCardSpikeAction::DisarmInFrame:
            if (!mCardSpike || !mCardSpike->ready()) { out.error = "no card set built"; return false; }
            mCardSpike->setArmed(desc.action == SurfaceCardSpikeAction::ArmInFrame);
            out.ok = true;
            break;
        case SurfaceCardSpikeAction::Dump: {
            if (!mCardSpike || !mCardSpike->ready()) { out.error = "no card set built"; return false; }
            std::string err;
            out.ok = mCardSpike->dump(desc.dumpPath, err);
            out.error = err;
            break;
        }
        case SurfaceCardSpikeAction::Capture: {
            if (!mCardSpike || !mCardSpike->ready()) {
                out.error = "no card set built";
                return false;
            }
            const unsigned rounds = std::max(1u, desc.rounds);
            double sum = 0.0, graph = 0.0;
            for (unsigned i = 0; i < rounds; ++i) {
                float g = 0.0f;
                sum += double(mCardSpike->captureRound(&g));
                graph += double(g);
            }
            out.cpuMsPerRound = float(sum / double(rounds));
            out.cpuMsPerCard = out.cpuMsPerRound / float(SurfaceCardSpike::kCards);
            out.graphMsPerRound = float(graph / double(rounds));
            out.ok = true;
            break;
        }
        }
        if (mCardSpike) {
            out.cards = SurfaceCardSpike::kCards;
            out.cardSize = mCardSpike->cardSize();
            out.triangles = mCardSpike->triangles();
            out.vramBytes = mCardSpike->vramBytes();
            out.texelWorld = mCardSpike->texelWorld();
            out.itemSlot = mCardSpike->itemSlot();
        }
        return out.ok;
    }
    catch (Ogre::Exception &e) {
        out.ok = false;
        out.error = e.getFullDescription();
        return false;
    } catch (std::exception &e) {
        out.ok = false;
        out.error = std::string("engine: ") + e.what();
        return false;
    }
}

}   // namespace detail
}   // namespace engine
}   // namespace jahshaka
