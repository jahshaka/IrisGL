// Mesh creation, update and destruction, plus the v2 geometry builder.
#include "EnginePrivate.h"

#include <OgreLodStrategy.h>
#include <OgreLodStrategyManager.h>
#include <OgreViewport.h>
#include <OgreLodStrategyPrivate.inl>

#include <string>
#include <algorithm>
#include <unordered_map>

namespace jahshaka { namespace engine { namespace detail {

// ---- ATOM stage 1: THE VIEW'S LOD RULE, EVALUATED PER PASS ------------------
//
// THE STRATEGY IS OURS BECAUSE THE QUANTITY IS OURS. Ogre ships four: two
// distance strategies, whose per-object value carries no projection term at
// all, and two pixel-count ones, whose value is a projected AREA. Ours is the
// only one that speaks the currency the bake produces — a world-space DEVIATION
// — so the mesh's thresholds can be the BAKED ERRORS THEMSELVES and the level
// the renderer picks is `lodLevelForWorldError` (Types.h) evaluated by Ogre's
// own four-wide SoA loop:
//
//     value = (distance(centre, eye) - radius) * 2 * budget / (proj[1][1] * H)
//
// i.e. the world-space error that covers `budget` pixels at THIS pass's camera
// and THIS pass's render target. Every pass evaluates its own: the view, a
// planar reflector's virtual camera, a probe cube face, a 256-pixel thumbnail,
// and each eye of a headset. A 2160x2376 VR eye gets the same PIXEL error as
// the desktop window, which is the whole point (the reference-projection
// version got twice it).
//
// WHAT WE DO NOT DO, and why it is not a workaround: nothing here reaches into
// the pin. `LodStrategy` is the engine's documented extension point,
// `LodStrategy::lodSet` is public and is the ONE function MovableObject grants
// friendship to, and `LodStrategyManager::addStrategy` takes ownership exactly
// as it does for upstream's four. The only pin change this lane makes is
// ogre-patch 0075, which adds the hysteresis band `lodSet` has no way to
// express (the switch is a step in both directions otherwise).
//
// AT 1080 LINES AND 45 DEGREES THIS IS ARITHMETICALLY THE OLD RULE: the stage-1
// thresholds were `error * proj11 * 0.5 * 1080 / budget` and the strategy's
// value was `distance - radius`, so the comparison is the same one multiplied
// through by the same constant. Every rig picture at 1920x1080 is therefore
// byte-identical; an OFFSCREEN shot at 640x360 is NOT, and correctly so — a
// third of the lines is a third of the pixels an error covers.
namespace {

class JahWorldErrorLodStrategy final : public Ogre::LodStrategy
{
public:
    JahWorldErrorLodStrategy() : Ogre::LodStrategy("jah_world_error") {}

    /// The finest level's threshold: no view can afford a negative deviation.
    Ogre::Real getBaseValue() const override { return Ogre::Real(0); }

    /// The value is multiplied by the bias, so a factor passes through: larger
    /// affords more error, i.e. swaps to a coarser level sooner.
    Ogre::Real transformBias(Ogre::Real factor) const override { return factor; }

    Ogre::Real getValueImpl(const Ogre::MovableObject *object,
                            const Ogre::Camera *camera) const override
    {
        const Ogre::Real perPixel = worldPerPixel(camera) * camera->_getLodBiasInverse();
        if (camera->getProjectionType() == Ogre::PT_ORTHOGRAPHIC) return perPixel;
        const Ogre::Real d = object->getWorldAabb().mCenter.distance(camera->getDerivedPosition()) -
                             object->getWorldRadius();
        return std::max(d, Ogre::Real(0)) * perPixel;
    }

