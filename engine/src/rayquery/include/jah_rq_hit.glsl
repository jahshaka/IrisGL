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
//     JAH_VOX_ORIGIN(c)    vec4   xyz = the box's world origin, w padding (the
//                                 voxels hold radiance: PHOTON-7 stage 1 deleted
//                                 the per-cascade multiplier DRAG-1 undid)
//     JAH_VOX_INVSIZE(c)   vec4   xyz = 1/the box's world size, w = one cell
//     JAH_SKY_ON           bool   the environment cube is bound
//     JAH_SKY_COLOUR       vec3   with no cube bound, the environment's own flat
//                                 radiance (the SH's constant band); unread with
//                                 the cube, which is the capture of the sky as
//                                 drawn and holds the Sky Light's gain itself
//                                 (PHOTON-7 10-3 R5)
//
// ...and the sampler arrays `voxelHalf/voxelX/voxelY/voxelZ/voxelCovP/N/voxelPosP/N` (VOXEL-R13: `voxelHalf` the half
// store, level 0's six half lights) and the combined
// cube sampler `skyCube`, under those names, with kMaxCascades entries.
//
// THE ARITHMETIC IS rq_reflect.comp's, UNCHANGED, and the long rationale for
// every term of it lives at the call site in that file (the anisotropic slot
// blend in the ray's own direction, the six-cell crossfade at a cascade's face,
// the half-cell step into the surface's own cell, the opacity divide). What
// moved here is the CODE, not a decision: every reader gets the hit surface's
// own level-0 half (BISTRO-REST-1: never a footprint level, which spans what
// stands beyond the hit's walls), a mirror's with no crossfade.

#ifndef JAH_RQ_HIT_GLSL
#define JAH_RQ_HIT_GLSL

#include "jah_rq_finite.glsl"
#include "jah_voxel_halves.glsl"

// THE READ ITSELF IS THE ONE VOXEL RADIANCE READER'S (PHOTON-READER-1): the
// directional three-axis blend, the total's level 0
// and the usable-sample guard live in jah_voxel_sample.glsl, which the pixel
// shader's cone march, the bounce job and the irradiance field read too. This
// file keeps what is the HIT's own: which cascade answers, the step into the
// hit's cell, the six-cell crossfade and the opacity divide.
// The ray jobs declare combined sampler arrays under the names below, so the
// sample macros default to them.
#ifndef JAH_VOX_SAMPLE_ISO
// THE HALF STORE (VOXEL-R13: level 0's six half lights, one volume - jah_voxel_halves.glsl), `voxelHalf`
#define JAH_VOX_SAMPLE_ISO( c, u, l ) textureLod( voxelHalf[c], u, l )
#define JAH_VOX_SAMPLE_X( c, u, l ) textureLod( voxelX[c], u, l )
#define JAH_VOX_SAMPLE_Y( c, u, l ) textureLod( voxelY[c], u, l )
#define JAH_VOX_SAMPLE_Z( c, u, l ) textureLod( voxelZ[c], u, l )
// THE PER-HALF-AXIS COVERAGE (PHOTON-VOXEL-3/-4: the faces looking +a, then -a), bound
// BY NAME into `voxelCovP` / `voxelCovN` (RQ-COV-SLOT-1: it used to ride `voxelX` by the
// light-volume list's order), and THE SURFACE POSITION per half (`voxelPosP` / `voxelPosN`):
// the origin plane's test reads it (the hit's point reads pass no origin plane).
#define JAH_VOX_SAMPLE_COVP( c, u, l ) textureLod( voxelCovP[c], u, l )
#define JAH_VOX_SAMPLE_COVN( c, u, l ) textureLod( voxelCovN[c], u, l )
#define JAH_VOX_SAMPLE_POSP( c, u, l ) textureLod( voxelPosP[c], u, l )
#define JAH_VOX_SAMPLE_POSN( c, u, l ) textureLod( voxelPosN[c], u, l )
#endif
#ifndef JAH_VOX_INVRES
// 1 / the resolution per axis = the cell over the box's size.
#define JAH_VOX_INVRES( c ) ( JAH_VOX_INVSIZE( c ).xyz * JAH_VOX_INVSIZE( c ).w )
#endif
#include "jah_voxel_sample.glsl"

// THE ONE ENVIRONMENT, bound to the ray jobs' own names (a job that binds its own first - the probe integrate's SH:
// PHOTON-7 10-3 R4 (c1) - keeps it).
#ifndef JAH_ENV_CUBE_ON
#define JAH_ENV_CUBE_ON JAH_SKY_ON
#define JAH_ENV_SAMPLE( d, l ) textureLod( skyCube, d, l ).xyz
#define JAH_ENV_MIPS float( textureQueryLevels( skyCube ) )
// With no cube the environment is FLAT here (the host's constant band): the
// only coefficient the ray jobs are handed.
#define JAH_ENV_SH_C0 JAH_SKY_COLOUR
#define JAH_ENV_SH_C1 vec3( 0.0 )
#define JAH_ENV_SH_C2 vec3( 0.0 )
#define JAH_ENV_SH_C3 vec3( 0.0 )
#define JAH_ENV_SH_C4 vec3( 0.0 )
#define JAH_ENV_SH_C5 vec3( 0.0 )
#define JAH_ENV_SH_C6 vec3( 0.0 )
#define JAH_ENV_SH_C7 vec3( 0.0 )
#define JAH_ENV_SH_C8 vec3( 0.0 )
#endif
#include "jah_environment.glsl"

