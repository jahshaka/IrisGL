// THE LIGHTS' SPECULAR LOBE AT A CARD TEXEL, TOWARD THE READING RAY (PHOTON-3 stage 2) - or, in a GATHER job
// (JAH_HIT_LOBE_INTEGRAL defined), by the lobe's integral mu ( F0 A + w B ) E / pi (the lead's ruling: a gather of a
// few stochastic rays cannot sample a polished hit's near-delta lobe; jah_rq_hit_direct.glsl's header).
//
// The card relight (JahCardLight_cs.glsl) used to re-emit a surface's specular lobe by its integral -
// mu ( F0 A( mu, r ) + w B( mu, r ) ) of every light, the same way to every direction - so a polished metal
// floor's card sent the sun's lobe equally into every ray. A hit's radiance along its ray is
// sum over the lights of E_i mu_i vis_i f_s( L_i, V = -dir ), f_s = BRDF_Default's D Vis F; the read knows V,
// and every light it needs is in the ray tier's light table (RayQueryTier::updateHitTable,
// jah_rq_hit_direct.glsl's layout) - so it evaluates the lobe here, exactly, per light, as the relight takes
// them: the directionals, points and spots (no area light's lobe), a lamp by ITS OWN traced visibility (PHOTON-7
// stage 5, 5-3: the bit of its slot in the card's lamp table, found by the lamp's world id - the table's word 31 -
// or, past the table, the overflow's weighted byte), a
// directional through the cloud sheet, and THE SUN (the light the table flags, the cards' traced sun:
// SurfaceCache::cardSun) by the texel's own stored visibility (the SunVis layer: the still world's traced term
// times the movers' and the cloud's). Its cosine-weighted integral over V is
// what the relight re-emitted: the energy is unchanged, only the direction moves.
//
// HOW A CALLER BINDS IT: JAH_HIT_TABLE (uvec2: the table's device address, zero = none), and optionally
// JAH_CARD_CLOUD( P ) (the cloud sheet's transmittance at P for a directional light; 1 when undefined);
// jah_fork_brdf.glsl included before (jahSpecularLobe, the falloff and the spot cone). This file also
// declares the table's reference type for jah_rq_hit_direct.glsl.
//
// No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_CARD_LIGHTS_GLSL
#define JAH_RQ_CARD_LIGHTS_GLSL

#include "jah_fork_brdf.glsl"

layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer JahHitTableRef
{
	uint u[];
};

const uint kJahHitLightWords = 32u;

#ifndef JAH_CARD_CLOUD
#define JAH_CARD_CLOUD( P ) 1.0
#endif

#ifdef JAH_HIT_LOBE_INTEGRAL
// A GATHER ray's read takes the lobe's integral (the lead's ruling, jah_rq_hit_direct.glsl's header): the table's
// directional albedo, defined by the includer after this file (jah_spec_albedo.glsl).
void jahSpecularAlbedo( float v, float r, out float a, out float b );
#endif

vec3 jahCardLightsLobe( vec3 P, vec3 N, vec3 V, float r, vec3 F0, float w, float sunVis, uvec4 lampBits,
						uvec4 lampIds[12] )
{
	const uvec2 a = JAH_HIT_TABLE;
	if( ( a.x | a.y ) == 0u )
		return vec3( 0.0 );
	JahHitTableRef T = JahHitTableRef( a );
	const uint count = T.u[0];
	const uint base = T.u[2];
	vec3 sum = vec3( 0.0 );
	for( uint i = 0u; i < count; ++i )
	{
		const uint o = base + i * kJahHitLightWords;
		const vec4 v0 = uintBitsToFloat( uvec4( T.u[o], T.u[o + 1u], T.u[o + 2u], T.u[o + 3u] ) );
		const vec4 v1 = uintBitsToFloat( uvec4( T.u[o + 4u], T.u[o + 5u], T.u[o + 6u], T.u[o + 7u] ) );
		const vec4 v2 = uintBitsToFloat( uvec4( T.u[o + 8u], T.u[o + 9u], T.u[o + 10u], T.u[o + 11u] ) );
		const vec4 v3 = uintBitsToFloat( uvec4( T.u[o + 12u], T.u[o + 13u], T.u[o + 14u], T.u[o + 15u] ) );
		const int type = int( v0.w + 0.5 );
		vec3 L;
		float vis = 1.0;
		if( type == 0 )
		{
			L = normalize( v0.xyz );
			// the cards' traced sun (v2.w = 1: the table's flag) by the texel's stored visibility; another
			// directional unshadowed, through the cloud sheet (the relight's rule)
			vis = v2.w > 0.5 ? sunVis : JAH_CARD_CLOUD( P );
		}
		else if( type == 1 || type == 2 )
		{
			const vec3 d = v0.xyz - P;
			const float dist = length( d );
			if( !( dist > 0.0 ) || ( v1.w > 0.0 && dist > v1.w ) )
				continue;
			L = d / dist;
			// ITS OWN BIT: its world id (word 31) in the card's lamp table, or the overflow's byte past it
			const uint id = T.u[o + 31u];
			float lampVis = jahSunVisRest( lampBits );
			for( int q = 0; q < int( kJahCardLampTable ); ++q )
				if( lampIds[q >> 2][q & 3] == id )
				{
					lampVis = jahSunVisLamp( lampBits, uint( q ) );
					break;
				}
			vis = lampVis * jahLightAttenuation( dist, v1.w, v2.w );
			if( type == 2 )
			{
				const float spotCos = dot( -L, v2.xyz );
				vis *= spotCos >= v3.y ? jahSpotAttenuation( spotCos, v3.xyz ) : 0.0;
			}
		}
		else
			continue;
		const float mu = dot( N, L );
		if( !( mu > 0.0 ) || !( vis > 0.0 ) )
			continue;
#ifdef JAH_HIT_LOBE_INTEGRAL
		float sa, sb;
		jahSpecularAlbedo( mu, r, sa, sb );
		sum += ( mu * vis * 0.318309886 ) * v1.xyz * ( F0 * sa + w * sb );
#else
		// A LAMP HAS A SIZE (PHOTON-7 stage 11): a point or spot light's glint widened by its sphere's angular size, its
		// energy the point lobe's (JahBrdf's jahSphereLightWiden, the pixel's and the hit's one function)
		float rS = r;
		float sNorm = 1.0;
		if( type == 1 || type == 2 )
		{
			float aW;
			sNorm = jahSphereLightWiden( max( r * r, 0.001 ), v2.w, length( v0.xyz - P ), mu, aW );
			rS = sqrt( aW );
		}
		sum += ( mu * vis * sNorm ) * v1.xyz * jahSpecularLobe( N, L, V, rS, F0, w );
#endif
	}
	return sum;
}

#endif   // JAH_RQ_CARD_LIGHTS_GLSL