    void lodUpdateImpl(const size_t numNodes, Ogre::ObjectData objData,
                       const Ogre::Camera *camera, Ogre::Real bias,
                       Ogre::Real hysteresis) const override
    {
        OGRE_ALIGNED_DECL(Ogre::Real, lodValues[ARRAY_PACKED_REALS], OGRE_SIMD_ALIGNMENT);
        const Ogre::Real perPixel = worldPerPixel(camera) * camera->_getLodBiasInverse() * bias;

        // ORTHOGRAPHIC: one pixel is the same world length everywhere in the
        // frustum, so the allowed error does not depend on the object at all
        // and the distance term must NOT enter — an orthographic view that
        // simplified what is far from the camera would be a plain defect.
        if (camera->getProjectionType() == Ogre::PT_ORTHOGRAPHIC) {
            const Ogre::ArrayReal flat(Ogre::Mathlib::SetAll(perPixel));
            for (size_t i = 0; i < numNodes; i += ARRAY_PACKED_REALS) {
                CastArrayToReal(lodValues, flat);
                lodSet(objData, lodValues, hysteresis);
                objData.advanceLodPack();
            }
            return;
        }

        Ogre::ArrayVector3 cameraPos;
        cameraPos.setAll(camera->_getCachedDerivedPosition());
        const Ogre::ArrayReal scale(Ogre::Mathlib::SetAll(perPixel));
        const Ogre::ArrayReal zero(Ogre::Mathlib::SetAll(Ogre::Real(0)));

        for (size_t i = 0; i < numNodes; i += ARRAY_PACKED_REALS) {
            Ogre::ArrayReal *RESTRICT_ALIAS worldRadius =
                reinterpret_cast<Ogre::ArrayReal * RESTRICT_ALIAS>(objData.mWorldRadius);
            // The SAME quantity the distance strategy computes — the distance
            // from the bounding SPHERE — turned into a world error by one
            // scalar the whole pass shares.
            Ogre::ArrayReal v = objData.mWorldAabb->mCenter.distance(cameraPos) - (*worldRadius);
            v = Ogre::Mathlib::Max(v, zero) * scale;
            CastArrayToReal(lodValues, v);
            // The band is THIS PASS'S (ogre-patch 0075): the value is the same
            // arithmetic for every pass, the band is not.
            lodSet(objData, lodValues, hysteresis);
            objData.advanceLodPack();
        }
    }

private:
    /// The world-space error one metre of view distance can hide at this
    /// camera's projection and its render target's height (a length per metre
    /// of distance); for an orthographic camera, the world length of one pixel
    /// outright. Constant across a pass, so it is computed once per
    /// `lodUpdateImpl` rather than per object.
    ///
    /// THE ARITHMETIC IS THE QUALITY CURRENCY'S (Types.h, SUB-ERROR): one
    /// sample's world footprint at one metre, times the budget in samples. It
    /// used to be spelled out here; the spelling is now shared with the
    /// cascade, the card and the GPU cull, and `engine.lod_rule_parity` holds
    /// the GLSL copy to it.
    ///
    /// WHAT IT STILL DOES NOT DO, and it is a defect of the strategy rather
    /// than of the currency: it passes no `meshToWorldScale`. Ogre's LOD values
    /// are per MESH (`applyLodValues`, which writes `MeshData::lodBounds`
    /// straight in) and the strategy's value is a WORLD length, so a 10x-scaled
    /// instance is compared against a bound measured in mesh units and takes a
    /// level whose real deviation is ten times what it asked for. The cascade's
    /// rule (the voxel gather, `OgreScene::cascadeGatherInputs`) and the GPU cull both divide by the
    /// instance's scale; this one cannot without moving the picture of every
    /// scaled instance, so the fix is a lane with a pixel gate of its own.
    /// (ATOM-SUBSTRATE-1 finding, 2026-09-22.)
    static Ogre::Real worldPerPixel(const Ogre::Camera *camera)
    {
        // A pass whose camera has never been given a viewport cannot be
        // measured; the pin's own pixel strategies dereference it blind. The
        // reference height keeps such a pass on the stage-1 numbers instead of
        // crashing, and there is no such pass in this engine today.
        const Ogre::Viewport *vp = camera->getLastViewport();
        const Ogre::Real height = vp ? Ogre::Real(vp->getActualHeight()) : Ogre::Real(1080);
        if (!(height > 0.0f)) return Ogre::Real(0);
        if (camera->getProjectionType() == Ogre::PT_ORTHOGRAPHIC) {
            const Ogre::Real orthoH = camera->getOrthoWindowHeight();
            return allowedWorldError(kLodBudgetPixels,
                                     sampleFootprintOrtho(float(orthoH), float(height)));
        }
        const Ogre::Matrix4 &proj = camera->getProjectionMatrix();
        const Ogre::Real p11 = proj[1][1];
        // One metre of distance: the per-object value multiplies this by its own.
        return allowedWorldError(
            kLodBudgetPixels, sampleFootprintPerspective(1.0f, float(p11), float(height)));
    }
};

}   // namespace

// THE SWITCH HYSTERESIS (ATOM-3 A7, ogre-patch 0075), as a fraction of the
// threshold being crossed. Upstream's `lodSet` flips at the exact threshold in
// both directions, so an object parked on one changes level every frame the
// camera dithers — 71 pops in 600 frames on a camera oscillating by 2 % of the
// switch distance; 0 with this band, and every real transition kept at 20 %
// (spikes/atom-3/FINDINGS.md §4). 0.10 holds the level across a 10 % window of
// the switch distance, which is ~0.6 m of dolly travel at the measured pose and
// is bounded by construction: a value genuinely past the band switches on the
// frame it gets there.
//
// IT IS A PER-PASS NUMBER (ATOM-3-FIX; patch 0075 amended): it reaches
// `lodSet` from the PASS DEFINITION that asked for the LOD update, so the
// watched view carries it and a planar reflector's mirrored camera, a PiP
// inset, a probe cube face and a thumbnail do not (they take the exact level
// their own value asks for, which is what keeps a capture assertable). The
// route is `ChainDesc::lodHysteresis` -> `chain::build`'s sweep over the
// view's own scene passes -> `CompositorPassSceneDef::mLodHysteresis`. The
// first version of the band was process-wide state on the strategy, which made
// the direction state one slot per OBJECT shared by every camera in the frame
// — see the patch header.
static const float kLodHysteresis = 0.10f;

// What a WATCHED view's scene passes get, read once (the run-wide diagnostic
// latch every measurable engine rule in this tree carries — JAHSHAKA_NO_RAY_QUERY,
// JAHSHAKA_NO_CASCADE_LOD — so the band's A/B is a run of the shipped binary and
// not a build). Zero everywhere else, by construction.
float jahLodHysteresis()
{
    static const float band =
        std::getenv("JAHSHAKA_NO_LOD_HYSTERESIS") == nullptr ? kLodHysteresis : 0.0f;
    return band;
}

// (THE SUITE'S WAY IN IS NO LONGER HERE — LOD-LATCH-1, 2026-09-18. A band is
// only ever given to a view a person watches over time, and `View::readPixels`
// refuses an on-screen view, so the suite that reads what the band does needs an
// offscreen view WITH a band. That used to be a process-wide env latch,
// `JAHSHAKA_LOD_HYSTERESIS_OFFSCREEN`, read once by a function-local static; it
// is now `View::setLodHysteresisOffscreen`, a per-view field beside PostFx's,
// the overlay's and the PiP's own `allowOffscreen` — one picture's property, in
// the description of that picture, settable by a host. Unset it still is
// everywhere else: every thumbnail, preview, screenshot and pixel suite takes
// the exact level, frame after frame.)

// Registered once per process, before any mesh's LOD values are written
// (`applyLodValues` reads the default strategy's base value) and before any
// Item exists (`Item::_initialise` caches the value array's address). The
// manager OWNS what it is given, exactly as it owns upstream's four.
void installJahLodStrategy()
{
    Ogre::LodStrategyManager &mgr = Ogre::LodStrategyManager::getSingleton();
    if (mgr.getStrategy("jah_world_error") == nullptr)
        mgr.addStrategy(OGRE_NEW JahWorldErrorLodStrategy());
    mgr.setDefaultStrategy("jah_world_error");
}

// ---- Meshes ----
MeshId OgreScene::createMesh(const MeshData &data) {
    if (data.positions.empty() || data.positions.size() % 3 != 0) { mError = "createMesh: positions must be xyz triples"; return 0; }
    if (data.indices.empty() || data.indices.size() % 3 != 0)     { mError = "createMesh: indices must be triangles"; return 0; }
    const size_t nv = data.vertexCount();
    for (unsigned i : data.indices) if (i >= nv) { mError = "createMesh: index out of range"; return 0; }
    // THE LOD LEVELS ARE VALIDATED HERE AND NOWHERE ELSE (ATOM inventory row
    // AT-DUP as amended by the lane's audit). `buildMeshV2` used to re-walk every
    // index of every level and silently END THE CHAIN at the first bad one; 1.7
    // deleted that because the BAKE is the validator and a bake cannot deliver a
    // malformed level (`MeshBake::readMesh` refuses the blob). But `createMesh` is
    // a PUBLIC boundary and three suites hand-build `lodIndices`, so the deletion
    // left a hand-built level going straight to the GPU as an out-of-range index
    // buffer — a driver fault, not a message. One O(indices) pass per upload, the
    // same price level 0 already pays, and the answer is a REFUSAL with a reason.
    for (size_t L = 0; L < data.lodIndices.size(); ++L) {
        const std::vector<unsigned> &level = data.lodIndices[L];
        if (level.empty() || level.size() % 3 != 0) {
            mError = "createMesh: LOD level " + std::to_string(L + 1) +
                     " is not a positive whole number of triangles";
            return 0;
        }
        for (unsigned i : level)
            if (i >= nv) {
                mError = "createMesh: LOD level " + std::to_string(L + 1) +
                         " names vertex " + std::to_string(i) + " of " + std::to_string(nv);
                return 0;
            }
    }
    if (!data.lodBounds.empty() && data.lodBounds.size() != data.lodIndices.size()) {
        mError = "createMesh: one bound per LOD level, or none at all";
        return 0;
    }
    if (!data.normals.empty() && data.normals.size() != data.positions.size()) { mError = "createMesh: normals count mismatch"; return 0; }
    if (!data.uvs.empty() && data.uvs.size() != nv * 2) { mError = "createMesh: uv count mismatch"; return 0; }
    if (!data.blendIndices.empty() && !data.hasSkinData()) {
        mError = "createMesh: blend indices/weights must be 4 per vertex, both present";
        return 0;
    }
    JAH_TRY {
        MeshRec rec; rec.name = processUniqueName("mesh");
        rec.dynamic = data.dynamic;
        rec.hasSkinData = data.hasSkinData();
        for (unsigned char b : data.blendIndices)
            rec.maxBlendIndex = std::max(rec.maxBlendIndex, unsigned(b));
        rec.mesh = buildMeshV2(rec.name, data, data.dynamic ? &rec.interleaved : nullptr);
        // ATOM stage 1: kept so a LOD-bias change can re-derive the switch
        // distances. Exactly as many entries as the mesh got extra VAOs — which
        // is every level it was given, since the levels were validated above and
        // `buildMeshV2` no longer drops any.
        if (!data.lodBounds.empty() && rec.mesh && rec.mesh->getNumSubMeshes() > 0) {
            const size_t levels = rec.mesh->getSubMesh(0)->mVao[Ogre::VpNormal].size();
            if (levels > 1)
                rec.lodBounds.assign(data.lodBounds.begin(),
                                     data.lodBounds.begin() + ptrdiff_t(std::min(levels - 1, data.lodBounds.size())));
        }
        // SURFACE-CACHE phase 1 -> 2: the same index for the mesh's CARDS. The
        // list is the bake's, in mesh space, and it is stored whole — the cache
        // is the only consumer and it re-derives everything world-side per
        // instance, so nothing here is transformed or filtered.
        if (!data.cards.empty() && rec.mesh) mCardsByMesh[rec.mesh.get()] = data.cards;
        // ...and the by-Ogre-mesh INDEX, which is how a consumer holding only an
        // `Ogre::Item *` — the cascade voxeliser — reaches the record (ATOM stage
        // 1). An index and not a copy of the bounds (AT-DUP), which is why it is
        // filled after the record is in the map.
        const bool chained = !rec.lodBounds.empty() && rec.mesh;
        Ogre::Mesh *const ogreMesh = rec.mesh.get();
        mMeshes[++mNextMeshId] = std::move(rec);
        if (chained) mMeshIdByOgreMesh[ogreMesh] = mNextMeshId;
        return mNextMeshId;
    } JAH_CATCH(mError, 0);
}

bool OgreScene::updateMeshVertices(MeshId id, const std::vector<float> &positions,
                                   const std::vector<float> &normals) {
    auto it = mMeshes.find(id);
    if (it == mMeshes.end()) { mError = "updateMeshVertices: unknown mesh"; return false; }
    MeshRec &rec = it->second;
    if (!rec.dynamic) { mError = "updateMeshVertices: mesh was not created with MeshData::dynamic"; return false; }
    const size_t nv = rec.interleaved.size() / 12;
    if (positions.size() != nv * 3) { mError = "updateMeshVertices: positions count mismatch"; return false; }
    if (!normals.empty() && normals.size() != nv * 3) { mError = "updateMeshVertices: normals count mismatch"; return false; }
    JAH_TRY {
        Ogre::Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
        for (size_t v = 0; v < nv; ++v) {
            float *dst = &rec.interleaved[v * 12];
            dst[0] = positions[v*3]; dst[1] = positions[v*3+1]; dst[2] = positions[v*3+2];
            if (!normals.empty()) { dst[3] = normals[v*3]; dst[4] = normals[v*3+1]; dst[5] = normals[v*3+2]; }
            mn.makeFloor(Ogre::Vector3(dst[0], dst[1], dst[2]));
            mx.makeCeil(Ogre::Vector3(dst[0], dst[1], dst[2]));
        }
        // BT_DEFAULT buffers upload through a staging buffer — correct on Vulkan
        // (Ogre inserts the transfer + barriers); no mapping, no frame coupling.
        Ogre::VertexArrayObject *vao = rec.mesh->getSubMesh(0)->mVao[Ogre::VpNormal][0];
        Ogre::VertexBufferPacked *vbuf = vao->getVertexBuffers()[0];
        vbuf->upload(rec.interleaved.data(), 0, Ogre::uint32(nv));
        // Culling reads bounds cached per Item, not the mesh's: refresh both.
        const Ogre::Aabb aabb = Ogre::Aabb::newFromExtents(mn, mx);
        rec.mesh->_setBounds(aabb, false);
        rec.mesh->_setBoundingSphereRadius(aabb.getRadius());
        for (auto &kv : mNodes)
            if (kv.second.meshRef == id && kv.second.item) {
                kv.second.item->setLocalAabb(aabb);
                // New vertex data is a new caster shape even when the bounds did
                // not move (lamp-map cache): its lamps re-render in the frame it
                // lands — flagged in the walk this function already makes.
                markShadowShapeDirty(kv.second);
            }
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::destroyMesh(MeshId id) {
    auto it = mMeshes.find(id);
    if (it == mMeshes.end()) return false;
    JAH_TRY {
        invalidateGiCaches();   // BEFORE the mesh dies: IR frees its by-VAO caches now
        for (auto &kv : mNodes) if (kv.second.meshRef == id) detachItem(kv.first, kv.second);
        if (it->second.mesh) { mMeshIdByOgreMesh.erase(it->second.mesh.get()); mCardsByMesh.erase(it->second.mesh.get()); }
        it->second.mesh.reset();
        Ogre::MeshManager &mm = Ogre::MeshManager::getSingleton();
        if (mm.resourceExists(it->second.name)) mm.remove(it->second.name);
        mMeshes.erase(it);
        return true;
    } JAH_CATCH(mError, false);
}

MeshId OgreScene::createLineMesh(const std::vector<Vec3> &points, bool strip) {
    if (points.size() < 2) { mError = "createLineMesh: need at least 2 points"; return 0; }
    JAH_TRY {
        MeshRec rec; rec.name = processUniqueName("lines");
        // LINE MESHES CARRY NO TANGENTS (position/normal/uv only, below). They
        // are overlay geometry — grid, wires, gizmos — and never wear a
        // normal-mapped PBR material, but attachMesh's I-6 refusal reads this
        // flag rather than assuming, so it has to be honest.
        rec.hasTangents = false;
        const size_t nv = points.size();
        struct V { float px = 0.f, py = 0.f, pz = 0.f, nx = 0.f, ny = 0.f, nz = 0.f,
                         u = 0.f, v = 0.f; };
        V *verts = reinterpret_cast<V *>(OGRE_MALLOC_SIMD(sizeof(V) * nv, Ogre::MEMCATEGORY_GEOMETRY));
        Ogre::Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
        for (size_t i = 0; i < nv; ++i) {
            verts[i] = { points[i].x, points[i].y, points[i].z, 0, 1, 0, 0, 0 };
            mn.makeFloor(toOgre(points[i])); mx.makeCeil(toOgre(points[i]));
        }
        Ogre::VaoManager *vaoMgr = mRoot->getRenderSystem()->getVaoManager();
        Ogre::VertexElement2Vec decl;
        decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION));
        decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_NORMAL));
        decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES));
        Ogre::VertexBufferPacked *vbuf = vaoMgr->createVertexBuffer(decl, Ogre::uint32(nv), Ogre::BT_IMMUTABLE, verts, true);
        Ogre::VertexBufferPackedVec vbufs; vbufs.push_back(vbuf);
        Ogre::VertexArrayObject *vao = vaoMgr->createVertexArrayObject(
            vbufs, nullptr, strip ? Ogre::OT_LINE_STRIP : Ogre::OT_LINE_LIST);
        rec.mesh = Ogre::MeshManager::getSingleton().createManual(rec.name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        Ogre::SubMesh *sub = rec.mesh->createSubMesh();
        sub->mVao[Ogre::VpNormal].push_back(vao);
        sub->mVao[Ogre::VpShadow].push_back(vao);
        const Ogre::Aabb aabb = Ogre::Aabb::newFromExtents(mn, mx);
        rec.mesh->_setBounds(aabb, false);
        rec.mesh->_setBoundingSphereRadius(std::max(aabb.getRadius(), 0.001f));
        mMeshes[++mNextMeshId] = rec;
        return mNextMeshId;
    } JAH_CATCH(mError, 0);
}

