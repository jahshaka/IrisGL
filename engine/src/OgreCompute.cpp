// ATOM P3 / PHOTON SHARED INFRASTRUCTURE — the compute substrate's VIEW half
// (SPECS/atom/A4_SUBSTRATE_CULL_DESIGN.md sections 1-2; NANITE_SPEC section 4.2-4.3;
//  ogre-patch 0032 and 0027).
//
// TWO THINGS LIVE HERE and they share one reason: both need a VIEW.
//
//   * `OgreEngine::gpuCull` — the boundary's entry into ATOM's generic cull. The
//     chain itself is OgreGpuCull.cpp, beside the tables it reads; what this file
//     contributes is finding the view's depth pyramid.
//   * The pyramid's own readbacks (`hzbStatus`, `readHzbLevel`) — so a suite can
//     ASSERT the reduction texel by texel rather than trust it.
//
// WHAT WAS HERE AND IS GONE (ATOM-SUBSTRATE-1, 2026-09-22): `indirectDispatchProbe`
// and its three compute jobs `Jahshaka/IndirectCount`, `IndirectWork` and
// `IndirectWorkCpu`. They were the PROOF of patch 0032 while it had no consumer;
// it has one now, and the cull's job 3 makes the same claim on the same device
// with real work behind it: the survivor count is written by a compute shader
// and the next dispatch is sized from it. `engine.gpu_cull` asserts the
// GPU-written count and the groups that ran at 0, 7 and the whole table — the
// same three shapes (empty, an arbitrary small count no host arithmetic could
// predict, and the worst case) that `compute.indirect_dispatch` used, over real
// instances instead of a synthetic list. A proof with a consumer is the
// consumer's suite.
#include "EnginePrivate.h"

