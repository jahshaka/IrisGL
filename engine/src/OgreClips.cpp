// Clips on the engine skeleton (ANIMATION_ENGINE_MIGRATION_SPEC).
//
// WHAT THIS FILE OWNS. The host states, per frame, which clips are active, at
// what absolute time, with what intent-weight. Everything else — sampling,
// blending, the FK over the bone hierarchy — is Ogre's, threaded and SIMD.
// The document's clip evaluator is retired; this is what replaces it.
//
// FOUR THINGS THAT ARE NOT OBVIOUS AND ARE ALL LOAD-BEARING.
//
// 1. THE KEYS ARE BIND-RELATIVE DELTAS. Ogre resets a bone to its bind pose and
//    then ACCUMULATES (SkeletonInstance::resetToPose + SkeletonTrack::
//    applyKeyFrameRigAt: finalPos += interp*w; finalScale *= lerp(1,interp,w);
//    finalRot = finalRot * nlerpShortest(w, IDENTITY, interpRot)). With w = 1
//    and final* at bind, storing
//        pos = abs - bind ; rot = inverse(bindRot)*absRot ; scale = abs/bind
//    reproduces `abs` exactly. The host sends ABSOLUTE parent-local TRS and
//    this file does the conversion, because the conversion needs the rig's bind
//    pose and the boundary should not carry it twice.
//
// 2. ATTACH EVERYTHING BEFORE ENABLING ANYTHING. SkeletonInstance::
//    mActiveAnimations holds raw SkeletonAnimation* into mAnimations, and
//    addAnimationsFromSkeleton push_backs into that same vector — reallocating
//    it — while carefully rescuing only the bone-weight buffers. It does not
//    fix up mActiveAnimations. So attaching a clip while another plays dangles
//    every active pointer. attachClips refuses while anything is enabled, and
//    says so.
//
// 3. THE CLIP DEF MUST BE BUILT FROM THE NODE'S OWN RIG, never from the clip
//    file's skeleton. addAnimationsFromSkeleton constructs each SkeletonAnimation
//    against the OTHER def's block layout and bone-to-weight map; a structural
//    mismatch is undefined behaviour, not a no-op (the header says so). Same
//    bones, same order, same hierarchy, or nothing.
//
// 4. PER-BONE WEIGHT NORMALIZATION IS OURS. Ogre's own "internal flag that
//    prevents blending unanimated bones" is not reliable per bone —
//    SkeletonAnimation::_initialize sets the whole 4-bone block to ONE whenever
//    a track uses more than half its slots. The damage lands on the OTHER clip:
//    a bone only clip A animates, blended A=0.5/B=0.5, receives half of A and
//    looks like "the character shrank slightly", never like an error. So the
//    backend records each clip's coverage at attach time, caches the weight
//    slot pointer per covered bone, and renormalizes per bone on every
//    setClipStates — one float store each.
#include "EnginePrivate.h"

#include <OgreSkeleton.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>
#include <Animation/OgreBone.h>
#include <OgreOldBone.h>
#include <OgreOldSkeletonManager.h>
#include <Animation/OgreSkeletonInstance.h>
#include <Animation/OgreSkeletonManager.h>
#include <Animation/OgreSkeletonAnimation.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace jahshaka { namespace engine { namespace detail {

namespace {

/// A clip whose length is <= 0 is padded to this, not refused: every Mixamo
/// CHARACTER download ships a single-frame T-pose clip and the Avatar page
/// selects it by default. Engine-side a zero length is `fmod(t, 0)` = NaN while
/// looping and a Debug-only assert in getKeyFramesAtTime otherwise, so the pad
/// is not cosmetic. One millisecond: short enough that no UI can scrub inside
/// it, long enough that no float division underflows.
constexpr float kMinClipLength = 1.0e-3f;

std::string clipResourceName(const std::string &key) {
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "jahClip_%016llx", h);
    return std::string(buf);
}

inline Ogre::Quaternion toOgreQuat(const Quat &q) {
    return Ogre::Quaternion(q.w, q.x, q.y, q.z);
}

inline float safeDivide(float a, float b) {
    return std::fabs(b) > 1e-8f ? a / b : 1.0f;
}

}  // namespace

