// ATOM-2 stage 1 — THE DENSITY COUNT AND THE CUT PROBE (SPECS/ATOM-2.md), a view's two measuring instruments of
// the visibility buffer's cluster cut.
//
// THE DENSITY COUNT. At the Atom view's quad (after the post chain, OgreAtomDraw.cpp's listener) one compute job
// counts the frame's VISIBLE Atom pixels per (GPU scene slot, DAG depth) over the view's id image — a pixel counts
// where the final depth is the id pass's, exactly the pixels the Atom view paints (JahAtomDensityCount_cs.glsl) —
// and, in the 'density' mode, a second job turns each slot's count into the BASE mesh's level-0 triangles per
// visible pixel in the view's own table texture, which the quad colours (JahAtomDensityResolve_cs.glsl,
// JahAtomView_ps.glsl mode 5). The count runs only while the view is Density or the probe's frame is this one: the
// view Off records nothing here.
//
// THE CUT PROBE. Scene::armAtomCutProbe arms the scene; the first view of it whose next frame draws the id pass
// TAKES the arming (atomDensityFrame) and that frame's two culls run the cut job's `cut_probe` permutation: every
// group of every surviving instance's DAG, judged by the job's own function, written with the terms it was judged
// on (JahCullCut_cs.glsl). Scene::atomCutProbe then reads the frame back — the two probe buffers, the frame's
// pixel counts, the two lists' drawn-cluster records and the request the first list was culled with — with
// SYNCHRONOUS readRequests (a measuring door's read, never a frame's), before any later frame overwrites them.
//
// NOTHING HERE IS VULKAN: Ogre's UAV buffers, an Ogre UAV texture and HlmsCompute jobs dispatched through Ogre's
// barrier solver (the id pass's own discipline for its pyramid, OgreAtomIdPass.cpp recordPyramid).
#include "EnginePrivate.h"
#include "GpuCull.h"
#include "GpuScene.h"