#ifndef JAH_HIT_GATE_DECLARED
#define JAH_HIT_GATE_DECLARED
/// C2, THE HIT's OWN SPECULAR TRACED (PHOTON-7 11-2 (3)): set by a reader that traces the hit's lobe itself
/// (jahHitSpecularLobeTraced, jah_rq_hit_radiance.glsl) around the store's read - the store then leaves out the INDIRECT
/// specular it holds only as the lobe's integral (the card's lobe share, the voxels' bounce share); the lights' direct
/// lobe stays (a light is not geometry: no ray meets it). False everywhere else.
bool jahHitTracedLobe = false;
#endif


/// WHICH CASCADE ANSWERED the last jahVoxelRadiance (the finest that contributed; -1 none) —
/// the gather-hits instrument's reading (FAR-SUN-1), nothing else reads it.
int jahVoxelAnswerCascade = -1;

/// THE STORE'S BOUNCE PART at the last jahVoxelRadiance's hit (FAR-SUN-2; jah_rq_hit_direct.glsl): the read of the total
/// less THE SAME READ of the direct chain (every lamp's direct light and the emissive, LightInjection_piece_cs.any) -
/// jah_voxel_sample.glsl's JAH_VOX_SAMPLE_DX, BISTRO-REST-1: one reader, the hit's level-0 half. With no bounce and no sky in
/// the store it is exactly 0; where the store holds only bounce, the read. A hit whose direct light is computed at the
/// hit adds this, and nothing else of the store.
vec3 jahVoxelBounceOut = vec3( 0.0 );

