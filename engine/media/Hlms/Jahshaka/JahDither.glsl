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
// `fragColour` IS what the hardware rounds. The dither therefore belongs on
// the shader's own output value, unscaled: it is already the quantiser's axis.
// Added on any other axis it would be the wrong size — the same offset is 0.9
// of a code in the darks and 2.3 in the brights once an sRGB encode sits
// between the shader and the storage.
//
// THE AMPLITUDE is 0.498 of a code, peak — a rectangular distribution just
// inside half a code. Just inside, and not exactly half, so that a value
// ALREADY SITTING EXACTLY ON A CODE rounds back to that code: an already
// quantised picture survives a dithered write unchanged, bit for bit. That
// invariant is what lets a dithered write sit in front of a picture somebody
// else already quantised (the looks stage, the SMAA blend) without moving it,
// and it is what keeps "a look at amount 0 is byte-identical to no look" an
// assertion rather than a tolerance.
//
// THE KEY IS THE PIXEL COORDINATE AND NOTHING ELSE — no frame counter, no
// time. A still frame is therefore the same picture on every run, which is the
// contract every pixel suite, every thumbnail and the --engine-selftest hash
// depend on. Interleaved gradient noise (Jimenez, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014) rather than an
// ordered Bayer matrix: it has no visible tile, and no texture to create,
// upload and keep resident (a manual texture in this engine is a rule of its
// own — see the SSAO noise fact in CLAUDE.md).
//
// ONE NOISE VALUE FOR ALL THREE CHANNELS, deliberately. Each channel is still
// dithered correctly — the offset is uniform on (-0.5, 0.5) and independent of
// any channel's fractional part, so each channel's expected rounded value is
// its true value — while the residual reads as LUMINANCE noise instead of
// colour speckle, and a grey picture stays exactly grey (R == G == B), which
// the desaturate look's structural assertion needs.
//
// `scale` is 1 normally and 0 under JAHSHAKA_NO_DITHER, which is the
// single-process A/B the guard suite and the selftest-hash A/B are built on.

float jahDitherOffset( vec2 pixelCoord, float scale )
{
	float ign = fract( 52.9829189 *
					   fract( dot( pixelCoord, vec2( 0.06711056, 0.00583715 ) ) ) );
	// (-0.498, 0.498) codes, expressed in the 0..1 range the target stores.
	return ( ign - 0.5 ) * (0.996 / 255.0) * scale;
}

vec3 jahDither8( vec3 displayCode, vec2 pixelCoord, float scale )
{
	return displayCode + jahDitherOffset( pixelCoord, scale );
}
