// THE GATHER'S REST PER SCREEN CELL (PHOTON-7 stage 8B, B1; SPECS/PHOTON_7_SPEC.md 8B, the lead's rulings §3500, §3502).
//
// MOVING is the VIEW's (the camera, a restart of the estimator): every cell settles. With the camera still each cell is
// ACTIVE or HELD:
//   ACTIVE  - its probes are traced and its pixels integrated, on its COMPONENT's rest clock K (0 = settling, 1 ... N the
//             rest mean, the shipped rest law of rq_probe_integrate.comp). THE CLOCK IS A CONNECTED REGION's (the lead's
//             ruling (b)): the cells are grouped in BLOCKS of B x B, B = 2 (the ring's reach + 1) + 1, and the active
//             blocks that touch form one component with one K - so every probe an active pixel's taps, its filter and
//             its placement's neighbourhood read is traced at that pixel's own rest index (the placement's jitter, the
//             adaptive window and the bilinear are one offset a frame over a component), and two components never share
//             a probe (a whole block lies between them). A cell an input reaches (rq_probe_resume.comp) RESUMES and puts
//             ITS component's clock back to 0; a component stays at 0 while a cell of it is still converging (its own
//             picture moved a code over the last N frames); other components keep their rests.
//   HELD    - its picture is its rest mean at K = N, bit for bit: never traced for itself, never integrated (its pixels
//             write their history through the ping-pong). A held cell within the ring's reach of an active one is
//             traced at that one's index for it, and still never integrated.
// The clocks live on the GPU (the resume decides them): every gather job reads its cell's K and sample index here.
#ifndef JAH_PROBE_REST_GLSL
#define JAH_PROBE_REST_GLSL

const uint kCellHeld = 1u;      ///< the cell holds its rest picture
const uint kCellRing = 2u;      ///< ...and is traced this frame for an active neighbour (never integrated)
const uint kCellReached = 4u;   ///< an input reached it this frame (the instrument's bit; the resume's own)
const uint kCellField = 8u;     ///< a pixel of it read the field's fallback since the camera stopped (the integrate's)
const uint kCellConverged = 16u;  ///< settling, its history caught up with its samples (the resume's)
/// THE CELL's WORDS (the cells' state buffer, kCellWords a cell): [0] the state bits, [1] the ring bit; the integrate's
/// sums this frame over its pixels, in display codes x 16: [2] of this frame's SAMPLES, [3] of the HISTORY's answer, [4]
/// the pixels; the resume's (float bits): [5] the samples' cell mean followed over N frames (exponential), [6] its
/// variance, [7] the frames since the cell's last reach, [8] unused.
const uint kCellWords = 10u;

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
/// THE CLOCKS: [0] the least K over the active components (0xFFFFFFFF: none active); [1] unused; [2] any cell reached
/// this frame; [3] the cells reached; [4] held, [5] active after the finalize; [6] the frames any cell resumed on
/// (cumulative); [7] the cells traced as ring. Then per cell (8 + 2 c) its sample index and (9 + 2 c) its K; then per
/// block (8 + 2 cells + b) its K and (8 + 2 cells + blocks + b) the frame's flags (the test's: 1 active, 2 reached,
/// 4 an active cell not converged).
layout( std430, set = 0, binding = JAH_REST_CLOCK_BINDING ) JAH_REST_CLOCK_QUALIFIER buffer JahRestClock
{
	uint jahClock[];
};

/// A CELL's REST CLOCK as the integrate's rest law reads it (0: settling) and its frame's sample index (what the
/// placement's jitter, the adaptive window, the rays and the bilinear key on).
float jahRestKOf( uint cell ) { return float( jahClock[9u + 2u * cell] ); }
/// ...and whether its demand stamps the cards (PHOTON-7 stage 10's per-reader snapshot, per cell since 8B): settling or
/// moving (K 0) it stamps, resting only at its rest indices [1, span] (span 0: always) - the stop's decision reads the
/// readers' latest rest span (SurfaceCache::refreshResidency's window, from the least active clock's start)
bool jahCardStampsOf( uint cell, float span )
{
	const uint k = jahClock[9u + 2u * cell];
	return span < 0.5 || k == 0u || float( k ) <= span;
}
uint jahFrameIndexOf( uint cell ) { return jahClock[8u + 2u * cell]; }

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