namespace {

/// The shadow-caster VAO (POST_CHAIN_SPEC §11), built in O(n).
///
/// Ogre's own VertexShadowMapHelper does this by reading the finished vertex and
/// index buffers back from the GPU and comparing EVERY vertex against every other
/// one (OgreVertexShadowMapHelper.cpp:302-315). We still hold the source arrays,
/// so the same job is a hash pass: build the position-only (plus blend
/// indices/weights) vertex, look it up, keep the first occurrence, remap the
/// indices. Same output, no readback, linear.
///
/// Returns null when there is nothing to gain (the caller then aliases the main
/// VAO, which is Ogre's "useSameVaos" fallback).
/// The optimized shadow VAO: a position-only (plus blend indices/weights) vertex
/// buffer with duplicate vertices merged, and ONE VAO over it — LEVEL 0's. An
/// EMPTY return means "no optimized form", and the caller aliases the normal VAOs
/// for every level instead.
///
/// ONE, NOT ONE PER LEVEL, SINCE ogre-patch 0088 (ATOM inventory row AT-A11).
/// `SubMesh::destroyShadowMappingVaos` used to decide ALIAS-versus-INDEPENDENT
/// for the whole shadow list from one test on entry 0, so a MIXED list — an
/// independent VAO at 0 and aliases above it — read as independent: it destroyed
/// the aliased entries and `~SubMesh` destroyed the same VAOs and their shared
/// vertex buffer a second time ("Vertex Buffer has already been destroyed or
/// doesn't belong to this VaoManager", measured 2026-09-15 by the suite that came
/// with this feature). Only the two pure shapes were legal, so this built one
/// independent VAO per level to stay inside one of them. Patch 0088 makes the
/// alias test per entry, and the mixed list — which is the shape a LOD chain
/// wants — is legal.
///
/// WHY THE MIXED LIST IS THE RIGHT SHAPE, and not merely the newly-allowed one:
/// the optimized form exists so a shadow pass streams 12 bytes per vertex instead
/// of 48, and its value is proportional to the vertex fetch the pass actually
/// does. Each level halves its triangles, so the coarse levels of every mesh in a
/// scene together account for a vanishing share of that fetch — while an
/// independent index buffer and VertexArrayObject per level per mesh is VRAM for
/// the life of the mesh. So level 0 gets the shrunk buffer and the coarse levels
/// alias their own normal VAOs (the CALLER does the aliasing — this function
/// returns the one VAO it built).
std::vector<Ogre::VertexArrayObject *> buildShadowVaos(
    Ogre::VaoManager *vaoMgr, const MeshData &data, const std::vector<float> &blendW,
    bool skinned, const std::vector<const std::vector<unsigned> *> &levels) {
    const size_t nv = data.vertexCount(), ni = data.indices.size();
    if (nv == 0 || ni == 0 || levels.empty()) return {};

    // Element ORDER matches Ogre's: POSITION, BLEND_INDICES, BLEND_WEIGHTS.
    Ogre::VertexElement2Vec decl;
    decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION));
    if (skinned) {
        decl.push_back(Ogre::VertexElement2(Ogre::VET_UBYTE4, Ogre::VES_BLEND_INDICES));
        decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT4, Ogre::VES_BLEND_WEIGHTS));
    }
    const size_t stride = Ogre::VaoManager::calculateVertexSize(decl);

    // One scratch vertex, then the compacted buffer. Both are plain bytes: the
    // dedup key IS the byte pattern, exactly as Ogre's memcmp compares it.
    std::vector<unsigned char> packed(stride * nv);
    for (size_t v = 0; v < nv; ++v) {
        unsigned char *dst = &packed[v * stride];
        std::memcpy(dst, &data.positions[v * 3], sizeof(float) * 3);
        if (skinned) {
            std::memcpy(dst + 12, &data.blendIndices[v * 4], 4);
            std::memcpy(dst + 16, &blendW[v * 4], sizeof(float) * 4);
        }
    }

    // hash -> candidate vertex indices (collisions resolved by memcmp).
    std::unordered_multimap<size_t, unsigned> seen;
    seen.reserve(nv);
    std::vector<unsigned> remap(nv, 0);
    std::vector<unsigned char> unique;
    unique.reserve(stride * nv);
    unsigned uniqueCount = 0;
    for (size_t v = 0; v < nv; ++v) {
        const unsigned char *src = &packed[v * stride];
        size_t h = 1469598103934665603ull;                    // FNV-1a
        for (size_t b = 0; b < stride; ++b) { h ^= src[b]; h *= 1099511628211ull; }
        unsigned hit = 0xFFFFFFFFu;
        auto range = seen.equal_range(h);
        for (auto it = range.first; it != range.second; ++it) {
            if (std::memcmp(&unique[size_t(it->second) * stride], src, stride) == 0) {
                hit = it->second;
                break;
            }
        }
        if (hit == 0xFFFFFFFFu) {
            hit = uniqueCount++;
            unique.insert(unique.end(), src, src + stride);
            seen.emplace(h, hit);
        }
        remap[v] = hit;
    }

    // Nothing merged AND nothing narrower to stream: only true when the mesh is
    // already position-only, which our layout never is — keep the guard anyway.
    if (uniqueCount == 0) return {};

    unsigned char *vertexData = reinterpret_cast<unsigned char *>(
        OGRE_MALLOC_SIMD(stride * uniqueCount, Ogre::MEMCATEGORY_GEOMETRY));
    std::memcpy(vertexData, unique.data(), stride * uniqueCount);

    Ogre::VertexBufferPacked *vbuf = nullptr;
    Ogre::IndexBufferPacked *ibuf = nullptr;
    try {
        vbuf = vaoMgr->createVertexBuffer(decl, Ogre::uint32(uniqueCount), Ogre::BT_IMMUTABLE,
                                          vertexData, true);
    } catch (...) {
        // On failure the buffer never took ownership — free it and fall back to
        // the aliased VAO rather than losing the mesh.
        OGRE_FREE_SIMD(vertexData, Ogre::MEMCATEGORY_GEOMETRY);
        return {};
    }

    // The index type follows the SHADOW vertex count, which can only shrink.
    std::vector<Ogre::VertexArrayObject *> out;
    Ogre::VertexBufferPackedVec shadowVbufs;
    shadowVbufs.push_back(vbuf);
    {
        const std::vector<unsigned> *level = levels.front();   // LEVEL 0, and only it
        const size_t count = level->size();
        Ogre::IndexBufferPacked *ibuf = nullptr;
        if (uniqueCount <= 65535u) {
            Ogre::uint16 *idx = reinterpret_cast<Ogre::uint16 *>(
                OGRE_MALLOC_SIMD(sizeof(Ogre::uint16) * count, Ogre::MEMCATEGORY_GEOMETRY));
            for (size_t i = 0; i < count; ++i) idx[i] = Ogre::uint16(remap[(*level)[i]]);
            ibuf = vaoMgr->createIndexBuffer(Ogre::IndexBufferPacked::IT_16BIT, Ogre::uint32(count),
                                             Ogre::BT_IMMUTABLE, idx, true);
        } else {
            Ogre::uint32 *idx = reinterpret_cast<Ogre::uint32 *>(
                OGRE_MALLOC_SIMD(sizeof(Ogre::uint32) * count, Ogre::MEMCATEGORY_GEOMETRY));
            for (size_t i = 0; i < count; ++i) idx[i] = remap[(*level)[i]];
            ibuf = vaoMgr->createIndexBuffer(Ogre::IndexBufferPacked::IT_32BIT, Ogre::uint32(count),
                                             Ogre::BT_IMMUTABLE, idx, true);
        }
        out.push_back(vaoMgr->createVertexArrayObject(shadowVbufs, ibuf, Ogre::OT_TRIANGLE_LIST));
    }
    return out;
}

}   // namespace

