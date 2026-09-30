// THE PLANET'S ATMOSPHERE'S SKY PASS (SKY-ATMOSPHERE-1). The sky's radiance for
// the camera ray, read from the sky-view table (a sun of unit illuminance) and
// multiplied by the sun at the top of the air, in the renderer's units. The
// read is held on its own side of the horizon (jahAtmoDirUv): a ray that
// reaches space never blends with a texel of the planet, so the horizon is a
// line. The planet under it — its albedo lit by the sun, through the air in
// front of it — is in the table. No sun disc: that is SunDisc's quad.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D skyViewLut;
vulkan( layout( ogre_s0 ) uniform sampler skySampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// bottom radius, top radius, the observer's radius, the aerial far (km)
	uniform vec4 atmoPlanet;
	// xyz = unit vector towards the sun
	uniform vec4 atmoSunDir;
	// rgb = the sun's illuminance at the top of the air (renderer units)
	uniform vec4 atmoSkyE;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec3 cameraDir;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

#include "JahAtmosphere.glsl"

// NaN AND INF ON THE BITS (DOCS/traps/ENGINE.md: x == x is folded to true here).
bool jahFinite( float x ) { return ( floatBitsToUint( x ) & 0x7FFFFFFFu ) < 0x7F800000u; }

void main()
{
	const vec3 dir = normalize( inPs.cameraDir );
	const vec2 uv = jahAtmoDirUv( dir, atmoSunDir.xyz, atmoPlanet, vec2( 192.0, 108.0 ) );
	vec3 L = textureLod( vkSampler2D( skyViewLut, skySampler ), uv, 0.0 ).rgb * atmoSkyE.rgb;
	if( !( jahFinite( L.x ) && jahFinite( L.y ) && jahFinite( L.z ) ) )
		L = vec3( 0.0 );
	fragColour = vec4( L, 1.0 );
}
