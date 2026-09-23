// WHAT A RAY'S HIT IS WORTH — the ONE copy, shared by every ray job
// (GATHER-1a, 2026-09-21; SCREEN_PROBE_GATHER_SPEC.md section 10 item 7).
//
// This file holds the two functions that turn a ray's outcome into radiance:
// `jahVoxelRadiance`, which reads the cascade chain's light volumes at a hit,
// and `jahSkyRadiance`, which answers an escaping one through the ONE
// ENVIRONMENT (jah_environment.glsl — the disc-free sky cube at the ray's own
// footprint, PHOTON-ENV-1's GA-SKY). They used to exist twice
// — once in rq_reflect.comp and once, copied, in rq_probe_gather.comp — and two
// consumers of one cache that can disagree about what a voxel MEANS is exactly
// the class of defect the voxel program keeps finding. The CMake rule passes
// `-I` at this directory (glslang's `GL_GOOGLE_include_directive`), so there is
// no generated concatenation and no build step of its own.
//
// HOW A CALLER USES IT. The bindings a shader declares are its own; this file
// reads them through macros the caller defines FIRST, so nothing here fixes a
// binding number:
//
//     JAH_VOX_COUNT        int    how many cascades are bound
//     JAH_VOX_ANISO        bool   the volumes carry the three anisotropic slots
//     JAH_VOX_ORIGIN(c)    vec4   xyz = the box's world origin, w = the
//                                 cascade's radiance multiplier (the bake's
//                                 normalisation undone — DRAG-1)
//     JAH_VOX_INVSIZE(c)   vec4   xyz = 1/the box's world size, w = one cell
//     JAH_SKY_ON           bool   the environment cube is bound
//     JAH_SKY_COLOUR       vec3   with the cube bound, the environment light's
//                                 gain on it (the Sky Light's intensity times
//                                 its tint); with none, the environment's own
//                                 flat radiance (the SH's constant band) — the
//                                 host hands whichever of the two applies
//
// ...and the sampler arrays `voxelIso/voxelX/voxelY/voxelZ` and the combined
// cube sampler `skyCube`, under those names, with kMaxCascades entries.
//
// THE ARITHMETIC IS rq_reflect.comp's, UNCHANGED, and the long rationale for
// every term of it lives at the call site in that file (the anisotropic slot
// blend in the ray's own direction, the six-cell crossfade at a cascade's face,
// the half-cell step into the surface's own cell, the per-cascade multiplier
// that undoes VctLighting's baking normalisation, the opacity divide). What
// moved here is the CODE, not a decision: the reflection path passes the
// footprint its lobe implies and asks for no crossfade when it is a mirror; the
// gather passes the footprint ONE RAY of its probe covers. Both get the same
// answer for the same footprint, which is the point.

#ifndef JAH_RQ_HIT_GLSL
#define JAH_RQ_HIT_GLSL

#include "jah_rq_finite.glsl"

// THE READ ITSELF IS THE ONE VOXEL RADIANCE READER'S (PHOTON-READER-1): the
// anisotropic three-axis blend, the isotropic slot at Low, the footprint's mip
// and the usable-sample guard live in jah_voxel_sample.glsl, which the pixel
// shader's cone march, the bounce job and the irradiance field read too. This
// file keeps what is the HIT's own: which cascade answers, the step into the
// hit's cell, the six-cell crossfade, the multiplier and the opacity divide.
// The ray jobs declare combined sampler arrays under the names below, so the
// sample macros default to them.
#ifndef JAH_VOX_HAS_ANISO
#define JAH_VOX_HAS_ANISO 1
#endif
#ifndef JAH_VOX_SAMPLE_ISO
#define JAH_VOX_SAMPLE_ISO( c, u, l ) textureLod( voxelIso[c], u, l )
#define JAH_VOX_SAMPLE_X( c, u, l ) textureLod( voxelX[c], u, l )
#define JAH_VOX_SAMPLE_Y( c, u, l ) textureLod( voxelY[c], u, l )
#define JAH_VOX_SAMPLE_Z( c, u, l ) textureLod( voxelZ[c], u, l )
#endif
#include "jah_voxel_sample.glsl"

// THE ONE ENVIRONMENT, bound to the ray jobs' own names.
#define JAH_ENV_CUBE_ON JAH_SKY_ON
#define JAH_ENV_SAMPLE( d, l ) textureLod( skyCube, d, l ).xyz
#define JAH_ENV_MIPS float( textureQueryLevels( skyCube ) )
#define JAH_ENV_GAIN JAH_SKY_COLOUR
#define JAH_ENV_SH( n ) JAH_SKY_COLOUR
#include "jah_environment.glsl"

