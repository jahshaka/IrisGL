#pragma once
// THE ENGINE's GIVOXELS TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <cstdint>
#include <string>
#include <vector>

namespace jahshaka { namespace engine {

/// WHAT THE VOXEL LIGHTING VOLUME ACTUALLY HOLDS — a TEST AND TOOL readback
/// (PHOTON-M3), never a per-frame path: it flushes the render system's
/// commands and blocks on a texture download of the whole 3D volume.
///
/// It exists because the voxel volume is the one place in this renderer where
/// a PHYSICAL quantity is stored in a FIXED-RANGE format, and "the bounce's
/// fixed point fits in the format" cannot be measured from the picture: a
/// clipped voxel makes a picture that is merely dimmer than it should be, with
/// the same hue and no hitch, exactly like a scene with less bounce in it
/// (PHOTON-M2's finding F1, where a saturated gather read as a "contraction").
/// What a guard needs is the histogram's top bin, and that is this.
///
/// EVERY VALUE IS SCENE RADIANCE — what the volume holds (PHOTON-7 stage 1
/// deleted the per-box normalisation). `formatMax` is the ceiling that format
/// can represent (1.0 for the UNORM formats, 0 for a float one: unbounded), so
/// `peak` against `formatMax` and `voxelsAtMax` against `voxelsLit` are the
/// whole question.
/// THE IRRADIANCE FIELD'S ATLASES, AS BYTES (PHOTON-WRITER-1) — a TEST AND TOOL
/// readback (Scene::giFieldAtlas): the irradiance and depth-moment atlases
/// exactly as the GPU holds them, with what it takes to find one probe's tile.
/// A probe's tile is its SLOT (the window's modulo: slot = (window-local +
/// windowOffset) mod probes), at column (slot * bordered) mod width, row
/// ((slot * bordered) / width) * bordered, `bordered` texels square.
struct GiFieldAtlas {
    bool available = false;
    unsigned probes[3] = { 0u, 0u, 0u };
    unsigned windowOffset[3] = { 0u, 0u, 0u };
    unsigned irradWidth = 0, irradHeight = 0, irradBordered = 0, irradBytesPerTexel = 0;
    std::vector<unsigned char> irradiance;
    unsigned depthWidth = 0, depthHeight = 0, depthBordered = 0, depthBytesPerTexel = 0;
    std::vector<unsigned char> depth;
};

struct GiVoxelStats {
    /// False when there is nothing to read: no VCT arm on this scene, no such
    /// cascade, a headless stand-in, or a download this device refused. Every
    /// number below is then 0 and none of them means anything.
    bool available = false;
    /// Which cascade of the chain was read (0 = the innermost, the one the
    /// camera stands in).
    int  cascade = 0;
    /// The volume's resolution, so a caller can weigh the counts below.
    int  width = 0, height = 0, depth = 0;
    /// The pixel format the TOTAL volume is stored in, spelled as Ogre spells
    /// it ("PFG_RGBA8_UNORM_SRGB", "PFG_RGBA16_FLOAT").
    std::string format;
    /// The largest value that format can hold in the store's units — 1.0 for
    /// UNORM, 0.0 for a float format (no ceiling worth naming).
    float formatMax = 0.0f;
    /// The peak channel over the whole TOTAL volume, and over the DIRECT
    /// volume beside it when the cascade bounces (the fixed point's D term) - in
    /// radiance: a voxel holds the surface's outgoing radiance (PHOTON-7 stage 1
    /// deleted the per-box normalisation k).
    float peak = 0.0f;
    float peakDirect = 0.0f;
    /// The mean of the channel maximum over LIT voxels — what the picture's
    /// indirect term is made of, and the number a convergence test watches
    /// stop moving.
    double meanLit = 0.0;
    /// Voxels with any light in them at all (> 0 in any channel) and, of
    /// those, how many sit on the format's top bin in ANY channel. On a UNORM
    /// total a non-zero `voxelsAtMax` IS the clip: the bounce's fixed point
    /// did not fit, and every one of those voxels is darker than the physics
    /// asked for, in a hue the clip has shifted.
    long long voxelsLit = 0;
    long long voxelsAtMax = 0;
    /// The same count on the DIRECT volume (radiance since PHOTON-7 stage 1: on a
    /// UNORM store any light brighter than 1 sits there), and the total voxel
    /// count for the record.
    long long directAtMax = 0;
    long long voxels = 0;
    /// Voxels whose peak channel is ABOVE 1.0 in the store's units — zero by
    /// definition on a UNORM store, and on a float one exactly the set an
    /// 8-bit store would have CLIPPED. It is the measurement that says whether
    /// the format's range is being used or merely provided.
    long long voxelsAboveOne = 0;

