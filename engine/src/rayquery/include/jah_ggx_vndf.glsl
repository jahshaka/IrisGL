// THE GGX VISIBLE-NORMAL SAMPLE - one text for the passes that draw a GGX lobe (PHOTON-7 stage 7, 7-3: the gather's
// rough specular, rq_probe_integrate.comp; photon-7r's reflection draw carries the same function, jah_reflect_draw.glsl,
// and the two become one include at the photon-7r merge). Pure; nothing here reads a binding.
#ifndef JAH_GGX_VNDF_GLSL
#define JAH_GGX_VNDF_GLSL

/// THE GGX VISIBLE-NORMAL SAMPLE (Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 2018): a half vector
/// in the surface's tangent frame given the view direction in that frame. Sampling the VNDF cancels D and the masking
/// term, leaving the estimator weight F G2 / G1 - NEITHER applied: the readers write a radiance HlmsPbs multiplies by the
/// surface's own BRDF (F here would apply it twice); G2 / G1 is 1 at normal incidence and falls at grazing, the
/// approximation the probe path makes too. At alpha 0 it returns the normal exactly.
vec3 jahGgxVndf( vec3 Ve, float alpha, vec2 u )
{
	const vec3 Vh = normalize( vec3( alpha * Ve.x, alpha * Ve.y, Ve.z ) );
	const float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
	const vec3 T1 = lensq > 0.0 ? vec3( -Vh.y, Vh.x, 0.0 ) * inversesqrt( lensq ) : vec3( 1.0, 0.0, 0.0 );
	const vec3 T2 = cross( Vh, T1 );
	const float r = sqrt( u.x );
	const float phi = 6.2831853 * u.y;
	const float t1 = r * cos( phi );
	float t2 = r * sin( phi );
	const float s = 0.5 * ( 1.0 + Vh.z );
	t2 = ( 1.0 - s ) * sqrt( 1.0 - t1 * t1 ) + s * t2;
	const vec3 Nh = t1 * T1 + t2 * T2 + sqrt( max( 0.0, 1.0 - t1 * t1 - t2 * t2 ) ) * Vh;
	return normalize( vec3( alpha * Nh.x, alpha * Nh.y, max( 0.0, Nh.z ) ) );
}

#endif
