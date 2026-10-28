// THE TRANSMITTANCE LAYER's PER-MAP CLEAR (PHOTON-7 10-4 R8; OgreShadow.cpp): 1 - nothing between a map's light and its
// casters - over exactly one shadow map's rectangle of atlasT (the pass's viewport and scissor are that map's rect, set
// by the node's builder), for the per-map clear strategy JahShadowClear_ps.glsl's note explains: a render-pass clear
// would wipe every cached map's layer with it.
#version ogre_glsl_ver_330

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out float fragColour;

void main()
{
	fragColour = 1.0;
}
