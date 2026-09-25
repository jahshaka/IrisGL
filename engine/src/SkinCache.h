// THE GPU SKIN CACHE (PHOTON-SKIN-1, RY-R4; SPECS/photon/C4_RAYS_BEYOND_REFLECTIONS_DESIGN.md §2).
//
// WHAT IT IS. A rigged Item draws its bind-pose vertex buffer through Ogre's
// vertex shader, which skins every vertex per pass — so the triangles the
// picture shows exist nowhere in memory. Everything that reads geometry OUTSIDE
// a raster pass (a bottom-level acceleration structure, the ray hit's geometric
// normal, the voxeliser's rows) used to read the BIND POSE: a walking character
// would have reflected and cast a ray shadow as a T-pose, which is why skinned
// items were excluded from the traced set (audit C-5).
//
// The cache is a SECOND vertex buffer per rigged Item, in the raster's own
// 48-byte layout (float3 position, float3 normal, float4 tangent, float2 uv —
// OgreMesh.cpp's `V`), written by the `Jahshaka/SkinCache` compute job
// (media/Hlms/Jahshaka/JahSkinCache_cs.glsl) on each frame the item's POSE moved.
// The Item's VAO is not changed: Ogre keeps skinning the picture. The cache is
// the copy the rays see — the item's own bottom-level structure is built from it
// (and refit when the pose moves), and its geometry rows are the GPU scene's
// per-instance ROW OVERRIDE (`GpuInstance::raster[2]`), so a reader of rows reads
// the posed triangles.
//
// POSITIONS ARE IN THE ITEM'S LOCAL SPACE. A BLAS is built in object space and
// the TLAS instance carries the node's world transform; Ogre's bone matrices are
// in world space (Bone::_getFullTransform = node x derived x reverse bind), so
// the host takes them back once per bone (the node's inverse world, on the CPU).
// Moving the character therefore costs no skin pass and no refit — only a pose
// change does.
//
// This header declares the BUFFER half, which lives beside the Item's own
// buffers in OgreMesh.cpp; the dispatch lives with its consumer, the ray tier
// (OgreRayQuery.cpp).
#ifndef JAHSHAKA_ENGINE_SKINCACHE_H
#define JAHSHAKA_ENGINE_SKINCACHE_H

#include <OgrePrerequisites.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Ogre {
class Item;
class VaoManager;
class VertexBufferPacked;
}  // namespace Ogre

namespace jahshaka {
namespace engine {
namespace detail {

/// The cache's vertex: the raster's layout, 12 floats.
constexpr uint32_t kSkinCacheStride = 48u;

/// One `Jahshaka/SkinCache` job record, std430, 32 bytes — JahSkinCache_cs.glsl's
/// `SkinJob`, word for word.
struct SkinJobRecord {
    uint32_t sourceRow = 0u;        ///< the MESH's geometry row (level 0, submesh 0)
    uint32_t vertexCount = 0u;
    uint32_t paletteBase = 0u;      ///< first palette ROW (vec4) of this item's bones
    uint32_t cacheAddressLo = 0u;
    uint32_t cacheAddressHi = 0u;
    uint32_t tangentOffset = 0xFFFFFFFFu;   ///< source byte offsets beyond what the row says
    /// The blend-index element's byte offset (low 16 bits) and the blend-weight
    /// element's (high 16 bits) — a vertex is far shorter than 64 KB.
    uint32_t blendOffsets = 0u;
    /// Bones in this item's palette (its blend-index map's length): the job
    /// clamps a blend index into it, so a malformed index reads this item's last
    /// bone and never the next item's rows.
    uint32_t boneCount = 0u;
};
static_assert(sizeof(SkinJobRecord) == 32, "the skin job's record is a shader contract");

/// ONE ITEM'S CACHE BUFFER and what the job needs to know about the source vertex
/// beyond its geometry row.
struct SkinCacheBuffer {
    Ogre::VertexBufferPacked *vertices = nullptr;
    uint32_t vertexCount = 0u;
    uint64_t address = 0u;          ///< device address of vertex 0 (VaoManager's)
    uint32_t tangentOffset = 0xFFFFFFFFu;
    uint32_t blendIndexOffset = 0u;
    uint32_t blendWeightOffset = 0u;
};

/// Creates `item`'s cache: a device-local vertex buffer of the level-0 vertex
/// count in the raster's layout, with a device address (the pools carry the
/// STORAGE and SHADER_DEVICE_ADDRESS bits — fork a480b5e2f / patch 0039) and the
/// acceleration-structure build-input bit where the device has rays. Refuses (with
/// `err`) an item whose level-0 source is not what the job reads: one submesh, one
/// vertex buffer holding a float3 position, a float4-able blend-weight element and
/// a UBYTE4 blend-index element, 4-byte aligned — which is exactly what
/// `buildMeshV2` builds for every skinned mesh this engine has.
bool createSkinCacheBuffer(Ogre::VaoManager *vao, const Ogre::Item *item, SkinCacheBuffer &out,
                           std::string &err);
/// Destroys it (Ogre's delayed destruction keeps it for the frames in flight).
void destroySkinCacheBuffer(Ogre::VaoManager *vao, SkinCacheBuffer &buf);

/// THE CACHE'S GEOMETRY ROWS, one per level of the item's mesh (submesh 0): each is
/// the mesh's own row for that level (its index address, index width, bias) with
/// the vertex address, the stride and the layout replaced by the cache's. 48 bytes
/// each, `Ogre::VctVoxelizer::GeometryRow`, handed out as raw words so no
/// consumer of this header drags in HlmsPbs. `levels[l]` is empty when the mesh's
/// level l has no row (no index buffer, no address).
bool describeSkinCacheRows(Ogre::VaoManager *vao, const Ogre::Item *item,
                           const SkinCacheBuffer &buf,
                           std::vector<std::vector<uint32_t>> &levels);

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_SKINCACHE_H
