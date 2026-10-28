// THE NEAREST PANE's DEPTH, SEEDED (PHOTON-7 10-4 fix round, the one read's F1; OgreShadow.cpp addPanePass): one shadow
// map's rectangle of the opaque atlas copied into atlasTD before the map's panes are drawn into it depth-only - so a
// pane behind the nearest opaque caster fails the depth test there and atlasTD holds min(the opaque caster, the nearest
// pane in front of it). The same texel of the same layout (atlasTD is the atlas's size), fetched where this fragment
// stands; no colour (a depth-only target). No directive mark anywhere in this file.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D depthTexture;
vulkan( layout( ogre_s0 ) uniform sampler pointSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

in vec4 gl_FragCoord;

void main()
{
	gl_FragDepth = texelFetch( vkSampler2D( depthTexture, pointSampler ), ivec2( gl_FragCoord.xy ), 0 ).x;
}
