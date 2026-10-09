#version ogre_glsl_ver_330

#include "JahScreen.glsl"

// THE BLOOM LADDER'S LAST STEP (BLOOM-1): the first level plus the tent
// upsample of the accumulated second (the same 3x3 tent as
// JahBloomUp_ps.glsl), divided by the number of levels - every level carries
// the highlight's whole energy at its own width, so the mean is the bloom of
// unit gain, wide tails and a tight core in one term - and written in the
// encoding the tonemap quad reads: it adds fromSRGB( bloom ) * 16 * amount,
// so this writes sqrt( bloom / 16 ).
//
// JAH_BLOOM_LEVELS must equal OgreChain.cpp's kBloomLevels.
#define JAH_BLOOM_LEVELS 6.0

vulkan_layout( ogre_t0 ) uniform JahScreenTexture firstLevel;
vulkan_layout( ogre_t1 ) uniform JahScreenTexture secondLevel;
vulkan( layout( ogre_s0 ) uniform sampler bilinearSampler0 );
vulkan( layout( ogre_s1 ) uniform sampler bilinearSampler1 );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

vec3 tap( vec2 uv )
{
	return texture( jahScreenSampler( secondLevel, bilinearSampler1 ), jahScreenUv( uv ) ).xyz;
}

void main()
{
	const vec2 t = vec2( 1.0 ) / vec2( jahScreenSize( jahScreenSampler( secondLevel, bilinearSampler1 ), 0 ) );
	const vec2 uv = inPs.uv0;
	vec3 s = tap( uv ) * 4.0;
	s += ( tap( uv + vec2( -t.x, 0.0 ) ) + tap( uv + vec2( t.x, 0.0 ) ) +
		   tap( uv + vec2( 0.0, -t.y ) ) + tap( uv + vec2( 0.0, t.y ) ) ) * 2.0;
	s += tap( uv + vec2( -t.x, -t.y ) ) + tap( uv + vec2( t.x, -t.y ) ) +
		 tap( uv + vec2( -t.x,  t.y ) ) + tap( uv + vec2( t.x,  t.y ) );
	const vec3 first = texture( jahScreenSampler( firstLevel, bilinearSampler0 ), jahScreenUv( uv ) ).xyz;
	const vec3 bloom = ( first + s * ( 1.0 / 16.0 ) ) * ( 1.0 / JAH_BLOOM_LEVELS );
	fragColour = vec4( sqrt( max( bloom, vec3( 0.0 ) ) * 0.0625 ), 1.0 );
}