const float kMaxRadiance = 1024.0;

/// The radiance leaving `hitPos` towards where the ray came from, read out of
/// the cascade chain's light volumes.
///
/// `footprint` is how wide, in world units, the thing that asked is: a mip
/// exists for exactly that width and reading the finest one aliases. `mirror`
/// takes the containing cascade WHOLE and mip 0 — a crossfade of two texel
/// sizes is still a blur, and a mirror is the one surface that must not be
/// blurred. `ok` is false when no bound cascade holds anything there, which the
/// CALLER decides what to do about (the gather draws it black; a reflection
/// hands the pixel back).
vec3 jahVoxelRadiance( vec3 hitPos, vec3 dir, float footprint, bool mirror, out bool ok )
{
	ok = false;
	const int count = JAH_VOX_COUNT;
	// HOW WIDE THE HAND-OVER BAND IS, in cells of the cascade being left. Three
	// DIRECTIONAL texels (a directional texel is two cells: the anisotropic
	// textures are half resolution on two axes with both signs packed into the
	// third), which is the narrowest band that can hide a 2x texel step and the
	// widest that cannot reach a cascade's own centre.
	const float kBlendCells = 6.0;
	vec3 acc = vec3( 0.0 );
	float accW = 0.0;
	for( int c = 0; c < count; ++c )
	{
		if( accW >= 0.999 )
			break;
		const vec4 invSize = JAH_VOX_INVSIZE( c );
		const vec4 origin = JAH_VOX_ORIGIN( c );
		const vec3 pos = hitPos + dir * ( 0.5 * invSize.w );
		const vec3 ls = ( pos - origin.xyz ) * invSize.xyz;
		if( any( lessThan( ls, vec3( 0.0 ) ) ) || any( greaterThan( ls, vec3( 1.0 ) ) ) )
			continue;					// not this cascade's business; try the next one out
		// The mip for this cascade's own texel size. `invSize.w` is the cell in
		// world units; a directional texel is two of them.
		const float texel = 2.0 * max( invSize.w, 1e-6 );
		float lod = 0.0;
		if( !mirror )
			lod = jahVoxelFootprintLod( footprint, texel );
		const vec4 s = jahVoxelSample( c, ls, dir, lod );
		if( !jahVoxelSampleUsable( s ) )
			continue;					// this cascade holds nothing here
		float w = 1.0;
		if( !mirror )
		{
			const vec3 e = min( ls, vec3( 1.0 ) - ls );
			const float edge = min( min( e.x, e.y ), e.z );
			// A cell expressed in this cascade's normalised space: cell (world)
			// times 1/size (world) on the widest axis.
			const float cellLS = invSize.w * max( max( invSize.x, invSize.y ), invSize.z );
			w = clamp( edge / max( kBlendCells * cellLS, 1e-6 ), 0.0, 1.0 );
		}
		w = min( w, 1.0 - accW );
		if( w <= 0.0 )
			continue;
		acc += ( s.xyz / s.w ) * ( w * max( origin.w, 0.0 ) );
		accW += w;
		ok = true;
	}
	if( !ok )
		return vec3( 0.0 );
	// Whatever weight the chain could actually answer for is the whole answer:
	// a hit in the last cascade's outermost band has nothing further out to fade
	// into, and normalising there is "this is all there is", not a hole.
	return clamp( acc / max( accW, 1e-6 ), vec3( 0.0 ), vec3( kMaxRadiance ) );
}

/// What an escaping ray sees: the environment in its direction, AT ITS OWN
/// FOOTPRINT (GA-SKY). `tanHalfAngle` is the half-angle of the solid angle the
/// ray stands for — one of a probe's N directions covers 2 pi / N, a
/// reflection's stochastic sample the spacing between the samples its mean
/// holds — and the cone lookup reads the prefiltered chain at the mip whose lobe
/// matches it: mip 0 for a mirror, and never the finest mip for a ray that
/// stands for a wide solid angle (reading it aliases exactly as a cone march
/// with no cone does). The cube carries no sun disc (the disc is the direct
/// sun, a light — the capture excludes it), so a miss cannot count the sun a
/// second time.
vec3 jahSkyRadiance( vec3 dir, float tanHalfAngle )
{
	const vec3 s = jahEnvCone( dir, tanHalfAngle );
	if( !finite3( s ) )
		return vec3( 0.0 );
	return clamp( s, vec3( 0.0 ), vec3( kMaxRadiance ) );
}

#endif   // JAH_RQ_HIT_GLSL