    /// THE SOURCE, beside the lit volumes above (VOXEL-CLIP-1). The lit volume
    /// is what the cones read; the EMISSIVE VOXEL STORE is what the injection
    /// SEEDS it from, so a clip there cannot be told from a dim emitter anywhere
    /// downstream. These read the voxeliser's own emissive volume, in SCENE
    /// RADIANCE (the voxeliser writes the material's emissive as authored).
    ///
    /// `emissiveFormat` is empty when this cascade has no voxeliser to read.
    /// `peakEmissive` is the largest channel in the volume — for a scene with
    /// one emitter, its authored radiance. `emissiveAtMax` counts texels on a
    /// UNORM store's top bin, which IS the clip, and `emissiveAboveOne` counts
    /// what an 8-bit store could not have held.
    std::string emissiveFormat;
    float peakEmissive = 0.0f;
    long long emissiveAtMax = 0;
    long long emissiveAboveOne = 0;

    /// THE BYTES (PHOTON-WRITER-1): a 64-bit FNV-1a hash over the raw bytes of
    /// every light volume a reader of this cascade samples — all ten of the
    /// voxel lighting's list (mip 0 of each: the total, the three axis volumes, the
    /// coverage and the position per half, the back side, the normal), in that
    /// order — as 16 hex digits. Two readings are equal exactly when the
    /// volumes are byte-identical: it is the instrument of the proof that one
    /// at-rest sweep IS the chain's fixed point (gi.chain_converge).
    std::string lightDigest;
    /// THE VOXELISER'S STORE, hashed the same way (ENGINE-UPGRADE-U5 stage 2): level 0 of the albedo,
    /// the emissive, the normal, the coverage and the surface position per half, the area weight, the
    /// metal and the emissive factor per half (all eleven volumes a build writes) - what a build WRITES,
    /// independent of the light's history - so two builds of one place compare by 16 hex digits (the
    /// voxeliser's work partition proved bit for bit on a real scene, ENGINE-UPGRADE-U5 stage 2).
    std::string storeDigest;
    /// THE DIRECT TERM ALONE, hashed the same way (PHOTON-2 stage 0): mip 0 of the cascade's DIRECT volume (the
    /// injection's sun, lamps and emission before any bounce — what the bounce iterates from; fork ae2ed529f's D
    /// term), so "is a voxel's injected light a function of the world point" is read without the bounce folded in
    /// (lightDigest folds every bounce, which reads the other cascades as they stand). Empty on a cascade with no
    /// direct volume (one that does not bounce) or a download the device refused.
    std::string directDigest;
};

/// ONE CASCADE'S VOXELS, WHOLE (PHOTON-VOXEL-3) — the test and tool readback of
/// mip 0 of a cascade's TOTAL light volume and of the voxeliser's ALBEDO volume,
/// decoded to linear floats, with the lattice they sit on. It is the instrument
/// of the coverage rule: a voxel's colour integrates to the surface area inside
/// it, so a wall's column of voxels is summed and composited against the wall
/// as authored, which no aggregate can do. Blocks exactly like giVoxelStats.
/// THE FEEDBACK'S BRICK STATE, read back for a law (PHOTON-7 stage 10, the orbit's census - Scene::giFeedbackBricks): per
/// brick of a cascade (x fastest over `dims`), its flags word (bit 0 active, bit 11 its contraction MEASURED), its `r`
/// (the measured contraction once bit 11 is set, the operator's bound before) and its last pass's relative move.
struct GiFeedbackBricks {
    bool available = false;
    unsigned dims[3] = { 0u, 0u, 0u };
    std::vector<unsigned> flags;
    std::vector<float> r;
    std::vector<float> lastMove;
};

/// THE VOXEL SIDES' ESCAPE SHARE (PHOTON-7 11-2 (6), the openings' instrument): cascade 0's grid (GiVoxelVolume's
/// width/height/depth, x fastest), one word a voxel as the sides' TLAS gather last wrote it - over its six halves (VOXEL-R13)
/// the share of their cosine rays that left the world (an EXIT: the sky): bits 0-7 the largest half's, 8-15 the halves'
/// coverage-weighted mean, bit 16 written. 0 = never gathered (empty, or no pass since the placement).
struct GiVoxelSideShares {
    bool available = false;
    int  width = 0, height = 0, depth = 0;
    std::vector<uint32_t> words;
};

struct GiVoxelVolume {
    bool available = false;
    int  width = 0, height = 0, depth = 0;
    /// Cascade's voxel origin (the corner of voxel 0,0,0) and cell, world metres.
    float origin[3] = { 0.f, 0.f, 0.f };
    float cell[3] = { 0.f, 0.f, 0.f };
    /// width*height*depth RGBA, x fastest: THE CELL'S LIGHT (VOXEL-R13: derived from
    /// the six halves below - rgb / a their coverage-weighted mean radiance, a the
    /// largest half's coverage k; the readers read the halves), the voxeliser's albedo
    /// volume (rgb = the surfaces' area-mean albedo, a = c = max of the six
    /// per-half-axis coverages), the COVERAGE PER HALF-AXIS (rgb = O_x, O_y, O_z of
    /// the faces looking +a (P) and -a (N): the fraction of the voxel's face along
    /// that axis they cover - a ray travelling +a sees the N half; a unused) and the
    /// SURFACE POSITION per half (rgb = O_a x p_a, p_a where along axis a those faces
    /// lie IN THE VOXEL'S OWN CELL, [0, 1] across it (PHOTON-7 stage 1: the same at every
    /// placement of the box; the cascade-normalised place is (i_a + p_a) / res_a) -
    /// O-premultiplied, so p_a = rgb / O_a; a the caster share's packed bits).
    std::vector<float> light;
    std::vector<float> albedo;
    std::vector<float> coverageP, coverageN;
    std::vector<float> positionP, positionN;
    /// THE REST OF THE VOXELISER'S LEVEL 0 (ENGINE-UPGRADE-U5, the reader's decision 11 g): the emissive
    /// (rgb, the material roughness in a), decoded like `albedo`; and the raw integer area weight a build
    /// keeps per voxel (`accumWeight`, width*height*depth). With the arrays above, every level-0 volume a
    /// build writes: the mips are regenerated from level 0.
    std::vector<float> emissive;
    /// THE EMISSIVE FACTOR PER HALF-AXIS (JUNCTION-NORMAL-1), rgb per axis of the faces looking +a (P) and -a (N),
    /// as stored: u = f / (1 + f) on 1022 steps of the 10-bit UNORM (f = c / (1022 - c), c = round(u x 1023)) - a
    /// half emits `emissive` x f.
    std::vector<float> emitP, emitN;
    std::vector<unsigned> accumWeight;
    /// THE DIRECT TERM (FAR-SUN-2), level 0, the cell's as `light` (from `halfDirect`, which the whole door returns
    /// too, as `halfLight`): every lamp's direct
    /// term and the emissive - what a hit's read takes out of the store (jah_rq_hit.glsl's bounce
    /// share 1 - D / T). Empty where the cascade keeps none (no bounce texture).
    std::vector<float> direct;
    /// THE ALBEDO VOLUME'S MIP LEVELS 1..n (ENGINE-UPGRADE-ASYNC-3 stage 1), decoded like `albedo`
    /// (level 0): albedoLevels[L - 1] is level L, max(1, size >> L) per axis, RGBA, x fastest — the
    /// compute chain's proof (each texel the blit's filter of the level below).
    std::vector<std::vector<float>> albedoLevels;
    /// THE DIRECTIONAL VOLUMES, EVERY LEVEL (PHOTON-7 stage 2's read D1): directional[a][L] is axis a's (X, Y, Z) level L,
    /// RGBA decoded like `light`, x fastest, at the texture's own size per level (max(1, size >> L) per axis; level 0 is
    /// the volume's width - both signs packed along x, a half each - by half its height and depth). What every cone past
    /// level 0 reads: step 0 composites them from the level-0 light and step 1 builds the levels, so they are a function
    /// of the level-0 light - gi.scroll_slab D compares a commit's with a whole regeneration (gi.regenerateDirectional).
    std::vector<std::vector<float>> directional[3];
    /// THE HALF STORE (VOXEL-R13; Scene::giVoxelHalves fills these with the coverage, the position and the
    /// placement; giVoxelVolume all four kinds, giVoxelLight `halfLight`): level 0's six HALF LIGHTS in `halfLight`, one volume 6 x width wide (x fastest): axis a's -a half's
    /// texel of cell x at 2 a width + x, its +a half's at ( 2 a + 1 ) width + x, RGBA decoded: (radiance x k, k), k the
    /// half's coverage capped at 1 - each half lit and gathered from its own faces; `halfDirect` their direct terms, the
    /// same layout (empty without a bounce). And THE HALVES' OWN FACES per axis at the volume's own size: `halfSide[a]`
    /// (rg the +a half's, ba the -a half's centroid across the axis in the cell's [0, 1], the other two axes in increasing
    /// order) and `halfNormal[a]` (the same halves' mean normal as a hemisphere octahedron about the half's own direction;
    /// 0.5 0.5 the axis).
    std::vector<float> halfLight;
    std::vector<float> halfDirect;
    std::vector<float> halfSide[3];
    std::vector<float> halfNormal[3];
};

}}  // namespace jahshaka::engine
