// Jahshaka/CardLight — THE LIT CARD (PHOTON-CARDS-1, SC-1c): one texel of the
// surface cache's atlas per thread, its RADIANCE written into the sixth layer.
//
// WHAT A TEXEL IS. The capture (JahCardCapture_piece_ps.any) stored, per card
// texel, kD (the datablock's diffuse ALREADY divided by pi), the shading
// normal in the capture camera's view space, the distance from the capture
// camera's plane, the emissive radiance, and (shadow term, GGX alpha). The
// camera was orthographic, so the texel's WORLD POSITION is arithmetic: the
// camera position, plus the texel's place in the ortho window along u and v,
// minus the stored distance along the card's outward axis d. One workgroup
// z-slice per card (the host's relight list), 8 x 8 threads, a 128 page is
// 16 x 16 groups; a smaller card's surplus threads return.
//
// DIRECT = the sum over the scene's lights of HlmsPbs's own BRDF_Default
// diffuse lobe, TRANSCRIBED from 200.BRDFs_piece_ps.any (BRDF_Default, the
// normalised Disney diffuse: energyBias, energyFactor, fd90, lightScatter,
// viewScatter) and the light loops of 800.PixelShader_piece_ps.any
// (DoDirectionalLights / DoPointLights / DoSpotLights: the attenuation
// 1 / (0.5 + (linear + quadratic d) d), the range fade of patch 0018, the spot
// cone) — the same arithmetic `pbsDirect()` in test_gi_field_energy.cpp writes
// out in C++, so a test can hold this to that closed form.
//
// THE VIEW DIRECTION IS THE NORMAL. A cached texel is read from any direction,
// so it stores the view-INDEPENDENT diffuse: V = N makes the lobe's one
// view-dependent factor (viewScatter, which multiplies (1 - NdotV)^5) exactly
// 1. The specular lobe is not cached (Lumen's rule: the surface cache is a
// diffuse store).
//
// VISIBILITY. The SUN's is the stored shadow term (the capture's PSSM term of
// the first shadow-casting directional light — the prepass writes that one
// term and nothing else). A point or spot light is UNSHADOWED here: its
// visibility is the traced residue's, where rays exist (PHOTON P5). Area
// lights are not summed (their LTC path is not transcribed). Stated, all three.
//
// INDIRECT: not yet (the stop point of PHOTON-CARDS-1 before the one
// environment lands) — the radiance is DIRECT + EMISSIVE.
//
// No at-sign in any comment of this file (the Hlms parser reads them).
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

vulkan_layout( ogre_t0 ) uniform texture2D cardAlbedo;
vulkan_layout( ogre_t1 ) uniform texture2D cardNormal;
vulkan_layout( ogre_t2 ) uniform texture2D cardDepth;
vulkan_layout( ogre_t3 ) uniform texture2D cardEmissive;
vulkan_layout( ogre_t4 ) uniform texture2D cardShadowRough;

// ONE CARD TO RELIGHT. rect = (atlasX, atlasY, size, 0); camPos = the capture
// camera's position; axisU.xyz / axisV.xyz = the card's u and v with the ortho
// window's width / height in .w; axisD.xyz = the card's outward axis.
struct CardRelight
{
	vec4 rect;
	vec4 camPos;
	vec4 axisU;
	vec4 axisV;
	vec4 axisD;
};
layout( std430, ogre_U0 ) readonly restrict buffer relightLayout { CardRelight relights[]; };

// ONE LIGHT, in WORLD space, in the pass buffer's own layout (OgreHlmsPbs.cpp's
// light0Buf writer): position.xyz = the position, or the direction TOWARDS a
// directional light; position.w = the type (0 directional, 1 point, 2 spot);
// diffuse.xyz = colour times power; diffuse.w = 1 when its visibility is the
// captured shadow term; attenuation = (range, linear, quadratic, 1 / range);
// spotParams = (1 / (cos inner/2 - cos outer/2), cos outer/2, falloff).
struct CardLight
{
	vec4 position;
	vec4 diffuse;
	vec4 attenuation;
	vec4 spotDirection;
	vec4 spotParams;
};
layout( std430, ogre_U1 ) readonly restrict buffer lightLayout
{
	uvec4 lightCount;
	CardLight lights[];
};

