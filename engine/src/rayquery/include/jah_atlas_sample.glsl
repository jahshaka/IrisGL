// THE PROBE ATLAS TEXEL'S "NO SAMPLE" DISTANCE - one constant for the gather's jobs and the hit write-back
// (rq_hit_composite.comp), which writes a gather record's texel too.
#ifndef JAH_ATLAS_SAMPLE_GLSL
#define JAH_ATLAS_SAMPLE_GLSL

/// THE ATLAS TEXEL'S DISTANCE (w) when the ray has NO SAMPLE this frame (PHOTON-2 stage 2's tail, 1.6 A2; stage 1b H3): its
/// record was dropped by a full hit list, or the decode left it UNSHADED (a stale decode twin's frame). -1 is a MISS (the
/// sky, a sample at infinity); this is no reading at all - the filter and the SH projection leave the texel out
/// (rq_probe_filter.comp), so neither a drop nor an unshaded record darkens a probe.
const float kAtlasNoSample = -2.0;

#endif   // JAH_ATLAS_SAMPLE_GLSL
