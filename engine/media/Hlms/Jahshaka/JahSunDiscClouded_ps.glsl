// THE SUN DISC SEEN THROUGH THE CLOUD LAYER (CLOUDS-2D-1). The disc is drawn
// after the layer (render queue 5 against the layer's 0, the disc must stay out
// of the environment capture), so a layer cannot cover it by drawing over it.
// Instead, while a layer exists, the disc's quad wears THIS material: the
// disc of JahSunDisc_ps.glsl, times the fraction of its light that crosses the
// sheet unscattered along the same ray -- the same geometry and the same field
// the layer draws with (JahCloudLayer.glsl), so a cloud that hides the sky in
// front of the sun hides the sun by exactly as much. With no layer the disc
// wears its own material and nothing here runs.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D cloudField;
vulkan( layout( ogre_s0 ) uniform sampler cloudSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// xyz = unit vector from the scene TOWARDS the sun; w = cos(angular radius).
	uniform vec4 sunDirection;
	// rgb = the disc's radiance, already scaled by the host. a is unused.
	uniform vec4 sunColour;
	uniform vec4 cameraPos;
	// x = altitude (m), y = 1 / tile (1/m), zw = scroll (m)
	uniform vec4 cloudLayer;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec3 cameraDir;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

#define JAH_CLOUD_TAU( uv ) texture( vkSampler2D( cloudField, cloudSampler ), uv ).x
#include "JahCloudLayer.glsl"

void main()
{
	const vec3 dir = normalize( inPs.cameraDir );
	const float cosAngle = dot( dir, sunDirection.xyz );
	const float cosOuter = sunDirection.w;
	const float cosInner = cos( acos( clamp( cosOuter, -1.0, 1.0 ) ) * 0.88 );
	const float disc = smoothstep( cosOuter, cosInner, cosAngle );
	const JahCloudHit hit = jahCloudIntersect( dir, cameraPos.xyz, cloudLayer );
	const float t = jahCloudViewTransmittance( JAH_CLOUD_TAU( hit.uv ), hit );
	fragColour = vec4( sunColour.rgb * ( disc * t ), 1.0 );
}
