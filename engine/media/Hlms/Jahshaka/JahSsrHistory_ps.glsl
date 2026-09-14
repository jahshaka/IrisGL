// Jahshaka SSR, the COLOUR HISTORY COPY (SMOKE-ENGINE-1 item 1, 2026-09-14).
//
// This is Ogre/Copy/4xFP32 with one thing added: what it writes is guaranteed
// to be a finite, bounded radiance.
//
// WHY IT EXISTS. The SSR chain is the renderer's only CLOSED LOOP — the scene's
// colour is copied into `kSsrPrev`, JahSsrResolve reads it as `prevFrame`, the
// result reaches HlmsPbs as the pass' `ssrTexture` and is lerped into
// envColourS, and that becomes the scene's colour again. Before this pass
// existed the loop's STORAGE could hold anything the scene produced: the
// history is RGBA16_FLOAT, so a radiance above 65504 is stored as +Inf, and one
// +Inf is all it takes for the resolve's firefly clamp to compute Inf * 0 and
// hand a NaN back into the loop, where it circulates for the life of the
// workspace. (The resolve guards its own reads too — two guards, because the
// loop has two ends and each one alone leaves the other's failure mode open:
// this one bounds what is STORED, the resolve's bounds what is CONSUMED, which
// also covers the frame after a workspace rebuild, when the history has been
// written by nothing at all.)
//
// It is a CONDITIONAL, so a scene whose radiance is already in range is copied
// bit for bit and no existing frame moves.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D tex;
vulkan( layout( ogre_s0 ) uniform sampler texSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

// The same ceiling JahSsrResolve_ps.glsl uses, and for the same reason: about
// twelve stops above white, six below the half-float format's own ceiling.
const float kSsrMaxRadiance = 1024.0;

void main()
{
	vec4 c = texture( vkSampler2D( tex, texSampler ), inPs.uv0 );
	// One test for NaN, Inf and a runaway finite value: a NaN fails every
	// comparison, an Inf fails the magnitude one.
	const float m = max( abs( c.x ), max( abs( c.y ), abs( c.z ) ) );
	// A finite value too bright to be useful is held at the ceiling; a
	// NON-FINITE one is stored as zero, so the resolve's neighbourhood filter
	// sees "nothing here" rather than a manufactured white. (Measured: the
	// ceiling-as-white spelling removes the same black holes and replaces them
	// with white sparkle over 20 % of the viewport — SMOKE-ENGINE-1.)
	if( m > kSsrMaxRadiance )
		c.xyz = m < 3.0e38 ? min( c.xyz, vec3( kSsrMaxRadiance ) )
						   : vec3( 0.0 );
	fragColour = c;
}
