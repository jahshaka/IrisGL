// THE CARDS' SUNVIS TEXEL (RGBA16_UNORM; PHOTON-7 10-4 R9 (3), the lead's ruling: 48 lamps exact a texel). One texel
// carries what a card texel's direct light was traced through:
//   r = the SUN's visibility in its high byte (the still world's traced term times the movers' and the cloud sheet's)
//       and the FRACTIONAL lamps' in its low byte (the area lights, and the lamps past the card's table, each weighted
//       by its share of the texel's direct luminance) - both 8-bit, as the RGBA8 texel held them;
//   g, b, a = the 48-bit LAMP MASK, 16 bits each (g bits 0-15): bit j the traced visibility of lamp j of the card's
//       48-lamp table (its records' world ids), exact per lamp.
// Unorm16 holds every integer k / 65535 exactly: a channel is written k / 65535 and read back as k.
// Shared by the ray jobs (jah_rq_card_bindings.glsl, rq_card_movers.comp) and the relight (JahCardLight_cs.glsl, through
// the generated piece JahCardSunVis). No Hlms directive mark anywhere in this file.

#ifndef JAH_CARD_SUNVIS_GLSL
#define JAH_CARD_SUNVIS_GLSL

/// The table's size: the card's lamps with an exact bit each.
const uint kJahCardLampTable = 48u;

uint jahSunVisWord( float c ) { return uint( clamp( c, 0.0, 1.0 ) * 65535.0 + 0.5 ); }
/// The sun's visibility, in [0, 1].
float jahSunVisSun( vec4 t ) { return float( jahSunVisWord( t.x ) >> 8u ) * ( 1.0 / 255.0 ); }
/// The fractional lamps' visibility, in [0, 1].
float jahSunVisRest( vec4 t ) { return float( jahSunVisWord( t.x ) & 0xFFu ) * ( 1.0 / 255.0 ); }
/// The lamp mask: bits 0-31 (x) and 32-47 (y).
uvec2 jahSunVisMask( vec4 t )
{
	return uvec2( jahSunVisWord( t.y ) | ( jahSunVisWord( t.z ) << 16u ), jahSunVisWord( t.w ) );
}
/// Lamp j's bit of a mask (j < 48).
bool jahSunVisBit( uvec2 mask, uint j ) { return ( ( j < 32u ? mask.x >> j : mask.y >> ( j - 32u ) ) & 1u ) != 0u; }
/// The texel: `sun` and `rest` in [0, 1] (8-bit each), `mask` the 48 lamp bits.
vec4 jahSunVisPack( float sun, float rest, uvec2 mask )
{
	const uint s = uint( clamp( sun, 0.0, 1.0 ) * 255.0 + 0.5 ), f = uint( clamp( rest, 0.0, 1.0 ) * 255.0 + 0.5 );
	return vec4( float( ( s << 8u ) | f ), float( mask.x & 0xFFFFu ), float( mask.x >> 16u ), float( mask.y & 0xFFFFu ) ) *
		   ( 1.0 / 65535.0 );
}
/// An untraced texel's lamps: every lamp visible.
const uvec2 kJahSunVisAllLamps = uvec2( 0xFFFFFFFFu, 0xFFFFu );

#endif   // JAH_CARD_SUNVIS_GLSL