void OgreScene::applyLodValues(const Ogre::MeshPtr &mesh, const std::vector<float> &bounds) const {
    // THE THRESHOLDS ARE THE BAKED BOUNDS THEMSELVES (ATOM-3 A1, and the bound
    // rather than the simplifier's error since ATOM-BAKE-1's AT-A5). The
    // strategy's per-object value is the world-space deviation that view can
    // afford (JahWorldErrorLodStrategy, above), so `lodSet`'s `lower_bound - 1`
    // over this array IS `lodLevelForWorldError` — one rule, no second copy of
    // it, and no reference projection baked into a distance.
    //
    // THE BIAS DIVIDES THEM, which is the same dial it always was seen from the
    // other side: a bias above 1 shrinks every threshold, so a given view
    // affords a coarser level sooner. 0 means NEVER swap, and the only way to
    // say that in an ascending array is a threshold nothing can reach.
    //
    // Ascending with the strategy's base value first — what LodStrategy::lodSet
    // binary-searches. Patch 0059 is what makes this expressible at all:
    // Mesh::mLodValues is protected and _setLodInfo's body is commented out
    // upstream.
    Ogre::Mesh::LodValueArray values;
    values.push_back(Ogre::LodStrategyManager::getSingleton().getDefaultStrategy()->getBaseValue());
    float previous = values[0];
    for (float error : bounds) {
        // Monotonic by construction (the bake accumulates), but a blob that is
        // not strictly increasing would make a level unreachable rather than
        // wrong — nudge instead of trusting.
        float v = (mLodBias > 0.0f && error > 0.0f && std::isfinite(error))
                      ? error / mLodBias
                      : std::numeric_limits<float>::max();
        if (!(v > previous)) v = std::nextafter(previous, std::numeric_limits<float>::max());
        values.push_back(v);
        previous = v;
    }
    mesh->_setLodValues(values);
}

