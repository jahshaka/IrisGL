// ONE VOXEL RADIANCE READ — one cascade, one point (PHOTON-READER-1; the
// render audit's section 7.1 "ONE VOXEL RADIANCE READER").
//
// THE ONE SOURCE, FOUR CONSUMERS. This file is plain Vulkan GLSL and it is read
// two ways:
//   * glslang's offline path (the ray jobs: rq_reflect, rq_probe_gather) reads
//     it through `#include`, with `-I src/rayquery/include`;
//   * the Hlms paths (the pixel shader's cone march, the bounce-injection job,
//     the irradiance field's generation job) read it as the piece
//     `JahVoxelSample`, which the build WRAPS this file into
//     (irisgl/engine/CMakeLists.txt, `Hlms/Jahshaka/JahVoxelSample_piece_all.any`).
// No hand copy of it exists anywhere. The wrapper makes this file Hlms input,
// so NO CHARACTER OF IT MAY BE THE HLMS DIRECTIVE MARK, comments included (the
// Hlms parser executes that mark wherever it finds it — DOCS/traps/ENGINE.md).
// It includes nothing (the Hlms path has no include): a caller includes
// jah_rq_finite.glsl FIRST (the wrapper puts it in the same piece).
//
// HOW A CALLER BINDS IT. The shader's bindings are its own; this file reads the
// volumes through macros the caller defines before it:
//
//     JAH_VOX_HAS_ANISO          0 or 1, a PREPROCESSOR constant: the three
//                                anisotropic slots are DECLARED in this shader
//     JAH_VOX_ANISO              bool: they carry data (a runtime choice is
//                                fine; the Low tier builds without them)
//     JAH_VOX_SAMPLE_ISO(c,u,l)  vec4: cascade c's isotropic volume at uvw u,
//                                mip l (Low: every mip; anisotropic tiers: the
//                                full-resolution level the march starts in)
//     JAH_VOX_SAMPLE_X(c,u,l)    vec4: the X-axis anisotropic volume (both
//     JAH_VOX_SAMPLE_Y(c,u,l)    signs packed side by side along u.x, so u.x
//     JAH_VOX_SAMPLE_Z(c,u,l)    runs over [0, 1) and the halves are 0.5 apart)
//
// THE VOLUME'S DIRECTIONAL READ IS THE ONLY RULE HERE — the anisotropic three-
// axis blend in the direction of travel: the half of each axis volume that was
// composited TOWARDS where the reader looks, weighted by the squared direction
// component. That is VctLighting's own anisotropic encoding (upstream
// AnisotropicMipVctStep0/1) read back, and it was written out twice — once in
// upstream's cone march (Vct_piece_ps.any) and once in the ray hit
// (jah_rq_hit.glsl) — with identical arithmetic. It is now written once.

#ifndef JAH_VOXEL_SAMPLE_GLSL
#define JAH_VOXEL_SAMPLE_GLSL

/// The packed-halves coordinate of an anisotropic read: x is saturated into the
/// box and folded into the first half (the sign half is added per axis by the
/// read below).
vec3 jahVoxelAnisoUvw( vec3 posLS )
{
	vec3 uvw = posLS;
	uvw.x = clamp( uvw.x, 0.0, 1.0 ) * 0.5;
	return uvw;
}

#if JAH_VOX_HAS_ANISO
/// Cascade c's anisotropic radiance at `anisoUvw` (jahVoxelAnisoUvw) seen along
/// `dir`, mip `lod`: each axis volume read from the half composited towards the
/// reader (the sign of that component of `dir`), weighted by dir squared.
vec4 jahVoxelSampleAniso( int c, vec3 anisoUvw, vec3 dir, float lod )
{
	vec3 isNegative;
	isNegative.x = dir.x < 0.0 ? 0.5 : 0.0;
	isNegative.y = dir.y < 0.0 ? 0.5 : 0.0;
	isNegative.z = dir.z < 0.0 ? 0.5 : 0.0;
	vec4 xc = JAH_VOX_SAMPLE_X( c, anisoUvw + vec3( isNegative.x, 0.0, 0.0 ), lod );
	vec4 yc = JAH_VOX_SAMPLE_Y( c, anisoUvw + vec3( isNegative.y, 0.0, 0.0 ), lod );
	vec4 zc = JAH_VOX_SAMPLE_Z( c, anisoUvw + vec3( isNegative.z, 0.0, 0.0 ), lod );
	vec3 w = dir * dir;
	return w.x * xc + w.y * yc + w.z * zc;
}

