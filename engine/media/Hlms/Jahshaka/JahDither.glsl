// THE ONE DITHER (lane DITHER-1) — included by every shader in this engine
// that turns a floating-point picture into 8-bit display codes.
//
// THE DEFECT IT ANSWERS. The renderer computes in RGBA16F and the window, the
// offscreen render target and the VR eye image are all 8-bit UNORM; 10-bit
// output is not available on this path (the X surface offers B8G8R8A8 only).
// A smooth shading gradient therefore lands on a staircase of display codes,
// and a staircase that SLIDES with the camera is what the owner reported as
// "faint circular rippling in the ground plane while flying" (the contours are
// the GI cascade's radial structure, quantised). Every renderer dithers its
// final output, at any bit depth, for exactly this reason: a zero-mean
// perturbation smaller than one code turns a hard contour into noise whose
// LOCAL MEAN follows the true gradient, which is what the eye integrates.
//
// THE DOMAIN. This engine's tonemapper emits the DISPLAY CODE itself (the
// measurement is written out on IEditorViewport::ScreenshotGrade), and the
// targets it writes are plain UNORM, so the value a shader hands to
// `fragColour` IS what gets rounded. The dither therefore belongs on the
// shader's own output value, unscaled: it is already the quantiser's axis.
//
// ================= WHY THIS FILE IS ALL INTEGER ARITHMETIC ==================
//
// BECAUSE THE PICTURE MUST BE BIT-STABLE ACROSS VENDORS, DRIVERS AND BACKENDS,
// AND FLOATING POINT IS NOT. Two things would otherwise decide it for us:
//
//   1. THE NOISE. A float `fract(52.9829189 * fract(dot(p, k)))` is evaluated
//      with whatever contraction the compiler chooses (a fused multiply-add
//      rounds once where a separate multiply and add round twice), and `fract`
//      of a large product is catastrophically sensitive to that last bit. Two
//      drivers — or one driver after an update, or NVIDIA against MoltenVK and
//      lavapipe — would disagree on tens of pixels of a 256x256 frame.
//   2. THE ROUNDING. Handing a float to a UNORM attachment lets the HARDWARE
//      pick the code: Vulkan requires a value within one ULP of the correctly
//      rounded result and lets an implementation break a tie either way, so
//      the pixel exactly on a boundary is the hardware's choice, not ours.
//
// Every pixel suite in this tree, every thumbnail and the --engine-selftest
// hash rest on the picture being the same picture, so neither may decide it.
// So: the noise is computed in 32-bit UNSIGNED INTEGER arithmetic, whose
// wraparound is exact and mandated by the language, and the shader QUANTISES
// ITSELF — `floor(v * 255 + 0.5 + n) / 255` — so the hardware receives a value
// that is already exactly on a code and has nothing left to decide.
//
// WHAT THAT DOES AND DOES NOT BUY, stated exactly rather than overclaimed.
// The OFFSET is now a pure function of the integer pixel coordinate: zero
// float sensitivity, identical on every implementation, to the bit. The
// ROUNDING is now ours: the hardware receives a value already on a code and
// breaks no ties. What remains is the ordinary sensitivity every quantiser
// has — a pixel whose pre-floor sum sits within one ULP of an integer can
// still fall either side of it if the value reaching this function differs in
// its last bit — and that value is the FILMIC CURVE's output, float arithmetic
// this file does not own and never did. So this is not a claim that two
// drivers render the same picture; it is a claim that the DITHER adds no
// disagreement of its own, where the float form added a great deal (a 1-ULP
// input difference multiplied by 52.98 inside a fract is a completely
// different offset, not a nearby one).
//
// THE NOISE IS STILL INTERLEAVED GRADIENT NOISE (Jimenez, "Next Generation
// Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014) — the
// same function, evaluated exactly. Its two dot constants and its multiplier
// are held as Q32 fixed point (a uint is the fraction x/2^32), where a uint
// multiply-and-wrap IS `fract` and `mulHi` below is the fractional multiply.
// It is chosen over white noise for its spectrum: no visible tile, and no
// texture to create, upload and keep resident (a manual texture in this engine
// is a rule of its own — see the SSAO noise fact in CLAUDE.md).
//
// THE KEY IS THE INTEGER PIXEL COORDINATE AND NOTHING ELSE — no frame counter,
// no time. A still frame is therefore the same picture on every run.
//
// THE AMPLITUDE is 0.498 of a code, peak — a rectangular distribution just
// inside half a code. Just inside, and not exactly half, so that a value
// ALREADY SITTING EXACTLY ON A CODE rounds back to that code: for an integer
// k, floor(k + 0.5 + n) == k for every |n| <= 0.498, EXACTLY, with room to
// spare for the float error in k/255*255. An already quantised picture
// therefore survives a dithered write unchanged, bit for bit, which is what
// lets a dithered write sit in front of a picture somebody else already
// quantised without moving it.
//
// ONE NOISE VALUE FOR ALL THREE CHANNELS, deliberately. Each channel is still
// dithered correctly — the offset is uniform on (-0.498, 0.498) and
// independent of any channel's fractional part, so each channel's expected
// rounded value is its true value — while the residual reads as LUMINANCE
// noise instead of colour speckle, and a grey picture stays exactly grey
// (R == G == B), which the desaturate look's structural assertion needs.
//
// `scale` is 1 normally and 0 under the no-dither override, which is the
// single-process A/B the guard suite and the selftest-hash A/B are built on.

