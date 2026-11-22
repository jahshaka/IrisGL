#pragma once
// THE ENGINE's PUBLISHED TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "jahshaka/engine/Fog.h"
#include "jahshaka/engine/FrameInput.h"
#include "jahshaka/engine/GiGather.h"
#include "jahshaka/engine/GiParams.h"
#include "jahshaka/engine/GiStatus.h"
#include "jahshaka/engine/GpuSceneRecords.h"
#include "jahshaka/engine/Ids.h"
#include "jahshaka/engine/Math.h"
#include "jahshaka/engine/Monitor.h"
#include "jahshaka/engine/Overlay.h"
#include "jahshaka/engine/PostFx.h"
#include "jahshaka/engine/RayQuery.h"
#include "jahshaka/engine/Shaders.h"
#include "jahshaka/engine/Skeleton.h"
#include "jahshaka/engine/Sky.h"
#include "jahshaka/engine/Stats.h"
#include "jahshaka/engine/Vr.h"

namespace jahshaka { namespace engine {

/// WHAT A SCENE PUBLISHED FOR THE FRAME IT LAST DREW (ENGINE-UPGRADE-U2; Scene::published()): the engine's output the
/// document reads WITHOUT asking — under U3 the document thread reads this snapshot and nothing else of the engine's
/// state. Written once at the close of every frame the engine renders, as a whole-frame swap (Engine.h, THE SNAPSHOT).
struct PublishedFrame {
    /// A bone RIDER's TAG (the socket's tag point on the bone): the rider's parent, as drawn. The document composes the
    /// rider's local under it and re-expresses a world write under it (iris::graph::setRiderTag).
    struct RiderTag {
        NodeId rider = 0;
        Vec3   position;
        Quat   rotation;
        Vec3   scale = Vec3(1.0f, 1.0f, 1.0f);
        /// WHO THE RIDER IS TAGGED TO (ENGINE-UPGRADE-U3 stage 3, the facts): the owner node and the bone — the engine
        /// frees a tag by itself when the owner's skeleton is replaced, so a host's guard reads this, never a latch.
        NodeId      owner = 0;
        std::string bone;
    };
    unsigned long long   frame = 0;     ///< the engine frame it was published at (0 = never drawn)
    /// The record the frame applied (FrameInput::serial; an empty hand-over carries the last) — ENGINE-UPGRADE-U3.
    unsigned long long   serial = 0;
    /// THE SCENE'S CLOCK AS THIS FRAME LEFT IT (ONE-FRAME-1): the newest grid state it drew (its document's lifetime
    /// serial, FrameInputScene::stepSerial), its time so far (the steps its frames advanced x the period: what its
    /// particles, meter, looks and clouds have lived) and this frame's share of it (0 on a frame nobody stepped it).
    unsigned long long   stepSerial = 0;
    double               sceneSeconds = 0.0;
    double               frameSceneSeconds = 0.0;
    std::vector<RiderTag> riderTags;     ///< SORTED by rider (decision 36: the lookups are binary searches)
    /// The rider's owner as the frame drew it (0 = no tag), and its bone into `bone` when given.
    NodeId boneAttachmentOf(NodeId rider, std::string *bone = nullptr) const {
        const auto it = std::lower_bound(riderTags.begin(), riderTags.end(), rider,
                                         [](const RiderTag &t, NodeId id) { return t.rider < id; });
        if (it == riderTags.end() || it->rider != rider) return 0;
        if (bone) *bone = it->bone;
        return it->owner;
    }
    /// THE ENGINE-MUTATED FACTS (ENGINE-UPGRADE-U3 stage 3, decision 13): what the engine itself changes under a
    /// host — a skeleton share (dropped when an instance is replaced), the planar reflectors flagged (armed or waiting
    /// for the arm), the reflector actors made, the clips each rigged node carries by content id and engine name. ONE
    /// block, rebuilt only on the frame a fact changed; every other frame shares the same block (`facts` is the
    /// same pointer), so a host compares pointers to learn "nothing moved".
    struct Facts {
        struct Share {
            NodeId follower = 0;
            NodeId source = 0;
        };
        std::vector<Share> shares;            ///< SORTED by follower id (every lookup below is a binary search)
        std::vector<NodeId> reflectors;       ///< flagged planar reflectors, SORTED
        unsigned reflectorArms = 0;           ///< reflector actors armed since the scene was made (a diagnostic)
        struct Clip {
            std::string id;                   ///< the host's content id (ClipDesc::id)
            std::string name;                 ///< the engine's name for it (clipNameFor)
        };
        struct NodeClips {
            NodeId node = 0;
            std::vector<Clip> clips;          ///< in attach order
        };
        std::vector<NodeClips> clips;         ///< SORTED by node id
        // BINARY SEARCHES (U3 stage 3 fix round, decision 36): the mirror asks per rigged node and per rider per sync,
        // so a linear scan was O(n^2) at crowd scale.
        NodeId shareSourceOf(NodeId follower) const {
            const auto it = std::lower_bound(shares.begin(), shares.end(), follower,
                                             [](const Share &s, NodeId id) { return s.follower < id; });
            return it != shares.end() && it->follower == follower ? it->source : 0;
        }
        bool sharesSkeleton(NodeId id) const { return shareSourceOf(id) != 0; }
        bool isPlanarReflector(NodeId id) const { return std::binary_search(reflectors.begin(), reflectors.end(), id); }
        const std::vector<Clip> *clipsOf(NodeId id) const {
            const auto it = std::lower_bound(clips.begin(), clips.end(), id,
                                             [](const NodeClips &c, NodeId n) { return c.node < n; });
            return it != clips.end() && it->node == id ? &it->clips : nullptr;
        }
        std::vector<std::string> clipNamesOf(NodeId id) const {
            std::vector<std::string> out;
            if (const std::vector<Clip> *c = clipsOf(id)) for (const Clip &k : *c) out.push_back(k.name);
            return out;
        }
    };
    std::shared_ptr<const Facts> facts;
    /// THE COMPLETIONS (U3 stage 3; decision 34): every NAMED message's answer (SceneMessage::token, accepted or
    /// refused) and every REFUSAL of an unnamed one (token 0) — a refusal belongs to the message that caused it,
    /// never to the frame's error string. Kept for the last kCompletionFrames published frames (the frame-error
    /// list's span; never an entry count, so an adopting sync of any size loses none), by a sequence that only
    /// grows — a host remembers the last it read.
    struct Completion {
        unsigned long long seq = 0;
        unsigned long long token = 0;
        bool               ok = true;
        std::string        error;   ///< the engine's reason when refused
        unsigned long long frame = 0;   ///< the published frame whose drain answered it
    };
    static constexpr unsigned kCompletionFrames = 16;
    std::shared_ptr<const std::vector<Completion>> completions;
    const Facts &factsBlock() const {
        static const Facts kNone;
        return facts ? *facts : kNone;
    }
    /// THE SCENE'S GI MACHINE as the frame closed it (Scene::giStatus at the close; U3 stage 1 — the readers move to
    /// it in stage 2).
    GiStatus             gi;
    /// THE AIR THE SUN'S BEAM CROSSES, as the frame left it (ENGINE-UPGRADE-U3 stage 2, F-A): the realistic sky's model
    /// and the observer's altitude the engine's tables were built for — the inputs of atmosphereSunTint(), the
    /// stateless boundary function a host calls while it builds the next record. `on` false = no realistic sky
    /// (the tint is white).
    struct Atmosphere {
        bool  on = false;
        float rayleighScale = 1.0f;
        float mieScale = 1.0f;
        bool  ozone = true;
        float planetRadiusKm = 6360.0f;
        float atmosphereHeightKm = 100.0f;
        float observerKm = 0.0f;
    };
    Atmosphere           atmosphere;
    // ---- THE SCENE'S STATUS AS THE FRAME CLOSED IT (ENGINE-UPGRADE-U3 stage 2: what the host read synchronously) ----
    HeightFogStatus      heightFog;
    bool                 skyAmbientShValid = false;   ///< skyAmbientSh holds the sky's SH (Scene's old getter's bool)
    float                skyAmbientSh[27] = {};
    /// THE ENVIRONMENT IS STILL LANDING (2026-10-20): a sky change requests a capture, its SH read and its specular
    /// convolution; until all three land the PREVIOUS reflection and ambient stay bound by design (SKY-SWAP-1, ASYNC-SHADERS-1:
    /// an asynchronous scene's convolution permutation may be compiling in the background — wall-clock, not frames). A
    /// host holds a loading cover, a test counts frames to this clearing, never a guessed count.
    bool                 environmentPending = false;
    /// THE SKY's DEFERRED READS over the scene's life (PHOTON-7 stage 10-3, item 0): the environment SH's and the cloud
    /// sheet's clear-sky ambient's reads issued as a ticket and taken at their due frame (a drag, the cloud scroll's
    /// captures) rather than read in the capture's own frame. 0 = every sky light this scene took landed synchronously.
    unsigned             skyShDeferredReads = 0;
    /// THE SPECULAR CONVOLUTION's DEFERRED TRIES over the scene's life (COLD-BOOT-1): a try of an asynchronous scene that
    /// met a permutation still on the background compiler and was retried the next frame on its kept workspace. 0 = every
    /// convolution was a result at its first try (its permutations built, or the frame owed the whole picture).
    unsigned long long   iblConvolutionDeferrals = 0;
    /// THE FRAME's COMPLETENESS (COLD-BOOT-1 item 3, the lead's ruling: THE REST LAW's INPUT IS A COMPLETE FRAME): the
    /// scene's last drawn frame was INCOMPLETE when a view of it drew a PLACEHOLDER or SKIPPED a draw whose permutation is
    /// still compiling, a CARD READER was refused by its ray build in flight, or a material wore a FALLBACK while its
    /// texture loads. An incomplete scene is not at rest (GiStatus::giAtRest false, no rest decision of its cards), and
    /// the frame in which it becomes complete is a change of what its readers read: the cards' demand window reopens and
    /// the readers rest anew (a rest held over a placeholder picture was never the scene's rest). A failure is FINAL,
    /// never pending: a failed permutation draws its placeholder as its answer, a failed reader build makes no reader, a
    /// failed texture load is the fallback. The predicate a view needs to show only complete frames (SHADER-SYSTEM-1).
    struct Completeness {
        bool incomplete = false;
        /// Its terms on that frame: bit 0 a placeholder drawn, 1 a pending draw skipped, 2 a card reader refused by its
        /// ray build, 3 a material awaiting its texture; 4 a TEST term (the door frames.incompleteHold, never set in the
        /// app: a frame incomplete with no demand change, gi.card_build_late.window); 5 a RAY INPUT REFUSED (GATHER-HANG-1
        /// stage 2: a ray job refused its dispatch for a view not made, or JAH_RQ_INIT a ray for an undefined input -
        /// RayQueryTier::rayInputRefused); 6 a COMPUTE DISPATCH DEFERRED (SHADER-SYSTEM-1 7a: an asynchronous scene's
        /// atmosphere tables, sky convolution or lighting arm met a permutation still building in the background since
        /// the last note, or still owes its retry - the frame drew without that work). Every term - 8, 16, 32 and 64 -
        /// is its own bit.
        unsigned terms = 0u;
        /// The drawn frames it has been incomplete IN A ROW (0 = complete): a scene stuck incomplete shows here (and on
        /// the startup gate's card line) instead of as a silent never-rest.
        unsigned long long incompleteFrames = 0ull;
        /// Its incomplete -> complete edges over the scene's life (the cards' demand change; the readers' rest keys).
        unsigned long long completionSerial = 0ull;
    };
    Completeness         completeness;
    unsigned             cloudClearDeferredReads = 0;
    CloudStatus          clouds;
    AtmosphereStatus     atmosphereStatus;
    RayQueryStatus       rayQuery;
    SunContactStatus     sunContact;
    AreaVisStatus        areaVis;   ///< PHOTON-7 7-A: the pixel's area light visibility
    int                  activePlanarReflectors = 0;
    bool                 atomViewPaintable = false;
    /// THE ATOM CUT'S HIDDEN SHARE as the frame closed it (ATOM-8 stage 0; AtomDrawStatus::hiddenValid's figure, read
    /// without a wait): valid only while a view of the scene shows the Atom view's Triangles or Density mode — the
    /// stats overlay's Atom line.
    struct AtomHidden {
        bool valid = false;
        unsigned long long drawn = 0ull, hidden = 0ull;
    };
    AtomHidden           atomHidden;
    /// Why each Photon view cannot paint right now ("" = it can), indexed by PhotonView.
    std::string          photonViewRefusals[kPhotonViewCount];
    /// THE SCENE'S MOBILITY AND RIG COUNTS as the frame closed them (U3 stage 2 read 12(b): running totals kept at the
    /// mutation sites, copied here every frame — no walk of the graph).
    MobilityStatus       mobility;
    RigStats             rigs;
    /// THE SCENE'S COUNTERS as the frame closed them (ENGINE-UPGRADE-U3 stage 2b: the getters they replace are deleted;
    /// each is a counter the engine keeps for another reason, copied here — the meaning is the old getter's, kept in
    /// Engine.h's comment where it stood).
    SceneChangeStats     changes;                       ///< structural edits so far (SceneChangeStats)
    unsigned             particleDefinitionsCreated = 0;   ///< particle definitions this scene ever created (the pool's leak bound)
    unsigned long long   lightWriteSerial = 0;          ///< moves when any light the injection reads changed
    unsigned             decalAtlasCapacity = 0;        ///< slices per decal atlas (one figure for every DecalMap)
    unsigned             decalAtlasUsed[3] = {};        ///< slices taken, indexed by DecalMap (Diffuse, Normal, Emissive)
    GpuSceneStatus       gpuScene;
    unsigned             cullTwins = 0;                 ///< cull twins alive (mirror.cull_twin)
    unsigned             cullTwinCandidates = 0;        ///< twins waiting for the sweep — never more than cullTwins
    /// PER NODE: the live particles of every node with a particle system (SIMD-rounded up, as Ogre packs them), and
    /// the bones every rigged node streams to its shader. A node not listed has none.
    struct NodeCount {
        NodeId node = 0;
        size_t count = 0;
    };
    std::vector<NodeCount> particles;
    std::vector<NodeCount> streamedBones;
    unsigned particlesOf(NodeId id) const {
        for (const NodeCount &c : particles) if (c.node == id) return unsigned(c.count);
        return 0u;
    }
    size_t streamedBonesOf(NodeId id) const {
        for (const NodeCount &c : streamedBones) if (c.node == id) return c.count;
        return 0u;
    }
    /// The sky's SH into `out` when the scene has one (skyAmbientShValid); `out` untouched otherwise.
    bool copySkyAmbientSh(float out[27]) const {
        if (!skyAmbientShValid) return false;
        for (int i = 0; i < 27; ++i) out[i] = skyAmbientSh[i];
        return true;
    }
    /// THE POSES OF THE RIGS A HOST ASKED FOR (Scene::publishBonePoses): each rig's bones, parent-local, in the mesh
    /// node's frame — what Scene::bonePoses read, as drawn by this frame.
    struct RigPoses {
        NodeId                node = 0;
        std::vector<BonePose> poses;
    };
    std::vector<RigPoses> bonePoses;
    /// THE NAMED NODES' WORLD POSES (the VR proxies' and rays' — Scene::setVrProxyNodes / setVrRayNodes), as drawn.
    struct NodePose {
        NodeId node = 0;
        Vec3   position;
        Quat   rotation;
    };
    std::vector<NodePose> nodePoses;
    /// THE GI CONFIGURATION THE DRAIN LAST APPLIED (U3 stage 2, F-B): what a host compares its own pending record's
    /// `gi` against (Engine::pendingGiConfiguration — a computation on the submitting side, never a read of the engine).
    std::optional<GiParams> giApplied;
};

/// A view's presented-frame counters (View::framesPresented / blankFramesPresented) as the frame closed them,
/// keyed by the view's NAME (unique among live views): a pointer would dangle in the snapshot for the two frames
/// after a destroyView.
struct ViewFrames {
    /// THE VIEW'S ID (ENGINE-UPGRADE-U3 stage 3, decision 21): reserved when the engine makes the view, never
    /// reused — a row is found by it, never by name, so a view re-created under the same name has NO row until
    /// its first frame publishes (a reader sees "not yet drawn", never a dead view's picture).
    NodeId             id = 0;
    std::string        name;
    unsigned long long presented = 0;       ///< View::framesPresented (reset when a scene is bound)
    unsigned long long blank = 0;           ///< View::blankFramesPresented
    unsigned           width = 0, height = 0;   ///< the ACHIEVED target size (View::width/height)
    unsigned           sampleCount = 1;     ///< the ACHIEVED MSAA count
    unsigned           pipGeneration = 0;
    unsigned           workspaceGeneration = 0;
    unsigned long long globalsPushes = 0;
    PostFxDesc         postFx;              ///< the EFFECTIVE chain the view drew with (a VR policy applied)
    /// THE CHAIN's ROUTES AS BUILT (PHOTON-7-9A; the engine's decision, published for a host - the lab's `routes`
    /// verb): the screen-probe gather, the no-ray route's screen integrate, the ray reflection. The fixtures' door
    /// (inspect::ViewEcho::routes) answers the same three on the same frame (engine.published_routes holds them equal).
    bool               routeGather = false, routeIntegrate = false, routeReflect = false;
};

/// WHAT THE ENGINE PUBLISHED FOR THE FRAME IT LAST CLOSED (ENGINE-UPGRADE-U3 stage 1; Engine::published()): the
/// engine-wide half of the snapshot (each scene's half is its PublishedFrame). Every field the document thread reads
/// of the engine's per-frame state today through a synchronous getter is here, written once at the close of every
/// frame as a whole-frame swap (Engine.h, THE SNAPSHOT) — stage 2 moves the readers onto it.
struct PublishedEngineFrame {
    /// ONE FRAME'S ERROR (U3 F3): the record it applied, the frame, the text.
    struct FrameError {
        unsigned long long serial = 0;
        unsigned long long frame = 0;
        std::string        text;
    };
    unsigned long long frame = 0;          ///< frames closed so far (0 = never drawn); PublishedFrame::frame's stamp
    /// The record this frame applied (FrameInput::serial). AN EMPTY HAND-OVER (a frame nothing was submitted for) is
    /// legal and carries the LAST serial — `recordApplied` false (U3 F2).
    unsigned long long serial = 0;
    bool               recordApplied = false;
    unsigned long long handOver = 0;       ///< which hand-over this frame took (FrameSubmitStatus::handOvers' count)
    /// WHAT THIS FRAME DREW (ONE-FRAME-1 stage 2; VR's door D4): the grid state — the newest step serial of the scene of
    /// the frame's first enabled presented view (the scene Ogre's frame clock follows; 0 when no view drew) — and the
    /// head pose: the session's serial of the pose this frame drew (its count of located poses; 0 = no located pose
    /// this frame, no session) at its predicted display time (ns, the runtime's clock; 0 without one). All three are
    /// the RENDER half's: the pose is located on the render thread after the hand-over, so no host writes them. A host
    /// reader (the wearer's interaction) knows which grid state and which pose a frame used.
    unsigned long long stepSerialDrawn = 0;
    unsigned long long poseSerial = 0;
    long long          displayTimeNs = 0;
    /// THE INTERPOLATION THIS FRAME DREW (U3 stage 5's rows): the weight of the newest step's nodes between their two
    /// grid states (1 = cur, every lockstep frame) and whether the frame was a free one.
    float              presentAlpha = 1.0f;
    bool               presentFree = false;
    FrameInputStatus   input;              ///< the drain's own status (the render side)
    VrStatus           vr;                 ///< the session as this frame left it (the last begun frame's pose)
    /// THE REST OF THE VR DOOR (ENGINE-UPGRADE-U3 stage 6): what the host's VR getters read — the session's state,
    /// what the last probe or session learned (VrInfo), each hand's joints as this frame located them (empty: none)
    /// and the session's suggested-binding blocks. Engine::vrState / vrInfo / vrHandJoints / vrBindingBlocks on the
    /// host's side are these fields, never a wait.
    VrState            vrState = VrState::Unavailable;
    VrInfo             vrInfo;
    std::vector<VrPose> vrJoints[VrHandCount];
    std::vector<VrBindingBlock> vrBindingBlocks;
    StreamingWork      streaming;
    AsyncShaderStats   shaders;            ///< the compile counters (`compiledInForeground` = livecompiles' total)
    bool               hasEnabledViews = false;
    bool               paceOwesWork = false;   ///< Engine::framePaceOwesWork as the frame closed it
    /// THE FRAMES RENDERED ON THE STREAMING PACE, a running total over the ENGINE's life (the pace is the engine's,
    /// Engine::setFramePace, in force until set again): a caller that holds Streaming and forgets to give Complete back
    /// shows here (the selftest asserts its frames add none). A per-view pace (SHADER-SYSTEM-1 stage 7 /
    /// PICTURE-REQUEST-1) moves it to the view's record.
    unsigned long long streamingFrames = 0;
    std::vector<ViewFrames> views;
    /// The view named `name` as this frame closed it; null when no live view has that name.
    const ViewFrames *view(NodeId id) const {
        for (const ViewFrames &v : views) if (v.id == id) return &v;
        return nullptr;
    }
    /// THE FRAME'S ERROR (U3 F3): what THIS frame wrote ("" = nothing failed). A failing CALL's error is not here — it
    /// stays Engine::lastError()'s until stage 3 makes every call a message with its own completion.
    std::string        frameError;
    /// ...and THE FRAMES' ERRORS, newest last: the last kFrameErrorsKept frames that wrote one, each keyed by its
    /// serial, with the running count of frames that ever wrote one. A reader that did not see every frame (the error
    /// pump drains once a tick; a scripted run draws many) takes the entries the count says it has not seen — an
    /// earlier error is never hidden behind a later one.
    static constexpr unsigned kFrameErrorsKept = 16;
    std::vector<FrameError> frameErrors;
    unsigned long long framesWithError = 0;
    /// THE COMMANDS' REFUSALS (decision 34): every message an engine scene refused in the last kFrameErrorsKept
    /// frames, with the reason, oldest first — the same refusals the scenes' completions carry, gathered for the one
    /// reader that wants them all (the error pump). Numbered by input.messagesRefused (the running count): a reader
    /// takes the newest `messagesRefused - taken`.
    struct Refusal {
        unsigned long long frame = 0;
        std::string        text;
    };
    std::vector<Refusal> refusals;
    /// A VR SESSION IS RUNNING as the frame closed it (decision 45, D5): what the engine learns on its own — the runtime
    /// ended the session — reaches the host here, never as a write of the host's record.
    bool vrSessionLive = false;
    /// THE ENGINE MESSAGES' COMPLETIONS (decision 39 c): the named ones' answers and every refusal, the last
    /// kFrameErrorsKept frames, by a sequence that only grows — PublishedFrame::Completion's shape.
    std::shared_ptr<const std::vector<PublishedFrame::Completion>> completions;
    bool               deviceLost = false;
    GpuFault           gpuFault = GpuFault::None;
    MonitorStatus      monitor;
    /// THE SHADOW ATLAS AND THE RENDERER'S FIGURES, EVERY FRAME (U3 stage 3): the layout, the cumulative counts and
    /// the frame timing are cheap reads and always here; the per-pass shadow counters (ShadowStatus::countersMeasured)
    /// and the geometry counting (RenderStats::metricsRecording) are MEASURED only while a reader subscribes
    /// (Engine::publishShadowStatus / publishRenderStats) — zero, and said so, otherwise.
    ShadowStatus       shadow;
    RenderStats        render;
    /// What the PREVIOUS publish cost the loop, microseconds (the snapshot's own price: the engine's half + every
    /// scene's, their GiStatus included) — read by the U3 measurements, never a bar.
    double             publishMicros = 0.0;
};

/// THE ATMOSPHERE'S TINT ON THE DIRECT SUNLIGHT — a STATELESS boundary function (ENGINE-UPGRADE-U3 stage 2, F-A;
/// it was Scene::atmosphereSunTint). The fraction of the sun's beam that survives the trip from the top of the air to
/// the observer, per channel, relative to the trip it makes at the zenith (the transmittance table's own integral:
/// the same three species, the same coefficients, the same ray), times the earth's occlusion of the disc across the
/// astronomical sunset band. Exactly (1,1,1) at noon — the user's picked colour IS the noon colour — falling, blue
/// first, as the sun goes down; white when `air.on` is false (no realistic sky). Pure: never a read of the engine.
/// `toSun` need not be unit length. A host building a record tints its sun by THE AIR THAT RECORD SETS
/// (atmosphereAirOf, below): the published air is the last frame's, so a record that changes the sky and tints by it
/// lights the new sky with the old sun for a frame (PHOTON-7 stage 7: the sun's colour then landed a frame after the
/// sky, a second change - every card relit behind the first - that the screen gather's rough lobe showed as a
/// one-frame dip on a metal sphere, scripting.e2e.sky_swap_transient's repro arm).
Colour atmosphereSunTint(const PublishedFrame::Atmosphere &air, const Vec3 &toSun);
/// THE AIR A REALISTIC SKY DESCRIBES (PHOTON-7 stage 7): the model the engine builds its tables from for `sky` (the
/// one mapping - the engine's own setSky takes its model from it) at the observer's altitude `observerKm` (the
/// engine's, Scene::published().atmosphere.observerKm: it follows the eye, a continuous input) - atmosphereSunTint's
/// input for the record that SETS this sky. `on` is true: the caller asks it of a realistic sky only.
PublishedFrame::Atmosphere atmosphereAirOf(const AtmosphereSky &sky, float observerKm);

}}  // namespace jahshaka::engine