// WHICH LEVEL EVERY DRAWN OBJECT IS ON (ATOM P1's readout — the gap OWN-TRI left).
// `mCurrentMeshLod` is the byte the render queue indexes the VAO list with, so this
// reads the DECISION and not a re-derivation of it: there is no camera here, no
// threshold walk and no bias — asking the strategy again from outside would be a
// second answer that could disagree with the picture.
void OgreScene::objectLods(std::vector<ObjectLodDesc> &out) const {
    out.clear();
    out.reserve(mNodes.size());
    for (const auto &kv : mNodes) {
        const Ogre::Item *item = kv.second.item;
        if (!item) continue;
        const Ogre::Mesh *mesh = item->getMesh().get();
        if (!mesh || mesh->getNumSubMeshes() == 0) continue;
        ObjectLodDesc d;
        d.node = kv.first;
        d.name = item->getName();
        d.level = unsigned(item->getCurrentMeshLod());
        d.levels = unsigned(mesh->getSubMesh(0)->mVao[Ogre::VpNormal].size());
        if (d.levels == 0) d.levels = 1;
        for (unsigned si = 0; si < mesh->getNumSubMeshes(); ++si) {
            const auto &vaos = mesh->getSubMesh(si)->mVao[Ogre::VpNormal];
            if (vaos.empty()) continue;
            // The clamp is the render queue's own: a level past the end of a
            // sub-mesh's list draws its coarsest.
            const size_t pick = std::min(size_t(d.level), vaos.size() - 1u);
            d.triangles += (unsigned long long)(vaos[pick]->getPrimitiveCount() / 3u);
        }
        out.push_back(d);
    }
}

