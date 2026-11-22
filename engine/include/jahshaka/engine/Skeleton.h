#pragma once
// THE ENGINE's SKELETON TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <cstddef>
#include <string>
#include <vector>
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

// ---- Rigs (GPU_SKINNING_SPEC) ----------------------------------------------
/// One bone of a rig, in its BIND pose. The transform is LOCAL to the parent
/// bone; a root bone (parent < 0) is local to the mesh node the rig deforms.
struct BoneDesc {
    std::string name;
    int         parent = -1;      ///< index into SkeletonDesc::bones, -1 = root
    Vec3        bindPosition;
    Quat        bindRotation;
    Vec3        bindScale{1.0f, 1.0f, 1.0f};
};

/// A rig: bones in the order MeshData::blendIndices names. Order is otherwise
/// free — a parent may follow its child — but the hierarchy must be acyclic.
///
/// `id` must be derived from the STRUCTURE ONLY — the ordered bone names, the
/// hierarchy and the bind transforms — and never from the source file, the
/// clip set or anything else. The backend caches the translated rig by this id
/// for the life of the process, so (a) two loads of the same rig from different
/// files must resolve to ONE cached rig (which is what lets clips authored in
/// one file drive a character loaded from another), and (b) a rig that differs
/// in any bone must get a different id or it silently aliases the cached one.
struct SkeletonDesc {
    std::string           id;
    std::vector<BoneDesc> bones;
};

/// What a scene's rigs COST right now (AVATAR_RIG_PERF_SPEC §3.5).
///
/// Every field is a number the rig-perf program makes a claim about, so every
/// field is readable from a suite rather than inferred from a log:
///
///   rigged        nodes carrying a skinned renderable.
///   instances     DISTINCT SkeletonInstances behind them — the count Ogre
///                 evaluates in updateAllAnimations, i.e. one per PIECE until
///                 pieces share, one per CHARACTER after.
///   shared        nodes rendering from somebody else's instance
///                 (`rigged - instances` unless a follower's master went away).
///   streamedBones the bone matrices HlmsPbs streams per pass, summed over the
///                 rigged nodes: each node contributes the length of its
///                 blend-index map, which is the whole rig under the identity
///                 map and only the piece's own bones under a compacted one.
struct RigStats {
    size_t rigged = 0;
    size_t instances = 0;
    size_t shared = 0;
    size_t streamedBones = 0;
};

/// WHAT THE SCENE'S STRUCTURAL EDITS COST, IN COUNTS (ENGINE-UPGRADE-1 stage 2): running totals
/// since the scene was created, so a caller reads a delta around the edit it makes. The
/// reparent of a document subtree out of the scene and back in (the mirror's eviction then
/// re-adoption) destroys and re-creates one Item per mesh node; the GI side of those edits must
/// stay O(the edit), never one rebuild request per node (mirror.reparent_local reads these).
struct SceneChangeStats {
    unsigned long long itemsCreated = 0;        ///< Items attachMesh created
    unsigned long long itemsDestroyed = 0;      ///< Items destroyed with their node (removeNode)
    unsigned long long giInvalidations = 0;     ///< GI cache invalidations requested (invalidateGiCaches)
    unsigned long long probeBuildRestarts = 0;  ///< staged probe builds rewound to their first stage
    /// Creates that came WITHOUT a host-reserved id (the engine reserved one): 0 for a scene a mirror feeds — every
    /// command it gives names its id (U3 stage 3, mirror.messages_in_order).
    unsigned long long engineAssignedIds = 0;
};

/// WHAT THIS SCENE'S MOBILITY LOOKS LIKE (SPECS/REALTIME_REFLECTIONS_SPEC.md
/// §3.3, lane R1). The host resolves every node's mobility from the document
/// (drivers, parents, the user's setting) and pushes the answer with
/// Scene::setNodeMovable; these are what the engine RECORDED, and the way to
/// see that a push landed. The renderer SPENDS it on the CHANNEL alone: a
/// movable object leaves the probe captures (a flip stales the probe grid with
/// reason Mobility) and stays GI geometry - the voxel store holds it at its live
/// pose, by region (PHOTON-7 stage 8A), so no flip is a GI edge and none costs a
/// rebuild (the old `mobilityRebuilds` counter and the play-time soft
/// promotion's MobilityChange are deleted with that edge).
struct MobilityStatus {
    size_t movableItems = 0;    ///< movable nodes carrying drawable geometry
    size_t movableLights = 0;   ///< movable nodes carrying a light
    size_t movableNodes = 0;    ///< every node the host marked movable
};

/// A posed bone: LOCAL to its parent bone (a root bone: local to the mesh node).
/// This is absolute local TRS, not a delta from the bind pose.
struct BonePose {
    Vec3 position;
    Quat rotation;
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// ---- Clips (ANIMATION_ENGINE_MIGRATION_SPEC) --------------------------------
/// One key of one bone track. The TRS is ABSOLUTE and LOCAL TO THE PARENT BONE
/// — the host has already composed away any pivot chain the source file had
/// (iris::ClipExtractor). Times in SECONDS, strictly increasing.
///
/// NOT a delta from the bind pose: the backend converts to the bind-relative
/// form the engine accumulates in, because that conversion needs the rig's bind
/// pose and the boundary should not make the host carry it twice.
struct BoneKey {
    float time = 0.0f;
    Vec3  position;
    Quat  rotation;
    Vec3  scale{1.0f, 1.0f, 1.0f};
};

/// One bone's track. `bone` indexes SkeletonDesc::bones.
struct BoneTrack {
    int                  bone = -1;
    std::vector<BoneKey> keys;
};

/// A clip, ready to attach to a node's rig.
///
/// `id` MUST be a content hash of the rig id and every track. The backend
/// caches the translated clip under it for the LIFE OF THE PROCESS and the
/// cache is by name, so an id derived from anything else (a file path, a clip
/// name, an asset guid) makes a re-imported clip alias the old one forever —
/// the same failure mode as the VCT datablock-pointer cache.
///
/// `name` is what setClipStates and clipNames speak. It is uniquified per node
/// at attach time if it collides, and the mapping is reported by clipNames.
///
/// `length` in seconds. A length <= 0 is PADDED to a minimum, not refused:
/// engine-side a zero length is fmod(t, 0) = NaN, so the padding is not
/// cosmetic. It is a GUARD — a clip whose keys span no time and whose file
/// declares no duration. The one-frame T-pose every Mixamo character download
/// ships arrives one frame long since smoke L10 item 2 (the document keeps the
/// file's declared duration), so it no longer reaches the pad.
struct ClipDesc {
    std::string            id;
    std::string            name;
    float                  length = 0.0f;
    std::vector<BoneTrack> tracks;
};

/// What the host asserts about one clip, this frame. ABSOLUTE time only —
/// there is deliberately no addTime on this boundary, because a relative clock
/// makes every pose assertion order-dependent.
///
/// `weight` is raw INTENT. The backend normalizes PER BONE from the clips'
/// coverage (a bone only clip A animates gets all of A at any weight split,
/// never half of it) and reports the result through clipBoneWeights.
struct ClipState {
    std::string name;
    bool        enabled = true;
    float       time    = 0.0f;
    float       weight  = 1.0f;
    bool        looping = true;
};

}}  // namespace jahshaka::engine
