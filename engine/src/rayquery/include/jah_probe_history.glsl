// THE GATHER'S PIXEL HISTORY, AS ITS READER SEES IT (ENGINE-UPGRADE-U5 decision 21): the
// packed texel's geometry word and the same-surface read of the previous frame's history
// under a world point. One reader: the integrate (rq_probe_integrate.comp: the pixel's own
// running mean). (The filter read the AGE of the history under a probe until
// ENGINE-POLISH-PHOTON-2 deleted the age filter.)
//
// A CALLER includes this after jah_probe_params.glsl and jah_reproject.glsl and after its
// own binding of the previous frame's history as `historyPrev` (rg32ui, one layer per eye).

#ifndef JAH_PROBE_HISTORY_GLSL
#define JAH_PROBE_HISTORY_GLSL

/// The history's count saturates here (4 bits); the blend reads it only up to
/// 1 / the floor (GatherTuning::historyFrames, 1..15; 10 shipped).
const uint kHistoryCountMax = 15u;

/// THE GEOMETRY WORD (PHOTON-2 stage 2d): the view distance in bits 0-20 (a float without its sign
/// and its ten low mantissa bits, rounded: 13 bits of mantissa, 0.006 % - under the depth test's
/// 0.5 % floor by two orders), the surface CLASS in bit 21 (1 = foliage: an alpha-tested - cut-out -
/// surface, the prepass's normals alpha, JahGBufClass_piece_ps.any; PHOTON-2 K5), the count in 22-25 and the
/// mean coverage (6 bits, rounded like the mean, with `u`) in 26-31. (It held an octahedral normal
/// in 12-21 for a 25-degree test, which Lumen's rule does not make: its 10 bits went to the
/// distance and the class.) Zero words are "no surface here", which the distance test rejects.
uint packGeom( float dist, bool foliage, uint count, float coverage, float u )
{
	const uint d = min( ( floatBitsToUint( max( dist, 0.0 ) ) + 512u ) >> 10u, 0x1FFFFFu );
	const uint cov = uint( min( floor( clamp( coverage, 0.0, 1.0 ) * 63.0 + u ), 63.0 ) );
	return d | ( foliage ? ( 1u << 21u ) : 0u ) | ( min( count, kHistoryCountMax ) << 22u ) | ( cov << 26u );
}
void unpackGeom( uint g, out float dist, out bool foliage, out uint count, out float coverage )
{
	dist = uintBitsToFloat( ( g & 0x1FFFFFu ) << 10u );
	foliage = ( g & ( 1u << 21u ) ) != 0u;
	count = ( g >> 22u ) & 15u;
	coverage = float( g >> 26u ) / 63.0;
}

/// THE SAME-SURFACE TEST, LUMEN'S (PHOTON-2 stage 2d; LumenScreenProbeGatherTemporal.usf, the facts
/// file's item 4): the history's distance within `threshold` of this frame's, relative - 1 % of the
/// distance (3 % for FOLIAGE, whose cards and leaves shimmer at the 1 % test), jittered per pixel and
/// frame by lerp( 0.5, 1.5, noise ) and divided by lerp( 0.1, 1.0, NoV ) so a grazing surface, whose
/// reprojected depth moves most per pixel, may differ by up to ten times as much - and the SAME CLASS
/// (a foliage pixel never blends a non-foliage history, nor the reverse). NO NORMAL TEST (Lumen's
/// RejectBasedOnNormal is 0): the class and the depth decide; the 5 % / 25-degree rule it replaces
/// restarted a canopy's pixels on every leaf's facing. knobs2.x = 1 is the test door that takes the
/// CLASS out of the test (GatherTuning::classOff, PHOTON-2 K5's guard: every pixel the 1 % rule, no
/// class split; the class bit is still written, so a reader can tell the foliage pixels apart).
const float kGatherDepthThreshold = 0.01;
const float kGatherDepthThresholdFoliage = 0.03;
/// Set by the last jahGatherSameSurface that accepted: only the FOLIAGE class's looser bound let it through (its
/// depth delta between the 1 % and the 3 % bounds) - PHOTON-2 L8's count (rq_probe_integrate.comp).
bool gFoliageRuleOnly = false;
bool jahGatherSameSurface( float wasDist, bool wasFoliage, float dist, bool foliage, float NoV, float noise )
{
	const bool classTest = p.knobs2.x < 0.5;
	const float scale = mix( 0.5, 1.5, noise ) / mix( 0.1, 1.0, clamp( NoV, 0.0, 1.0 ) );
	const float t = ( classTest && foliage ? kGatherDepthThresholdFoliage : kGatherDepthThreshold ) * scale;
	const float delta = abs( wasDist - dist );
	const bool same = wasDist > 0.0 && ( !classTest || wasFoliage == foliage ) && delta < dist * t;
	gFoliageRuleOnly = same && classTest && foliage && !( delta < dist * kGatherDepthThreshold * scale );
	return same;
}

/// THE HISTORY UNDER A WORLD POINT: the previous frame's packed texel of eye `eye` where
/// `world` lay (through that eye's PREVIOUS camera, onto that eye's own layer - an eye's mean
/// is never read from the other eye's texels), accepted only on THE SAME SURFACE
/// (jahGatherSameSurface: the jittered relative depth over NoV and the class). knobs5.w = 1 is
/// the test door that turns the test off (GatherTuning::historyValidationOff; never shipped).
/// False = no usable history (a first frame, off the previous image, behind its camera, another
/// surface, an empty texel).
bool jahHistoryAt( vec3 world, bool foliage, float NoV, float noise, int eye, out uvec2 texel, out uint count )
{
	texel = uvec2( 0u );
	count = 0u;
	vec2 was;
	float z;
	const JahEye before = jahEyeBefore( eye );
	if( jahReproject( world, before.camPos, before.perspective, before.rayTL, before.rayRight,
					  before.rayDown, before.fwd, was, z ) != 0 )
		return false;
	const ivec2 size = ivec2( jahEyeWidth(), int( p.resolution.w ) );
	const ivec2 pc = clamp( ivec2( was * vec2( size ) ), ivec2( 0 ), size - 1 );
	const uvec2 t = imageLoad( historyPrev, ivec3( pc, eye ) ).xy;
	float wasDist;
	bool wasFoliage;
	uint wasCount;
	float wasCoverage;
	unpackGeom( t.y, wasDist, wasFoliage, wasCount, wasCoverage );
	const bool accept = p.knobs5.w > 0.5 || jahGatherSameSurface( wasDist, wasFoliage, z, foliage, NoV, noise );
	if( wasCount == 0u || !accept )
		return false;
	texel = t;
	count = wasCount;
	return true;
}

#endif