#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>
#include <OgreCamera.h>
#include <OgreRoot.h>
#include <OgreViewport.h>
#include <OgreTextureGpu.h>
#include <OgreTextureGpuManager.h>
#include <OgreAsyncTextureTicket.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorNode.h>
#include <Vao/OgreAsyncTicket.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace jahshaka { namespace engine {
namespace detail {

namespace {
/// Defined below, beside the pyramid's own readbacks: the live `jahHzb` of a
/// view's workspace, or null.
Ogre::TextureGpu *findHzbTexture(OgreView *view);
}   // namespace

/// ATOM P3's ENTRY POINT from the boundary. The work is in OgreGpuCull.cpp
/// beside the tables it reads; what lives here is the VIEW half — finding the
/// depth pyramid the request wants to test against, which is this file's own
/// `findHzbTexture` below.
///
/// THE PYRAMID IT BINDS IS THE ONE THAT IS THERE. Called before a frame, that is
/// the PREVIOUS frame's: the seed pass rewrites mip 0 inside the frame's own
/// compositor graph, so until that pass runs the texture still holds the last
/// completed frame's closest-depth chain. That ORDERING is the design's
/// previous-frame contract, and it costs nothing — a `jahHzbPrev` copy would pay
/// mip 0's whole bandwidth (half the build's measured 0.045 ms at 1080p) to
/// deliver what the order of operations already delivers. What the consumer owes
/// in exchange is the matching matrix, which is why `viewProj` is in the request
/// and not taken from the camera here.
bool OgreEngine::gpuCull(Scene *scene, View *view, const GpuCullRequest &request, bool readBack,
                         GpuCullResult &out) {
    out = GpuCullResult();
    OgreScene *s = static_cast<OgreScene *>(scene);
    if (!s || !mRoot) return false;
    Ogre::TextureGpu *hzb = request.hzbLevels ? findHzbTexture(static_cast<OgreView *>(view))
                                              : nullptr;
    if (request.hzbLevels && !hzb) {
        mLastError = "gpuCull: the request asks for the depth pyramid and this view builds none "
                     "(PostFxDesc::hzb)";
        return false;
    }
    return s->runGpuCull(request, hzb, readBack, out);
}

/// THE VIEW HALF OF A REQUEST — every convention in one place (Engine.h).
///
/// THE PROJECTION IS THE ONE THE DEPTH BUFFER HAS: `getProjectionMatrixWithRSDepth`
/// carries the render system's own z range, which on Vulkan at this pin is
/// REVERSE-Z in [0, w] — and the pyramid is a copy of that buffer, so a cull
/// testing against it must project the same way. Its y is UP in clip space (the
/// flip lives in Ogre's NEGATIVE viewport height — the stage-0 spike's finding
/// 2.4), and the shader inverts y when it turns NDC into texels because the
/// attachment's row 0 is the top.
///
/// THE SIX PLANES are read off the rows of that matrix (Gribb/Hartmann), so they
/// are right whatever the projection is and inherit no convention of Ogre's
/// frustum classes. Note which two the z rows give under Vulkan's [0, w]:
/// `row2` alone is one of them and `row3 - row2` the other, NOT the [-w, w]
/// pair a GL-era derivation would write — and under reverse-Z the first is the
/// FAR plane, which changes nothing about the volume they bound together.
bool OgreEngine::fillCullView(View *view, GpuCullRequest &out) const {
    OgreView *v = static_cast<OgreView *>(view);
    if (!v) return false;
    Ogre::Camera *cam = v->camera();
    if (!cam) return false;
    fillCullFrustum(cam, float(v->height()), out);
    // THE PYRAMID IS ONLY OFFERED WHEN IT HOLDS SOMETHING. A pyramid that has
    // been BUILT but never written is an uninitialised allocation, and a cull
    // against it would reject geometry on the strength of another texture's
    // leftovers.
    HzbStatus hst;
    out.hzbLevels = hzbStatus(view, hst) && hst.built && hst.primed ? hst.levels : 0u;
    return out.viewportHeight > 0.0f;
}

/// The frustum, the eye and the level rule's two terms of a cull request, from a
/// camera and the height of the target its pass renders into (the id pass's own
/// request, which never offers a pyramid).
void fillCullFrustum(const Ogre::Camera *cam, float viewportHeight, GpuCullRequest &out) {

    const Ogre::Matrix4 vpm = cam->getProjectionMatrixWithRSDepth() * cam->getViewMatrix();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.viewProj[r * 4 + c] = float(vpm[r][c]);

    const int rows[6] = { 0, 0, 1, 1, 2, 2 };
    const float signs[6] = { +1.0f, -1.0f, +1.0f, -1.0f, 0.0f, -1.0f };
    for (int p = 0; p < 6; ++p) {
        float pl[4];
        for (int c = 0; c < 4; ++c) {
            const float w = signs[p] == 0.0f ? 0.0f : float(vpm[3][c]);
            pl[c] = w + signs[p] * float(vpm[rows[p]][c]);
        }
        if (signs[p] == 0.0f)
            for (int c = 0; c < 4; ++c) pl[c] = float(vpm[2][c]);
        const float n = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
        for (int c = 0; c < 4; ++c) out.planes[p * 4 + c] = n > 0.0f ? pl[c] / n : pl[c];
    }

    const Ogre::Vector3 eye = cam->getDerivedPosition();
    out.eye[0] = float(eye.x);
    out.eye[1] = float(eye.y);
    out.eye[2] = float(eye.z);
    // The currency's two terms come from the ORDINARY projection: proj[1][1] is
    // the same in both (the RS-depth form differs in the z row only), and the
    // height is the pass's target.
    out.projScaleY = float(cam->getProjectionMatrix()[1][1]);
    // THE VIEW'S OWN HEIGHT, not the camera's last viewport: an offscreen view's
    // camera reports no viewport outside a pass (measured — `getLastViewport()`
    // is null between frames), and the number the currency wants is the height
    // of the target this request's pass renders into, which the View knows for
    // certain.
    out.viewportHeight = viewportHeight;
}


// ===========================================================================
// THE HIERARCHICAL DEPTH PYRAMID — readback and shape (NANITE_SPEC §4.3)
//
// The pyramid itself is built by the compositor (OgreChain.cpp): one R32_FLOAT
// texture with a full mip chain, written by one compute pass per level right
// after the opaque pass. These two verbs exist so a suite can ASSERT the
// reduction rather than trust it, and so a future consumer can ask whether
// there is anything to bind.
// ===========================================================================
namespace {

/// The live `jahHzb` texture of a view's workspace, or null. Walks the node
/// sequence because a chain is several nodes and only one of them declares it.
Ogre::TextureGpu *findHzbTexture(OgreView *view) {
    if (!view) return nullptr;
    Ogre::CompositorWorkspace *ws = view->workspace();
    if (!ws) return nullptr;
    const Ogre::CompositorNodeVec &nodes = ws->getNodeSequence();
    for (Ogre::CompositorNode *node : nodes) {
        if (!node) continue;
        // getDefinedTexture THROWS on a node that does not declare it, and only
        // one node of the chain does — so the miss is caught, not tested for.
        try {
            if (Ogre::TextureGpu *tex = node->getDefinedTexture(Ogre::IdString("jahHzb")))
                return tex;
        } catch (const Ogre::Exception &) {}
    }
    return nullptr;
}

}  // namespace

bool OgreEngine::hzbStatus(View *view, HzbStatus &out) const {
    out = HzbStatus();
    if (!mRoot) return false;
    JAH_TRY {
        Ogre::TextureGpu *tex = findHzbTexture(static_cast<OgreView *>(view));
        if (!tex) return false;
        out.built = true;
        out.levels = tex->getNumMipmaps();
        out.width = tex->getWidth();
        out.height = tex->getHeight();
        Ogre::RenderSystem *rs = mRoot->getRenderSystem();
        out.reverseDepth = rs && rs->isReverseDepth();
        OgreView *v = static_cast<OgreView *>(view);
        out.farthest = v->postFx().hzbFarthest;
        // PRIMED = a frame has been PRESENTED with the chain that owns this
        // texture. The texture exists from the build and holds whatever the
        // allocation last did until the seed pass of a presented frame writes
        // mip 0; the per-workspace counter is reset by every chain rebuild,
        // which is exactly the event that invalidates the contents again.
        out.primed = v->workspaceFramesPresented() > 0ull;
        return true;
    }
    catch (...) { out = HzbStatus(); return false; }
}

bool OgreEngine::readHzbLevel(View *view, unsigned level, std::vector<float> &out,
                              unsigned &width, unsigned &height) {
    out.clear();
    width = height = 0u;
    if (!mRoot) return false;
    JAH_TRY {
        Ogre::TextureGpu *tex = findHzbTexture(static_cast<OgreView *>(view));
        if (!tex) { mLastError = "readHzbLevel: this view builds no HZB"; return false; }
        if (level >= tex->getNumMipmaps()) {
            mLastError = "readHzbLevel: level out of range";
            return false;
        }
        const Ogre::uint32 w = std::max(tex->getWidth() >> level, 1u);
        const Ogre::uint32 h = std::max(tex->getHeight() >> level, 1u);

        // THE FLUSH IS LOAD-BEARING (CLAUDE.md, sky/IBL adoption facts): an
        // AsyncTextureTicket reads whatever is in VRAM, and the pyramid's
        // dispatches are only RECORDED into the open command buffer until
        // something submits it. Without this the readback can return a freed
        // allocation's pixels — not zeros, somebody else's picture.
        Ogre::RenderSystem *rs = mRoot->getRenderSystem();
        rs->flushCommands();

        Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
        Ogre::AsyncTextureTicket *ticket = tm->createAsyncTextureTicket(
            w, h, 1u, Ogre::TextureTypes::Type2D, tex->getPixelFormat());
        ticket->download(tex, Ogre::uint8(level), true);
        const Ogre::TextureBox box = ticket->map(0);
        out.resize(size_t(w) * size_t(h));
        for (Ogre::uint32 y = 0; y < h; ++y)
            std::memcpy(&out[size_t(y) * w], box.at(0, y, 0), size_t(w) * sizeof(float));
        ticket->unmap();
        tm->destroyAsyncTextureTicket(ticket);
        width = w; height = h;
        return true;
    }
    catch (Ogre::Exception &e) { mLastError = e.getFullDescription(); return false; }
}

}  // namespace detail
}}  // namespace jahshaka::engine
