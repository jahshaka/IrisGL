// THE ONE sRGB TRANSFER PAIR (SRGB-ENCODE-1) -- included by every shader in
// this engine that turns a LINEAR picture into DISPLAY codes, or has to look
// at a display value as light again.
//
// WHY IT EXISTS. Every target this engine presents is 8-bit UNORM (the window,
// the offscreen render target, the VR eye image, the LDR buffers SMAA and the
// looks work on), and a UNORM attachment stores the number it is handed. The
// film curve's output and the ungraded scene are LINEAR radiance, so without
// this the display showed linear values as codes: an 18 percent card at 0.18
// reached the screen as code 46 where every sRGB display (and every reference
// renderer) shows 118 -- about 2.7 stops dark at mid grey.
//
// THE CURVE is the exact piecewise sRGB OETF (IEC 61966-2-1), not a 2.2 power:
// a linear toe of slope 12.92 below 0.0031308, then 1.055 x^(1/2.4) - 0.055.
// A power law differs by up to ~3 codes near black, exactly where the dither
// that follows the encode is working.
//
// WHERE IT IS APPLIED -- EXACTLY ONCE ON EVERY DISPLAY PATH:
//   * HDR/FinalToneMapping (the fork's tonemap quad): after the film curve and
//     its contrast tail, BEFORE the dither, so the dither is in display space;
//   * Jahshaka/DisplayEncode: the ungraded composite (HDR off, the passthrough
//     shape, the ungraded picture-in-picture inset).
// Captures (planar mirrors, probes, the sky's IBL, the surface cache) and the
// Plain grade instrument are never encoded: they are radiance, not pictures.
//
// The encode clamps to [0, 1] first: a UNORM write clamps anyway, and the
// power is then never handed a negative number (the grade tail can emit one).

vec3 jahSrgbEncode( vec3 c )
{
	c = clamp( c, vec3( 0.0 ), vec3( 1.0 ) );
	const vec3 lo = c * 12.92;
	const vec3 hi = 1.055 * pow( c, vec3( 1.0 / 2.4 ) ) - vec3( 0.055 );
	return mix( hi, lo, lessThanEqual( c, vec3( 0.0031308 ) ) );
}

// The inverse (the sRGB EOTF's piecewise form), for a display-space shader
// that must average LIGHT (Jahshaka/Look/RadialBlur).
vec3 jahSrgbDecode( vec3 c )
{
	c = clamp( c, vec3( 0.0 ), vec3( 1.0 ) );
	const vec3 lo = c * ( 1.0 / 12.92 );
	const vec3 hi = pow( ( c + vec3( 0.055 ) ) * ( 1.0 / 1.055 ), vec3( 2.4 ) );
	return mix( hi, lo, lessThanEqual( c, vec3( 0.04045 ) ) );
}
