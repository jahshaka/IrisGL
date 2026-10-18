#pragma once
// THE ENGINE'S OWN RECORD OF WHAT A HOST SET — TEST-FACING ONLY (ENGINE-UPGRADE-U3 stage 2).
//
// Stage 2 deleted from Engine.h every getter that only echoed a value the host itself set (F5: "the host keeps what
// it set" — a snapshot would be stale for a caller reading back its own write, and a synchronous read of the engine
// is the door U3 closes). The engine fixtures still assert that the ENGINE took what they pushed (a flag that must
// survive a re-attach, a mask that must reach the right node), so the engine's own record is readable HERE, in
// LOCKSTEP — after the frame a fixture waited for, the render side is idle and the read is ordered after it.
//
// NOTHING UNDER src/ OR irisgl/mirror MAY INCLUDE THIS HEADER (source.testing_rules R11): the host keeps its own.
#include "Engine.h"

namespace jahshaka { namespace engine { namespace inspect {

/// The scene's record of the host's pushes (each the value its setter last accepted).
class SceneEcho {
public:
    virtual ~SceneEcho() = default;
    virtual SkyDesc        sky() const = 0;
    virtual float          shaderTime() const = 0;
    virtual float          lodBias() const = 0;
    virtual bool           isLoading() const = 0;
    virtual bool           atomOcclusionEnabled() const = 0;
    virtual SunContactDesc sunContact() const = 0;
    virtual bool           nodeProbeGridExcluded(NodeId) const = 0;
    virtual bool           nodeHelper(NodeId) const = 0;
    virtual bool           nodeBackdrop(NodeId) const = 0;
    virtual bool           nodeVrHelper(NodeId) const = 0;
    virtual void           vrProxyNodes(NodeId out[2]) const = 0;
    virtual void           vrRayNodes(NodeId out[2]) const = 0;
    virtual bool           nodeMovable(NodeId) const = 0;
    virtual unsigned       nodeLightMask(NodeId) const = 0;
    virtual bool           nodeCastShadow(NodeId) const = 0;
    virtual FaceCull       nodeFaceCull(NodeId) const = 0;
    /// The rig the fixture attached (Scene::attachSkinnedMesh): is it live, and its bone names in rig index order —
    /// what the host's own SkeletonDesc already says (the getters are DOC since U3 stage 2).
    virtual bool                     hasSkeleton(NodeId) const = 0;
    virtual std::vector<std::string> boneNames(NodeId) const = 0;
    /// THE CARD WALK'S COST (U3 stage 3 fix round, decision 36's measurement): the last walk of the card casters'
    /// records (PhotonComponent::cardMoverFrame — a slot-count change, or a renumbered slot), in µs, the slots it
    /// walked, and how many walks ran. A measuring fixture's reading.
    struct CardWalk {
        double             micros = 0.0;
        unsigned           slotCount = 0;   // (not `slots`: a Qt keyword where Qt is included)
        unsigned long long walks = 0;
    };
    virtual CardWalk cardWalk() const = 0;
    /// THE GPU SCENE'S LAST WALK, BY PHASE (ENGINE-UPGRADE-U4 stage 1, premise P2): the named slots resolved, the
    /// compare (Ogre's pool: `shares` = the workers plus the frame's thread), the compose (the frame's thread) and,
    /// inside the compose, the change feed's observers (the notify share), in µs; the slots compared and composed;
    /// the walks run.
    struct GpuWalk {
        double             nameMicros = 0.0, compareMicros = 0.0, composeMicros = 0.0, notifyMicros = 0.0;
        unsigned           slotsCompared = 0, slotsComposed = 0, shares = 0;
        unsigned long long walks = 0;
        /// THE DIRECT INDEXES' REACH (U4 decision 12.1 i): the Ogre-id and the registry-id page tables — the ids their
        /// pages span and the pages allocated.
        unsigned long long ogreIdReach = 0, ogreIdPages = 0, nodeIdReach = 0, nodeIdPages = 0;
        /// THE COMPOSE INSIDE THE COMPARE'S TASK (U4 stage 4): `compareMicros` is the task (the shares' compare and
        /// compose), `composeMicros` the serial part after the barrier, `replayMicros` of it the replay (the tails,
        /// the observers); `composeDigest` the last walk's digest while gpuscene.composeDigest is on.
        double             replayMicros = 0.0;
        unsigned long long composeDigest = 0;
        /// (decision 18.3) shares recomposed on the frame's thread after a throw, ever; (18.4) the last walk's tails —
        /// the Atom routes and the material queue pushes applied; (17.1) the observers' time in the timed notifies,
        /// ever, µs — the ones with a lane half (their ordered halves; the card feed) and the rest (the ray feed).
        unsigned long long shareRecomposes = 0;
        /// (decision 22.4 b) shares whose recompose threw again, ever — their unstaged slots marked for the next compose.
        unsigned long long shareGiveUps = 0;
        unsigned           tailAtoms = 0, tailQueues = 0;
        double             observerLaneMicros = 0.0, observerOtherMicros = 0.0;
        /// THE NAME LIST'S GUARD (U4 decision 16.2): listed entries that are not a live record (by address, never
        /// dereferenced) — 0 is the law — and the list's length.
        unsigned           namedDangling = 0, namedListed = 0, namedLive = 0;
        unsigned           namedNotFlagged = 0, namedDuplicates = 0;   ///< (decision 18.5: both 0)
    };
    virtual GpuWalk gpuWalk() const = 0;
    /// THE SUN TRACE'S CPU (ENGINE-UPGRADE-U4 stage 1, premise P1): the last SurfaceCache::traceSun call — its whole
    /// time in µs, the still casters' moves it took, the cards it held, the cache's frame it ran in, the calls so far.
    /// All zero where the scene has no card cache.
    struct TraceSun {
        double             micros = 0.0;
        unsigned           casterMoves = 0, cards = 0;
        unsigned long long frame = 0, calls = 0;
        /// The still casters' HIT SET (step 1): the cards a moved still caster shades this call, and a digest of the
        /// per-card mask — the identity a faster search is held to.
        unsigned           casterHits = 0;
        unsigned long long casterHitDigest = 0;
        /// THE GRID'S GUARD (the arm "cards.sunGridParity"): calls checked against the exhaustive search, and the
        /// cards whose answer differed — 0 is the law.
        unsigned long long parityChecks = 0, parityMisses = 0;
        /// ...of `micros`: the scene's moves frame (the producer of the moves, PhotonComponent::cardMoverFrame) and
        /// step 1 (the still casters' search, their footprints included).
        double             hookMicros = 0.0, stepOneMicros = 0.0;
        /// ...and the movers' searches: step 3 (the changed footprints' cards) and step 3b (the pending cards a
        /// current footprint reaches).
        double             step3Micros = 0.0, step3bMicros = 0.0;
        /// ...and the rest, named: step 1b (the still trace — the per-hit term), step 2 (the movers' changed boxes),
        /// step 4 (the order and the pick), the movers' trace job.
        double             stillMicros = 0.0, step2Micros = 0.0, step4Micros = 0.0, traceJobMicros = 0.0;
        /// The cards the call's grids held apart as `wide` (the most of its grids).
        unsigned           wideCards = 0;
    };
    virtual TraceSun traceSun() const = 0;
    /// THE GI PLAN'S LOG (ENGINE-UPGRADE-U4 stage 3): the last PhotonComponent::planFrame — its CPU in µs, the boxes
    /// the walk handed the cascades, the cascades pending after its mark (a bit each), the one it rebuilds (-1 none),
    /// the plans run.
    struct GiPlan {
        double             micros = 0.0;
        unsigned           boxes = 0, pending = 0;
        int                rebuild = -1;
        unsigned long long plans = 0;
        /// THE GEOMETRY SIGNATURE (the drain's plan part): its full passes, and the guard's (gi.geomSigParity)
        /// passes checked and misses — 0 is the law.
        unsigned long long sigFullPasses = 0, sigChecks = 0, sigMisses = 0;
        /// THE ROOT BOXES' GUARD (gi.rootBoxParity): marks checked, cascades the items' own boxes reach that the
        /// marks missed (0 is the law), cascades marked that no item box reaches (the union's extra).
        unsigned long long rootChecks = 0, rootMisses = 0, rootExtras = 0;
        /// THE PLAN'S PARTS (µs; the monitor's engine.plan children): the geometry signature's last pass, the walk
        /// with its boxes and marks, the scheduler's pick.
        double             signatureMicros = 0.0, boxesMicros = 0.0, pickMicros = 0.0;
        /// THE PLACEMENT'S GUARD (gi.pccPlacementLights, U4 decision 14): probe-placement stages checked, the stages
        /// that changed the scene manager's light count, the count before and after the last one, the probes checked
        /// and the largest offset (m) of a placed probe's derived node from where it was set — 0 changes and 0 m are
        /// the law.
        unsigned long long pccStages = 0, pccStagesLit = 0, pccLightChanges = 0;   ///< (Lit: began with a light list)
        unsigned           pccLightsBefore = 0, pccLightsAfter = 0, pccProbes = 0;
        float              pccProbeOffsetMax = 0.0f;
        /// ...and the frame's dirty-probe collection (decision 16.1): collections read, the light count after the last.
        unsigned long long pccCollections = 0;
        unsigned           pccCollectLights = 0;
    };
    virtual GiPlan giPlan() const = 0;
};
/// The scene's record; null for a scene this engine did not make.
const SceneEcho *scene(const Scene *s);

/// The view's record of the host's pushes (setters and the record's view part, as applied).
class ViewEcho {
public:
    virtual ~ViewEcho() = default;
    virtual NodeId                 cameraNode() const = 0;
    virtual Colour                 background() const = 0;
    virtual bool                   shadows() const = 0;
    virtual bool                   isEnabled() const = 0;
    virtual const PostFxDesc      &postFx() const = 0;
    virtual bool                   helpersVisible() const = 0;
    virtual bool                   vrHelpersVisible() const = 0;
    virtual bool                   lodHysteresisOffscreen() const = 0;
    virtual const ViewOverlayDesc &overlay() const = 0;
    virtual const ViewPipDesc     &pip() const = 0;
    virtual bool                   asyncShaders() const = 0;
};
/// The view's record; null for a view this engine did not make.
const ViewEcho *view(const View *v);

/// The engine's test probes (ENGINE-UPGRADE-U3 stage 3): readings only a fixture between its own frames asks for.
class EngineEcho {
public:
    virtual ~EngineEcho() = default;
    /// The renderer's CURRENT command buffer, as an opaque tag: it changes exactly when the queue submits, so a tag
    /// unchanged across a call proves the call neither submitted nor committed a command buffer (0 = cannot tell).
    virtual unsigned long long queueSubmissionTag() const = 0;
    /// ONE FREE-RUNNING FRAME, ON REQUEST (U3 stage 5's fixtures: frame.interpolation's closed form): the frame the
    /// render thread draws on its own in the owner's mode — the newest record, interpolated at the presentation
    /// clock, the clock advanced by the display period — drawn now and waited for.
    virtual void presentationFrame() = 0;
    /// The last free frame's interpolation: its nodes and its microseconds (scene `s`), for the apply's measure row.
    /// ...`micros` the apply's writes, `namingMicros` the naming of what they wrote to the scans (decision 56).
    virtual void lastInterpolation(const Scene *s, unsigned &nodes, double &micros, double &namingMicros) const = 0;
    /// THE FRAME'S POOL SPANS (U4 stage 6): each frame's span from its scene-graph update to the end of its render —
    /// where the SceneManagers' workers run — in steady_clock nanoseconds, the newest last (a bounded ring). Safe from
    /// any thread.
    virtual void poolSpans(std::vector<std::pair<unsigned long long, unsigned long long>> &out) const = 0;
};
/// The engine's probes; null for an engine of another kind.
const EngineEcho *engine(const Engine *e);

}}}   // namespace jahshaka::engine::inspect