#include <Compositor/OgreCompositorNode.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreTextureGpu.h>
#include <OgreTextureGpuManager.h>
#include <Vao/OgreAsyncTicket.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace jahshaka {
namespace engine {
namespace detail {

namespace {
constexpr unsigned long long kNever = ~0ull;
/// The count buffer: a header of four words ([0] the slot capacity), then 16 words a slot (one a DAG depth).
constexpr uint32_t kCountHeader = 4u;
constexpr uint32_t kDepths = 16u;
/// The density table's width: the Atom view's table layout (AtomComponent's kViewTableWidth, the shader's 1024).
constexpr uint32_t kTableWidth = 1024u;
/// A probe record is two uvec4 after a one-uvec4 header.
constexpr uint32_t kProbeHeader = 1u;
constexpr uint32_t kProbeRecordElements = 2u;
}  // namespace

struct AtomDensityState {
    Ogre::UavBufferPacked *counts = nullptr;   ///< kCountHeader + kDepths x capacity words
    uint32_t capacity = 0u;                    ///< slots the counts cover
    Ogre::TextureGpu *table = nullptr;         ///< R32_UINT, kTableWidth x rows, UAV: the Density mode's colours
    Ogre::UavBufferPacked *probe[2] = { nullptr, nullptr };   ///< the first list's records, the late list's
    uint32_t probeCapacity = 0u;               ///< records each buffer holds
    unsigned long long frames = 0ull;          ///< the view's own frames, counted at their head
    unsigned long long countedAt = kNever;     ///< the frame whose pixels the counts hold
    unsigned long long probeAt = kNever;       ///< the frame that took the probe's arming
    unsigned width = 0u, height = 0u;          ///< the id image the count read
    bool count = false, resolve = false;       ///< this frame's work
    std::string probeError;                    ///< why the probe's frame could not record
};

namespace {
Ogre::VaoManager *vaoOf() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    return rs ? rs->getVaoManager() : nullptr;
}
Ogre::TextureGpuManager *texturesOf() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    return rs ? rs->getTextureGpuManager() : nullptr;
}
void destroyBuffer(Ogre::UavBufferPacked *&b) {
    if (b)
        if (Ogre::VaoManager *vao = vaoOf()) vao->destroyUavBuffer(b);
    b = nullptr;
}
void destroyProbe(AtomDensityState &st) {
    destroyBuffer(st.probe[0]);
    destroyBuffer(st.probe[1]);
    st.probeCapacity = 0u;
}
Ogre::DescriptorSetUav::BufferSlot uavSlot(Ogre::UavBufferPacked *b, Ogre::ResourceAccess::ResourceAccess access) {
    Ogre::DescriptorSetUav::BufferSlot slot = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    slot.buffer = b;
    slot.offset = 0;
    slot.sizeBytes = 0;   // to the end
    slot.access = access;
    return slot;
}
Ogre::DescriptorSetTexture2::TextureSlot texSlot(Ogre::TextureGpu *t) {
    Ogre::DescriptorSetTexture2::TextureSlot ts = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
    ts.texture = t;
    return ts;
}
void dispatch(Ogre::RenderSystem *rs, Ogre::HlmsCompute *hc, Ogre::HlmsComputeJob *job) {
    Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
    job->analyzeBarriers(rt);
    rs->executeResourceTransition(rt);
    hc->dispatch(job, nullptr, nullptr);
}
/// `count` elements of `bytes` each out of a UAV buffer: readRequest records the download and the ticket's map()
/// waits on Ogre's own fence for it (OgreGpuCull.cpp's readUints, the documented route).
void readElements(Ogre::UavBufferPacked *b, size_t first, size_t count, size_t bytes, std::vector<uint32_t> &out) {
    out.assign(count * bytes / sizeof(uint32_t), 0u);
    if (!b || !count) return;
    Ogre::AsyncTicketPtr ticket = b->readRequest(first, count);
    std::memcpy(out.data(), ticket->map(), count * bytes);
    ticket->unmap();
}
float bitsToFloat(uint32_t u) {
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}
uint32_t floatToBits(float f) {
    uint32_t u = 0u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
/// The instance's largest axis scale (JahLevelRuleScale's jahWorldMaxAxisScale, from the table's rows).
float maxAxisScale(const GpuInstance &e) {
    float best = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float x = e.world[0 + c], y = e.world[4 + c], z = e.world[8 + c];
        best = std::max(best, std::sqrt(x * x + y * y + z * z));
    }
    return best;
}
uint32_t meshIndexOf(const GpuInstance &e) {
    uint32_t m = 0u;
    std::memcpy(&m, &e.boundsMin[3], sizeof(m));
    return m;
}
}  // namespace

void AtomDensityStateDelete::operator()(AtomDensityState *p) const {
    if (!p) return;
    destroyBuffer(p->counts);
    destroyProbe(*p);
    if (p->table)
        if (Ogre::TextureGpuManager *tm = texturesOf()) tm->destroyTexture(p->table);
    delete p;
}

