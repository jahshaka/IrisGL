// SURFACE-CACHE-0 — the phase-0 DESIGN SPIKE's private state (2026-09-21).
//
// NOT A FEATURE. This header exists so exactly two translation units can see
// the same object: OgreSurfaceCards.cpp, which captures a mesh's six axis
// cards, and OgreRayQuery.cpp, which reads them at a reflection ray's hit. It
// is inert unless a test asks for it (Scene::surfaceCardSpike), it is created
// by that call and by nothing else, and with it absent every byte of the
// engine's behaviour is what it was — which is what keeps both selftest hashes
// where they are.
//
// The shape is the assessment's S2 with the phases folded flat: cards captured
// through a JahshakaPcc-shaped compositor workspace into resident Type2DArray
// atlases (five layers: normal, shadow+roughness, albedo, emissive, depth), and
// the LIGHTING done at the READ rather than in an atlas of its own. Lighting at
// the read is the harder arm for cards — a shipped cache would fetch one
// pre-lit texel — so the per-ray cost measured here is an upper bound.
#pragma once

#include "jahshaka/engine/Types.h"

#include <Compositor/OgreCompositorWorkspaceListener.h>

#include <chrono>
#include <string>
#include <vector>

namespace Ogre {
class Camera;
class CompositorWorkspace;
class Item;
class SceneManager;
class TextureGpu;
}   // namespace Ogre

namespace jahshaka {
namespace engine {
namespace detail {

class OgreScene;

/// WHAT A COMPONENT WOULD NOT DO — the debt this shape carries, recorded here
/// rather than rediscovered at phase 2 (a spike is allowed these; a shipped
/// `jahshaka::engine::SurfaceCache` is not):
///
///   * `SurfaceCardSpikeDesc`/`Result`/`Action` sit on the PUBLIC engine
///     boundary (`Types.h`) for one test's benefit. A Component's own state
///     belongs behind a verb, not in the document-facing header.
///   * `OgreScene` grants this class FRIENDSHIP to reach its item table and its
///     SceneManager. A Component takes what it needs as arguments.
///   * FIVE FIXED BINDINGS and 27 `vec4` of parameters hold exactly ONE card
///     set. The shipping shape is one atlas binding per layer plus an SSBO of
///     card rects indexed by the instance slot (the report's §2).
///   * `recordReflect` walks the scene's light list EVERY FRAME to find the sun
///     for the card's direct term. A lighting job owns that, once.
///   * `gCapturing` is a PROCESS-GLOBAL bool. It works because one capture runs
///     at a time on one thread; a Component with a budget would have several.
///   * A node whose `itemSlot` is npos is recorded as slot 0, which ALIASES the
///     real slot 0. Harmless here (the spike cards one node of a fixture whose
///     items all have slots) and wrong in general: the key needs its own
///     "no slot" value.
///   * A SCRATCH SceneManager per card set costs a graph walk every frame for
///     its whole life (see OgreSurfaceCards.cpp's note) — phase 2 captures in
///     the real scene.
///
/// One mesh instance's six axis-aligned cards, captured and resident.
///
/// IT IS ITS OWN WORKSPACE LISTENER, and that is what lets a capture run INSIDE
/// a frame: `jah_card_capture` has to be set on the Hlms while the capture
/// workspace executes and cleared again before anything else in the frame
/// prepares a pass, and the workspace's own pre/post callbacks are exactly that
/// bracket. Running inside the frame is also the only way the monitor's
/// per-pass GPU timestamps cover the capture (it attaches its listeners at a
/// frame's head), and it is what a shipped capture would do anyway.
class SurfaceCardSpike final : public Ogre::CompositorWorkspaceListener {
public:
    static constexpr unsigned kCards = 6u;

    explicit SurfaceCardSpike(OgreScene *scene) : mScene(scene) {}
    ~SurfaceCardSpike();

