// THE REFLECTION'S STATE TEXEL (PHOTON-7 stage 6, law L8: explicit state) - one rg32ui per trace texel and eye,
// ping-ponged like the history (a texel's state is read at its REPROJECTED coordinate - any texel - while the others
// write theirs, the history's race), replacing the rg32f distance pair whose mantissas carried the rest's stop count
// and the mean's mover age, the five flags summed into the sample's fp16 w and the count in the history's w (AB E13,
// S6). Every field is an integer field of an integer word: nothing is read back through a float's rounding.
//
//   .x  [ 0:15] surfDist  fp16 - the surface's view distance the texel's answer is for; 0 = NO ANSWER this frame
//                                 (the filter's "written this frame" test, the reprojection's 5 % test)
//       [16:31] hitT      fp16 - the mean ray length (the distance the rays in the mean travelled; the virtual point)
//   .y  [ 0: 7] N         6.2 fixed - the moving history's sample count (0 .. 63.75, step 0.25)
//       [ 8:17] v         the rest samples the texel's rest mean holds (0 .. 1023)
//       [18:21] stop      consecutive rest frames the rest mean's standard error stayed under a code (0 .. 15)
//       [22:26] moverAge  frames since a ray of the texel last hit a mover, saturating at 31, stored INVERTED so a
//                         cleared texel reads "never"
//       [27]    resume    this frame the texel RESUMES at rest (a moved instance crossed its ray, or its estimator
//                         switched)
//       [28]    fixed     this frame's estimator is the lobe's centre (the lobe under one texel at its hit)
//       [29]    surfaceOnly  only the SURFACE history may be used this frame (a moving reflector, a moving hit, a
//                         history found through the moving hit's reflected image)
//       [30]    ownMoving the reflector itself moved this frame
//       [31]    noHistory the texel carried no history into this frame (its blend is 1)
// The per-frame bits (27-31) are the trace's message to the passes after it in the same frame (the hit write-back, the
// temporal pass, the filter); the trace's next frame reads them back as the texel's LAST estimator.
#ifndef JAH_REFLECT_STATE_GLSL
#define JAH_REFLECT_STATE_GLSL

struct JahReflectState
{
	float surfDist;
	float hitT;
	float N;
	uint  v;
	uint  stop;
	uint  moverAge;
	bool  resume;
	bool  fixedEstimator;
	bool  surfaceOnly;
	bool  ownMoving;
	bool  noHistory;
};

const float kJahStateMaxN = 63.75;
const uint  kJahStateMaxV = 1023u;
const uint  kJahStateMaxStop = 15u;
const uint  kJahStateMaxAge = 31u;
/// The largest distance the fp16 fields hold below their infinity.
const float kJahStateMaxDist = 65000.0;

JahReflectState jahStateNone()
{
	JahReflectState s;
	s.surfDist = 0.0;
	s.hitT = 0.0;
	s.N = 0.0;
	s.v = 0u;
	s.stop = 0u;
	s.moverAge = kJahStateMaxAge;
	s.resume = false;
	s.fixedEstimator = false;
	s.surfaceOnly = false;
	s.ownMoving = false;
	s.noHistory = false;
	return s;
}

JahReflectState jahStateUnpack( uvec2 w )
{
	JahReflectState s;
	const vec2 d = unpackHalf2x16( w.x );
	s.surfDist = d.x;
	s.hitT = d.y;
	s.N = float( w.y & 0xFFu ) * 0.25;
	s.v = ( w.y >> 8u ) & 0x3FFu;
	s.stop = ( w.y >> 18u ) & 0xFu;
	s.moverAge = kJahStateMaxAge - ( ( w.y >> 22u ) & 0x1Fu );
	s.resume = ( w.y & ( 1u << 27u ) ) != 0u;
	s.fixedEstimator = ( w.y & ( 1u << 28u ) ) != 0u;
	s.surfaceOnly = ( w.y & ( 1u << 29u ) ) != 0u;
	s.ownMoving = ( w.y & ( 1u << 30u ) ) != 0u;
	s.noHistory = ( w.y & ( 1u << 31u ) ) != 0u;
	return s;
}

uvec2 jahStatePack( JahReflectState s )
{
	uvec2 w;
	w.x = packHalf2x16( vec2( clamp( s.surfDist, 0.0, kJahStateMaxDist ), clamp( s.hitT, 0.0, kJahStateMaxDist ) ) );
	const uint n = uint( clamp( s.N, 0.0, kJahStateMaxN ) * 4.0 + 0.5 );
	w.y = n | ( min( s.v, kJahStateMaxV ) << 8u ) | ( min( s.stop, kJahStateMaxStop ) << 18u ) |
		  ( ( kJahStateMaxAge - min( s.moverAge, kJahStateMaxAge ) ) << 22u ) | ( s.resume ? 1u << 27u : 0u ) |
		  ( s.fixedEstimator ? 1u << 28u : 0u ) | ( s.surfaceOnly ? 1u << 29u : 0u ) |
		  ( s.ownMoving ? 1u << 30u : 0u ) | ( s.noHistory ? 1u << 31u : 0u );
	return w;
}

/// THE TRACE TEXEL'S PIXEL (PHOTON-7 stage 6, the ray origin): the full-resolution pixel whose surface a trace texel
/// reflects - its block's centre pixel, (scale / 2) in (the block itself at full resolution), clamped into an odd-sized
/// target - read with texelFetch by every pass, so the trace, the temporal pass and the filter reconstruct the SAME
/// surface point (a point-sampled read at the block's centre, a corner between four pixels at half resolution, picked
/// one of them by rounding while the reconstruction used the corner: up to half a pixel of slope off the surface).
ivec2 jahTexelPixel( ivec2 tc, ivec2 scale, ivec2 fullRes )
{
	return min( tc * scale + scale / 2, fullRes - 1 );
}
vec2 jahTexelUv( ivec2 tc, ivec2 scale, ivec2 fullRes )
{
	return ( vec2( jahTexelPixel( tc, scale, fullRes ) ) + 0.5 ) / vec2( fullRes );
}

#endif
