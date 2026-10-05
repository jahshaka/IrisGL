#version ogre_glsl_ver_330

#include "JahScreen.glsl"

// THE UNGRADED COMPOSITE (SRGB-ENCODE-1): a linear scene target to a display
// target, through the one sRGB encode (JahSrgb.glsl says where else it runs).
// Alpha passes through untouched. Point sampled: source and destination are
// the same size, so this is a per-pixel transfer, never a filter.

vulkan_layout( ogre_t0 ) uniform JahScreenTexture sourceTexture;

vulkan( layout( ogre_s0 ) uniform sampler pointSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

#include "JahSrgb.glsl"

void main()
{
	const vec4 src = texture( jahScreenSampler( sourceTexture, pointSampler ), jahScreenUv( inPs.uv0 ) );
	fragColour = vec4( jahSrgbEncode( src.rgb ), src.a );
}
