// THE ID PASS'S DEPTH MERGED INTO THE SCENE'S (ATOM-4 stage 1; OgreChain.cpp addAtomOcclusionPasses).
//
// The late id pass draws into the Atom view's depth (the id pass's own, Atom-only), the stock depth pass into the
// scene's; this quad writes the first into the second UNDER THE DEPTH TEST (Jahshaka/AtomDepthMerge: depth_func
// less_equal, which the render system turns round under reverse depth), so the scene's depth ends as the nearest of
// the two at every pixel: exactly what one depth would hold had both drawn into it. A copy of a float into a D32 is
// exact; no colour is written (the target is depth only).
//
// The pixel comes from uv0 - the quad's own mapping onto the target - never gl_FragCoord (JahAtomView_ps.glsl's
// rule); the texture is the target's size, so every pixel reads its own texel.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D atomIdDepth;

vulkan( layout( ogre_s0 ) uniform sampler pointSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

void main()
{
	ivec2 size = textureSize( atomIdDepth, 0 );
	ivec2 xy = clamp( ivec2( inPs.uv0 * vec2( size ) ), ivec2( 0 ), size - ivec2( 1 ) );
	gl_FragDepth = texelFetch( atomIdDepth, xy, 0 ).x;
}