/// THE OCCUPANCY ESTIMATE THE ESCAPE RIDES (the fork's escape rule: the mean of
/// both packed halves of every axis volume, then the MIN over the three axes,
/// one mip FINER than the colour read).
///
/// WHY NOT THE COLOUR READ'S OWN ALPHA. Each axis volume is front-to-back
/// COMPOSITED along its axis, so a surface one voxel thick reads opacity 1 along
/// its own normal at every coarser mip however little of the cell it fills —
/// the right opacity for a ray CROSSING the cell and the wrong one for a cone
/// that STARTS on that surface: as its footprint grows it re-enters the cell
/// that holds its own slab and the direction-weighted alpha saturates within a
/// few steps (measured on an open floor with nothing above it: the isotropic
/// march kept 93 % of the ambient, the directional alpha kept 4 %). The MIN of
/// the three axis composites is the cell's mean occupancy up to the compositing
/// (a slab one layer thick in an N-layer cell reads 1 along its normal, about
/// 1/N along the other two; a solid cell reads 1 on all three), and one mip
/// finer restores most of what the coarser footprint alone loses (min3 at the
/// colour's mip kept 26 % of the open-floor ambient, one mip finer 65 %). It
/// still UNDER-estimates the escape, deliberately: the exact route (the
/// isotropic volume's own mip alpha) reaches 95 % on the open floor and lets a
/// cone escape through a one-voxel wall — a sealed room's floor flooded by +46
/// and +51 of 255 at Medium.
///
/// WHY BOTH HALVES. The half an axis volume is read from is chosen by the SIGN
/// of that component of the direction. The colour read weights each axis by
/// the squared component, so as a component goes to zero the half it chose
/// stops mattering; a MIN has no such weight, and on every axis-aligned surface
/// two components are zero in exact arithmetic and a coin toss in the shader
/// (the last bits of a round trip through the view matrix, decided by where
/// the camera stands in the world — measured: the same voxels read 8-9 codes
/// brighter beyond 32 m from the origin). The mean of the two halves is the
/// direction-free reading, still a composite (a one-voxel wall still reports 1
/// along its normal, the sealed-room leak stays shut) and continuous in the
/// direction. Six alpha fetches.
float jahVoxelOccupancy( int c, vec3 anisoUvw, float colourLod )
{
	float escapeLod = max( colourLod - 1.0, 0.0 );
	float ex = 0.5 * ( JAH_VOX_SAMPLE_X( c, anisoUvw, escapeLod ).w +
					   JAH_VOX_SAMPLE_X( c, anisoUvw + vec3( 0.5, 0.0, 0.0 ), escapeLod ).w );
	float ey = 0.5 * ( JAH_VOX_SAMPLE_Y( c, anisoUvw, escapeLod ).w +
					   JAH_VOX_SAMPLE_Y( c, anisoUvw + vec3( 0.5, 0.0, 0.0 ), escapeLod ).w );
	float ez = 0.5 * ( JAH_VOX_SAMPLE_Z( c, anisoUvw, escapeLod ).w +
					   JAH_VOX_SAMPLE_Z( c, anisoUvw + vec3( 0.5, 0.0, 0.0 ), escapeLod ).w );
	return min( ex, min( ey, ez ) );
}
#endif

/// Cascade c's radiance at `posLS` (the cascade's normalised [0, 1] box) seen
/// along `dir`, mip `lod`: the directional read where the anisotropic slots
/// carry data, the isotropic volume otherwise (Low: VctLighting was built
/// without the anisotropic chains and slot 0 carries every mip — directionally
/// averaged, and the honest answer available).
vec4 jahVoxelSample( int c, vec3 posLS, vec3 dir, float lod )
{
#if JAH_VOX_HAS_ANISO
	if( JAH_VOX_ANISO )
		return jahVoxelSampleAniso( c, jahVoxelAnisoUvw( posLS ), dir, lod );
#endif
	return JAH_VOX_SAMPLE_ISO( c, posLS, lod );
}

/// THE MIP FOR A WORLD FOOTPRINT against a cascade's directional texel (both in
/// world units): the mip whose texel is as wide as the thing that asked, never
/// finer than the finest level and never past the sixth. A directional texel is
/// TWO cells — the anisotropic volumes are half resolution on two axes with
/// both signs packed into the third.
float jahVoxelFootprintLod( float footprint, float texel )
{
	return clamp( log2( max( footprint, texel ) / texel ), 0.0, 6.0 );
}

/// A read that holds nothing, or holds something that is not a number, is no
/// answer (the finite test is on the BITS: a self-equality is folded to true on
/// this stack — jah_rq_finite.glsl).
bool jahVoxelSampleUsable( vec4 s )
{
	return s.w > 0.02 && finite3( s.xyz ) && finite1( s.w );
}

#endif   // JAH_VOXEL_SAMPLE_GLSL
