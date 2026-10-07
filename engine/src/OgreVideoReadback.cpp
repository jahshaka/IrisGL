// THE VIDEO READBACK (VIDEO-REC-1; View::setVideoReadback, Engine.h).
//
// A recorder's frames: a view's final picture converted to NV12 on the GPU and
// read back through a ring of three AsyncTextureTickets, so the host never waits
// on the GPU and never reads more than 1.5 bytes a pixel.
//
// WHERE IT RUNS: at the end of the view's OWN workspace (workspacePosUpdate), on
// a frame the host armed — after the whole chain, the overlay pass included, has
// written the target. One compute dispatch (`Jahshaka/VideoNv12`, media
// JahVideoNv12_cs.glsl: the BT.709 limited-range matrix on the display codes),
// the compositor's own compute discipline (close the render pass, the job's
// barriers through Ogre's solver, dispatch, release the bindings), then one copy
// of the NV12 target into the oldest free ticket.
//
// NO FENCE OF ITS OWN AND NO FLUSH. The copies use Ogre's frame-count tracking: a
// ticket is done when the frame that recorded it has finished on the GPU
// (VulkanVaoManager::isFrameFinished reads that frame's own fence). Ogre keeps at
// most three frames in flight and waits for frame N-3 before recording frame N, so
// a host that takes one frame per frame always finds a free slot: three is the
// ring the brief asked for, and it is also the smallest one that never drops at
// steady state. A ticket is never QUERIED in the frame that recorded it (Ogre
// switches a ticket queried there more than three times to accurate tracking, a
// flush per frame — OgreVulkanAsyncTextureTicket.cpp).
#include "EnginePrivate.h"

#include <OgreAsyncTextureTicket.h>
#include <OgreDescriptorSetTexture.h>
#include <OgreDescriptorSetUav.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgrePixelFormatGpuUtils.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>
#include <OgreRoot.h>
#include <OgreTextureBox.h>
#include <OgreTextureGpu.h>
#include <OgreTextureGpuManager.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Vao/OgreVaoManager.h>

#include <cstring>

