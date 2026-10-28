// THE CARDS' SUNVIS TEXEL (RGBA32_UINT; PHOTON-7 10-4 R9 (3), the lead's ruling: 48 lamps exact a texel; the fix round's
// F3: a lamp seen through glass keeps its fraction, never a bit). One texel carries what a card texel's direct light was
// traced through:
//   x = the SUN's visibility (bits 8-15: the still world's traced term times the movers' and the cloud sheet's), the
//       FRACTIONAL lamps' (bits 0-7: the area lights and the lamps past the card's table, each weighted by its share of
//       the texel's direct luminance) and the TRANSMITTED lamps' (bits 16-23: the table's lamps whose ray crossed panes
//       and reached them, 0 < v < 1 - each weighted by its share), 8 bits each;
//   y, z (bits 0-15) = the 48-bit LAMP MASK: bit j the table's lamp j unblocked (its ray reached it through no pane);
//   z (bits 16-31), w = the 48-bit TRANSMITTED MASK: bit j the table's lamp j reached through panes - it reads the
//       transmitted lamps' visibility (exact for one such lamp a texel; several share their share-weighted one - a named
//       limit, the one read's F3). A lamp in neither mask is blocked: 0.
// Shared by the ray jobs (jah_rq_card_bindings.glsl, rq_card_movers.comp) and the relight (JahCardLight_cs.glsl, through
// the generated piece JahCardSunVis). No Hlms directive mark anywhere in this file.

#ifndef JAH_CARD_SUNVIS_GLSL
#define JAH_CARD_SUNVIS_GLSL

/// The table's size: the card's lamps with an exact bit each.
const uint kJahCardLampTable = 48u;

/// The sun's visibility, in [0, 1].
float jahSunVisSun( uvec4 t ) { return float( ( t.x >> 8u ) & 0xFFu ) * ( 1.0 / 255.0 ); }
/// The fractional lamps' visibility, in [0, 1].
float jahSunVisRest( uvec4 t ) { return float( t.x & 0xFFu ) * ( 1.0 / 255.0 ); }
/// The transmitted lamps' visibility, in [0, 1].
float jahSunVisThrough( uvec4 t ) { return float( ( t.x >> 16u ) & 0xFFu ) * ( 1.0 / 255.0 ); }
/// The lamp mask: bits 0-31 (x) and 32-47 (y).
uvec2 jahSunVisMask( uvec4 t ) { return uvec2( t.y, t.z & 0xFFFFu ); }
/// The transmitted mask: bits 0-31 (x) and 32-47 (y).
uvec2 jahSunVisThroughMask( uvec4 t ) { return uvec2( ( t.z >> 16u ) | ( t.w << 16u ), t.w >> 16u ); }
/// Lamp j's bit of a mask (j < 48).
bool jahSunVisBit( uvec2 mask, uint j ) { return ( ( j < 32u ? mask.x >> j : mask.y >> ( j - 32u ) ) & 1u ) != 0u; }
/// THE TABLE'S LAMP j's visibility: 1 unblocked, the transmitted lamps' through panes, 0 blocked.
float jahSunVisLamp( uvec4 t, uint j )
{
	return jahSunVisBit( jahSunVisMask( t ), j ) ? 1.0 : jahSunVisBit( jahSunVisThroughMask( t ), j ) ? jahSunVisThrough( t ) : 0.0;
}
uint jahSunVisByte( float v ) { return uint( clamp( v, 0.0, 1.0 ) * 255.0 + 0.5 ); }
/// The texel: `sun`, `rest` and `through` in [0, 1] (8-bit each), `mask` and `throughMask` 48 bits each.
uvec4 jahSunVisPack( float sun, float rest, float through, uvec2 mask, uvec2 throughMask )
{
	return uvec4( jahSunVisByte( rest ) | ( jahSunVisByte( sun ) << 8u ) | ( jahSunVisByte( through ) << 16u ), mask.x,
				  ( mask.y & 0xFFFFu ) | ( throughMask.x << 16u ),
				  ( throughMask.x >> 16u ) | ( ( throughMask.y & 0xFFFFu ) << 16u ) );
}
/// An untraced texel's lamps: every lamp visible.
const uvec2 kJahSunVisAllLamps = uvec2( 0xFFFFFFFFu, 0xFFFFu );

#endif   // JAH_CARD_SUNVIS_GLSL