// The VAO-list SHAPE, for the suite that has to see what ogre-patch 0088 bought
// (AT-A11). Counting the shadow entries that are NOT in the normal list is the same
// test the patched `destroyShadowMappingVaos` makes, which is the point: the number
// this reports is the number of VAOs and index buffers the mesh really owns.
bool OgreScene::meshVaoShape(MeshId mesh, unsigned &levels, unsigned &shadowIndependent) const {
    levels = 0;
    shadowIndependent = 0;
    const auto it = mMeshes.find(mesh);
    if (it == mMeshes.end() || !it->second.mesh || it->second.mesh->getNumSubMeshes() == 0)
        return false;
    const Ogre::SubMesh *sub = it->second.mesh->getSubMesh(0);
    levels = unsigned(sub->mVao[Ogre::VpNormal].size());
    for (Ogre::VertexArrayObject *v : sub->mVao[Ogre::VpShadow]) {
        const auto &normal = sub->mVao[Ogre::VpNormal];
        if (std::find(normal.begin(), normal.end(), v) == normal.end()) ++shadowIndependent;
    }
    return true;
}

void OgreScene::setLodBias(float bias) {
    if (!(bias >= 0.0f)) bias = 0.0f;
    if (bias == mLodBias) return;
    mLodBias = bias;
    // Every Item holds a POINTER to its mesh's value array (Item::_initialise),
    // so rewriting the arrays in place moves every instance in the scene with
    // no rebuild and no re-attach. The array LENGTH never changes here.
    for (auto &kv : mMeshes) {
        if (kv.second.lodBounds.empty() || !kv.second.mesh) continue;
        applyLodValues(kv.second.mesh, kv.second.lodBounds);
    }
}

