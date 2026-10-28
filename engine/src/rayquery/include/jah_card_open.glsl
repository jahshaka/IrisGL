// THE CARD TEXEL'S OPEN FRACTION (PHOTON-7 11-2 (5), the lead's ruling): a card texel is a radiance cache sampled by
// AREA, so a texel whose coverage varies across it - a contact edge, half of it under a block resting on its surface -
// takes its AREA MEAN. The still mode of the card trace (rq_card_movers.comp) tests four sub-points of each captured
// texel (the quarter-centres on the texel's plane) by the one origin law's buried test (jahRayOriginBuried: the lift
// crossing a front face - the point covered by a surface in contact) and writes, once per capture, the layer's texel
// ( 1 - f, the open sub-points' centroid offset in texels u + 0.5, v + 0.5, 1 = written ). Every positional reader (the
// sun and lamp traces, the cards' gather, the relight) takes the texel at that centroid, and the relight scales its
// outgoing light by f: a wholly open texel (f 1, no offset) bit for bit as before, a covered one 0, a straddling one
// f x its open part's light. An unwritten texel (a 0) reads open. No Hlms directive mark here.
#ifndef JAH_CARD_OPEN_GLSL
#define JAH_CARD_OPEN_GLSL
/// ( f, the centroid's offset in texels ) from the layer's texel
vec3 jahCardOpenRead( vec4 o )
{
	if( !( o.a > 0.5 ) )
		return vec3( 1.0, 0.0, 0.0 );
	return vec3( 1.0 - o.r, o.g - 0.5, o.b - 0.5 );
}
vec4 jahCardOpenPack( float f, vec2 offset )
{
	return vec4( 1.0 - clamp( f, 0.0, 1.0 ), clamp( offset + 0.5, 0.0, 1.0 ), 1.0 );
}
/// THE SUB-POINTS: the texel's four quarter-centres, in texels from its centre
vec2 jahCardOpenSub( int k )
{
	return vec2( ( k & 1 ) != 0 ? 0.25 : -0.25, ( k & 2 ) != 0 ? 0.25 : -0.25 );
}
#endif   // JAH_CARD_OPEN_GLSL
