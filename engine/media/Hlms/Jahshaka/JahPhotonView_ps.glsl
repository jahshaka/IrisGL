// THE PHOTON VIEW'S COMPOSITE SHADER (JahPhotonView.material has the contract).
//
// A texelFetch clamped into the overlay (no filtering, no derivatives), at the
// pixel the quad's own uv0 maps onto the target — the overlay is the target's
// size, so a texel is a pixel. Straight alpha: the blend state does the rest.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D photonOverlay;

vulkan( layout( ogre_s0 ) uniform sampler pointSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

void main()
{
	ivec2 size = textureSize( photonOverlay, 0 );
	ivec2 pix = clamp( ivec2( inPs.uv0 * vec2( size ) ), ivec2( 0 ), size - ivec2( 1 ) );
	vec4 o = texelFetch( photonOverlay, pix, 0 );
#ifdef PHOTON_COVERAGE
	// The photon scene pass' layer: covered wherever anything was drawn.
	const bool covered = any( greaterThan( o, vec4( 0.0 ) ) );
	fragColour = vec4( clamp( o.rgb, vec3( 0.0 ), vec3( 1.0 ) ), covered ? 1.0 : 0.0 );
#else
	fragColour = vec4( clamp( o.rgb, vec3( 0.0 ), vec3( 1.0 ) ), clamp( o.a, 0.0, 1.0 ) );
#endif
}