/// The radiance leaving `hitPos` towards where the ray came from, read out of
/// the cascade chain's light volumes - the side the ray hit, by `hitNormal`.
///
/// The read is the hit surface's own level-0 half (below: never a footprint level). `mirror`
/// takes the containing cascade WHOLE — a crossfade of two texel sizes is still a blur,
/// and a mirror is the one surface that must not be blurred. `ok` is false when no bound cascade holds anything there: the hit
/// is then a record for the visibility-buffer decode (jah_rq_hit_record.glsl,
/// PHOTON-HIT-SHADE-1) in both callers.
vec3 jahVoxelRadiance( vec3 hitPos, vec3 rayDir, vec3 hitNormal, bool mirror, out bool ok )
{
	// THE READ IS ANCHORED TO THE HIT'S NORMAL, NEVER TO THE RAY (PHOTON-7 stage 10, the lead's ruling on card_residency
	// (3)): a surface's radiance toward a ray is the radiance of the side the ray HIT, so the texel pair, the side, the
	// directional level and the read point are all taken from -n (the direction that meets the face head-on, oriented
	// along the ray). Along the ray, a far or grazing hit on a narrow object's face read the object's OTHER faces at the
	// footprint's level - room B's pillar: the lamp-lit -z face (2.23) into the +x face's answer (0.56), its readers 98
	// codes apart with the face's card resident or not; the card, which reads the face itself, was right.
	vec3 dir = rayDir;
	if( dot( hitNormal, hitNormal ) > 0.0 )
	{
		const vec3 nn = normalize( hitNormal );
		dir = dot( nn, rayDir ) <= 0.0 ? -nn : nn;
	}
	ok = false;
	jahVoxelAnswerCascade = -1;
	jahVoxelBounceOut = vec3( 0.0 );
	vec3 accBounce = vec3( 0.0 );
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
		// THE LEVEL BY THE CAMERA (PHOTON-7 stage 10, 10-2e; the lead's ruling): an inner cascade answers a hit only
		// within its LEVEL RADIUS of the GI camera (max-norm: the boxes are cubes) - half its box less ( 0.5 + h ) of a
		// step and a cell (covered by its box at every placement it can reach) less the measured margin (the bounce's reach) -
		// blending out over the band below it; the outermost by its box (nothing further out to hand to). So two
		// placements the engine can reach - a walk and a teleport to one pose - read the same level from the same
		// cells (the grid is world-anchored), where the box rule read the finest box holding the point. A cache's rays
		// (levelCam.w 0) read the finest box holding the hit, its edge blend below: a cache's store is a world function.
		float wLevel = -1.0;
		if( JAH_VOX_LEVEL_ON && c < count - 1 )
		{
			const vec3 dv = abs( hitPos - JAH_VOX_LEVEL_CAM );
			const float d = max( max( dv.x, dv.y ), dv.z );
			const vec4 lv = JAH_VOX_LEVEL( c );
			wLevel = mirror ? ( d <= lv.x ? 1.0 : 0.0 ) : clamp( ( lv.x - d ) / max( lv.y, 1e-6 ), 0.0, 1.0 );
			if( !( wLevel > 0.0 ) )
				continue;				// past this cascade's level: the next one out answers
		}
		// THE HIT READS ITS OWN SURFACE'S LEVEL-0 HALF, NEVER A FOOTPRINT (BISTRO-REST-1, the lead's ruling (1)): the store's
		// answer at a hit - its bounce part B = T - D and the total - is the hit surface's own level-0 half, both terms
		// through the same reader at the same level (lod 0). A footprint level's texel spans what stands beyond the hit's
		// walls: the directional chain's levels composite ALONG the read direction, so a coarse texel's +Y face holds the
		// TOPMOST face it covers - a sealed room's floor hit read the sky-lit roof's bounce (gi.sealed_sky (3): 0.32 % of
		// the roof in cascade 0's inside halves, the bar 0.053 %). The share the bounce read replaced hid it by weighing
		// the footprint's read with the pick's own (dark) share. (Deleted with the footprint: its lod, the cache's cap at
		// a cascade's world-anchored K and the view's cap - the hit read has no level but 0; a mirror's was 0 already.)
		const float lod = 0.0;
		// THE HIT READS THE TEXEL ITS SURFACE WAS VOXELISED INTO (PHOTON-VOXEL-5 item (iv)).
		// Along the ray's dominant axis the surface lies in one of the two texels either side
		// of the hit - the voxels are half-open, so a face ON a texel plane belongs to the
		// texel above it whichever way the ray travels. The read half a cell past the hit
		// (trilinear) took the texel BEHIND a face that sits in the lower half of its own:
		// a static crate's face on a texel plane read the empty interior and the hit was
		// handed back (gi.hit_voxel; HIT-SHADE-1's F-A: 0.045 against the raster's 0.366).
		// Both texels are read whole (their centres along the axis, the lateral position
		// kept) and the one holding the surface - the larger opacity along the ray - answers.
		vec4 s;
#ifdef JAH_VOX_SAMPLE_DX
		vec3 sD = vec3( 0.0 );   // the direct chain's read, through the same reader (jah_voxel_sample.glsl jahVoxSampleD)
#endif
		{
			const vec3 ir = JAH_VOX_INVRES( c );
			const int ax = jahVoxelAxis( dir, ir );
			const float at = ( hitPos[ax] - origin[ax] ) * invSize[ax];
			const float k = floor( at / ir[ax] );
			vec3 lsA = ls, lsB = ls;
			lsA[ax] = ( k + 0.5 ) * ir[ax];
			const float kOther = ( at / ir[ax] - k ) < 0.5 ? k - 1.0 : k + 1.0;
			lsB[ax] = ( kOther + 0.5 ) * ir[ax];
			const vec4 sA = jahVoxelSample( c, lsA, dir, lod );
#ifdef JAH_VOX_SAMPLE_DX
			const vec3 dA = jahVoxSampleD;
#endif
			const vec4 sB = jahVoxelSample( c, lsB, dir, lod );
			s = sB.w > sA.w ? sB : sA;
#ifdef JAH_VOX_SAMPLE_DX
			sD = sB.w > sA.w ? jahVoxSampleD : dA;
#endif
		}
		if( !jahVoxelSampleUsable( s ) )
			continue;					// this cascade holds nothing here
		float w = 1.0;
		if( wLevel >= 0.0 )
			w = wLevel;
		else if( !mirror )
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
		vec3 rad = s.xyz / s.w;
#ifdef JAH_VOX_SAMPLE_DX
		// THE STORE'S BOUNCE PART, READ (BISTRO-REST-1, the lead's ruling - form 3): the same read of the DIRECT chain - its
		// every tap at the read's own uvw, level 0, clamp and weights (jah_voxel_sample.glsl) - subtracted: B = T - D at the
		// hit's own level-0 half, no quotient, no precision gate, LINEAR in the store (a loop through it has the operator's
		// gain). Level 0 holds T = B + D texel by texel (the half store and its direct terms; the directional level 0 and
		// the direct's, written by the injection and step 0 over one set of coverages), and both reads take the same taps
		// with the same weights, so a room lit from inside neither goes negative nor counts its direct twice. An empty read
		// (s.w 0) never comes here. THE CLAMP below takes ONLY the two half-float stores' rounding: T and D are each rounded
		// once to the half float, so T - D >= -1/2 a step of each, never a term.
		// (The share it replaces read ( T - D ) / T at the PICK texel against a footprint-level read, weighed by a
		// precision trust in T: the read amplified a half's bounce up to 3-4x over the dim range and two halves of one
		// coarse cell orbited at period 2 - Bistro Exterior at Epic never at rest; gi.share_law.)
		accBounce += max( rad - sD / s.w, vec3( 0.0 ) ) * w;
#endif
		acc += rad * w;
		accW += w;
		if( !ok )
			jahVoxelAnswerCascade = c;
		ok = true;
	}
	if( !ok )
		return vec3( 0.0 );
	jahVoxelBounceOut = clamp( accBounce / max( accW, 1e-6 ), vec3( 0.0 ), vec3( kMaxRadiance ) );
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
