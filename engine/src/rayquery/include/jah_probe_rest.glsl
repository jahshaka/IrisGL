// THE GATHER'S REST PER SCREEN CELL (PHOTON-7 stage 8B, B1; SPECS/PHOTON_7_SPEC.md 8B, the lead's rulings §3500, §3502).
//
// MOVING is the VIEW's (the camera, a restart of the estimator): every cell settles. With the camera still each cell is
// ACTIVE or HELD:
//   ACTIVE  - its probes are traced and its pixels integrated, on the view's ONE REST CLOCK K (0 = settling, 1 ... N the
//             rest mean, the shipped rest law of rq_probe_integrate.comp). ONE clock for every active cell (rule (a)): the
//             placement's grid offset, the adaptive window and the integrate's bilinear are ONE offset a frame for every
//             cell (rq_probe_place.comp), so two neighbouring cells on two rest indices would read each other's probes at
//             the wrong index and the held picture would follow the path. A cell any input of which changed (rq_probe_
//             resume.comp's four tests) RESUMES and puts the clock back to 0 - every active cell re-rests with it.
//   HELD    - its picture is its rest mean at K = N, bit for bit: it is never traced for itself and never integrated (its
//             pixels write their history through the ping-pong, rq_probe_integrate.comp). A held cell inside the RING of
//             an active one is traced at the active clock (its probes are what the active pixels' taps and the filter
//             read) and still never integrated.
// The clock lives on the GPU (the resume decides it): every gather job reads K and the frame's sample index here.
#ifndef JAH_PROBE_REST_GLSL
#define JAH_PROBE_REST_GLSL

const uint kCellHeld = 1u;      ///< the cell holds its rest picture
const uint kCellRing = 2u;      ///< ...and is traced this frame for an active neighbour (never integrated)
const uint kCellReached = 4u;   ///< an input reached it this frame (the instrument's bit; the resume's own)
const uint kCellField = 8u;     ///< a pixel of it read the field's fallback since the camera stopped (the integrate's)

/// THE BINS OF THE REST's DISTANCE STORE: 8 x 8 over the WHOLE SPHERE of world directions (an octahedral map), so the
/// cell's uniform probe and its adaptive twin - two tangent frames - fill one store. Each bin keeps the least and the
/// most HIT distance its rays found since the camera stopped (two float words), and one bit a bin says a ray of it
/// ESCAPED (to the far plane: it reads the sky, and anything that comes into its path blocks it) - 130 words, 520 B a
/// cell: [cell * 130 + bin] least, [+ 64 + bin] most, [+ 128 + bin / 32] the escape bits.
const uint kRestBins = 64u;
const uint kRestBinSide = 8u;
const uint kRestStoreWords = 130u;

#ifndef JAH_REST_CLOCK_QUALIFIER
#define JAH_REST_CLOCK_QUALIFIER readonly
#endif
/// [0] K, the rest clock; [1] the frame's sample index (the rest's own kRestSequenceBase + seed + K, or the moving
/// counter); [2] any cell reached this frame; [3] the cells reached this frame; [4] held, [5] active after the
/// finalize; [6] the frames that resumed (cumulative); [7] the cells traced as ring.
layout( std430, set = 0, binding = JAH_REST_CLOCK_BINDING ) JAH_REST_CLOCK_QUALIFIER buffer JahRestClock
{
	uint jahClock[8];
};

/// THE REST CLOCK as the integrate's rest law reads it (0: no rest frame).
float jahRestK() { return float( jahClock[0] ); }
/// THE FRAME'S SAMPLE INDEX (what the placement's jitter, the adaptive window, the rays and the bilinear key on).
uint jahFrameIndex() { return jahClock[1]; }

/// A world direction's bin (the full-sphere octahedron, z < 0 folded over the diamond's edges).
vec2 jahRestOctUv( vec3 d )
{
	vec3 o = d / max( abs( d.x ) + abs( d.y ) + abs( d.z ), 1e-20 );
	if( o.z < 0.0 )
	{
		const vec2 s = vec2( o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0 );
		o.xy = ( 1.0 - abs( o.yx ) ) * s;
	}
	return clamp( o.xy * 0.5 + 0.5, vec2( 0.0 ), vec2( 1.0 ) );
}
uint jahRestBin( vec3 d )
{
	const uvec2 b = min( uvec2( jahRestOctUv( d ) * float( kRestBinSide ) ), uvec2( kRestBinSide - 1u ) );
	return b.y * kRestBinSide + b.x;
}
/// ...and the direction at a point of the map's square (the inverse fold).
vec3 jahRestOctDir( vec2 uv )
{
	const vec2 f = uv * 2.0 - 1.0;
	vec3 n = vec3( f, 1.0 - abs( f.x ) - abs( f.y ) );
	if( n.z < 0.0 )
	{
		const vec2 s = vec2( n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0 );
		n.xy = ( 1.0 - abs( n.yx ) ) * s;
	}
	return normalize( n );
}

#endif   // JAH_PROBE_REST_GLSL
