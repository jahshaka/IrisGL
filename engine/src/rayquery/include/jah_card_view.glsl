// A CARD READ FROM A DIRECTION — THE VIEW TERM OF THE DIFFUSE LOBE, RESTORED AT
// THE READ (PHOTON-CARDS-5). One file, two consumers: the surface cache's relight
// job (JahCardLight_cs.glsl, the piece `JahCardView` this file is wrapped into)
// writes what the read needs, and every ray job's card read (jah_rq_card.glsl,
// through glslang) applies it.
//
// WHY. HlmsPbs's diffuse is the normalised Disney lobe (the fork's JahBrdf):
//   kD x E x NdotL x energy x lightScatter( fd90 ) x viewScatter( fd90 ),
//   fd90 = 0.5 r + 2 r ( V.H )^2,  scatter( x ) = 1 + ( fd90 - 1 )( 1 - x )^5.
// It is VIEW-DEPENDENT: at a grazing eye with the light behind it the pixel is
// up to 1.6-1.8x its head-on value (measured, PHOTON-CARDS-5: 1.581 for the
// gi.hit_shade (d) sun at N.V = 0.15, the lobe's closed form 1.581). A card texel
// is read from every direction, so it STORES the head-on value (V = N: the view
// scatter is 1, fd90 takes H = normalize( L + N )) and the READ restores the
// view term for the ray that reads it. The environment half is the same story
// with a closed form: the card stores the lobe's hemispherical-mean albedo
// jahDiffuseAlbedoHemi( r ), the pixel reflects jahDiffuseAlbedo( N.V, r ).
//
// WHAT THE READ NEEDS, per texel: the three stored terms apart (the Radiance
// layer = direct + indirect + emissive, the Indirect layer, the Emissive layer:
// the direct half is the difference), the stored normal and roughness, and ONE
// LIGHT DIRECTION — the light-weighted mean of the lights' directions, the
// weight each light's share of the direct half's luminance. The relight writes
// it octahedrally into the two alpha channels no reader used (the Albedo and
// Normal layers' alpha, both RGBA8, 8 bits a component: about 0.5 degree): NO
// NEW LAYER, NO VRAM. (V.H)^2 = ( 1 + L.V ) / 2 is LINEAR in L, so the mean of
// fd90 over the lights is fd90 of the MEAN direction exactly — and the view
// scatter, linear in fd90, with it. What is stored is that mean NORMALISED (two
// channels hold a direction, not its length), so the reconstruction is EXACT for
// one light and, for several, off by the spread of their directions: the
// error is r x ( 1 - N.V )^5 x ( L_mean - normalize( L_mean ) ).V of the direct
// half at most, plus the light scatter's cross term (1 - N.L)^5, which the one
// direction also stands in for.
//
// No Hlms directive mark anywhere in this file (it is wrapped into a piece).
// Needs jahDisneyDiffuse (JahBrdf) and jahDiffuseAlbedo / jahDiffuseAlbedoHemi
// (JahDiffuseAlbedo) defined first: the fork's text, inserted as pieces by the
// relight and unwrapped for glslang by the build (jah_fork_brdf.glsl).

#ifndef JAH_CARD_VIEW_GLSL
#define JAH_CARD_VIEW_GLSL

/// A unit direction to two numbers in [0, 1] (the octahedral map).
vec2 jahCardOctEncode( vec3 n )
{
	n /= max( abs( n.x ) + abs( n.y ) + abs( n.z ), 1e-8 );
	vec2 e = n.xy;
	if( n.z < 0.0 )
	{
		const vec2 s = vec2( e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0 );
		e = ( 1.0 - abs( e.yx ) ) * s;
	}
	return e * 0.5 + 0.5;
}

/// ...and back.
vec3 jahCardOctDecode( vec2 f )
{
	const vec2 e = f * 2.0 - 1.0;
	vec3 n = vec3( e.x, e.y, 1.0 - abs( e.x ) - abs( e.y ) );
	if( n.z < 0.0 )
	{
		const vec2 s = vec2( n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0 );
		n.xy = ( 1.0 - abs( n.yx ) ) * s;
	}
	return normalize( n );
}

/// THE RADIANCE A CARD TEXEL SENDS TOWARDS `V` (unit, from the surface to the
/// viewer). `radiance`, `indirect`, `emissive` = the three stored terms; `N` =
/// the stored (shading) normal, world; `L` = the stored mean light direction,
/// world, unit; `r` = the perceptual roughness. The direct half takes the lobe
/// at V over the lobe the relight stored (V = N) — the energy factor cancels,
/// the rest is JahBrdf's own function; the environment half takes the
/// directional albedo at N.V over the hemispherical mean the relight stored.
/// At V = N the direct half is returned as stored.
vec3 jahCardViewRadiance( vec3 radiance, vec3 indirect, vec3 emissive, vec3 N, vec3 L, float r,
						  vec3 V )
{
	const vec3 direct = max( radiance - indirect - emissive, vec3( 0.0 ) );
	const float NdotV = clamp( dot( N, V ), 1e-4, 1.0 );
	const float NdotL = clamp( dot( N, L ), 0.0, 1.0 );
	float viewFactor = 1.0;
	if( NdotL > 0.0 )
	{
		const float VdotH = clamp( dot( V, normalize( L + V ) ), 0.0, 1.0 );
		const float NdotHn = clamp( dot( N, normalize( L + N ) ), 0.0, 1.0 );
		const float atV = jahDisneyDiffuse( NdotL, NdotV, VdotH, r );
		const float atN = jahDisneyDiffuse( NdotL, 1.0, NdotHn, r );
		viewFactor = atN > 0.0 ? atV / atN : 1.0;
	}
	const float envFactor = jahDiffuseAlbedo( NdotV, r ) / max( jahDiffuseAlbedoHemi( r ), 1e-4 );
	return direct * viewFactor + indirect * envFactor + emissive;
}

#endif   // JAH_CARD_VIEW_GLSL
