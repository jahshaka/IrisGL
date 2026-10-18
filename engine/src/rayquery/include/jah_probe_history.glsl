// THE GATHER'S PIXEL HISTORY, AS ITS READERS SEE IT (ENGINE-UPGRADE-U5 decision 21): the
// packed texel's geometry word and the same-surface read of the previous frame's history
// under a world point. Two readers: the integrate (rq_probe_integrate.comp: the pixel's own
// running mean) and the filter (rq_probe_filter.comp: the AGE of the history under a probe,
// which sets the probe-space filter's width). ONE text, so the two read one history the
// same way.
//
// A CALLER includes this after jah_probe_params.glsl and jah_reproject.glsl and after its
// own binding of the previous frame's history as `historyPrev` (rg32ui, one layer per eye).

#ifndef JAH_PROBE_HISTORY_GLSL
#define JAH_PROBE_HISTORY_GLSL

/// The history's count saturates here (4 bits); the blend reads it only up to
/// 1 / the floor (GatherTuning::historyFrames, 1..15; 10 shipped).
const uint kHistoryCountMax = 15u;

/// THE HISTORY'S GEOMETRY WORD's normal: an octahedral pair at 5 bits each
/// (about 3 degrees; the test is at 25). Zero words are "no surface here",
/// which the distance test rejects.
vec2 octEncode( vec3 n )
{
	vec2 o = n.xy / max( abs( n.x ) + abs( n.y ) + abs( n.z ), 1e-6 );
	if( n.z < 0.0 )
		o = ( 1.0 - abs( o.yx ) ) * vec2( o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0 );
	return o;
}
vec3 octDecode( vec2 o )
{
	vec3 n = vec3( o, 1.0 - abs( o.x ) - abs( o.y ) );
	if( n.z < 0.0 )
		n.xy = ( 1.0 - abs( n.yx ) ) * vec2( n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0 );
	return normalize( n );
}

/// THE GEOMETRY WORD: the distance (a half without its sign, its mantissa
/// rounded to 7 bits: 0.4 % against the 5 % test) in bits 0-11, the normal's
/// octahedral pair in 12-21, the count in 22-25 and the mean coverage (6 bits,
/// rounded like the mean, with `u`) in 26-31.
uint packGeom( float dist, vec3 n, uint count, float coverage, float u )
{
	const uint h = packHalf2x16( vec2( dist, 0.0 ) ) & 0x7FFFu;
	const uint d = min( ( h + 4u ) >> 3u, 0xFFFu );
	const uvec2 q = uvec2( clamp( round( ( octEncode( n ) * 0.5 + 0.5 ) * 31.0 ), vec2( 0.0 ),
								  vec2( 31.0 ) ) );
	const uint cov = uint( min( floor( clamp( coverage, 0.0, 1.0 ) * 63.0 + u ), 63.0 ) );
	return d | ( q.x << 12u ) | ( q.y << 17u ) | ( min( count, kHistoryCountMax ) << 22u ) | ( cov << 26u );
}
void unpackGeom( uint g, out float dist, out vec3 n, out uint count, out float coverage )
{
	dist = unpackHalf2x16( ( g & 0xFFFu ) << 3u ).x;
	n = octDecode( vec2( float( ( g >> 12u ) & 31u ), float( ( g >> 17u ) & 31u ) ) / 31.0 * 2.0 -
				   1.0 );
	count = ( g >> 22u ) & 15u;
	coverage = float( g >> 26u ) / 63.0;
}

/// THE HISTORY UNDER A WORLD POINT: the previous frame's packed texel of eye `eye` where
/// `world` lay (through that eye's PREVIOUS camera, onto that eye's own layer - an eye's mean
/// is never read from the other eye's texels), accepted only on THE SAME SURFACE: the same
/// distance (the 5 % test) AND the same facing (the plane test's normal dot, p.plane.y) - a
/// corner or a crease reprojects onto a surface at the same distance facing elsewhere, which
/// the distance alone would pass. knobs5.w = 1 is the test door that turns BOTH tests off
/// (GatherTuning::historyValidationOff; never shipped). False = no usable history (a first
/// frame, off the previous image, behind its camera, another surface, an empty texel).
bool jahHistoryAt( vec3 world, vec3 normal, int eye, out uvec2 texel, out uint count )
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
	vec3 wasNormal;
	uint wasCount;
	float wasCoverage;
	unpackGeom( t.y, wasDist, wasNormal, wasCount, wasCoverage );
	const bool accept = p.knobs5.w > 0.5 ||
						( jahSameDistance( wasDist, z ) && dot( wasNormal, normal ) >= p.plane.y );
	if( wasCount == 0u || !accept )
		return false;
	texel = t;
	count = wasCount;
	return true;
}

#endif
