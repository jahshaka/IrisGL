#pragma once
// THE ENGINE's GPUSCENERECORDS TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include "jahshaka/engine/Ids.h"

namespace jahshaka { namespace engine {

/// WHAT THE TABLES HOLD AND WHAT KEEPING THEM COSTS. Every count is cumulative
/// over the scene's life except `lastDirty` / `lastCopyMs`, which are the last
/// update's.
struct GpuSceneStatus {
    /// False where there are no tables at all — the NULL render system's
    /// headless boot, where every consumer keeps its CPU path.
    bool     live = false;
    /// Live item slots (the dense item index's length). NOT named `slots`: that
    /// is one of Qt's moc keywords (`#define slots`), and this header is included
    /// by Qt translation units.
    unsigned slotCount = 0;
    unsigned capacity = 0;    ///< slots the table can hold before the next grow
    unsigned meshEntries = 0; ///< entries in the mesh table
    unsigned lastDirty = 0;   ///< slots the last update wrote
    unsigned long long writes = 0;     ///< slot writes, ever
    unsigned long long copyRuns = 0;   ///< device copies issued, ever (one per contiguous run)
    unsigned long long updates = 0;    ///< updates that copied anything, ever
    unsigned long long grows = 0;      ///< instance-table doublings, ever
    unsigned long long scans = 0;      ///< dirty scans run, ever
    unsigned long long aabbReads = 0;  ///< world AABBs the scan asked Ogre for, ever
    double   lastCopyMs = 0.0;         ///< CPU cost of the last update's staging + copies
    double   lastScanMicros = 0.0;     ///< ...of the last TIMED scan (0 when never timed)
    /// THE CHANGE FEED's notifications, ever (ATOM-CPU-WALKS-1): slot changes told to
    /// the consumers that derive from the table (the split's words, the ray tier's
    /// traced set, the card candidates) — each one is that many visits for each. A
    /// still frame adds none; a mover frame adds one per moved slot.
    unsigned long long feedNotifies = 0;
};

// --- THE GPU SCENE (SPECS/atom/A3_GPU_SCENE_SLICE_DESIGN.md) ----------------
//
// The scene's description as it lives ON THE DEVICE: one entry per item slot,
// written for the CHANGED subset each frame, read by the ray tier's instance
// job, by Atom's cull and by Photon's reprojection. These two structs are the
// TEST AND TOOL door onto it — the tables themselves are engine-private, and
// nothing in the application reads them.

/// ONE SLOT'S ENTRY, as the device holds it. `world` and `prevWorld` are
/// ROW-MAJOR 3x4 (twelve floats: three rows of four).
struct GpuSceneEntry {
    float world[12] = {};
    float prevWorld[12] = {};
    float boundsMin[3] = {};
    float boundsMax[3] = {};
    /// Its mesh's place in the mesh table, or 0xFFFFFFFF for a slot with no
    /// geometry.
    unsigned meshIndex = 0xFFFFFFFFu;
    /// The predicate bits ONE place computes: 1 visible, 2 caster, 4 mover,
    /// 8 GI-visible, 16 alpha-tested, 32 skinned, 64 overlay, 128 RAY-TRACED
    /// (the traced set), 256 drag mover, 512 ATOM (the visibility buffer's id pass
    /// draws it and the decode shades it — AtomDrawStatus).
    unsigned flags = 0u;
    NodeId   nodeId = 0u;      ///< the slot's node (the GPU holds Ogre's key for it; read back to the node's id)
    unsigned lightMask = 0u;
    /// THE PBS MATERIAL WORD (`GpuInstance.raster.x`, ATOM-S3-PARITY): {pool : 16 |
    /// slot : 16} of the item's material in the PBS const-buffer pool — what the
    /// visibility-buffer decode indexes materials with. 0xFFFFFFFF when the item's
    /// material is not a PBS one.
    unsigned pbsMaterialWord = 0xFFFFFFFFu;
    /// The byte offset of the item mesh's float4 tangent in its vertex
    /// (`GpuInstance.raster.y`), 0xFFFFFFFF when it has none.
    unsigned tangentOffset = 0xFFFFFFFFu;
    /// THE ROW OVERRIDE (`GpuInstance.raster.z`, PHOTON-SKIN-1): the geometry row
    /// of this item's SKIN CACHE — its posed vertices — or 0xFFFFFFFF when its
    /// mesh's own rows are its geometry (every unrigged item).
    unsigned skinRow = 0xFFFFFFFFu;
};

}}  // namespace jahshaka::engine