// ---------------------------------------------------------------------------
const OgreScene::RigRec *OgreScene::rigOf(NodeId id) const {
    auto nit = mNodes.find(id);
    if (nit == mNodes.end() || !nit->second.meshRef) return nullptr;
    auto mit = mMeshes.find(nit->second.meshRef);
    if (mit == mMeshes.end() || mit->second.rigId.empty()) return nullptr;
    auto rit = mRigs.find(mit->second.rigId);
    return rit == mRigs.end() ? nullptr : &rit->second;
}

// ---------------------------------------------------------------------------
bool OgreScene::attachClips(NodeId id, const ClipDesc *clips, size_t count) {
    Ogre::SkeletonInstance *skel = skeletonOf(id);
    if (!skel) { mError = "attachClips: the node has no rig"; return false; }
    if (!clips && count) { mError = "attachClips: null clips"; return false; }
    const RigRec *rig = rigOf(id);
    if (!rig) { mError = "attachClips: the node's rig is unknown to this scene"; return false; }
    const size_t boneCount = rig->desc.bones.size();

    NodeClips &nc = mClips[id];

    // R2. Not a style rule — see the file header.
    if (!skel->getActiveAnimations().empty()) {
        mError = "attachClips: refusing to attach while a clip is enabled (Ogre's "
                 "addAnimationsFromSkeleton reallocates the animation vector its active "
                 "list points into, and does not fix that list up). Disable every clip first.";
        return false;
    }

    JAH_TRY {
        // The node leaves manual-bone mode the first time a clip lands on it.
        // attachSkinnedMesh marks EVERY bone manual so setBonePoses' values
        // survive resetToPose; under clips that would make every clip ADD to
        // the last pushed pose instead of replacing it.
        if (!nc.clipModeEntered) {
            for (size_t i = 0; i < skel->getNumBones(); ++i) {
                Ogre::Bone *b = skel->getBone(i);
                const bool manual = nc.manualBones.count(b->getName().c_str()) > 0;
                skel->setManualBone(b, manual);
            }
            nc.clipModeEntered = true;
        }

        for (size_t c = 0; c < count; ++c) {
            const ClipDesc &desc = clips[c];
            if (desc.id.empty()) { mError = "attachClips: a clip has no id"; return false; }
            if (desc.tracks.empty()) {
                mError = "attachClips: clip '" + desc.name + "' has no tracks";
                return false;
            }
            // Idempotent per id.
            bool already = false;
            for (const auto &rec : nc.clips) if (rec.id == desc.id) already = true;
            if (already) continue;

            // R11: validate HOST-side. The engine's own guards on ordering and
            // on a positive length are Debug-only asserts, and RelWithDebInfo
            // is a shipping configuration now.
            for (const BoneTrack &track : desc.tracks) {
                if (track.bone < 0 || size_t(track.bone) >= boneCount) {
                    mError = "attachClips: clip '" + desc.name + "' names a bone the rig does not have";
                    return false;
                }
                if (track.keys.empty()) {
                    mError = "attachClips: clip '" + desc.name + "' has an empty track";
                    return false;
                }
                for (size_t k = 1; k < track.keys.size(); ++k) {
                    if (!(track.keys[k].time > track.keys[k - 1].time)) {
                        mError = "attachClips: clip '" + desc.name +
                                 "' has key times that are not strictly increasing";
                        return false;
                    }
                }
            }

            ClipRec rec;
            rec.id = desc.id;
            rec.length = desc.length > 0.0f ? desc.length : kMinClipLength;
            if (desc.length <= 0.0f) {
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka: clip '" + desc.name + "' has length " +
                        std::to_string(desc.length) + "; padded to " +
                        std::to_string(kMinClipLength) + "s (a zero-length clip is NaN engine-side).",
                    Ogre::LML_NORMAL);
            }

            // R8: SkeletonInstance::getAnimation is a linear search that returns
            // the FIRST match and throws if absent, so a second clip of the same
            // name would be unreachable forever. Uniquified here, and the
            // mapping is what clipNames reports back.
            rec.name = desc.name.empty() ? ("clip" + std::to_string(nc.clips.size())) : desc.name;
            {
                const std::string base = rec.name;
                int suffix = 2;
                while (skel->hasAnimation(Ogre::IdString(rec.name)))
                    rec.name = base + " " + std::to_string(suffix++);
            }

            // R7: the def cache is process-lifetime and keyed by NAME, so the
            // name has to be a content hash or a re-imported clip aliases the
            // stale one forever.
            rec.defName = clipResourceName(rig->desc.id + "\x1f" + desc.id + "\x1f" + rec.name);

            Ogre::v1::OldSkeletonManager &oldMgr = Ogre::v1::OldSkeletonManager::getSingleton();
            Ogre::v1::SkeletonPtr v1clip =
                std::static_pointer_cast<Ogre::v1::Skeleton>(oldMgr.getByName(rec.defName));
            if (!v1clip) {
                // (3) in the header: built from the NODE'S rig, never from the
                // clip's own file.
                std::vector<Ogre::v1::OldBone *> made;
                v1clip = buildV1Skeleton(rec.defName, rig->desc, &made);
                if (!v1clip) {
                    mError = "attachClips: could not build the clip skeleton for '" + rec.name + "'";
                    return false;
                }

                Ogre::v1::Animation *anim = v1clip->createAnimation(rec.name, rec.length);
                for (const BoneTrack &track : desc.tracks) {
                    const BoneDesc &bd = rig->desc.bones[size_t(track.bone)];
                    Ogre::v1::OldBone *bone = made[size_t(track.bone)];
                    Ogre::v1::OldNodeAnimationTrack *nodeTrack =
                        anim->createOldNodeTrack(static_cast<unsigned short>(bone->getHandle()), bone);
                    const Ogre::Quaternion invBind = toOgreQuat(bd.bindRotation).Inverse();
                    for (const BoneKey &key : track.keys) {
                        // Times are clamped into [0, length]: a key past the end
                        // would be unreachable, and the def build's
                        // extra-keyframe-at-the-end logic keys off the length.
                        const float t = std::min(std::max(key.time, 0.0f), rec.length);
                        Ogre::v1::TransformKeyFrame *kf = nodeTrack->createNodeKeyFrame(t);
                        // (1) in the header.
                        kf->setTranslate(Ogre::Vector3(key.position.x - bd.bindPosition.x,
                                                       key.position.y - bd.bindPosition.y,
                                                       key.position.z - bd.bindPosition.z));
                        kf->setRotation(invBind * toOgreQuat(key.rotation));
                        kf->setScale(Ogre::Vector3(safeDivide(key.scale.x, bd.bindScale.x),
                                                   safeDivide(key.scale.y, bd.bindScale.y),
                                                   safeDivide(key.scale.z, bd.bindScale.z)));
                    }
                }
                // Registers the def under the v1 resource's name. The subsequent
                // addAnimationsFromSkeleton(name, group) finds it in the def map
                // and never touches the resource system.
                Ogre::SkeletonManager::getSingleton().getSkeletonDef(v1clip.get());
            } else {
                Ogre::SkeletonManager::getSingleton().getSkeletonDef(v1clip.get());
            }

            const size_t before = skel->getAnimations().size();
            skel->addAnimationsFromSkeleton(rec.defName,
                                            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            if (skel->getAnimations().size() != before + 1) {
                mError = "attachClips: the clip def produced " +
                         std::to_string(skel->getAnimations().size() - before) +
                         " animations, expected exactly 1";
                return false;
            }
            rec.index = before;

            Ogre::SkeletonAnimation &sa = skel->getAnimationsNonConst()[rec.index];

            // R10: FRAMES == SECONDS, and only because SkeletonManager hardcodes
            // frameRate = 1.0f in both of its def constructors. If that ever
            // changes upstream, SkeletonAnimationDef::build DOUBLE-applies the
            // rate (mFrame = timestamp * rate, then fTime = mFrame * rate when
            // resampling) and every clip is silently sampled at the wrong time —
            // a mis-timed character, not a compile error. Checked here, once per
            // clip def, so it screams instead.
            if (std::fabs(float(sa.getNumFrames()) - rec.length) > 1e-3f) {
                mError = "attachClips: the engine's clip def reports " +
                         std::to_string(sa.getNumFrames()) + " frames for a " +
                         std::to_string(rec.length) +
                         "s clip. Frames are no longer seconds (SkeletonManager's hardcoded "
                         "frameRate = 1.0f changed); every clip time would be wrong.";
                return false;
            }

            sa.setEnabled(false);
            sa.setLoop(true);
            // All weighting is per bone (4). Leaving the clip-global weight at 1
            // makes clipBoneWeights the complete truth about what was applied.
            sa.mWeight = 1.0f;

            // Coverage + the cached weight slots. getBoneWeightPtr returns null
            // for a bone the clip does not animate — which is exactly how
            // coverage is discovered, without a second source of truth.
            for (const BoneTrack &track : desc.tracks) {
                const std::string &boneName = rig->desc.bones[size_t(track.bone)].name;
                Ogre::Real *ptr = sa.getBoneWeightPtr(Ogre::IdString(boneName));
                if (!ptr) continue;
                rec.coverage.push_back(track.bone);
                rec.weightPtr.push_back(ptr);
            }
            nc.clips.push_back(rec);
        }
        return true;
    } JAH_CATCH(mError, false);
}

