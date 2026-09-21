// WHAT A RAY'S HIT IS WORTH — the ONE copy, shared by every ray job
// (GATHER-1a, 2026-09-21; SCREEN_PROBE_GATHER_SPEC.md section 10 item 7).
//
// This file holds the two functions that turn a ray's outcome into radiance:
// `jahVoxelRadiance`, which reads the cascade chain's light volumes at a hit,
// and `jahSkyRadiance`, which answers an escaping one. They used to exist twice
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
//     JAH_SKY_ON           bool   a sky cubemap is bound
//     JAH_SKY_COLOUR       vec3   ...and the flat colour when it is not
//
// ...and the sampler arrays `voxelIso/voxelX/voxelY/voxelZ` and the cube
// `skyCube`, under those names, with kMaxCascades entries.
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
			lod = clamp( log2( max( footprint, texel ) / texel ), 0.0, 6.0 );
		vec4 s;
		if( JAH_VOX_ANISO )
		{
			vec3 isNegative;
			isNegative.x = dir.x < 0.0 ? 0.5 : 0.0;
			isNegative.y = dir.y < 0.0 ? 0.5 : 0.0;
			isNegative.z = dir.z < 0.0 ? 0.5 : 0.0;
			vec3 uvw = ls;
			uvw.x = clamp( uvw.x, 0.0, 1.0 ) * 0.5;
			const vec4 xc = textureLod( voxelX[c], uvw + vec3( isNegative.x, 0.0, 0.0 ), lod );
			const vec4 yc = textureLod( voxelY[c], uvw + vec3( isNegative.y, 0.0, 0.0 ), lod );
			const vec4 zc = textureLod( voxelZ[c], uvw + vec3( isNegative.z, 0.0, 0.0 ), lod );
			const vec3 w = dir * dir;
			s = w.x * xc + w.y * yc + w.z * zc;
		}
		else
		{
			// Low tier: VctLighting was built without the anisotropic chains and
			// slot 0 carries every mip. Directionally averaged, and the honest
			// answer available.
			s = textureLod( voxelIso[c], ls, lod );
		}
		if( s.w <= 0.02 || !finite3( s.xyz ) || !finite1( s.w ) )
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

/// What an escaping ray sees.
vec3 jahSkyRadiance( vec3 dir )
{
	if( !JAH_SKY_ON )
		return JAH_SKY_COLOUR;
	// Ogre samples cubemaps LEFT-HANDED (the sky/IBL adoption's fact).
	const vec4 s = textureLod( skyCube, vec3( dir.x, dir.y, -dir.z ), 0.0 );
	if( !finite3( s.xyz ) )
		return JAH_SKY_COLOUR;
	return clamp( s.xyz, vec3( 0.0 ), vec3( kMaxRadiance ) );
}

#endif   // JAH_RQ_HIT_GLSL