Ogre::TextureGpu *atomDensityFrame(OgreView *view, OgreScene *scene, AtomView mode, bool &paint) {
    paint = false;
    if (!view || !scene) return nullptr;
    AtomDensityStatePtr &sp = view->atomDensity();
    if (!sp) sp.reset(new AtomDensityState());
    AtomDensityState &st = *sp;
    ++st.frames;
    st.count = st.resolve = false;
    const bool probe = scene->takeAtomCutProbeArm();
    const bool density = mode == AtomView::Density;
    if (probe) {
        st.probeAt = st.frames;
        st.probeError.clear();
    }
    if (!probe && !density) return nullptr;
    GpuScene &gs = scene->gpuScene();
    Ogre::VaoManager *vao = vaoOf();
    Ogre::TextureGpuManager *tm = texturesOf();
    if (!gs.live() || !vao || !tm) {
        if (probe) st.probeError = "the view's frame has no GPU scene to count";
        return nullptr;
    }
    const uint32_t cap = std::max(gs.slotCapacity(), 1u);
    JAH_TRY {
        // THE COUNTS, zeroed for this frame (the slot capacity in the header: the job's bound).
        if (!st.counts || st.capacity < cap) {
            destroyBuffer(st.counts);
            st.counts = vao->createUavBuffer(kCountHeader + kDepths * size_t(cap), sizeof(uint32_t), 0, nullptr, false);
            st.capacity = cap;
        }
        std::vector<uint32_t> zeros(kCountHeader + kDepths * size_t(st.capacity), 0u);
        zeros[0] = st.capacity;
        st.counts->upload(zeros.data(), 0, zeros.size());
        // THE TABLE the quad binds in this frame: the Density mode's colours, and a texture of the right kind at the
        // quad's fourth unit in the probe's frame (whose mode 0 reads nothing). Kept for the view's life (16 KB at
        // 4,096 slots), regrown with the slot capacity.
        const uint32_t rows = (st.capacity + kTableWidth - 1u) / kTableWidth;
        if (st.table && st.table->getHeight() != rows) {
            tm->destroyTexture(st.table);
            st.table = nullptr;
        }
        if (!st.table) {
            st.table = tm->createTexture("jahAtomDensity/" + std::to_string(reinterpret_cast<uintptr_t>(&st)),
                                         Ogre::GpuPageOutStrategy::Discard, Ogre::TextureFlags::Uav,
                                         Ogre::TextureTypes::Type2D);
            st.table->setPixelFormat(Ogre::PFG_R32_UINT);
            st.table->setResolution(kTableWidth, rows);
            st.table->setNumMipmaps(1u);
            st.table->scheduleTransitionTo(Ogre::GpuResidency::Resident);
        }
        if (probe) {
            // THE PROBE'S BUFFERS: one record per group of every live slot's DAG bounds what either list can write
            // (a survivor is a live slot, judged once by one list).
            size_t records = 64u;
            const GpuInstance *m = gs.mirrorData();
            for (uint32_t s = 0; s < gs.slotCount(); ++s) {
                const uint32_t mi = meshIndexOf(m[s]);
                if (mi >= gs.meshEntryCount()) continue;
                records += floatToBits(gs.meshEntry(mi).localBoundsMin[3]);
            }
            if (!st.probe[0] || st.probeCapacity < records) {
                destroyProbe(st);
                for (int k = 0; k < 2; ++k)
                    st.probe[k] = vao->createUavBuffer(kProbeHeader + kProbeRecordElements * records,
                                                       4u * sizeof(uint32_t), 0, nullptr, false);
                st.probeCapacity = uint32_t(records);
            }
            const uint32_t header[4] = { 0u, st.probeCapacity, 0u, 0u };
            for (int k = 0; k < 2; ++k) st.probe[k]->upload(header, 0, 1u);
        }
    }
    catch (Ogre::Exception &e) {
        if (probe) st.probeError = "the probe's buffers could not be made: " + e.getDescription();
        return nullptr;
    }
    st.count = true;
    st.resolve = density;
    paint = probe;
    return (density || mode == AtomView::Off) ? st.table : nullptr;
}

