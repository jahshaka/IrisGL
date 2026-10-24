// THE REFLECTION'S STATE TEXEL (PHOTON-7 stage 6, law L8: explicit state) - one rg32ui per trace texel and eye,
// ping-ponged like the history (a texel's state is read at its REPROJECTED coordinate - any texel - while the others
// write theirs, the history's race), replacing the rg32f distance pair whose mantissas carried the rest's stop count
// and the mean's mover age, the five flags summed into the sample's fp16 w and the count in the history's w (AB E13,
// S6). Every field is an integer field of an integer word: nothing is read back through a float's rounding.
// THE FINAL LAYOUT (commit 6: Lumen's denoiser - its N step and ceiling, the rest's sequence index; the mover age and
// the estimator flags deleted with the moving hit's history and the six patch rules):
//
//   .x  [ 0:15] surfDist  fp16 - the surface's view distance the texel's answer is for; 0 = NO ANSWER this frame
//                                 (the trace declined the texel, or the temporal pass found neither a sample nor a history)
//       [16:31] hitT      fp16 - the THROW: the temporal mean of the resolved hit distance (the lobe's footprint at its
//                                 hit, so the centre ray's chance; the stopped texel's resume segment). Between the trace
//                                 and the temporal pass of one frame it holds the throw THIS frame's draw used (the resolve
//                                 re-derives a neighbour's chance from it).
//   .y  [ 0: 7] N         5.3 fixed - the frames in the moving mean (Lumen's NumFramesAccumulated: step 0.125, ceiling
//                         31.875 - above the moving MaxFrames 12, so the ceiling binds only a composed rest answer
//                         (W3): its first moving frame takes min( N c + 1, 12 ) = 12 for any confidence c >= 0.345)
//       [ 8:23] v         the rest samples the texel's rest mean holds - the rest SEQUENCE index (0 .. 65535)
//       [24:27] stop      consecutive rest frames the rest mean's standard error stayed under a code (0 .. 15; the texel
//                         has STOPPED at kReflectStopFrames)
//       [28]    resume    this frame the texel RESUMES at rest (a moved instance crossed its ray)
//       [29]    folded    this frame's VNDF draw was FOLDED above the drawn surface's geometric plane (READ_A W4): its
//                         density is p( L ) + p( R L ), not the VNDF's - the resolve weighs it so (READ_B Q2)
//       [30]    centre    this frame's draw was the lobe's CENTRE RAY (the mirror ramp's, jah_reflect_draw.glsl)
//       [31]    delta     this frame's direction is the MIRROR FALLBACK (a lobe sample that left the shading plane takes
//                         the mirror direction): a delta, never reweighted as a density - out of a neighbour's resolve
// All four bits are the trace's message to the passes after it in the same frame.
#ifndef JAH_REFLECT_STATE_GLSL
#define JAH_REFLECT_STATE_GLSL

struct JahReflectState
{
	float surfDist;
	float hitT;
	float N;
	uint  v;
	uint  stop;
	bool  resume;
	bool  folded;
	bool  centre;
	bool  delta;
};

const float kJahStateMaxN = 31.875;
const uint  kJahStateMaxV = 65535u;
const uint  kJahStateMaxStop = 15u;
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
	s.resume = false;
	s.folded = false;
	s.centre = false;
	s.delta = false;
	return s;
}

JahReflectState jahStateUnpack( uvec2 w )
{
	JahReflectState s;
	const vec2 d = unpackHalf2x16( w.x );
	s.surfDist = d.x;
	s.hitT = d.y;
	s.N = float( w.y & 0xFFu ) * 0.125;
	s.v = ( w.y >> 8u ) & 0xFFFFu;
	s.stop = ( w.y >> 24u ) & 0xFu;
	s.resume = ( w.y & ( 1u << 28u ) ) != 0u;
	s.folded = ( w.y & ( 1u << 29u ) ) != 0u;
	s.centre = ( w.y & ( 1u << 30u ) ) != 0u;
	s.delta = ( w.y & ( 1u << 31u ) ) != 0u;
	return s;
}

uvec2 jahStatePack( JahReflectState s )
{
	uvec2 w;
	w.x = packHalf2x16( vec2( clamp( s.surfDist, 0.0, kJahStateMaxDist ), clamp( s.hitT, 0.0, kJahStateMaxDist ) ) );
	const uint n = uint( clamp( s.N, 0.0, kJahStateMaxN ) * 8.0 + 0.5 );
	w.y = n | ( min( s.v, kJahStateMaxV ) << 8u ) | ( min( s.stop, kJahStateMaxStop ) << 24u ) |
		  ( s.resume ? 1u << 28u : 0u ) | ( s.folded ? 1u << 29u : 0u ) | ( s.centre ? 1u << 30u : 0u ) |
		  ( s.delta ? 1u << 31u : 0u );
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