Ogre::MeshPtr OgreScene::buildMeshV2(const std::string &name, const MeshData &data,
                                     std::vector<float> *interleavedOut) {
    const size_t nv = data.vertexCount(), ni = data.indices.size();
    std::vector<float> normals = data.normals;
    if (normals.empty()) {
        // Smooth normals from face normals — enough for a lit preview.
        normals.assign(nv * 3, 0.0f);
        for (size_t t = 0; t + 2 < ni; t += 3) {
            const unsigned a = data.indices[t], b = data.indices[t+1], c = data.indices[t+2];
            const float *pa = &data.positions[a*3], *pb = &data.positions[b*3], *pc = &data.positions[c*3];
            const float e1[3] = { pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2] };
            const float e2[3] = { pc[0]-pa[0], pc[1]-pa[1], pc[2]-pa[2] };
            const float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
            for (unsigned v : { a, b, c }) for (int k = 0; k < 3; ++k) normals[v*3+k] += n[k];
        }
        for (size_t v = 0; v < nv; ++v) {
            float *n = &normals[v*3];
            const float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; } else { n[1] = 1.0f; }
        }
    }
    std::vector<float> tangents = data.tangents;
    if (tangents.size() != nv * 4) {
        // Lengyel accumulation from uvs; a fixed frame when there are no uvs
        // (normal maps are meaningless without uvs anyway).
        tangents.assign(nv * 4, 0.0f);
        std::vector<float> bitan(nv * 3, 0.0f);
        if (!data.uvs.empty()) {
            for (size_t t = 0; t + 2 < ni; t += 3) {
                const unsigned a = data.indices[t], b = data.indices[t+1], c = data.indices[t+2];
                const float *pa = &data.positions[a*3], *pb = &data.positions[b*3], *pc = &data.positions[c*3];
                const float *ua = &data.uvs[a*2], *ub = &data.uvs[b*2], *uc = &data.uvs[c*2];
                const float e1[3] = { pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2] };
                const float e2[3] = { pc[0]-pa[0], pc[1]-pa[1], pc[2]-pa[2] };
                const float s1 = ub[0]-ua[0], t1 = ub[1]-ua[1], s2 = uc[0]-ua[0], t2 = uc[1]-ua[1];
                const float det = s1*t2 - s2*t1;
                if (std::fabs(det) < 1e-12f) continue;
                const float r = 1.0f / det;
                const float T[3] = { (t2*e1[0]-t1*e2[0])*r, (t2*e1[1]-t1*e2[1])*r, (t2*e1[2]-t1*e2[2])*r };
                const float B[3] = { (s1*e2[0]-s2*e1[0])*r, (s1*e2[1]-s2*e1[1])*r, (s1*e2[2]-s2*e1[2])*r };
                for (unsigned v : { a, b, c }) for (int k = 0; k < 3; ++k) {
                    tangents[v*4+k] += T[k]; bitan[v*3+k] += B[k];
                }
            }
        }
        for (size_t v = 0; v < nv; ++v) {
            const float *n = &normals[v*3];
            float *t = &tangents[v*4];
            // Gram-Schmidt against the normal, then normalize.
            const float ndt = n[0]*t[0] + n[1]*t[1] + n[2]*t[2];
            float tx = t[0]-n[0]*ndt, ty = t[1]-n[1]*ndt, tz = t[2]-n[2]*ndt;
            const float len = std::sqrt(tx*tx + ty*ty + tz*tz);
            if (len > 1e-8f) { tx /= len; ty /= len; tz /= len; }
            else { // any unit vector orthogonal to n
                if (std::fabs(n[0]) < 0.9f) { tx = 1.0f-n[0]*n[0]; ty = -n[0]*n[1]; tz = -n[0]*n[2]; }
                else                        { tx = -n[1]*n[0]; ty = 1.0f-n[1]*n[1]; tz = -n[1]*n[2]; }
                const float l2 = std::sqrt(tx*tx + ty*ty + tz*tz);
                tx /= l2; ty /= l2; tz /= l2;
            }
            const float cx = n[1]*tz - n[2]*ty, cy = n[2]*tx - n[0]*tz, cz = n[0]*ty - n[1]*tx;
            const float *b = &bitan[v*3];
            t[0] = tx; t[1] = ty; t[2] = tz;
            t[3] = (cx*b[0] + cy*b[1] + cz*b[2]) < 0.0f ? -1.0f : 1.0f;
        }
    }
    // GPU skinning (GPU_SKINNING_SPEC R8): blend indices and weights are REAL
    // vertex elements, appended after the uvs exactly where
    // SubMesh::_compileBoneAssignments would have put them — built once, here,
    // with no readback and no second buffer. The shader reads
    // `uvec4 blendIndices` (VET_UBYTE4 maps to R8G8B8A8_UINT on Vulkan, not a
    // normalized format) and `vec4 blendWeights`; the influence count comes from
    // the WEIGHTS element's type count (OgreHlms.cpp:3066-3067), so FLOAT4 is
    // what makes hlms_bones_per_vertex 4.
    const bool skinned = data.hasSkinData();
    // Ogre's vertex shader does a plain weighted sum with NO renormalisation
    // (800.VertexShader_piece_vs.any:97-127), and our importer stores assimp's
    // weights raw. Weights that don't sum to 1 shrink or inflate the character,
    // so normalise here, once, at upload.
    std::vector<float> blendW;
    if (skinned) {
        blendW = data.blendWeights;
        for (size_t v = 0; v < nv; ++v) {
            float *w = &blendW[v*4];
            const float sum = w[0] + w[1] + w[2] + w[3];
            if (sum > 1e-6f) { for (int k = 0; k < 4; ++k) w[k] /= sum; }
            else { w[0] = 1.0f; w[1] = w[2] = w[3] = 0.0f; }   // unweighted: ride bone 0
        }
    }
    struct V { float px = 0.f, py = 0.f, pz = 0.f, nx = 0.f, ny = 0.f, nz = 0.f,
                     tx = 0.f, ty = 0.f, tz = 0.f, tw = 0.f, u = 0.f, v = 0.f; };
    struct VS { V base; unsigned char bi[4] = {}; float bw[4] = {}; };
    const size_t vertexBytes = skinned ? sizeof(VS) : sizeof(V);
    unsigned char *raw = reinterpret_cast<unsigned char *>(
        OGRE_MALLOC_SIMD(vertexBytes * nv, Ogre::MEMCATEGORY_GEOMETRY));
    Ogre::Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    for (size_t v = 0; v < nv; ++v) {
        const float *p = &data.positions[v*3];
        const float *n = &normals[v*3];
        const float *t = &tangents[v*4];
        V base = { p[0], p[1], p[2], n[0], n[1], n[2], t[0], t[1], t[2], t[3],
                   data.uvs.empty() ? 0.0f : data.uvs[v*2], data.uvs.empty() ? 0.0f : data.uvs[v*2+1] };
        if (skinned) {
            VS *dst = reinterpret_cast<VS *>(raw + v * sizeof(VS));
            dst->base = base;
            for (int k = 0; k < 4; ++k) { dst->bi[k] = data.blendIndices[v*4+k]; dst->bw[k] = blendW[v*4+k]; }
        } else {
            *reinterpret_cast<V *>(raw + v * sizeof(V)) = base;
        }
        mn.makeFloor(Ogre::Vector3(p[0], p[1], p[2])); mx.makeCeil(Ogre::Vector3(p[0], p[1], p[2]));
    }
    if (interleavedOut) {
        // The CPU-skinning cache only exists for non-skinned dynamic meshes
        // (updateMeshVertices indexes a 12-float stride).
        interleavedOut->resize(nv * 12);
        for (size_t v = 0; v < nv; ++v)
            std::memcpy(&(*interleavedOut)[v * 12], raw + v * vertexBytes, sizeof(V));
    }
    Ogre::VaoManager *vaoMgr = mRoot->getRenderSystem()->getVaoManager();
    Ogre::VertexElement2Vec decl;
    decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION));
    decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_NORMAL));
    decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT4, Ogre::VES_TANGENT));
    decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES));
    if (skinned) {
        decl.push_back(Ogre::VertexElement2(Ogre::VET_UBYTE4, Ogre::VES_BLEND_INDICES));
        decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT4, Ogre::VES_BLEND_WEIGHTS));
    }
    // Dynamic (CPU-skinned) meshes get BT_DEFAULT: BufferPacked::upload() then
    // rewrites it through a staging buffer, fully synchronised on Vulkan.
    // BT_DYNAMIC_* was deliberately NOT used: those buffers cycle through
    // triple-buffered sections and must be re-mapped and re-filled EVERY frame
    // or a stale section shows; a mesh whose pose pauses would flicker back
    // N frames. Index buffer stays immutable either way.
    Ogre::VertexBufferPacked *vbuf = vaoMgr->createVertexBuffer(
        decl, Ogre::uint32(nv), data.dynamic ? Ogre::BT_DEFAULT : Ogre::BT_IMMUTABLE, raw, true);
    Ogre::IndexBufferPacked *ibuf = nullptr;
    if (nv <= 65535u) {
        Ogre::uint16 *idx = reinterpret_cast<Ogre::uint16 *>(OGRE_MALLOC_SIMD(sizeof(Ogre::uint16) * ni, Ogre::MEMCATEGORY_GEOMETRY));
        for (size_t i = 0; i < ni; ++i) idx[i] = Ogre::uint16(data.indices[i]);
        ibuf = vaoMgr->createIndexBuffer(Ogre::IndexBufferPacked::IT_16BIT, Ogre::uint32(ni), Ogre::BT_IMMUTABLE, idx, true);
    } else {
        Ogre::uint32 *idx = reinterpret_cast<Ogre::uint32 *>(OGRE_MALLOC_SIMD(sizeof(Ogre::uint32) * ni, Ogre::MEMCATEGORY_GEOMETRY));
        for (size_t i = 0; i < ni; ++i) idx[i] = data.indices[i];
        ibuf = vaoMgr->createIndexBuffer(Ogre::IndexBufferPacked::IT_32BIT, Ogre::uint32(ni), Ogre::BT_IMMUTABLE, idx, true);
    }
    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::SubMesh *sub = mesh->createSubMesh();
    Ogre::VertexBufferPackedVec vbufs; vbufs.push_back(vbuf);
    Ogre::VertexArrayObject *vao = vaoMgr->createVertexArrayObject(vbufs, ibuf, Ogre::OT_TRIANGLE_LIST);
    sub->mVao[Ogre::VpNormal].push_back(vao);

    // ---- ATOM stage 1: the LOD chain (SPECS/NANITE_SPEC.md §7) -------------
    //
    // One index buffer and one VAO per extra level, all from the SAME `vbuf`.
    // That is what keeps the levels inside one draw call: VaoManager::findVao
    // matches on {opType, indexBufferVbo, indexType, vertexBuffers}, so levels
    // built back to back out of the same index pool share a vaoName, and
    // RenderQueue::render then advances `drawCmd->numDraws` instead of emitting
    // a second draw command (finding B). The index TYPE follows the same
    // vertex-count test as level 0 for the same reason — a 32-bit level beside
    // a 16-bit one would be a different vaoName by itself.
    //
    // A DYNAMIC (CPU-skinned) mesh never gets levels: updateMeshVertices
    // rewrites mVao[VpNormal][0]'s buffer alone, so a coarser level would keep
    // drawing the bind pose. The bake builds no chain for a skinned mesh
    // either; this is the second lock on the same door.
    //
    // `accepted` is the ONE list both VAO lists are built from — the two lists
    // must not disagree about how long the chain is: the render queue indexes
    // both with one mCurrentMeshLod.
    //
    // THE LEVELS ARE NOT VALIDATED HERE (ATOM inventory row AT-DUP). There is ONE
    // validator on this path and it is `createMesh`, at the public boundary, where
    // a malformed level becomes a refusal with a reason instead of a driver fault.
    // This function used to walk every index of every level AGAIN and silently end
    // the chain at the first bad one — a second validator, with a different
    // remedy, over data the first one has already refused. What is still asserted
    // is the one thing this function owns: that the chain's LENGTH matches the
    // number of bounds it will publish as switch thresholds.
    std::vector<const std::vector<unsigned> *> accepted;
    accepted.push_back(&data.indices);
    if (!data.dynamic && data.lodBounds.size() == data.lodIndices.size())
        for (const std::vector<unsigned> &level : data.lodIndices)
            accepted.push_back(&level);
    for (size_t L = 1; L < accepted.size(); ++L) {
        const std::vector<unsigned> &level = *accepted[L];
        Ogre::IndexBufferPacked *lodIbuf = nullptr;
        if (nv <= 65535u) {
            Ogre::uint16 *idx = reinterpret_cast<Ogre::uint16 *>(
                OGRE_MALLOC_SIMD(sizeof(Ogre::uint16) * level.size(), Ogre::MEMCATEGORY_GEOMETRY));
            for (size_t i = 0; i < level.size(); ++i) idx[i] = Ogre::uint16(level[i]);
            lodIbuf = vaoMgr->createIndexBuffer(Ogre::IndexBufferPacked::IT_16BIT,
                                                Ogre::uint32(level.size()), Ogre::BT_IMMUTABLE, idx, true);
        } else {
            Ogre::uint32 *idx = reinterpret_cast<Ogre::uint32 *>(
                OGRE_MALLOC_SIMD(sizeof(Ogre::uint32) * level.size(), Ogre::MEMCATEGORY_GEOMETRY));
            for (size_t i = 0; i < level.size(); ++i) idx[i] = level[i];
            lodIbuf = vaoMgr->createIndexBuffer(Ogre::IndexBufferPacked::IT_32BIT,
                                                Ogre::uint32(level.size()), Ogre::BT_IMMUTABLE, idx, true);
        }
        sub->mVao[Ogre::VpNormal].push_back(
            vaoMgr->createVertexArrayObject(vbufs, lodIbuf, Ogre::OT_TRIANGLE_LIST));
    }

    // Shadow-caster VAO optimization (POST_CHAIN_SPEC.md §11). Aliasing the SAME
    // vao into both slots is what Ogre calls "useSameVaos": shadow passes then
    // stream the full 48-byte vertex (68 skinned) when they need 12 (+8). The
    // optimized form is a position-only (plus blend indices/weights) buffer with
    // duplicate vertices merged — extra VRAM per mesh, cheaper shadow passes.
    //
    // WE BUILD IT OURSELVES instead of calling Mesh::prepareForShadowMapping():
    // Ogre's VertexShadowMapHelper::shrinkVertexBuffer finds duplicates with a
    // NESTED LOOP over every vertex pair (OgreVertexShadowMapHelper.cpp:302-315,
    // O(n^2) memcmp), and it reads the vertex and index buffers back from the GPU
    // through AsyncTickets to do it. Measured on the Matcaps sample's 89k-vertex
    // Stanford Dragon: 10.2 SECONDS inside one createMesh — 82% of a 12.5 s scene
    // open, and the reason "opening a scene takes forever" (lane-openasync,
    // 2026-09-03). The same de-duplication with a hash is linear and needs no
    // readback: we still hold the source data. ~10.2 s -> ~40 ms for that mesh.
    //
    // NEVER for `data.dynamic` (CPU-skinned) meshes: updateMeshVertices uploads
    // the new pose into mVao[VpNormal][0]'s buffer ONLY. While VpShadow aliases
    // it, CPU-skinned shadows follow the deformation for free; give those meshes
    // an independent optimized shadow VAO and their shadows freeze at the pose
    // the mesh was built with — silent and visually confusing. GPU-skinned
    // meshes are fine: they deform in the vertex shader, which the optimized
    // buffer keeps the blend indices/weights for.
    std::vector<Ogre::VertexArrayObject *> shadowVaos;
    if (Ogre::Mesh::msOptimizeForShadowMapping && !data.dynamic)
        shadowVaos = buildShadowVaos(vaoMgr, data, blendW, skinned, accepted);
    if (shadowVaos.size() == 1) {
        // THE MIXED LIST (ogre-patch 0088): the shrunk VAO for level 0, and every
        // coarse level ALIASING its own normal VAO. Correct geometry at every
        // level — a shadow pass at level k still draws level k's triangles — for
        // one shadow vertex buffer and one shadow index buffer per mesh instead
        // of one per level.
        sub->mVao[Ogre::VpShadow].push_back(shadowVaos.front());
        for (size_t L = 1; L < sub->mVao[Ogre::VpNormal].size(); ++L)
            sub->mVao[Ogre::VpShadow].push_back(sub->mVao[Ogre::VpNormal][L]);
    } else {
        // ALL-ALIASED, when there is no optimized form to build at all.
        for (Ogre::VertexArrayObject *v : sub->mVao[Ogre::VpNormal])
            sub->mVao[Ogre::VpShadow].push_back(v);
    }

    // The LOD VALUE array: what LodStrategy::lodSet binary-searches every frame.
    // `distance_sphere` is the process-wide strategy (Mesh2::mLodStrategyName is
    // stored and never consulted — SceneManager::updateAllLodsThread asks
    // LodStrategyManager for the DEFAULT), and its SoA path sets the value to
    // (distance(worldAabbCentre, eye) - worldRadius) * bias, so the values are
    // switch DISTANCES, ascending, with 0 first. Set before any Item exists:
    // Item::_initialise caches this array's address.
    if (accepted.size() > 1) {
        std::vector<float> bounds(data.lodBounds.begin(),
                                  data.lodBounds.begin() + ptrdiff_t(accepted.size() - 1));
        applyLodValues(mesh, bounds);
    }
    const Ogre::Aabb aabb = Ogre::Aabb::newFromExtents(mn, mx);
    mesh->_setBounds(aabb, false);
    mesh->_setBoundingSphereRadius(aabb.getRadius());
    return mesh;
}

}}}  // namespace jahshaka::engine::detail
