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
};
/// The engine's probes; null for an engine of another kind.
const EngineEcho *engine(const Engine *e);

}}}   // namespace jahshaka::engine::inspect
