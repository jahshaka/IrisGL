// Jahshaka SSR, the HISTORY'S DEPTH (REFLECT-FIX-1).
//
// The depth the colour history was drawn at, copied beside it as an R32F value so the
// resolve can ask whether the history texel it reads showed the SURFACE its march hit
// (JahSsrResolve_ps.glsl, "THE SAME SURFACE"). One thing is added to a plain copy:
// where an EDITOR HELPER shows in the picture (the outline shells, depth-tested wires
// and markers, the grid — drawn alone into `helperMask` over the scene's depth) the
// texel shows no surface, and it is written as -1, which no depth buffer holds: the
// resolve declines it like any texel that showed something else.

#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D depthTexture;
vulkan_layout( ogre_t1 ) uniform texture2D helperMask;
vulkan( layout( ogre_s0 ) uniform sampler pointSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out float fragColour;

void main()
{
	const ivec2 at = ivec2( gl_FragCoord.xy );
	const vec4 helper = texelFetch( vkSampler2D( helperMask, pointSampler ), at, 0 );
	const float depth = texelFetch( vkSampler2D( depthTexture, pointSampler ), at, 0 ).x;
	fragColour = any( greaterThan( helper, vec4( 0.0 ) ) ) ? -1.0 : depth;
}