    /// Builds (or rebuilds at a new size) the card set for `node` and arms the
    /// capture workspace for `rounds` frames. The numbers come back through the
    /// frame monitor's own per-pass records, which is why this returns as soon
    /// as the workspace is armed.
    bool build(NodeId node, unsigned cardSize, std::string &err);
    /// One capture round, executed INLINE (the sky-IBL idiom: _beginUpdate /
    /// _update / _endUpdate) so its CPU cost is the cost of this call and
    /// nothing else's. Returns the wall milliseconds the whole six-card round
    /// took on the CPU.
    float captureRound(float *graphMsOut = nullptr);
    /// Writes every layer of every card as a PNG under `prefix` (a tool path).
    bool dump(const std::string &prefix, std::string &err);

    /// The item slot the card set covers — the same number the TLAS instance
    /// carries as its `instanceCustomIndex` (OgreScene::gatherRayInstances),
    /// which is what makes "is this hit on the carded mesh" one integer compare
    /// in the ray job and not a lookup.
    unsigned itemSlot() const { return mItemSlot; }
    bool     ready() const { return mReady; }
    unsigned cardSize() const { return mCardSize; }
    unsigned triangles() const { return mTriangles; }
    unsigned long long vramBytes() const;
    Ogre::CompositorWorkspace *workspace() const { return mWs; }

    Ogre::TextureGpu *normalTex() const { return mNormal; }
    Ogre::TextureGpu *shadowRoughTex() const { return mShadowRough; }
    Ogre::TextureGpu *albedoTex() const { return mAlbedo; }
    Ogre::TextureGpu *emissiveTex() const { return mEmissive; }
    Ogre::TextureGpu *depthTex() const { return mDepth; }

    /// Per card: the outward world axis, and the three rows that turn a world
    /// position into (u, v, distance-from-the-card-plane). Uploaded to the ray
    /// job's parameter block verbatim.
    struct CardXform {
        float axis[4] = { 0, 0, 0, 0 };
        float rowU[4] = { 0, 0, 0, 0 };
        float rowV[4] = { 0, 0, 0, 0 };
        float rowD[4] = { 0, 0, 0, 0 };
    };
    const CardXform &xform(unsigned i) const { return mXform[i]; }
    /// The depth tolerance a hit is accepted within: TWO card texels' world
    /// size. One texel is the finest thing a card can resolve, and two is that
    /// with a texel of slack either side of the sample — a hit lands anywhere
    /// inside its texel and the ray's own surface bias has already moved it off
    /// the surface, so a one-texel window rejects hits the card does see. (The
    /// factor and this sentence must agree; they did not, and the comment was
    /// the wrong one.)
    float depthTolerance() const { return mTexelWorld * 2.0f; }
    float texelWorld() const { return mTexelWorld; }

    /// Does the ray job read the cards this frame? The A/B's only switch.
    bool cardsEnabled() const { return mEnabled; }
    void setCardsEnabled(bool on) { mEnabled = on; }

    /// ARMED = the capture workspace runs as part of every frame. The spike's
    /// in-frame capture mode; `captureRound()` is the out-of-frame one, kept
    /// because it is the only way to time ONE capture's CPU cost on its own.
    void setArmed(bool on);
    bool armed() const { return mArmed; }

    void workspacePreUpdate(Ogre::CompositorWorkspace *) override;
    void workspacePosUpdate(Ogre::CompositorWorkspace *) override;

    void destroyAll();

private:
    bool makeTextures(std::string &err);
    bool makeWorkspace(std::string &err);

    OgreScene *mScene = nullptr;
    NodeId     mNode = 0;
    unsigned   mItemSlot = 0u;
    unsigned   mCardSize = 128u;
    unsigned   mTriangles = 0u;
    float      mTexelWorld = 0.01f;
    bool       mReady = false;
    bool       mEnabled = false;
    bool       mArmed = false;

    Ogre::SceneManager      *mScratch = nullptr;
    Ogre::Item              *mItem = nullptr;
    Ogre::Camera            *mCam[kCards] = {};
    Ogre::CompositorWorkspace *mWs = nullptr;
    std::string              mNodeDef, mWsDef;

    Ogre::TextureGpu *mNormal = nullptr, *mShadowRough = nullptr, *mAlbedo = nullptr,
                     *mEmissive = nullptr, *mDepth = nullptr, *mDepthBuffer = nullptr;

    CardXform mXform[kCards];
};

}   // namespace detail
}   // namespace engine
}   // namespace jahshaka
