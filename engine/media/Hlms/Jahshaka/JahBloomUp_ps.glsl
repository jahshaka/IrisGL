#version ogre_glsl_ver_330

#include "JahScreen.glsl"

// THE BLOOM LADDER'S UPSAMPLE (BLOOM-1): the level below, read through a 3x3
// TENT (1 2 1 / 2 4 2 / 1 2 1, over 16) one of its own texels wide, and ADDED
// to the level this pass draws into (Jahshaka/Bloom/Up blends one + one), so
// the ladder accumulates from its smallest level back to its first - the
// upsample of Jimenez 2014. Each level keeps the energy it was given; the
// resolve divides the sum by the number of levels.

vulkan_layout( ogre_t0 ) uniform JahScreenTexture sourceTexture;
vulkan( layout( ogre_s0 ) uniform sampler bilinearSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

vec3 tap( vec2 uv )
{
	return texture( jahScreenSampler( sourceTexture, bilinearSampler ), jahScreenUv( uv ) ).xyz;
}

void main()
{
	const vec2 t = vec2( 1.0 ) / vec2( jahScreenSize( jahScreenSampler( sourceTexture, bilinearSampler ), 0 ) );
	const vec2 uv = inPs.uv0;
	vec3 s = tap( uv ) * 4.0;
	s += ( tap( uv + vec2( -t.x, 0.0 ) ) + tap( uv + vec2( t.x, 0.0 ) ) +
		   tap( uv + vec2( 0.0, -t.y ) ) + tap( uv + vec2( 0.0, t.y ) ) ) * 2.0;
	s += tap( uv + vec2( -t.x, -t.y ) ) + tap( uv + vec2( t.x, -t.y ) ) +
		 tap( uv + vec2( -t.x,  t.y ) ) + tap( uv + vec2( t.x,  t.y ) );
	fragColour = vec4( s * ( 1.0 / 16.0 ), 1.0 );
}