layout( vulkan( ogre_u2 ) vk_comma @insertpiece( uav2_pf_type ) )
uniform restrict writeonly image2D cardRadiance;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

// BRDF_Default's diffuse at V = N, times NdotL (200.BRDFs_piece_ps.any).
float jahCardDiffuse( vec3 N, vec3 L, float perceptualRoughness )
{
	float NdotL = clamp( dot( N, L ), 0.0, 1.0 );
	vec3 H = normalize( L + N );
	float VdotH = clamp( dot( N, H ), 0.0, 1.0 );
	float energyBias = perceptualRoughness * 0.5;
	float energyFactor = mix( 1.0, 1.0 / 1.51, perceptualRoughness );
	float fd90 = energyBias + 2.0 * VdotH * VdotH * perceptualRoughness;
	float lightScatter = 1.0 + ( fd90 - 1.0 ) * pow( 1.0 - NdotL, 5.0 );
	// viewScatter = 1.0 + ( fd90 - 1.0 ) * pow( 1.0 - NdotV, 5.0 ) = 1 at V = N.
	return NdotL * lightScatter * energyFactor;
}

void main()
{
	CardRelight r = relights[gl_WorkGroupID.z];
	uint size = uint( r.rect.z );
	uvec2 t = gl_GlobalInvocationID.xy;
	if( t.x >= size || t.y >= size )
		return;
	ivec2 at = ivec2( uint( r.rect.x ) + t.x, uint( r.rect.y ) + t.y );

	vec3 radiance = vec3( 0.0, 0.0, 0.0 );
	float depth = texelFetch( cardDepth, at, 0 ).x;
	if( depth > 0.0 )
	{
		vec3 kD = texelFetch( cardAlbedo, at, 0 ).xyz;
		vec3 nV = texelFetch( cardNormal, at, 0 ).xyz * 2.0 - 1.0;
		vec3 N = normalize( r.axisU.xyz * nV.x + r.axisV.xyz * nV.y + r.axisD.xyz * nV.z );
		vec2 sr = texelFetch( cardShadowRough, at, 0 ).xy;
		// Patch 0043's range: stored = (alpha - 0.001) * 1.001001.
		float alpha = sr.y / 1.001001 + 0.001;
		float perceptualRoughness = sqrt( max( alpha, 0.0 ) );
		// Image row 0 is the card's +v side (the capture camera's +Y).
		vec2 f = ( vec2( t ) + 0.5 ) / float( size );
		vec3 P = r.camPos.xyz + r.axisU.xyz * ( r.axisU.w * ( f.x - 0.5 ) ) +
				 r.axisV.xyz * ( r.axisV.w * ( 0.5 - f.y ) ) - r.axisD.xyz * depth;

		vec3 direct = vec3( 0.0, 0.0, 0.0 );
		uint n = lightCount.x;
		for( uint i = 0u; i < n; ++i )
		{
			CardLight l = lights[i];
			float type = l.position.w;
			vec3 L;
			float atten = 1.0;
			if( type < 0.5 )
			{
				L = l.position.xyz;
			}
			else
			{
				L = l.position.xyz - P;
				float d = length( L );
				if( d > l.attenuation.x || d <= 0.0 )
					continue;
				L *= 1.0 / d;
				atten = 1.0 / ( 0.5 + ( l.attenuation.y + l.attenuation.z * d ) * d );
				atten *= max( ( l.attenuation.x - d ) * l.attenuation.w, 0.0 );
				if( type > 1.5 )
				{
					float spotCosAngle = dot( -L, l.spotDirection.xyz );
					if( spotCosAngle < l.spotParams.y )
						continue;
					float spotAtten = clamp( ( spotCosAngle - l.spotParams.y ) * l.spotParams.x, 0.0, 1.0 );
					atten *= pow( spotAtten, l.spotParams.z );
				}
			}
			float visibility = l.diffuse.w > 0.5 ? sr.x : 1.0;
			direct += l.diffuse.xyz * ( jahCardDiffuse( N, L, perceptualRoughness ) * atten * visibility );
		}
		radiance = direct * kD + texelFetch( cardEmissive, at, 0 ).xyz;
	}
	imageStore( cardRadiance, at, vec4( radiance, 1.0 ) );
}