void atomDensityRecord(OgreView *view, OgreScene *scene, Ogre::CompositorPass *quad) {
    AtomDensityState *st = view ? view->atomDensity().get() : nullptr;
    if (!st || !st->count || !st->counts || !scene || !quad) return;
    st->count = false;
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    Ogre::HlmsCompute *hc = root && root->getHlmsManager() ? root->getHlmsManager()->getComputeHlms() : nullptr;
    Ogre::HlmsComputeJob *countJob = hc ? hc->findComputeJobNoThrow("Jahshaka/AtomDensityCount") : nullptr;
    Ogre::HlmsComputeJob *resolveJob = hc ? hc->findComputeJobNoThrow("Jahshaka/AtomDensityResolve") : nullptr;
    if (!rs || !countJob || !resolveJob) {
        if (st->probeAt == st->frames) st->probeError = "the density jobs are not staged (JahshakaCompute.material.json)";
        return;
    }
    Ogre::TextureGpu *ids = nullptr, *idDepth = nullptr, *finalDepth = nullptr;
    JAH_TRY {
        const Ogre::CompositorNode *node = quad->getParentNode();
        ids = node->getDefinedTexture(Ogre::IdString(kAtomIdTexture));
        idDepth = node->getDefinedTexture(Ogre::IdString(kAtomViewDepthTexture));
        finalDepth = node->getDefinedTexture(Ogre::IdString(kSceneDepthTexture));
    }
    catch (Ogre::Exception &) {
        ids = nullptr;
    }
    if (!ids || !idDepth || !finalDepth) {
        if (st->probeAt == st->frames) st->probeError = "the Atom view's node holds no id image or depth";
        return;
    }
    GpuScene &gs = scene->gpuScene();
    JAH_TRY {
        rs->endRenderPassDescriptor();
        countJob->setTexture(0u, texSlot(ids));
        countJob->setTexture(1u, texSlot(idDepth));
        countJob->setTexture(2u, texSlot(finalDepth));
        countJob->_setUavBuffer(0u, uavSlot(st->counts, Ogre::ResourceAccess::ReadWrite));
        countJob->setNumThreadGroups((ids->getWidth() + 7u) / 8u, (ids->getHeight() + 7u) / 8u, 1u);
        dispatch(rs, hc, countJob);
        if (st->resolve && st->table && gs.instanceBuffer() && gs.meshBuffer()) {
            resolveJob->_setUavBuffer(0u, uavSlot(st->counts, Ogre::ResourceAccess::Read));
            resolveJob->_setUavBuffer(1u, uavSlot(gs.instanceBuffer(), Ogre::ResourceAccess::Read));
            resolveJob->_setUavBuffer(2u, uavSlot(gs.meshBuffer(), Ogre::ResourceAccess::Read));
            Ogre::DescriptorSetUav::TextureSlot t = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
            t.texture = st->table;
            t.access = Ogre::ResourceAccess::Write;
            resolveJob->_setUavTexture(3u, t);
            resolveJob->setNumThreadGroups((st->capacity + 63u) / 64u, 1u, 1u);
            dispatch(rs, hc, resolveJob);
        }
        st->countedAt = st->frames;
        st->width = unsigned(ids->getWidth());
        st->height = unsigned(ids->getHeight());
    }
    catch (Ogre::Exception &e) {
        if (st->probeAt == st->frames) st->probeError = "the density count did not record: " + e.getDescription();
    }
    // The jobs' descriptor sets hold raw pointers: nothing of this view's outlives the dispatch on them.
    for (Ogre::uint8 i = 0u; i < 3u; ++i)
        countJob->setTexture(i, Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
    countJob->_setUavBuffer(0u, Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
    for (Ogre::uint8 i = 0u; i < 3u; ++i)
        resolveJob->_setUavBuffer(i, Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
    resolveJob->_setUavTexture(3u, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
}

Ogre::UavBufferPacked *atomCutProbeBuffer(OgreView *view, bool late) {
    const AtomDensityState *st = view ? view->atomDensity().get() : nullptr;
    if (!st || st->probeAt != st->frames) return nullptr;
    return st->probe[late ? 1 : 0];
}

namespace {
/// The frame's per-slot pixel counts (kDepths words a slot), read back synchronously.
bool readCounts(AtomDensityState &st, std::vector<uint32_t> &counts) {
    if (!st.counts || st.countedAt != st.frames) return false;
    readElements(st.counts, 0, kCountHeader + kDepths * size_t(st.capacity), sizeof(uint32_t), counts);
    return true;
}
unsigned slotPixels(const std::vector<uint32_t> &counts, uint32_t slot) {
    unsigned n = 0u;
    for (uint32_t d = 0; d < kDepths; ++d) n += counts[kCountHeader + slot * kDepths + d];
    return n;
}
unsigned long long level0TrianglesOf(const GpuScene &gs, uint32_t slot) {
    const uint32_t mi = meshIndexOf(gs.entry(slot));
    return mi < gs.meshEntryCount() ? gs.meshEntry(mi).counts[1] / 3u : 0ull;
}
}  // namespace

/// THE DENSITY FIGURES (AtomDrawStatus; ATOM-2): the first view of the scene whose last frame counted.
void atomDensityStatus(OgreScene *scene, const std::vector<OgreView *> &views, AtomDrawStatus &out) {
    if (!scene || scene->atomView() != AtomView::Density) return;
    const GpuScene &gs = scene->gpuScene();
    for (OgreView *v : views) {
        AtomDensityState *st = v ? v->atomDensity().get() : nullptr;
        std::vector<uint32_t> counts;
        if (!st || !readCounts(*st, counts)) continue;
        out.densityValid = true;
        const uint32_t slots = std::min(st->capacity, gs.slotCount());
        for (uint32_t s = 0; s < slots; ++s) {
            const unsigned px = slotPixels(counts, s);
            if (!px) continue;
            for (uint32_t d = 0; d < kDepths; ++d) out.densityDepthPixels[d] += counts[kCountHeader + s * kDepths + d];
            const float tpp = float(level0TrianglesOf(gs, s)) / float(px);
            const int c = tpp > 4.0f ? 2 : tpp >= 1.0f ? 1 : 0;
            out.densityClassPixels[c] += px;
            ++out.densityClassObjects[c];
            out.densityPixels += px;
            ++out.densityObjects;
        }
        return;
    }
}

bool OgreScene::armAtomCutProbe() {
    if (hostPosts()) { bool r = false; requestOnEngine([&] { r = armAtomCutProbe(); }); return r; }
    JAH_HOST_CALL("Scene::armAtomCutProbe");
    if (!atomDrawOn()) return false;
    for (OgreView *v : mEngine ? mEngine->viewsOf(this) : std::vector<OgreView *>())
        if (AtomDensityState *st = v ? v->atomDensity().get() : nullptr) st->probeAt = kNever;
    mAtomCutProbeArmed = true;
    return true;
}

bool OgreScene::atomCutProbe(AtomCutProbe &out) {
    if (hostPosts()) { bool r = false; requestOnEngine([&] { r = atomCutProbe(out); }); return r; }
    JAH_HOST_CALL("Scene::atomCutProbe");
    out = AtomCutProbe();
    OgreView *view = nullptr;
    AtomDensityState *st = nullptr;
    for (OgreView *v : mEngine ? mEngine->viewsOf(this) : std::vector<OgreView *>()) {
        AtomDensityState *s = v ? v->atomDensity().get() : nullptr;
        if (s && s->probeAt != kNever) {
            view = v;
            st = s;
            break;
        }
    }
    if (!st) {
        // STILL OWED while the arming waits for a frame; nothing was armed otherwise.
        out.error = mAtomCutProbeArmed ? "armed: no frame of a view with the id pass has drawn yet"
                                       : "not armed (Scene::armAtomCutProbe)";
        return !mAtomCutProbeArmed;
    }
    const unsigned long long at = st->probeAt;
    st->probeAt = kNever;   // read once
    struct Release {
        AtomDensityState *s;
        ~Release() { destroyProbe(*s); }
    } release{ st };
    if (at != st->frames) {
        out.error = "a later frame of the view overwrote the probe's frame";
        return true;
    }
    if (!st->probeError.empty()) {
        out.error = st->probeError;
        return true;
    }
    if (!st->probe[0] || !st->probe[1]) {
        out.error = "the probe's buffers do not exist";
        return true;
    }
    JAH_TRY {
        // ---- THE REQUEST the first list was culled with, as the device holds it ----
        std::vector<uint32_t> words;
        readElements(view->atomCull().params(), 0, 1u, sizeof(GpuCullParams), words);
        GpuCullParams p;
        std::memcpy(&p, words.data(), sizeof(p));
        out.tolerance = p.lod[0];
        out.projScaleY = p.lod[1];
        out.viewportHeight = p.lod[2];
        out.orthographic = p.lod[3] > 0.5f;
        for (int i = 0; i < 3; ++i) out.eye[i] = p.eye[i];
        // ---- THE GROUPS, both lists ----
        const GpuScene &gs = gpuScene();
        std::map<uint32_t, AtomCutProbeObject> objects;
        auto objectOf = [&](uint32_t slot) -> AtomCutProbeObject & {
            auto it = objects.find(slot);
            if (it != objects.end()) return it->second;
            AtomCutProbeObject &o = objects[slot];
            o.slot = slot;
            if (slot < gs.slotCount()) {
                const GpuInstance &e = gs.entry(slot);
                o.node = nodeOfGpuKey(e.ids[0]);
                o.meshEntry = meshIndexOf(e);
                o.scale = maxAxisScale(e);
                o.level0Triangles = level0TrianglesOf(gs, slot);
            }
            return o;
        };
        const bool lateRecorded = view->chainAtomOcclusion() && view->atomFirstTested();
        for (int k = 0; k < (lateRecorded ? 2 : 1); ++k) {
            std::vector<uint32_t> head;
            readElements(st->probe[k], 0, 1u, 16u, head);
            const uint32_t n = std::min(head[0], head[1]);
            out.groupsDropped += head[2];
            std::vector<uint32_t> rec;
            readElements(st->probe[k], kProbeHeader, size_t(n) * kProbeRecordElements, 16u, rec);
            for (uint32_t r = 0; r < n; ++r) {
                const uint32_t *w = &rec[size_t(r) * 8u];
                AtomCutProbeGroup g;
                g.slot = w[0];
                g.group = w[1];
                g.depth = w[2];
                g.affordable = (w[3] & 1u) != 0u;
                g.terminal = (w[3] & 2u) != 0u;
                g.late = k == 1;
                g.error = bitsToFloat(w[4]);
                g.allowed = bitsToFloat(w[5]);
                g.distance = bitsToFloat(w[6]);
                g.radius = bitsToFloat(w[7]);
                out.groups.push_back(g);
                AtomCutProbeObject &o = objectOf(g.slot);
                ++o.groups;
                if (g.affordable) ++o.groupsAffordable;
                if (g.terminal) ++o.groupsTerminal;
            }
        }
        // ---- THE DRAWN CLUSTERS, both lists' records (a skip's slot is all ones) ----
        const std::vector<GpuCluster> &clusters = gs.clusterMirror();
        for (int k = 0; k < (lateRecorded ? 2 : 1); ++k) {
            GpuCull &list = k ? view->atomCullLate() : view->atomCull();
            if (!list.count() || !list.cutRecords()) continue;
            std::vector<uint32_t> cw;
            readElements(list.count(), 0, GpuCull::kCountElements, sizeof(uint32_t), cw);
            const uint32_t n = std::min(cw[8], list.cutRecordBudget());
            std::vector<uint32_t> rec;
            readElements(list.cutRecords(), 0, n, 16u, rec);
            for (uint32_t r = 0; r < n; ++r) {
                const uint32_t *w = &rec[size_t(r) * 4u];
                if (w[0] == 0xFFFFFFFFu || w[1] >= clusters.size()) continue;
                AtomCutProbeObject &o = objectOf(w[0]);
                const unsigned long long tris = clusters[w[1]].indexCount / 3u;
                ++o.drawnClusters;
                o.drawnTriangles += tris;
                o.depthTriangles[std::min(w[3], kDepths - 1u)] += tris;
            }
        }
        // ---- THE FRAME'S VISIBLE PIXELS ----
        std::vector<uint32_t> counts;
        if (readCounts(*st, counts)) {
            out.width = st->width;
            out.height = st->height;
            const uint32_t slots = std::min(st->capacity, gs.slotCount());
            for (uint32_t s = 0; s < slots; ++s) {
                if (!slotPixels(counts, s)) continue;
                AtomCutProbeObject &o = objectOf(s);
                for (uint32_t d = 0; d < kDepths; ++d) {
                    const unsigned px = counts[kCountHeader + s * kDepths + d];
                    o.depthPixels[d] += px;
                    o.pixels += px;
                    out.depthPixels[d] += px;
                    out.atomPixels += px;
                }
            }
        } else {
            out.error = "the probe's frame did not count its pixels (the Atom view's quad did not run)";
        }
        out.objects.reserve(objects.size());
        for (auto &kv : objects) out.objects.push_back(kv.second);
        out.valid = out.error.empty();
        return true;
    }
    catch (Ogre::Exception &e) {
        out.error = "the probe's read failed: " + e.getDescription();
        return true;
    }
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