// THE CODE DEPTH. 8-bit, because every target this engine's grade writes is
// (the window, the offscreen render target, the VR eye image, the LDR buffer
// SMAA works on, the looks stage's buffers, the picture-in-picture inset). It
// is a named constant and not a literal because a 10-bit target reached
// through this material would need ITS OWN depth here — 1023.0 — and the
// amplitude, which is half a code, would shrink with it. Nothing selects it
// today; a future output format is the thing that has to.
#define JAH_DITHER_CODE_MAX 255.0

// floor(a * b / 2^32) for two uint32s: the high half of the 64-bit product,
// built from 16-bit limbs so it needs nothing above GLSL 330 (no umulExtended,
// no 64-bit ints) and is exact on every implementation — uint arithmetic wraps
// modulo 2^32 by mandate.
uint jahDitherMulHi( uint a, uint b )
{
	uint a0 = a & 0xFFFFu, a1 = a >> 16;
	uint b0 = b & 0xFFFFu, b1 = b >> 16;
	uint p00 = a0 * b0;
	uint p01 = a0 * b1;
	uint p10 = a1 * b0;
	uint p11 = a1 * b1;
	uint mid = ( p00 >> 16 ) + ( p01 & 0xFFFFu ) + ( p10 & 0xFFFFu );
	return p11 + ( p01 >> 16 ) + ( p10 >> 16 ) + ( mid >> 16 );
}

// Interleaved gradient noise in Q32: the fraction n/2^32 in [0, 1).
//   float form : fract( 52.9829189 * fract( 0.06711056*x + 0.00583715*y ) )
//   here       : the same, with every fract an exact uint wraparound.
// The constants are round(c * 2^32) for the two dot terms and, for the
// multiplier, its integer part 52 (an exact uint multiply) plus its fraction
// 0.9829189 as Q32 (a fractional multiply, jahDitherMulHi above).
uint jahDitherIgnQ32( ivec2 p )
{
	uint t = uint( p.x ) * 288237660u + uint( p.y ) * 25070368u;   // Q32, wraps = fract
	return 52u * t + jahDitherMulHi( 4221604530u, t );             // Q32, wraps = fract
}

/// The dither offset in CODES: a rectangular distribution on
/// [-0.498, +0.498), exact, keyed on the integer pixel coordinate alone.
float jahDitherOffset( ivec2 pixelCoord, float scale )
{
	// Q32 -> [0, 1) as an EXACT float: a uint32 divided by 2^32 is representable
	// to the last bit that matters here, and the division is by a power of two.
	const float ign = float( jahDitherIgnQ32( pixelCoord ) ) * (1.0 / 4294967296.0);
	return ( ign - 0.5 ) * 0.996 * scale;
}

/// Quantises a DISPLAY-CODE colour to 8 bits with the dither, and hands back a
/// value that is already exactly on a code — so the hardware's own float->UNORM
/// rounding has nothing left to decide.
vec3 jahDither8( vec3 displayCode, ivec2 pixelCoord, float scale )
{
	const float n = jahDitherOffset( pixelCoord, scale );
	const vec3 codes = floor( displayCode * JAH_DITHER_CODE_MAX + 0.5 + n );
	return clamp( codes, 0.0, JAH_DITHER_CODE_MAX ) * ( 1.0 / JAH_DITHER_CODE_MAX );
}