// ---------------------------------------------------------------------------
std::vector<std::string> OgreScene::clipNames(NodeId id) const {
    auto it = mClips.find(id);
    if (it == mClips.end()) return {};
    std::vector<std::string> out;
    out.reserve(it->second.clips.size());
    for (const auto &rec : it->second.clips) out.push_back(rec.name);
    return out;
}

// ---------------------------------------------------------------------------
bool OgreScene::setClipStates(NodeId id, const ClipState *states, size_t count) {
    Ogre::SkeletonInstance *skel = skeletonOf(id);
    if (!skel) { mError = "setClipStates: the node has no rig"; return false; }
    if (!states && count) { mError = "setClipStates: null states"; return false; }
    auto it = mClips.find(id);
    if (it == mClips.end()) { mError = "setClipStates: the node has no clips"; return false; }
    NodeClips &nc = it->second;
    const RigRec *rig = rigOf(id);
    if (!rig) { mError = "setClipStates: the node's rig is unknown"; return false; }

    JAH_TRY {
        // ---- resolve intent ------------------------------------------------
        std::vector<float> raw(nc.clips.size(), 0.0f);
        std::vector<char>  on(nc.clips.size(), 0);
        for (size_t s = 0; s < count; ++s) {
            const ClipState &st = states[s];
            size_t idx = nc.clips.size();
            for (size_t i = 0; i < nc.clips.size(); ++i)
                if (nc.clips[i].name == st.name) { idx = i; break; }
            if (idx == nc.clips.size()) {
                mError = "setClipStates: no clip named '" + st.name + "' on this node";
                return false;
            }
            if (!st.enabled) continue;
            if (!(st.weight >= 0.0f)) {
                mError = "setClipStates: clip '" + st.name + "' has a negative or NaN weight";
                return false;
            }
            on[idx] = 1;
            raw[idx] = st.weight;
        }
        // A set that is all-zero is REFUSED rather than silently producing a
        // bind-pose character: it is always a caller bug and it is invisible.
        {
            bool anyEnabled = false, anyWeight = false;
            for (size_t i = 0; i < nc.clips.size(); ++i) {
                if (!on[i]) continue;
                anyEnabled = true;
                if (raw[i] > 0.0f) anyWeight = true;
            }
            if (anyEnabled && !anyWeight) {
                mError = "setClipStates: every enabled clip has weight 0";
                return false;
            }
        }

        // ---- (4) per-bone normalization ------------------------------------
        const size_t boneCount = rig->desc.bones.size();
        std::vector<float> perBoneTotal(boneCount, 0.0f);
        for (size_t i = 0; i < nc.clips.size(); ++i) {
            if (!on[i]) continue;
            for (int bone : nc.clips[i].coverage) perBoneTotal[size_t(bone)] += raw[i];
        }
        for (size_t i = 0; i < nc.clips.size(); ++i) {
            ClipRec &rec = nc.clips[i];
            for (size_t k = 0; k < rec.coverage.size(); ++k) {
                const size_t bone = size_t(rec.coverage[k]);
                float w = 0.0f;
                if (on[i] && perBoneTotal[bone] > 0.0f) w = raw[i] / perBoneTotal[bone];
                // R6: a manual bone is NOT reset to bind, so an enabled clip
                // would ADD to whatever the host wrote through setBonePoses —
                // "IK overrides the head" would silently become "IK plus the
                // clip's head motion". Zero here is what makes an override an
                // override.
                if (nc.manualBones.count(rig->desc.bones[bone].name)) w = 0.0f;
                *rec.weightPtr[k] = w;
            }
        }

        // ---- times, loop, enable ------------------------------------------
        for (size_t i = 0; i < nc.clips.size(); ++i) {
            ClipRec &rec = nc.clips[i];
            Ogre::SkeletonAnimation &sa = skel->getAnimationsNonConst()[rec.index];
            if (!on[i]) { if (sa.getEnabled()) sa.setEnabled(false); continue; }
            const ClipState *st = nullptr;
            for (size_t s = 0; s < count; ++s) if (states[s].name == rec.name) st = &states[s];
            if (!st) continue;
            sa.setLoop(st->looping);
            // ABSOLUTE time, always. setFrame is pure — fmod when looping, clamp
            // otherwise, and the per-track key cache searches both directions —
            // so the same t gives the same pose in any order, forever. addTime
            // is deliberately not reachable from this boundary.
            sa.setTime(st->time);
            if (!sa.getEnabled()) sa.setEnabled(true);
        }
        return true;
    } JAH_CATCH(mError, false);
}

