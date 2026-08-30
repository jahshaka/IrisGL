// Mesh creation, update and destruction, plus the v2 geometry builder.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

// ---- Meshes ----
MeshId OgreScene::createMesh(const MeshData &data) {
    if (data.positions.empty() || data.positions.size() % 3 != 0) { mError = "createMesh: positions must be xyz triples"; return 0; }
    if (data.indices.empty() || data.indices.size() % 3 != 0)     { mError = "createMesh: indices must be triangles"; return 0; }
    const size_t nv = data.vertexCount();
    for (unsigned i : data.indices) if (i >= nv) { mError = "createMesh: index out of range"; return 0; }
    if (!data.normals.empty() && data.normals.size() != data.positions.size()) { mError = "createMesh: normals count mismatch"; return 0; }
    if (!data.uvs.empty() && data.uvs.size() != nv * 2) { mError = "createMesh: uv count mismatch"; return 0; }
    JAH_TRY {
        MeshRec rec; rec.name = processUniqueName("mesh");
        rec.dynamic = data.dynamic;
        rec.mesh = buildMeshV2(rec.name, data, data.dynamic ? &rec.interleaved : nullptr);
        mMeshes[++mNextMeshId] = std::move(rec);
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
            if (kv.second.meshRef == id && kv.second.item) kv.second.item->setLocalAabb(aabb);
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::destroyMesh(MeshId id) {
    auto it = mMeshes.find(id);
    if (it == mMeshes.end()) return false;
    JAH_TRY {
        invalidateGiCaches();   // BEFORE the mesh dies: IR frees its by-VAO caches now
        for (auto &kv : mNodes) if (kv.second.meshRef == id) detachItem(kv.second);
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
        const size_t nv = points.size();
        struct V { float px, py, pz, nx, ny, nz, u, v; };
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
    struct V { float px, py, pz, nx, ny, nz, tx, ty, tz, tw, u, v; };
    V *verts = reinterpret_cast<V *>(OGRE_MALLOC_SIMD(sizeof(V) * nv, Ogre::MEMCATEGORY_GEOMETRY));
    Ogre::Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    for (size_t v = 0; v < nv; ++v) {
        const float *p = &data.positions[v*3];
        const float *n = &normals[v*3];
        const float *t = &tangents[v*4];
        verts[v] = { p[0], p[1], p[2], n[0], n[1], n[2], t[0], t[1], t[2], t[3],
                     data.uvs.empty() ? 0.0f : data.uvs[v*2], data.uvs.empty() ? 0.0f : data.uvs[v*2+1] };
        mn.makeFloor(Ogre::Vector3(p[0], p[1], p[2])); mx.makeCeil(Ogre::Vector3(p[0], p[1], p[2]));
    }
    if (interleavedOut) {
        const float *vf = reinterpret_cast<const float *>(verts);
        interleavedOut->assign(vf, vf + nv * 12);
    }
    Ogre::VaoManager *vaoMgr = mRoot->getRenderSystem()->getVaoManager();
    Ogre::VertexElement2Vec decl;
    decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION));
    decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_NORMAL));
    decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT4, Ogre::VES_TANGENT));
    decl.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES));
    // Dynamic (CPU-skinned) meshes get BT_DEFAULT: BufferPacked::upload() then
    // rewrites it through a staging buffer, fully synchronised on Vulkan.
    // BT_DYNAMIC_* was deliberately NOT used: those buffers cycle through
    // triple-buffered sections and must be re-mapped and re-filled EVERY frame
    // or a stale section shows; a mesh whose pose pauses would flicker back
    // N frames. Index buffer stays immutable either way.
    Ogre::VertexBufferPacked *vbuf = vaoMgr->createVertexBuffer(
        decl, Ogre::uint32(nv), data.dynamic ? Ogre::BT_DEFAULT : Ogre::BT_IMMUTABLE, verts, true);
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
    sub->mVao[Ogre::VpShadow].push_back(vao);
    const Ogre::Aabb aabb = Ogre::Aabb::newFromExtents(mn, mx);
    mesh->_setBounds(aabb, false);
    mesh->_setBoundingSphereRadius(aabb.getRadius());
    return mesh;
}

}}}  // namespace jahshaka::engine::detail