namespace jahshaka { namespace engine {
namespace detail {

namespace {
constexpr const char *kVideoJob = "Jahshaka/VideoNv12";
constexpr unsigned kThreads = 8u;   // the job's threads_per_group (JahshakaCompute.material.json)
}   // namespace

VideoReadback::~VideoReadback() { release(); }

bool VideoReadback::init(std::string &error) {
    Ogre::TextureGpu *src = mView ? mView->targetTexture() : nullptr;
    if (!src) { error = "setVideoReadback: the view has no target"; return false; }
    const Ogre::PixelFormatGpu fmt = src->getPixelFormat();
    // THE CODES ARE READ AS THEY ARE: an sRGB view of the target would linearise
    // them on the fetch and the matrix would run on light, not on the picture.
    if (Ogre::PixelFormatGpuUtils::isSRgb(fmt) || Ogre::PixelFormatGpuUtils::isInteger(fmt) ||
        Ogre::PixelFormatGpuUtils::getNumberOfComponents(fmt) < 3u) {
        error = std::string("setVideoReadback: the target's format ") +
                Ogre::PixelFormatGpuUtils::toString(fmt) + " is not a UNORM colour picture";
        return false;
    }
    if (src->getSampleDescription().isMultisample()) {
        error = "setVideoReadback: the target is multisampled";
        return false;
    }
    const unsigned w = src->getWidth(), h = src->getHeight();
    if (w == 0u || h == 0u || (w % 4u) != 0u || (h % 2u) != 0u) {
        error = "setVideoReadback: the target is " + std::to_string(w) + "x" + std::to_string(h) +
                "; NV12 here needs a width that is a multiple of 4 and an even height";
        return false;
    }
    Ogre::HlmsManager *hm = mRoot->getHlmsManager();
    Ogre::HlmsCompute *hc = hm ? hm->getComputeHlms() : nullptr;
    if (!hc || !hc->findComputeJobNoThrow(kVideoJob)) {
        error = std::string("setVideoReadback: the compute job ") + kVideoJob + " is not staged";
        return false;
    }
    try {
        Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
        mTarget = tm->createTexture(mView->name() + "/VideoNv12/" + std::to_string(reinterpret_cast<uintptr_t>(this)),
                                    Ogre::GpuPageOutStrategy::Discard, Ogre::TextureFlags::Uav,
                                    Ogre::TextureTypes::Type2D);
        mTarget->setResolution(w / 4u, h + h / 2u);
        mTarget->setPixelFormat(Ogre::PFG_R32_UINT);
        mTarget->setNumMipmaps(1u);
        // A UAV is a manual texture: Resident and data-ready at once, never a
        // notifyDataIsReady of ours (ENGINE trap 3).
        mTarget->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        for (Slot &s : mSlots)
            s.ticket = tm->createAsyncTextureTicket(w / 4u, h + h / 2u, 1u, Ogre::TextureTypes::Type2D,
                                                    Ogre::PFG_R32_UINT);
    } catch (Ogre::Exception &e) {
        error = describeOgreFailure(e);
        release();
        return false;
    }
    mWidth = w;
    mHeight = h;
    return true;
}

void VideoReadback::release() {
    if (!mRoot) return;
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    Ogre::TextureGpuManager *tm = rs ? rs->getTextureGpuManager() : nullptr;
    for (Slot &s : mSlots) {
        if (!s.ticket) continue;
        try {
            // A pending copy is waited out before its staging buffer goes: the
            // map waits for the frame that recorded it (at most the frames already
            // submitted), the unmap hands the buffer back.
            if (s.pending) { s.ticket->map(0); s.ticket->unmap(); }
            if (tm) tm->destroyAsyncTextureTicket(s.ticket);
        } catch (Ogre::Exception &) {
        }
        s = Slot();
    }
    if (mTarget && tm) {
        try { tm->destroyTexture(mTarget); } catch (Ogre::Exception &) {}
    }
    mTarget = nullptr;
}

void VideoReadback::workspacePosUpdate(Ogre::CompositorWorkspace *ws) {
    // The view's OWN workspace only — never the clean capture's second instance or
    // a PiP's: the picture is the frame this view rendered.
    if (!mArmed || !mView || ws != mView->workspace() || !mTarget) return;
    mArmed = false;
    Ogre::TextureGpu *src = mView->targetTexture();
    if (!src || src->getWidth() != mWidth || src->getHeight() != mHeight) {
        mError = "video readback: the view's target changed size; the readback is stale";
        ++mDropped;
        return;
    }
    Slot *free = nullptr;
    for (Slot &s : mSlots)
        if (!s.pending) { free = &s; break; }
    if (!free) { ++mDropped; return; }
    free->tag = mArmedTag;
    record(*free);
}

void VideoReadback::record(Slot &slot) {
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    Ogre::HlmsCompute *hc = mRoot->getHlmsManager()->getComputeHlms();
    Ogre::HlmsComputeJob *job = hc ? hc->findComputeJobNoThrow(kVideoJob) : nullptr;
    Ogre::TextureGpu *src = mView->targetTexture();
    if (!job || !src) { ++mDropped; return; }
    try {
        // THE COST, AS THE GPU SEES IT: a monitor row of its own (CacheKind::Video),
        // the dispatch and the copy inside one timestamp pair. Nothing while the
        // monitor is off.
        monitor::CacheScope scope(CacheKind::Video, WorkReason::Request, 0, "video.nv12", rs);
        rs->endRenderPassDescriptor();
        // The copy encoder of a previous frame's download, closed so the solver
        // reasons about a texture nothing else owns (OgreSky.cpp's rule).
        rs->endCopyEncoder();
        Ogre::DescriptorSetTexture2::TextureSlot in(Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
        in.texture = src;
        job->setTexture(0u, in, nullptr);
        Ogre::DescriptorSetUav::TextureSlot uav = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        uav.texture = mTarget;
        uav.access = Ogre::ResourceAccess::Write;
        job->_setUavTexture(0u, uav);
        const unsigned tilesX = mWidth / 4u, tilesY = mHeight / 2u;
        job->setNumThreadGroups((tilesX + kThreads - 1u) / kThreads, (tilesY + kThreads - 1u) / kThreads, 1u);
        {
            Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
            job->analyzeBarriers(rt);
            rs->executeResourceTransition(rt);
            hc->dispatch(job, nullptr, nullptr);
        }
        // A job's descriptor sets hold raw pointers: released before anything can
        // destroy what they name.
        job->setTexture(0u, Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
        job->_setUavTexture(0u, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
        // Frame-count tracking (no flush): see the file's note.
        slot.ticket->download(mTarget, 0u, false);
        slot.pending = true;
        slot.order = ++mOrder;
        slot.frame = rs->getVaoManager()->getFrameCount();
        ++mRecorded;
    } catch (Ogre::Exception &e) {
        mError = describeOgreFailure(e);
        ++mDropped;
    }
}

bool VideoReadback::take(VideoFrameNv12 &out, bool wait, std::string &error) {
    Slot *oldest = nullptr;
    for (Slot &s : mSlots)
        if (s.pending && (!oldest || s.order < oldest->order)) oldest = &s;
    if (!oldest) return false;
    try {
        if (!wait) {
            // Never queried in the frame that recorded it (the file's note).
            Ogre::VaoManager *vao = mRoot->getRenderSystem()->getVaoManager();
            if (vao->getFrameCount() == oldest->frame) return false;
            if (!oldest->ticket->queryIsTransferDone()) return false;
        }
        const Ogre::TextureBox box = oldest->ticket->map(0);
        out.width = mWidth;
        out.height = mHeight;
        out.tag = oldest->tag;
        const size_t rowBytes = mWidth;   // width/4 texels of 4 bytes
        const unsigned rows = mHeight + mHeight / 2u;
        out.nv12.resize(rowBytes * rows);
        for (unsigned y = 0; y < rows; ++y)
            std::memcpy(&out.nv12[size_t(y) * rowBytes], box.at(0, y, 0), rowBytes);
        oldest->ticket->unmap();
        oldest->pending = false;
        ++mDelivered;
        return true;
    } catch (Ogre::Exception &e) {
        error = describeOgreFailure(e);
        oldest->pending = false;
        return false;
    }
}

VideoReadbackStatus VideoReadback::status() const {
    VideoReadbackStatus st;
    st.on = mTarget != nullptr;
    st.width = mWidth;
    st.height = mHeight;
    st.ringSize = kRing;
    for (const Slot &s : mSlots) if (s.pending) ++st.pending;
    st.recorded = mRecorded;
    st.delivered = mDelivered;
    st.dropped = mDropped;
    st.error = mError;
    return st;
}

// ---- the View verbs ---------------------------------------------------------

bool OgreView::setVideoReadback(bool on) {
    if (!on) {
        if (mVideoReadback) {
            removeWorkspaceListener(mVideoReadback.get());
            mVideoReadback.reset();
        }
        return true;
    }
    if (mVideoReadback) return true;
    if (!isOffscreen()) {
        mError = "setVideoReadback: View '" + mName + "' is on screen; the readback is an offscreen view's";
        return false;
    }
    auto rb = std::make_unique<VideoReadback>(this, mRoot);
    std::string error;
    if (!rb->init(error)) { mError = error; return false; }
    mVideoReadback = std::move(rb);
    addWorkspaceListener(mVideoReadback.get());
    return true;
}

void OgreView::armVideoFrame(unsigned long long tag) {
    if (mVideoReadback) mVideoReadback->arm(tag);
}

bool OgreView::takeVideoFrame(VideoFrameNv12 &out, bool wait) {
    if (!mVideoReadback) { mError = "takeVideoFrame: the view's video readback is off"; return false; }
    std::string error;
    const bool ok = mVideoReadback->take(out, wait, error);
    if (!error.empty()) mError = error;
    return ok;
}

VideoReadbackStatus OgreView::videoReadbackStatus() const {
    return mVideoReadback ? mVideoReadback->status() : VideoReadbackStatus();
}

}}}  // namespace jahshaka::engine::detail