// ---------------------------------------------------------------------------
bool OgreScene::setBoneManual(NodeId id, const std::string &bone, bool manual) {
    Ogre::SkeletonInstance *skel = skeletonOf(id);
    if (!skel) { mError = "setBoneManual: the node has no rig"; return false; }
    JAH_TRY {
        Ogre::Bone *target = nullptr;
        for (size_t i = 0; i < skel->getNumBones(); ++i) {
            Ogre::Bone *b = skel->getBone(i);
            if (b->getName() == Ogre::String(bone)) { target = b; break; }
        }
        if (!target) { mError = "setBoneManual: no bone named '" + bone + "'"; return false; }
        NodeClips &nc = mClips[id];
        if (manual) nc.manualBones.insert(bone);
        else        nc.manualBones.erase(bone);
        // Before any clip is attached every bone is manual by construction
        // (attachSkinnedMesh) and clearing one would strand it at bind.
        if (nc.clipModeEntered) skel->setManualBone(target, manual);
        return true;
    } JAH_CATCH(mError, false);
}

// ---------------------------------------------------------------------------
bool OgreScene::bonePoses(NodeId id, BonePose *out, size_t count) const {
    Ogre::SkeletonInstance *skel = skeletonOf(id);
    if (!skel) { mError = "bonePoses: the node has no rig"; return false; }
    const RigRec *rig = rigOf(id);
    if (!rig) { mError = "bonePoses: the node's rig is unknown"; return false; }
    if (!out || count != skel->getNumBones() || count != rig->desc.bones.size()) {
        mError = "bonePoses: bone count does not match the rig";
        return false;
    }
    JAH_TRY {
        // `_getLocalSpaceTransform` is the bone's derived transform "as if the
        // skeleton weren't attached to a SceneNode" — i.e. in the MESH NODE's
        // frame, which is exactly the frame setBonePoses writes in and the frame
        // the document's own skin matrices live in. (_getDerivedTransform folds
        // the node's world matrix in and would be the wrong one.)
        std::vector<Ogre::Matrix4> derived(count);
        for (size_t i = 0; i < count; ++i)
            skel->getBone(i)->_getLocalSpaceTransform().store(&derived[i]);

        for (size_t i = 0; i < count; ++i) {
            const int parent = rig->desc.bones[i].parent;
            Ogre::Matrix4 local = derived[i];
            if (parent >= 0 && size_t(parent) < count)
                local = derived[size_t(parent)].inverseAffine() * derived[i];
            Ogre::Vector3 p, s;
            Ogre::Quaternion r;
            local.decomposition(p, s, r);
            out[i].position = Vec3(p.x, p.y, p.z);
            out[i].rotation = Quat(r.x, r.y, r.z, r.w);
            out[i].scale    = Vec3(s.x, s.y, s.z);
        }
        return true;
    } JAH_CATCH(mError, false);
}

// ---------------------------------------------------------------------------
std::vector<float> OgreScene::clipBoneWeights(NodeId id, const std::string &clip) const {
    auto it = mClips.find(id);
    if (it == mClips.end()) return {};
    const RigRec *rig = rigOf(id);
    if (!rig) return {};
    for (const auto &rec : it->second.clips) {
        if (rec.name != clip) continue;
        std::vector<float> out(rig->desc.bones.size(), 0.0f);
        for (size_t k = 0; k < rec.coverage.size(); ++k)
            out[size_t(rec.coverage[k])] = *rec.weightPtr[k];
        return out;
    }
    return {};
}

}}}  // namespace jahshaka::engine::detail
